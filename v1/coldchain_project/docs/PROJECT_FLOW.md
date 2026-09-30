# Project Flow & Architecture

## 1. System architecture

```
┌─────────────────────────┐          ┌──────────────────────────────┐
│        ESP32 UNIT         │          │        LAPTOP / SERVER          │
│                          │          │                                │
│  DHT22 ── ambient T/H    │  WiFi    │  Flask (app.py)                │
│  DS18B20 ── core temp     │ ───────► │   ├─ /api/data   (POST, ESP32) │
│  DS3231 ── RTC timestamp  │  HTTP    │   ├─ /api/latest /history      │
│  SSD1306 ── OLED readout  │  POST    │   ├─ /api/predict (ML)          │
│  microSD ── offline log   │  JSON    │   ├─ /login /admin /user        │
│  Buzzer+LED ── alerts     │          │   └─ /api/export /users         │
│                          │          │                                │
│  offline? -> backlog.csv  │          │  SQLite (coldchain.db)          │
│  online?  -> sync backlog │          │   ├─ readings                  │
└─────────────────────────┘          │   ├─ alerts                    │
                                        │   └─ users / devices           │
                                        │                                │
                                        │  ml_model/holdover_model.pkl    │
                                        │   (RandomForestRegressor)       │
                                        │                                │
                                        │  Dashboards (Jinja2 + Chart.js) │
                                        │   ├─ /admin  (full access)      │
                                        │   └─ /user   (live view only)   │
                                        └──────────────────────────────┘
```

## 2. Offline-first data flow (requirement #11 / #12, "the theme")

```
                     ┌─────────────────────┐
                     │  Read all sensors    │
                     │  + RTC timestamp     │
                     └──────────┬───────────┘
                                │
                     ┌──────────▼───────────┐
                     │  WiFi connected?      │
                     └──────────┬───────────┘
                     YES ───────┼─────── NO
                      │                    │
        ┌─────────────▼───────┐  ┌─────────▼──────────────┐
        │ HTTP POST /api/data   │  │ Append row to           │
        │ to Flask server       │  │ /backlog.csv on microSD │
        └─────────────┬───────┘  └─────────┬──────────────┘
             success? │                     │
        YES ─┬─── NO ─┘         wait for next 30s cycle,
             │                   keep buffering offline
   ┌─────────▼──────────┐
   │ Mark reading "live"  │
   │ ALSO: opportunistically
   │ flush any pending     │
   │ backlog.csv rows       │
   └─────────┬──────────┘
             │
   ┌─────────▼──────────────────┐
   │ For each buffered row:       │
   │  POST with source=backlog     │
   │  success -> drop from file     │
   │  fail    -> keep, retry later  │
   └───────────────────────────┘
```

This directly implements the requested rule:
> if WiFi is available, send data; if WiFi is not available, store locally;
> if WiFi is restored, upload the backlog.

The CSV columns are exactly `timestamp,core_temp,ambient_temp,ambient_hum,excursion`
so the file is human-readable and directly importable into Excel/pandas for your
report if you want to show raw offline-captured evidence.

## 3. ML holdover prediction — request/response

```
ESP32 reading arrives at /api/data
        │
        ▼
predict.predict_holdover(core_temp, ambient_temp, ambient_hum)
        │
        ▼
RandomForestRegressor.predict() -> hours remaining
        │
        ▼
stored in readings.predicted_holdover_hours
        │
        ▼
returned instantly in the POST response AND shown live on both dashboards
```

The same function backs the dashboard's "what-if" predictor
(`/api/predict?core_temp=..&ambient_temp=..&ambient_hum=..`), so a user can
simulate a scenario ("what if this box sits at 15°C ambient for a while?")
without waiting for a real excursion — useful for your demo/viva.

## 4. Role separation (requirement #6)

| Capability | `user` role | `admin` role |
|---|---|---|
| View live readings + charts | ✅ | ✅ |
| View / acknowledge alerts | view only | ✅ acknowledge |
| What-if holdover predictor | ✅ | ✅ |
| Export full history as CSV | ❌ | ✅ |
| Create new user accounts | ❌ | ✅ |
| View registered devices | ❌ | ✅ |

Enforced server-side in `app.py` via the `@login_required(role="admin")`
decorator — not just hidden in the UI — so a user can't reach admin
endpoints even by guessing the URL.
