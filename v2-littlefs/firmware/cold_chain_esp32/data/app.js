/* ==========================================================================
   Cold Chain Monitor — on-device dashboard logic
   Talks only to the ESP32 it is served from:
     GET  /api/live                 latest reading + system status
     GET  /api/history?range=live   last 10 min (5 s samples)
     GET  /api/history?range=trend  last 2 h (30 s samples)
     GET  /api/events               event log, newest first
     POST /api/mute[?off=1]         silence / re-enable the buzzer
   Times from the device are RTC local time encoded as unix seconds, so they
   are always formatted with UTC getters (no browser time-zone shift).
   ========================================================================== */
(function () {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const css = getComputedStyle(document.documentElement);
  const C = (name) => css.getPropertyValue(name).trim();
  const COL = {
    core: C('--s-core'), amb: C('--s-amb'), hum: C('--s-hum'),
    safe: C('--safe'), warn: C('--warn'), danger: C('--danger'),
    muted: C('--muted'), grid: C('--grid'), text: C('--text'),
  };
  const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  const POLL_LIVE_MS = 2500, POLL_EVENTS_MS = 5000, POLL_TREND_MS = 30000, LOST_AFTER_MS = 12000;

  const state = {
    live: null, liveAt: 0, lastOkAt: 0, first: true,
    hist: { live: null, trend: null }, range: 'live',
    seenEvents: new Set(), limits: { min: 2, max: 8 },
    alert: false, holdKey: null, table: false,
  };

  // ---------------------------------------------------------------- helpers
  const pad = (n) => String(n).padStart(2, '0');
  function fmtTime(t, sec = true) {
    const d = new Date(t * 1000);
    return pad(d.getUTCHours()) + ':' + pad(d.getUTCMinutes()) + (sec ? ':' + pad(d.getUTCSeconds()) : '');
  }
  function fmtDate(t) {
    const d = new Date(t * 1000);
    return d.getUTCFullYear() + '-' + pad(d.getUTCMonth() + 1) + '-' + pad(d.getUTCDate()) + ' ' + fmtTime(t);
  }
  function fmtDur(s) {
    s = Math.max(0, Math.floor(s));
    const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    if (d) return d + 'd ' + h + 'h';
    if (h) return h + 'h ' + pad(m) + 'm';
    return m + 'm ' + pad(s % 60) + 's';
  }
  function fmtBytes(b) { return b > 1048576 ? (b / 1048576).toFixed(2) + ' MB' : (b / 1024).toFixed(0) + ' KB'; }
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
  const ease = (t) => 1 - Math.pow(1 - t, 3);

  async function getJSON(url, opts = {}) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), 5000);
    try {
      const r = await fetch(url, Object.assign({ cache: 'no-store', signal: ctl.signal }, opts));
      if (!r.ok) throw new Error(r.status);
      return await r.json();
    } finally { clearTimeout(timer); }
  }

  // Smoothly animate any numeric value; calls fn(v) each frame.
  const tweens = new Map();
  function tween(key, from, to, dur, fn) {
    if (reduceMotion || from == null || isNaN(from)) { fn(to); tweens.delete(key); return; }
    tweens.set(key, { from, to, start: performance.now(), dur, fn });
  }
  function runTweens(now) {
    tweens.forEach((tw, key) => {
      const p = clamp((now - tw.start) / tw.dur, 0, 1);
      tw.fn(tw.from + (tw.to - tw.from) * ease(p));
      if (p >= 1) tweens.delete(key);
    });
  }
  const shown = {};   // last displayed values, so tweens start where the eye is
  function animNum(el, key, to, dec) {
    const from = shown[key];
    shown[key] = to;
    tween(key, from, to, 900, (v) => { el.textContent = v.toFixed(dec); });
  }

  function toast(msg) {
    const t = $('toast');
    t.textContent = msg; t.hidden = false;
    t.style.animation = 'none'; void t.offsetWidth; t.style.animation = '';
    clearTimeout(toast.h); toast.h = setTimeout(() => { t.hidden = true; }, 3200);
  }

  // ---------------------------------------------------------------- icons
  const ICON = {
    safe: '<svg viewBox="0 0 24 24"><path d="M5 12.5l4.5 4.5L19 7.5" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"/></svg>',
    alert: '<svg viewBox="0 0 24 24"><path d="M12 3L2 20h20L12 3z" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linejoin="round"/><path d="M12 9.5v5M12 17.2v.3" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/></svg>',
    warn: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="2.2"/><path d="M12 7v6M12 16.5v.3" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/></svg>',
    wait: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="2.2" stroke-dasharray="4 4"><animateTransform attributeName="transform" type="rotate" from="0 12 12" to="360 12 12" dur="3s" repeatCount="indefinite"/></circle></svg>',
  };
  const EV_ICON = ['i', '✓', '!', '⚠'];

  // ================================================================ GAUGE
  const G = { cx: 120, cy: 118, r: 88, a0: 135, sweep: 270, lo: -10, hi: 30 };
  const polar = (deg, r = G.r) => {
    const a = deg * Math.PI / 180;
    return [G.cx + r * Math.cos(a), G.cy + r * Math.sin(a)];
  };
  const tempToDeg = (t) => G.a0 + G.sweep * (clamp(t, G.lo, G.hi) - G.lo) / (G.hi - G.lo);
  function arc(d0, d1, r = G.r) {
    if (d1 - d0 < 0.01) d1 = d0 + 0.01;
    const [x0, y0] = polar(d0, r), [x1, y1] = polar(d1, r);
    return `M${x0.toFixed(2)} ${y0.toFixed(2)} A${r} ${r} 0 ${d1 - d0 > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
  }
  function buildGauge() {
    $('gTrack').setAttribute('d', arc(G.a0, G.a0 + G.sweep));
    $('gSafe').setAttribute('d', arc(tempToDeg(state.limits.min), tempToDeg(state.limits.max)));
    let ticks = '';
    for (let t = G.lo; t <= G.hi; t += 10) {
      const d = tempToDeg(t);
      const [x0, y0] = polar(d, G.r + 9), [x1, y1] = polar(d, G.r + 14), [tx, ty] = polar(d, G.r + 24);
      ticks += `<line x1="${x0}" y1="${y0}" x2="${x1}" y2="${y1}"/><text x="${tx}" y="${ty + 3}">${t}</text>`;
    }
    $('gTicks').innerHTML = ticks;
    setGauge(G.lo);
  }
  function setGauge(t) {
    const d = tempToDeg(t);
    $('gValue').setAttribute('d', arc(G.a0, d));
    const [x, y] = polar(d);
    $('gKnob').setAttribute('cx', x); $('gKnob').setAttribute('cy', y);
  }

  // ================================================================ THERMOMETER
  const TH = { lo: -10, hi: 50, yBottom: 162, yTop: 16 };
  function buildThermo() {
    let s = '';
    for (let t = TH.lo; t <= TH.hi; t += 10) {
      const y = thermoY(t);
      s += `<line x1="40" y1="${y}" x2="45" y2="${y}"/><text x="47" y="${y + 3}">${t}</text>`;
    }
    $('thermoTicks').innerHTML = s;
    setThermo(TH.lo);
  }
  const thermoY = (t) => TH.yBottom - (TH.yBottom - TH.yTop) * (clamp(t, TH.lo, TH.hi) - TH.lo) / (TH.hi - TH.lo);
  function setThermo(t) {
    const y = thermoY(t);
    const bar = $('mercuryBar');
    bar.setAttribute('y', y); bar.setAttribute('height', 200 - y);
  }

  // ================================================================ HUMIDITY
  function setHumidity(h) {
    // wave crest sits at y=10 inside the group; full vessel = 200 units
    const y = 200 - 200 * clamp(h, 0, 100) / 100 - 10;
    $('waveGroup').style.transform = `translateY(${y}px)`;
  }
  function humNote(h) {
    if (h < 30) return 'Dry air';
    if (h <= 70) return 'Normal range for a cold box';
    if (h <= 85) return 'Humid — watch for condensation on vials/labels';
    return 'Very humid — condensation likely';
  }

  // ================================================================ CHART ENGINE
  // Small canvas line chart: one y-axis, optional safe band, excursion shading,
  // animated draw-in, sliding time domain, crosshair + tooltip on hover/touch.
  function makeChart(canvas, tipEl, cfg) {
    const ch = { canvas, tipEl, cfg, data: null, reveal: 1, revealStart: 0, dom: null, domFrom: null, domStart: 0, hover: -1, w: 0, h: 0 };
    const ctx = canvas.getContext('2d');

    function resize() {
      const r = canvas.getBoundingClientRect();
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      ch.w = r.width; ch.h = r.height;
      canvas.width = Math.round(r.width * dpr); canvas.height = Math.round(r.height * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }
    new ResizeObserver(resize).observe(canvas);
    resize();

    ch.setData = (data, animate) => {
      const had = ch.data && ch.data.t.length;
      ch.data = data;
      if (!data || !data.t.length) return;
      const target = [data.t[0], data.t[data.t.length - 1]];
      if (animate || !had) { ch.reveal = 0; ch.revealStart = performance.now(); ch.dom = target; ch.domFrom = null; }
      else { ch.domFrom = ch.dom ? ch.dom.slice() : target; ch.domTo = target; ch.domStart = performance.now(); }
      if (!ch.dom) ch.dom = target;
    };

    function layout() {
      return { l: 44, r: 12, t: 12, b: 26, W: ch.w - 56, H: ch.h - 38 };
    }

    ch.draw = (now) => {
      const d = ch.data;
      ctx.clearRect(0, 0, ch.w, ch.h);
      if (!d || !d.t.length || ch.w < 10) return;

      // animate reveal + sliding domain
      if (ch.reveal < 1) ch.reveal = reduceMotion ? 1 : clamp((now - ch.revealStart) / 1100, 0, 1);
      if (ch.domFrom) {
        const p = reduceMotion ? 1 : clamp((now - ch.domStart) / 700, 0, 1), e = ease(p);
        ch.dom = [ch.domFrom[0] + (ch.domTo[0] - ch.domFrom[0]) * e, ch.domFrom[1] + (ch.domTo[1] - ch.domFrom[1]) * e];
        if (p >= 1) ch.domFrom = null;
      }
      const L = layout();
      let [t0, t1] = ch.dom;
      if (t1 - t0 < 60) t0 = t1 - 60;

      // y range: always include the safe band so it's never off-screen
      let lo = Infinity, hi = -Infinity;
      cfg.series.forEach((s) => d[s.key].forEach((v) => { if (v < lo) lo = v; if (v > hi) hi = v; }));
      if (cfg.band) { lo = Math.min(lo, cfg.band.lo - 1); hi = Math.max(hi, cfg.band.hi + 1); }
      if (cfg.fixed) { lo = cfg.fixed[0]; hi = cfg.fixed[1]; }
      const span = hi - lo || 1; lo -= span * 0.08; hi += span * 0.08;
      const step = niceStep((hi - lo) / 5);
      lo = Math.floor(lo / step) * step; hi = Math.ceil(hi / step) * step;

      const X = (t) => L.l + (t - t0) / (t1 - t0) * L.W;
      const Y = (v) => L.t + (1 - (v - lo) / (hi - lo)) * L.H;
      ch.X = X; ch.Y = Y; ch.L = L;

      // grid + y labels
      ctx.font = '11px ui-monospace, Consolas, monospace';
      ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
      for (let v = lo; v <= hi + 1e-9; v += step) {
        const y = Math.round(Y(v)) + 0.5;
        ctx.strokeStyle = COL.grid; ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(L.l, y); ctx.lineTo(L.l + L.W, y); ctx.stroke();
        ctx.fillStyle = COL.muted; ctx.fillText(+v.toFixed(1) + (cfg.unit || ''), L.l - 6, y);
      }
      // x labels
      ctx.textAlign = 'center'; ctx.textBaseline = 'top';
      const nx = Math.max(2, Math.floor(L.W / 90));
      for (let i = 0; i <= nx; i++) {
        const t = t0 + (t1 - t0) * i / nx;
        ctx.textAlign = i === 0 ? 'left' : i === nx ? 'right' : 'center';
        ctx.fillStyle = COL.muted; ctx.fillText(fmtTime(t, (t1 - t0) < 1200), X(t), L.t + L.H + 8);
      }

      ctx.save();
      ctx.beginPath(); ctx.rect(L.l, L.t - 4, L.W * ease(ch.reveal), L.H + 8); ctx.clip();

      // safe band
      if (cfg.band) {
        const yA = Y(cfg.band.hi), yB = Y(cfg.band.lo);
        ctx.fillStyle = 'rgba(67,217,200,.09)'; ctx.fillRect(L.l, yA, L.W, yB - yA);
        ctx.setLineDash([4, 4]); ctx.strokeStyle = 'rgba(67,217,200,.55)'; ctx.lineWidth = 1;
        [yA, yB].forEach((y) => { ctx.beginPath(); ctx.moveTo(L.l, y); ctx.lineTo(L.l + L.W, y); ctx.stroke(); });
        ctx.setLineDash([]);
      }
      // excursion shading
      if (cfg.excursions && d.x) {
        ctx.fillStyle = 'rgba(240,85,95,.13)';
        const n = d.t.length;
        for (let i = 0; i < n; i++) {
          if (!d.x[i]) continue;
          const a = i > 0 ? (d.t[i - 1] + d.t[i]) / 2 : d.t[i];
          const b = i < n - 1 ? (d.t[i] + d.t[i + 1]) / 2 : d.t[i];
          ctx.fillRect(X(a), L.t, Math.max(2, X(b) - X(a)), L.H);
        }
      }
      // series
      cfg.series.forEach((s) => {
        const vals = d[s.key];
        ctx.beginPath();
        d.t.forEach((t, i) => { const x = X(t), y = Y(vals[i]); i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
        if (s.area) {
          ctx.save();
          ctx.lineTo(X(d.t[d.t.length - 1]), L.t + L.H); ctx.lineTo(X(d.t[0]), L.t + L.H); ctx.closePath();
          const g = ctx.createLinearGradient(0, L.t, 0, L.t + L.H);
          g.addColorStop(0, hexA(s.color, .35)); g.addColorStop(1, hexA(s.color, 0));
          ctx.fillStyle = g; ctx.fill(); ctx.restore();
          ctx.beginPath();
          d.t.forEach((t, i) => { const x = X(t), y = Y(vals[i]); i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
        }
        ctx.strokeStyle = s.color; ctx.lineWidth = 2; ctx.lineJoin = 'round'; ctx.lineCap = 'round';
        ctx.stroke();
      });
      ctx.restore();

      // pulsing "latest" dots once fully drawn
      if (ch.reveal >= 1) {
        const i = d.t.length - 1, pulse = reduceMotion ? 0 : (Math.sin(now / 350) + 1) / 2;
        cfg.series.forEach((s) => {
          const x = X(d.t[i]), y = Y(d[s.key][i]);
          ctx.beginPath(); ctx.arc(x, y, 5 + pulse * 5, 0, 7); ctx.fillStyle = hexA(s.color, .25 * (1 - pulse)); ctx.fill();
          ctx.beginPath(); ctx.arc(x, y, 4, 0, 7); ctx.fillStyle = s.color; ctx.fill();
          ctx.lineWidth = 2; ctx.strokeStyle = '#0E1826'; ctx.stroke();
        });
      }

      // crosshair
      if (ch.hover >= 0 && ch.hover < d.t.length) {
        const i = ch.hover, x = X(d.t[i]);
        ctx.strokeStyle = 'rgba(231,238,245,.35)'; ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(x, L.t); ctx.lineTo(x, L.t + L.H); ctx.stroke();
        cfg.series.forEach((s) => {
          ctx.beginPath(); ctx.arc(x, Y(d[s.key][i]), 4.5, 0, 7);
          ctx.fillStyle = s.color; ctx.fill(); ctx.lineWidth = 2; ctx.strokeStyle = '#0E1826'; ctx.stroke();
        });
      }
    };

    function pick(ev) {
      const d = ch.data;
      if (!d || !d.t.length || !ch.X) return;
      const r = canvas.getBoundingClientRect(), mx = ev.clientX - r.left;
      let best = -1, bd = Infinity;
      d.t.forEach((t, i) => { const dist = Math.abs(ch.X(t) - mx); if (dist < bd) { bd = dist; best = i; } });
      if (bd > 40) { hide(); return; }
      ch.hover = best;
      let html = `<div class="t">${fmtDate(d.t[best])}</div>`;
      [...cfg.series].reverse().forEach((s) => {
        html += `<div class="r"><span><i class="sw" style="background:${s.color}"></i>${s.label}</span><b>${d[s.key][best].toFixed(s.dec)}${s.unit}</b></div>`;
      });
      if (cfg.excursions && d.x) {
        html += d.x[best] ? `<div class="st" style="color:${COL.danger}">⚠ Out of safe range</div>`
                          : `<div class="st" style="color:${COL.safe}">✓ Within 2–8 °C</div>`;
      }
      tipEl.innerHTML = html; tipEl.hidden = false;
      const x = ch.X(d.t[best]), tw = tipEl.offsetWidth;
      tipEl.style.left = (x + 14 + tw > ch.w ? x - tw - 14 : x + 14) + 'px';
      tipEl.style.top = '8px';
    }
    function hide() { ch.hover = -1; tipEl.hidden = true; }
    canvas.addEventListener('pointermove', pick);
    canvas.addEventListener('pointerdown', pick);
    canvas.addEventListener('pointerleave', hide);
    return ch;
  }
  function niceStep(raw) {
    const p = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / p;
    return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * p;
  }
  function hexA(hex, a) {
    const n = parseInt(hex.slice(1), 16);
    return `rgba(${n >> 16 & 255},${n >> 8 & 255},${n & 255},${a})`;
  }

  let tempChart, humChart;
  function buildCharts() {
    tempChart = makeChart($('tempChart'), $('tempTip'), {
      unit: '°', band: { lo: state.limits.min, hi: state.limits.max }, excursions: true,
      series: [
        { key: 'a', label: 'Box air', color: COL.amb, unit: ' °C', dec: 1 },
        { key: 'c', label: 'Product (core)', color: COL.core, unit: ' °C', dec: 2 },
      ],
    });
    humChart = makeChart($('humChart'), $('humTip'), {
      unit: '%',
      series: [{ key: 'h', label: 'Humidity', color: COL.hum, unit: ' %', dec: 1, area: true }],
    });
  }

  // ================================================================ FROST BACKGROUND
  function frost() {
    const cv = $('frost'), ctx = cv.getContext('2d');
    let W, H, flakes = [];
    function size() {
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      W = innerWidth; H = innerHeight;
      cv.width = W * dpr; cv.height = H * dpr; ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      const n = Math.round(clamp(W * H / 22000, 25, 80));
      flakes = Array.from({ length: n }, () => ({
        x: Math.random() * W, y: Math.random() * H, r: Math.random() * 1.8 + .4,
        vy: Math.random() * .35 + .12, vx: (Math.random() - .5) * .2, ph: Math.random() * 6.28,
      }));
    }
    addEventListener('resize', size); size();
    return (now) => {
      ctx.clearRect(0, 0, W, H);
      const rgb = state.alert ? '240,85,95' : '160,215,240';
      flakes.forEach((f) => {
        f.y += f.vy * (state.alert ? 2.2 : 1); f.x += f.vx + Math.sin(now / 1800 + f.ph) * .15;
        if (f.y > H + 4) { f.y = -4; f.x = Math.random() * W; }
        if (f.x < -4) f.x = W + 4; if (f.x > W + 4) f.x = -4;
        const a = .25 + .35 * (Math.sin(now / 900 + f.ph) + 1) / 2;
        ctx.beginPath(); ctx.arc(f.x, f.y, f.r, 0, 7); ctx.fillStyle = `rgba(${rgb},${a})`; ctx.fill();
      });
    };
  }

  // ================================================================ RENDER: LIVE
  function renderLive(j) {
    const prev = state.live;
    state.live = j; state.liveAt = Date.now();
    if (j.limits) state.limits = j.limits;
    $('deviceId').textContent = j.device_id || '—';

    const r = j.reading || {};
    const has = r.core_temp != null;
    const core = +r.core_temp, amb = +r.ambient_temp, hum = +r.ambient_hum;

    if (has) {
      animNum($('coreVal'), 'core', core, 1);
      tween('gauge', shown.gaugeT, core, 1100, setGauge); shown.gaugeT = core;
      animNum($('ambVal'), 'amb', amb, 1);
      tween('thermo', shown.thermoT, amb, 1100, setThermo); shown.thermoT = amb;
      animNum($('humVal'), 'hum', hum, 0);
      setHumidity(hum);
      $('humNote').textContent = humNote(hum);
      $('gauge').classList.toggle('alert', !!r.excursion);
      renderTrendArrow(core);
    }

    // ---- status banner
    const ex = j.excursion || {};
    const banner = $('banner');
    let st, title, sub;
    if (!has && j.sensor_fault) { st = 'warn'; title = 'Sensor fault'; sub = 'A sensor is not responding — check the DS18B20 / DHT22 wiring.'; }
    else if (!has) { st = 'wait'; title = 'Waiting for first reading'; sub = 'The unit samples its sensors every 5 seconds.'; }
    else if (ex.active) {
      st = 'alert';
      title = core > state.limits.max ? 'TOO WARM — vaccines at risk' : 'TOO COLD — freezing risk';
      sub = `Product at ${core.toFixed(1)} °C, outside ${state.limits.min}–${state.limits.max} °C` +
            (ex.since ? ` for ${fmtDur(j.now - ex.since)}` : '') + '. Move vaccines to a working cold box.';
    } else if (j.sensor_fault) { st = 'warn'; title = 'Sensor read error'; sub = 'Showing the last good reading. Check probe wiring.'; }
    else { st = 'safe'; title = 'SAFE — within 2–8 °C'; sub = `Product temperature is stable at ${core.toFixed(1)} °C.`; }
    if (banner.dataset.state !== st) $('bannerIcon').innerHTML = ICON[st];
    banner.dataset.state = st;
    $('bannerTitle').textContent = title;
    $('bannerSub').textContent = sub;
    state.alert = st === 'alert';

    const mb = $('muteBtn');
    mb.hidden = !ex.active;
    mb.classList.toggle('muted', !!ex.muted);
    mb.textContent = ex.muted ? `Silenced ${fmtDur(ex.mute_left_s)} · Unmute` : 'Silence alarm';

    // ---- stats
    const s = j.stats || {};
    $('sMin').textContent = s.min != null ? (+s.min).toFixed(1) + ' °C' : '—';
    $('sAvg').textContent = s.avg != null ? (+s.avg).toFixed(1) + ' °C' : '—';
    $('sMax').textContent = s.max != null ? (+s.max).toFixed(1) + ' °C' : '—';
    if (s.in_range_pct != null) {
      const p = +s.in_range_pct;
      animNum($('inRange'), 'inr', p, p >= 99.95 || p === 0 ? 0 : 1);
      $('ringFg').style.strokeDashoffset = 314.16 * (1 - p / 100);
      $('ringFg').style.stroke = p >= 95 ? COL.safe : p >= 80 ? COL.warn : COL.danger;
      $('inRangeNote').textContent = `${s.cycles} logged readings since boot`;
    }
    $('excEvents').textContent = ex.events || 0;

    renderHoldover(j);
    renderHealth(j);

    // new sample -> extend the live chart without refetching
    if (has && (!prev || !prev.reading || prev.reading.t !== r.t)) appendLive(r);
  }

  function renderTrendArrow(core) {
    const h = state.hist.live, el = $('coreTrend');
    if (!h || h.t.length < 3) { el.textContent = ' '; return; }
    const tNow = h.t[h.t.length - 1];
    let i = h.t.length - 1;
    while (i > 0 && tNow - h.t[i] < 60) i--;
    const d = core - h.c[i];
    el.className = 'trend' + (d > 0.15 ? ' up' : d < -0.15 ? ' down' : '');
    el.textContent = Math.abs(d) <= 0.15 ? '● Stable' : (d > 0 ? '▲ ' : '▼ ') + Math.abs(d).toFixed(1) + ' °C / min';
  }

  function renderHoldover(j) {
    const sv = j.server || {}, hg = $('hourglass');
    if (sv.holdover_h == null) {
      $('holdVal').textContent = '--';
      $('holdNote').textContent = 'Needs the Flask server (runs the ML model). Not reachable yet.';
      hg.classList.add('idle');
      return;
    }
    const h = +sv.holdover_h;
    hg.classList.remove('idle');
    if (state.holdKey !== sv.holdover_time) {
      if (state.holdKey != null) { hg.classList.remove('flip'); void hg.offsetWidth; hg.classList.add('flip'); }
      state.holdKey = sv.holdover_time;
    }
    animNum($('holdVal'), 'hold', h, 1);
    const age = j.now - sv.holdover_time;
    $('holdNote').textContent = (age > 300 ? `Last prediction ${fmtDur(age)} ago (server offline)` : `Predicted by the server at ${fmtTime(sv.holdover_time, false)}`) +
      ' · RandomForest estimate, not a guarantee.';
  }

  function tile(id, cls, val, sub) {
    const card = $(id).closest('.mini');
    card.classList.remove('ok', 'warn', 'bad'); if (cls) card.classList.add(cls);
    $(id).textContent = val; $(id + 'Sub').textContent = sub;
  }
  function renderHealth(j) {
    const w = j.wifi || {}, sv = j.server || {}, sd = j.sd || {}, fs = j.fs || {}, sys = j.sys || {};
    if (w.connected) {
      const q = w.rssi >= -60 ? 'Excellent' : w.rssi >= -70 ? 'Good' : w.rssi >= -80 ? 'Fair' : 'Weak';
      tile('hWifi', w.rssi >= -80 ? 'ok' : 'warn', w.rssi + ' dBm', q + ' · ' + (w.ssid || '') + ' · ' + w.ip);
      $('wifiText').textContent = 'Online'; $('wifiChip').className = 'chip ok';
    } else {
      tile('hWifi', 'warn', 'Offline', 'Logging to SD · retrying');
      $('wifiText').textContent = 'Offline'; $('wifiChip').className = 'chip bad';
    }
    tile('hAp', 'ok', w.ap_ip || '—', `${w.ap_ssid || ''} · ${w.ap_clients || 0} connected`);
    if (sv.last_ok) tile('hSrv', 'ok', 'Uploading', 'Last upload ' + (sv.last_send ? fmtTime(sv.last_send) : '—'));
    else tile('hSrv', w.connected ? 'bad' : 'warn', 'Not reachable', sv.host || '');
    if (!sd.ready) tile('hSd', 'bad', 'No card', 'Insert a FAT32 microSD');
    else tile('hSd', sd.backlog_rows > 0 ? 'warn' : 'ok', sd.backlog_rows + ' rows',
      sd.backlog_rows > 0 ? 'Waiting to upload' : sd.last_sync ? 'All synced · ' + fmtTime(sd.last_sync, false) : 'Nothing pending');
    if (fs.ready) {
      const pct = fs.total ? fs.used / fs.total * 100 : 0;
      tile('hFs', pct > 85 ? 'warn' : 'ok', pct.toFixed(0) + '% used', fmtBytes(fs.used) + ' of ' + fmtBytes(fs.total));
      $('fsBar').style.width = pct + '%';
    } else tile('hFs', 'bad', 'Not mounted', 'LittleFS error');
    tile('hUp', 'ok', fmtDur(j.uptime_s || 0), (sys.chip || 'ESP32') + ' · ' + (j.rtc_ok ? 'RTC ok' : 'no RTC'));
    $('factUrl').textContent = `http://${w.mdns || 'coldchain.local'}` + (w.connected ? ` · http://${w.ip}` : '') + ` · hotspot http://${w.ap_ip}`;
    $('factClock').textContent = !j.rtc_ok ? 'DS3231 not found — times are since boot' : j.ntp_synced ? 'DS3231, corrected from internet time' : 'DS3231 battery-backed RTC';
    $('factHeap').textContent = sys.heap_free ? fmtBytes(sys.heap_free) + ' of ' + fmtBytes(sys.heap_total) : '—';
  }

  // ================================================================ HISTORY
  function appendLive(r) {
    const h = state.hist.live;
    if (!h) return;
    if (h.t.length && h.t[h.t.length - 1] >= r.t) return;
    h.t.push(r.t); h.c.push(+r.core_temp); h.a.push(+r.ambient_temp); h.h.push(+r.ambient_hum); h.x.push(r.excursion ? 1 : 0);
    if (h.t.length > 120) ['t', 'c', 'a', 'h', 'x'].forEach((k) => h[k].shift());
    if (state.range === 'live') showRange(false);
  }
  async function loadHistory(range, animate) {
    try {
      const d = await getJSON('/api/history?range=' + range);
      state.hist[range] = d;
      if (state.range === range) showRange(animate);
    } catch (e) { /* keep last data; connection chip shows the problem */ }
  }
  function showRange(animate) {
    const d = state.hist[state.range];
    const empty = !d || !d.t.length;
    $('tempEmpty').textContent = state.range === 'trend' ? 'No logged readings yet — the first one is saved 30 s after boot.' : 'Collecting readings…';
    $('tempEmpty').hidden = !empty;
    if (empty) return;
    tempChart.setData(d, animate);
    humChart.setData(d, animate);
    $('humRangeTag').textContent = state.range === 'live' ? 'Live 10 min' : 'Trend 2 h';
    if (state.table) renderTable();
  }
  function renderTable() {
    const d = state.hist[state.range];
    if (!d) return;
    let rows = '';
    for (let i = d.t.length - 1, n = 0; i >= 0 && n < 40; i--, n++) {
      rows += `<tr><td>${fmtTime(d.t[i])}</td><td>${d.c[i].toFixed(2)}</td><td>${d.a[i].toFixed(1)}</td><td>${d.h[i].toFixed(0)}</td>` +
              `<td class="${d.x[i] ? 'bad' : 'good'}">${d.x[i] ? '⚠ Out of range' : '✓ Safe'}</td></tr>`;
    }
    $('tableBody').innerHTML = rows;
  }

  // ================================================================ EVENTS
  async function loadEvents() {
    try {
      const evs = await getJSON('/api/events');
      const first = state.seenEvents.size === 0;
      const ol = $('events');
      if (!evs.length) { ol.innerHTML = '<li class="ev-empty">No events yet</li>'; return; }
      ol.innerHTML = evs.map((e) => {
        const key = e.t + '|' + e.msg;
        const isNew = !first && !state.seenEvents.has(key);
        state.seenEvents.add(key);
        if (isNew && e.type === 3) toast('⚠ ' + e.msg);
        return `<li class="ev-${e.type}${isNew ? ' new' : ''}"><span class="ev-ico">${EV_ICON[e.type] || 'i'}</span>` +
               `<span class="ev-time">${fmtTime(e.t)}</span><span>${escapeHtml(e.msg)}</span></li>`;
      }).join('');
    } catch (e) { /* ignore */ }
  }
  function escapeHtml(s) { return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }

  // ================================================================ POLLING
  async function pollLive() {
    try {
      const j = await getJSON('/api/live');
      state.lastOkAt = Date.now();
      if (state.first) {
        state.first = false;
        if (j.limits) { state.limits = j.limits; buildGauge(); }
        document.body.classList.add('ready');
        $('splash').classList.add('gone');
        await Promise.all([loadHistory('live', true), loadHistory('trend', false), loadEvents()]);
      }
      renderLive(j);
    } catch (e) {
      if (state.first) $('splashMsg').textContent = 'Can’t reach the unit — join its WiFi “ColdChain-BOX01” and open http://192.168.4.1';
    }
    connectionState();
    setTimeout(pollLive, POLL_LIVE_MS);
  }
  function connectionState() {
    const lost = !state.lastOkAt || Date.now() - state.lastOkAt > LOST_AFTER_MS;
    document.body.classList.toggle('offline', lost && !state.first);
    const chip = $('liveChip');
    if (state.first) return;
    if (lost) {
      chip.className = 'chip bad'; $('liveText').textContent = 'Connection lost';
      const b = $('banner');
      if (b.dataset.state !== 'warn') $('bannerIcon').innerHTML = ICON.warn;
      b.dataset.state = 'warn';
      $('bannerTitle').textContent = 'Dashboard lost contact with the unit';
      $('bannerSub').textContent = 'Values below are the last ones received. The unit keeps logging on its own.';
      $('muteBtn').hidden = true;
    } else {
      const age = state.live && state.live.reading && state.live.reading.t ? state.live.now - state.live.reading.t : 0;
      chip.className = age > 20 ? 'chip bad' : 'chip ok';
      $('liveText').textContent = age > 20 ? 'Sensor stale' : 'Live';
    }
  }

  // ================================================================ UI WIRING
  document.querySelectorAll('.seg [data-range]').forEach((b) => b.addEventListener('click', () => {
    state.range = b.dataset.range;
    document.querySelectorAll('.seg [data-range]').forEach((x) => x.setAttribute('aria-selected', x === b));
    showRange(true);
    loadHistory(state.range, true);
  }));
  $('tableBtn').addEventListener('click', () => {
    state.table = !state.table;
    $('tableBtn').setAttribute('aria-pressed', state.table);
    $('tableWrap').hidden = !state.table;
    $('tempBox').hidden = state.table;
    if (state.table) renderTable();
  });
  $('muteBtn').addEventListener('click', async () => {
    const muted = state.live && state.live.excursion && state.live.excursion.muted;
    try {
      await fetch('/api/mute' + (muted ? '?off=1' : ''), { method: 'POST' });
      toast(muted ? 'Alarm sound re-enabled' : 'Buzzer silenced for 10 minutes — LED keeps flashing');
    } catch (e) { toast('Could not reach the unit'); }
  });
  document.querySelectorAll('.card').forEach((c, i) => c.style.setProperty('--i', i));

  // ================================================================ MAIN LOOP
  buildGauge(); buildThermo(); buildCharts(); setHumidity(0);
  const drawFrost = frost();
  function frame(now) {
    if (!document.hidden) {
      runTweens(now);
      if (!reduceMotion) drawFrost(now);
      tempChart.draw(now); humChart.draw(now);
      if (state.live) {
        const dt = (Date.now() - state.liveAt) / 1000;
        $('clock').textContent = fmtTime(state.live.now + dt);
      }
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);

  pollLive();
  setInterval(loadEvents, POLL_EVENTS_MS);
  setInterval(() => loadHistory('trend', false), POLL_TREND_MS);
  setInterval(connectionState, 2000);
})();
