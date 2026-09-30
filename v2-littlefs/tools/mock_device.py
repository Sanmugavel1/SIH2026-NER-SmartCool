"""
mock_device.py — preview the ESP32 LittleFS dashboard on your laptop, no hardware.
----------------------------------------------------------------------------------
Serves firmware/cold_chain_esp32/data/ exactly like the ESP32 does and fakes the
same JSON API (/api/live, /api/history, /api/events, /api/mute) with SIMULATED
sensor values, including a warm-up excursion every few minutes and a WiFi drop,
so you can see every animation/state before flashing the board.

    python3 tools/mock_device.py            -> http://localhost:8080
    python3 tools/mock_device.py --port 9000

Everything here is made-up demo data. The real numbers only come from the ESP32.
Uses only the Python standard library.
"""
import argparse
import json
import math
import os
import random
import time
from datetime import datetime
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "firmware", "cold_chain_esp32", "data")
LIMIT_MIN, LIMIT_MAX = 2.0, 8.0
BOOT = time.time()
# The ESP32 encodes RTC *local* time as unix seconds; mimic that.
TZ_SHIFT = int(datetime.now().astimezone().utcoffset().total_seconds())


def now_dev():
    return int(time.time()) + TZ_SHIFT


def core_at(t):
    """Fridge cycling 4-6 C, plus a door-left-open warm-up every 6 minutes."""
    base = 5.0 + 0.9 * math.sin(t / 90.0)
    phase = t % 360
    if 200 < phase < 300:                       # excursion window
        base += 6.5 * math.sin((phase - 200) / 100 * math.pi)
    return base + random.uniform(-0.05, 0.05)


def sample(t):
    c = core_at(t)
    a = c + 3.5 + 1.5 * math.sin(t / 40.0) + random.uniform(-0.2, 0.2)
    h = 58 + 8 * math.sin(t / 150.0) + random.uniform(-0.5, 0.5)
    return {"t": t, "c": round(c, 2), "a": round(a, 2), "h": round(h, 1), "x": int(c < LIMIT_MIN or c > LIMIT_MAX)}


state = {"mute_until": 0, "events": [], "exc_active": False, "exc_since": 0, "exc_events": 0}


def add_event(kind, msg):
    state["events"].insert(0, {"t": now_dev(), "type": kind, "msg": msg})
    del state["events"][24:]


def history(step, n):
    end = now_dev() // step * step
    rows = [sample(end - i * step) for i in range(n - 1, -1, -1)]
    return {k: [r[k] for r in rows] for k in ("t", "c", "a", "h", "x")}


def live():
    t = now_dev()
    s = sample(t // 5 * 5)
    exc = bool(s["x"])
    if exc and not state["exc_active"]:
        state.update(exc_active=True, exc_since=t, exc_events=state["exc_events"] + 1)
        add_event(3, f"EXCURSION: core {s['c']:.1f} C outside 2-8 C")
    elif not exc and state["exc_active"]:
        state["exc_active"] = False
        add_event(1, f"Back in safe range after {(t - state['exc_since']) // 60} min")

    up = time.time() - BOOT
    wifi_up = not (120 < up % 600 < 180)          # simulated 1-minute WiFi drop
    trend = history(30, 240)
    n = len(trend["c"])
    muted = time.time() < state["mute_until"]
    return {
        "device_id": "COLD_BOX_01 (SIMULATED)", "now": t, "uptime_s": int(up) + 7200,
        "rtc_ok": True, "ntp_synced": True, "sensor_fault": False,
        "reading": {"t": s["t"], "core_temp": s["c"], "ambient_temp": s["a"], "ambient_hum": s["h"], "excursion": exc},
        "limits": {"min": LIMIT_MIN, "max": LIMIT_MAX},
        "excursion": {"active": exc, "since": state["exc_since"] if exc else 0, "events": state["exc_events"],
                      "muted": muted, "mute_left_s": int(state["mute_until"] - time.time()) if muted else 0},
        "stats": {"cycles": n, "exc_cycles": sum(trend["x"]), "read_errors": 0,
                  "min": min(trend["c"]), "max": max(trend["c"]), "avg": round(sum(trend["c"]) / n, 2),
                  "in_range_pct": round(100 * (n - sum(trend["x"])) / n, 1)},
        "wifi": {"connected": wifi_up, "ssid": "LabWiFi", "ip": "192.168.1.57" if wifi_up else "",
                 "rssi": -58 if wifi_up else 0, "ap_ssid": "ColdChain-BOX01", "ap_ip": "192.168.4.1",
                 "ap_clients": 1, "mdns": "coldchain.local"},
        "server": {"host": "192.168.1.100:5000", "last_ok": wifi_up, "last_send": t - (t % 30) if wifi_up else t - 90,
                   "holdover_h": round(max(0.0, 60 - 4.2 * max(0, s["c"] - 5)), 1), "holdover_time": t - (t % 30)},
        "sd": {"ready": True, "backlog_rows": 0 if wifi_up else 2, "last_sync": t - 400, "last_sync_sent": 3},
        "fs": {"ready": True, "used": 187_392, "total": 1_441_792},
        "sys": {"heap_free": 196_000, "heap_total": 320_000, "chip": "ESP32-D0WD-V3"},
    }


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=DATA_DIR, **kw)

    def _json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == "/api/live":
            return self._json(live())
        if u.path == "/api/history":
            return self._json(history(5, 120) if q.get("range") == ["live"] else history(30, 240))
        if u.path == "/api/events":
            return self._json(state["events"])
        if u.path.startswith("/download/"):
            self.send_response(404); self.end_headers(); self.wfile.write(b"Not available in the simulator"); return
        return super().do_GET()

    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/api/mute":
            if parse_qs(u.query).get("off") == ["1"]:
                state["mute_until"] = 0
                add_event(0, "Alarm sound re-enabled from dashboard")
            else:
                state["mute_until"] = time.time() + 600
                add_event(0, "Alarm silenced for 10 min from dashboard")
            return self._json({"ok": True})
        self.send_response(404); self.end_headers()

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--excursion", action="store_true", help="start inside an excursion (for screenshots)")
    ap.add_argument("--safe", action="store_true", help="start in a calm, in-range period (for screenshots)")
    args = ap.parse_args()
    if args.excursion or args.safe:
        # shift the simulated clock so "now" lands in the wanted part of the 6-min cycle
        target = 250 if args.excursion else 100
        TZ_SHIFT += (target - (int(time.time()) + TZ_SHIFT) % 360) % 360
    add_event(0, "Unit booted - monitoring started (SIMULATED DATA)")
    add_event(1, "WiFi online - 192.168.1.57")
    print(f"Simulated cold-chain unit on http://localhost:{args.port}  (Ctrl+C to stop)")
    ThreadingHTTPServer(("", args.port), Handler).serve_forever()
