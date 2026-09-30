# Vaccine Cold Chain Management System — Complete Project Package

An ESP32-based IoT cold-chain monitor with offline-first SD logging, a Flask + SQLite
backend, a trained ML model that predicts safe holdover time, and two role-based
dashboards (admin / user).

```
coldchain_project/
├── firmware/
│   ├── cold_chain_esp32/cold_chain_esp32.ino   <- Arduino IDE sketch (flash to ESP32)
│   └── README_WIRING.md                         <- pin map + library list
├── wokwi/
│   ├── diagram.json                             <- Wokwi wiring for browser simulation
│   ├── wokwi.toml
│   └── README.md
├── backend/
│   ├── app.py                                   <- Flask server (routes/API/auth)
│   ├── database.py                               <- SQLite schema + helpers
│   ├── requirements.txt
│   ├── ml_model/
│   │   ├── train_model.py                        <- trains the holdover regressor
│   │   ├── predict.py                            <- loads model, used by app.py
│   │   ├── holdover_model.pkl                     <- pre-trained model (already built)
│   │   └── synthetic_training_data.csv            <- the data it was trained on
│   ├── templates/ (login.html, admin_dashboard.html, user_dashboard.html)
│   ├── static/css/style.css, static/js/dashboard.js
│   └── data/coldchain.db                          <- created on first run
└── docs/
    ├── README.md          <- this file
    └── PROJECT_FLOW.md    <- data-flow diagrams and design explanation
```

## 1. Run order (do this once, in this order)

### 1.1 Backend — Python environment
```bash
cd backend
python3 -m venv venv
source venv/bin/activate          # venv\Scripts\activate on Windows
pip install -r requirements.txt
```

### 1.2 Train the ML model (already done for you, but to retrain / verify)
```bash
python ml_model/train_model.py
```
This regenerates `synthetic_training_data.csv` and `holdover_model.pkl`. Expect
Test MAE around **1–2 hours** and R² around **0.99** — printed at the end of the run.

### 1.3 Initialise the database
```bash
python database.py
```
Creates `data/coldchain.db` with the schema and two seeded accounts:

| Username | Password | Role |
|---|---|---|
| `admin` | `admin123` | admin |
| `viewer` | `viewer123` | user |

**Change these passwords before any real deployment / demo outside your lab.**

### 1.4 Start the Flask server
```bash
python app.py
```
Server runs on `http://0.0.0.0:5000`. Find your machine's LAN IP (`ipconfig` /
`ifconfig`) — you'll need it for the ESP32 firmware's `SERVER_HOST`.

Browse to `http://localhost:5000/login` and sign in with either account above to
confirm the dashboards render (they'll show "--" placeholders until real data arrives).

### 1.5 Flash the ESP32
Open `firmware/cold_chain_esp32/cold_chain_esp32.ino` in Arduino IDE, follow
`firmware/README_WIRING.md` for libraries + wiring, edit the `USER CONFIG` block
(WiFi SSID/password, `SERVER_HOST` = your laptop's LAN IP), and upload.

No hardware yet? Use `/wokwi/` to simulate the whole rig in-browser first.

### 1.6 Watch it work
- Open the Serial Monitor (115200 baud) to see sensor reads, WiFi status, and
  send/log decisions in real time.
- Refresh the dashboard — live cards, charts and alerts update automatically
  every 5 seconds (`static/js/dashboard.js` polls the API).
- Unplug your WiFi router / walk the unit out of range: the OLED shows
  **OFFLINE**, readings get appended to `/backlog.csv` on the SD card instead
  of failing silently. Reconnect WiFi and watch the firmware log
  `[SYNC] Uploading offline backlog to server...` in Serial Monitor — those
  rows land in the dashboard tagged `source: backlog`.

## 2. How each of your requirements is satisfied

| # | Requirement | Where |
|---|---|---|
| 1, 3, 13 | Sensor interfacing code, coded per project flow, hardware instructions | `firmware/cold_chain_esp32/cold_chain_esp32.ino` + `README_WIRING.md` |
| 2, 8 | End-to-end software listing / full package (Arduino, Python, Flask, SQLite, Wokwi) | whole repo — see tree above |
| 4, 9, 11, 12 | Offline database via microSD, timestamped CSV, sync-on-reconnect | `logToSD()`, `uploadBacklog()`, `ensureBacklogHeader()` in the `.ino`; theme explained in `PROJECT_FLOW.md` |
| 5, 6, 16 | Precise dashboards, separate admin/user, graphs | `templates/*.html`, `static/js/dashboard.js` (Chart.js line charts) |
| 7, 17 | Trained holdover prediction model, linked to live DB | `ml_model/train_model.py`, `predict.py`, called from `app.py:api_data()` on every insert |
| 10, 14 | Web server: ESP32 → HTTP → Flask → SQLite → Dashboard | `app.py` (`/api/data`, `/api/latest`, `/api/history`) |
| 15, 18 | Reference the external project link | see chat — link was inaccessible (private/login-walled); this package was built directly from your written spec instead |

## 3. Talking points for your report / viva

- **Why RandomForest for holdover time?** Small, tabular, non-linear feature
  space (3 inputs) — RandomForest gives strong accuracy without needing GPU
  training, and `feature_importances_` gives you an easy, defensible
  interpretability chart for your report (core_temp dominates, as physically
  expected).
- **Why synthetic training data?** No public per-vial holdover dataset exists
  for a student project. The synthetic data is *physics-informed* — built
  from the same exponential-decay-with-deviation-from-2–8°C logic that real
  vaccine carrier/cold-box datasheets use to quote holdover time, with noise
  added so the model generalises rather than memorising a formula. This is
  explained in the docstring of `train_model.py` — cite it directly.
- **Why SQLite, not a bigger DB?** Single-file, zero-config, perfectly
  matches an offline-first edge deployment — the whole point of requirement
  #4 is that the system must work with *no* server dependency at the edge.
- **Offline-first correctness**: the sync logic in `uploadBacklog()` rewrites
  `backlog.csv` keeping only rows that failed to POST, so a partial network
  drop mid-sync never loses data and never double-uploads a fully-sent file.

## 4. Security note for a real deployment
This build uses a hardcoded API key and a Flask dev server for clarity/demo
purposes. Before any real-world use: rotate `API_KEY`/`app.secret_key` via
environment variables, put the Flask app behind HTTPS + a proper WSGI server
(gunicorn/nginx), and hash-check the ESP32's key on a per-device basis rather
than one shared key.
