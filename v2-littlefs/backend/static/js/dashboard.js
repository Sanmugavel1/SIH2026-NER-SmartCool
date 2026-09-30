/* ==========================================================================
   dashboard.js — shared logic for both admin & user dashboards.
   Polls the Flask JSON API every few seconds and updates:
     - live readout cards (core temp / ambient / humidity / holdover)
     - WiFi/link status pill
     - history line charts (core temp, ambient temp+humidity)
     - alerts table
     - what-if holdover predictor
   Admin-only widgets (export, user mgmt, device list, alert ack) are
   wired only if their DOM elements exist on the page.
   ========================================================================== */

const DEVICE_ID = window.DEVICE_ID || "COLD_BOX_01";
const POLL_MS = 5000;

let tempChart, envChart;

function classify(value, min, max) {
  if (value < min || value > max) return "danger";
  const margin = (max - min) * 0.15;
  if (value < min + margin || value > max - margin) return "warn";
  return "safe";
}

function fmt(v, decimals = 1) {
  return (v === null || v === undefined || isNaN(v)) ? "--" : Number(v).toFixed(decimals);
}

async function fetchJSON(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url} -> ${res.status}`);
  return res.json();
}

/* ---------------- live readouts ---------------- */
async function refreshLatest() {
  try {
    const r = await fetchJSON(`/api/latest?device_id=${DEVICE_ID}`);
    if (!r || !r.core_temp === undefined) return;

    const coreCls = classify(r.core_temp, 2, 8);
    setReadout("core-temp", fmt(r.core_temp), coreCls);
    setReadout("ambient-temp", fmt(r.ambient_temp), "");
    setReadout("ambient-hum", fmt(r.ambient_hum, 0), "");
    setReadout("holdover", fmt(r.predicted_holdover_hours), holdoverClass(r.predicted_holdover_hours));

    const statusEl = document.getElementById("status-pill");
    if (statusEl) {
      const isExcursion = !!r.excursion;
      statusEl.textContent = isExcursion ? "OUT OF RANGE" : "SAFE — WITHIN 2–8°C";
      statusEl.className = "status-pill " + (isExcursion ? "danger" : "safe");
    }

    const srcEl = document.getElementById("last-source");
    if (srcEl) srcEl.textContent = r.source === "backlog"
      ? `Synced from offline backlog · device time ${r.timestamp}`
      : `Live · device time ${r.timestamp}`;

    const wifiDot = document.getElementById("wifi-dot");
    const wifiText = document.getElementById("wifi-text");
    if (wifiDot && wifiText) {
      const online = r.source === "live";
      wifiDot.className = "dot " + (online ? "online" : "offline");
      wifiText.textContent = online ? "Last reading: LIVE" : "Last reading: from OFFLINE backlog";
    }
  } catch (e) { console.error(e); }
}

function holdoverClass(hours) {
  if (hours === null || hours === undefined) return "";
  if (hours < 4) return "danger";
  if (hours < 12) return "warn";
  return "safe";
}

function setReadout(id, value, cls) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = value;
  if (cls !== undefined) {
    const wrap = el.closest(".readout-value");
    if (wrap) wrap.className = "readout-value " + cls;
  }
}

/* ---------------- charts ---------------- */
function ensureCharts() {
  const tCanvas = document.getElementById("tempChart");
  const eCanvas = document.getElementById("envChart");
  if (tCanvas && !tempChart) {
    tempChart = new Chart(tCanvas, {
      type: "line",
      data: { labels: [], datasets: [
        { label: "Core temp (°C)", data: [], borderColor: "#43D9C8", backgroundColor: "rgba(67,217,200,0.08)", tension: 0.25, pointRadius: 0, fill: true },
        { label: "Safe max (8°C)", data: [], borderColor: "#F0555F", borderDash: [4,4], pointRadius: 0 },
        { label: "Safe min (2°C)", data: [], borderColor: "#F0555F", borderDash: [4,4], pointRadius: 0 },
      ]},
      options: chartOptions(),
    });
  }
  if (eCanvas && !envChart) {
    envChart = new Chart(eCanvas, {
      type: "line",
      data: { labels: [], datasets: [
        { label: "Ambient temp (°C)", data: [], borderColor: "#F0A83C", pointRadius: 0, tension: 0.25 },
        { label: "Ambient humidity (%)", data: [], borderColor: "#7E97B3", pointRadius: 0, tension: 0.25, yAxisID: "y1" },
      ]},
      options: chartOptions(true),
    });
  }
}

function chartOptions(dualAxis) {
  const opts = {
    responsive: true,
    animation: false,
    interaction: { mode: "index", intersect: false },
    scales: {
      x: { ticks: { color: "#7E97B3", maxTicksLimit: 8 }, grid: { color: "#26405F" } },
      y: { ticks: { color: "#7E97B3" }, grid: { color: "#26405F" } },
    },
    plugins: { legend: { labels: { color: "#E7EEF5", boxWidth: 12, font: { size: 11.5 } } } },
  };
  if (dualAxis) {
    opts.scales.y1 = { position: "right", ticks: { color: "#7E97B3" }, grid: { display: false } };
  }
  return opts;
}

async function refreshHistory() {
  try {
    const rows = await fetchJSON(`/api/history?device_id=${DEVICE_ID}&limit=120`);
    ensureCharts();
    const labels = rows.map(r => r.timestamp.slice(11, 19));

    if (tempChart) {
      tempChart.data.labels = labels;
      tempChart.data.datasets[0].data = rows.map(r => r.core_temp);
      tempChart.data.datasets[1].data = rows.map(() => 8);
      tempChart.data.datasets[2].data = rows.map(() => 2);
      tempChart.update();
    }
    if (envChart) {
      envChart.data.labels = labels;
      envChart.data.datasets[0].data = rows.map(r => r.ambient_temp);
      envChart.data.datasets[1].data = rows.map(r => r.ambient_hum);
      envChart.update();
    }
  } catch (e) { console.error(e); }
}

/* ---------------- alerts table ---------------- */
async function refreshAlerts() {
  const tbody = document.getElementById("alerts-body");
  if (!tbody) return;
  try {
    const alerts = await fetchJSON(`/api/alerts?device_id=${DEVICE_ID}`);
    tbody.innerHTML = alerts.length === 0
      ? `<tr><td colspan="4" style="color:var(--text-muted)">No excursion events recorded.</td></tr>`
      : alerts.slice(0, 15).map(a => `
        <tr>
          <td class="mono">${a.timestamp}</td>
          <td>${a.message}</td>
          <td>${a.acknowledged ? '<span class="tag live">Acknowledged</span>' : '<span class="tag excursion">Open</span>'}</td>
          <td>${a.acknowledged || !document.getElementById("ack-enabled") ? "" :
                `<button class="btn secondary" onclick="ackAlert(${a.id})">Acknowledge</button>`}</td>
        </tr>`).join("");
  } catch (e) { console.error(e); }
}

async function ackAlert(id) {
  await fetch(`/api/alerts/${id}/ack`, { method: "POST" });
  refreshAlerts();
}

/* ---------------- stats (admin header cards) ---------------- */
async function refreshStats() {
  const el = document.getElementById("stat-total");
  if (!el) return;
  try {
    const s = await fetchJSON(`/api/stats?device_id=${DEVICE_ID}`);
    document.getElementById("stat-total").textContent = s.total_readings;
    document.getElementById("stat-excursions").textContent = s.excursions;
    document.getElementById("stat-backlog").textContent = s.backlog_synced;
  } catch (e) { console.error(e); }
}

/* ---------------- what-if predictor ---------------- */
function wirePredictor() {
  const btn = document.getElementById("predict-btn");
  if (!btn) return;
  btn.addEventListener("click", async () => {
    const ct = document.getElementById("predict-core").value;
    const at = document.getElementById("predict-ambient").value;
    const ah = document.getElementById("predict-hum").value;
    const r = await fetchJSON(`/api/predict?core_temp=${ct}&ambient_temp=${at}&ambient_hum=${ah}`);
    document.getElementById("predict-result").textContent = `${fmt(r.predicted_holdover_hours)} hours`;
  });
}

/* ---------------- admin: user creation ---------------- */
function wireUserForm() {
  const form = document.getElementById("create-user-form");
  if (!form) return;
  form.addEventListener("submit", async (e) => {
    e.preventDefault();
    const username = document.getElementById("new-username").value.trim();
    const password = document.getElementById("new-password").value;
    const role = document.getElementById("new-role").value;
    const msg = document.getElementById("user-form-msg");
    const res = await fetch("/api/users", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username, password, role }),
    });
    const data = await res.json();
    msg.textContent = res.ok ? `User "${username}" created.` : (data.error || "Failed to create user.");
    msg.style.color = res.ok ? "var(--accent)" : "var(--danger)";
    if (res.ok) { form.reset(); refreshUsers(); }
  });
}

async function refreshUsers() {
  const tbody = document.getElementById("users-body");
  if (!tbody) return;
  const users = await fetchJSON("/api/users");
  tbody.innerHTML = users.map(u => `
    <tr><td>${u.username}</td><td>${u.role}</td><td class="mono">${u.created_at}</td></tr>
  `).join("");
}

/* ---------------- boot ---------------- */
function tick() {
  refreshLatest();
  refreshHistory();
  refreshAlerts();
  refreshStats();
}

document.addEventListener("DOMContentLoaded", () => {
  wirePredictor();
  wireUserForm();
  refreshUsers();
  tick();
  setInterval(tick, POLL_MS);
});
