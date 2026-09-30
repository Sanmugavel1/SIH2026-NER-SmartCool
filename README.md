# NER-SmartCool — Solar-Powered Smart Mini Cold Storage

**Smart India Hackathon 2026 · Team INVICTUS (Team ID 159400)**

| | |
|---|---|
| **Problem Statement ID** | SIH26210 |
| **Title** | Student Innovation — development solutions to enhance India's primary sector (Agriculture) and to manage and process agricultural produce |
| **Theme** | Agriculture, FoodTech & Rural Development |
| **Category** | Hardware |
| **Final presentation** | [`presentation/NER-SmartCool_SIH26210_INVICTUS_Final.pdf`](presentation/NER-SmartCool_SIH26210_INVICTUS_Final.pdf) ([.pptx](presentation/NER-SmartCool_SIH26210_INVICTUS_Final.pptx)) |
| **Solution video** | _link to be added_ |

---

## 1. The problem

Fresh vegetables in India's North Eastern Region (NER) spoil between harvest and market.
Farm gates and village collection points have little cold storage, and grid power is
unreliable. Farmers then have to sell straight away at whatever price is offered, or accept
spoilage.

## 2. Our solution

NER-SmartCool is a **decentralized, solar-powered mini cold-storage unit** for farm gates,
village collection centres and FPOs. It acts as a temporary storage buffer between
harvest and transport.

- **Cooling and power:** insulated chamber, DC compressor refrigeration and forced-air
  circulation, powered by Solar PV → MPPT → battery → DC bus, so it works off-grid or on a
  weak grid.
- **Smart control:** an ESP32 runs closed-loop control with hysteresis to hold crop-specific
  temperature and humidity. Energy-aware modes (**Normal / Energy-Aware / Critical**)
  protect the produce when the battery is low.
- **Fault detection:** high temperature, door open, low battery, solar-charging failure and
  cooling failure, with **local buzzer / LED / display alerts that need no internet**.
- **Offline-first data:** RTC-timestamped logging to MicroSD and internal flash (LittleFS),
  with store-and-forward sync over Wi-Fi (GSM optional) when a connection is available.
- **SHT prediction:** a machine-learning model estimates the **Safe Holdover Time**, i.e.
  how long the chamber stays inside safe limits under the current conditions.

## 3. Repository layout

```
SIH2026-NER-SmartCool/
├── presentation/          Final SIH 2026 deck (.pptx + .pdf)
├── v2-littlefs/           ★ VERSION 2 (latest): standalone on-board dashboard build
│   ├── firmware/            ESP32 sketch + LittleFS web dashboard (data/)
│   ├── backend/             Flask + SQLite server, admin/user dashboards, ML model
│   ├── tools/mock_device.py preview the on-board dashboard on a laptop, no hardware
│   ├── wokwi/               browser simulation of the full circuit
│   └── docs/                run guide, project flow, 14-page PDF guide
└── v1/                    VERSION 1: first working build + all review material
    ├── coldchain_project/   v1 firmware + Flask backend + ML + Wokwi
    ├── LM323_sensor_tests/  one sketch per sensor/module (bring-up tests)
    ├── 3d_prototype/        OpenSCAD model, STL, hardware connection diagram
    ├── first_review_package/
    ├── dashboard_mockup/
    ├── images/              photo of the hardware prototype
    ├── reviews/             zeroth / first / second review decks
    └── docs/                problem-statement research, component list
```

Both versions are also tagged: **`v1.0`** and **`v2.0`** (see *Releases/Tags* on GitHub).

## 4. Versions

| | Version 1 ([`v1/`](v1/)) | Version 2 ([`v2-littlefs/`](v2-littlefs/)), latest |
|---|---|---|
| Dashboard | Flask web dashboards on a laptop/server only | **Animated dashboard served by the ESP32 itself** from LittleFS, plus the Flask dashboards |
| Access without a server | Serial monitor and OLED only | Open `http://coldchain.local` or join the unit's own Wi-Fi AP `ColdChain-BOX01` and view live data |
| On-device logging | SD-card backlog while offline | SD backlog **plus** a rotating CSV log in internal flash, downloadable from the browser |
| Live API on the device | — | `/api/live`, `/api/history`, `/api/events`, `/api/mute`, CSV downloads |
| Alarm handling | Buzzer + LED | Buzzer + LED + 10-minute "silence alarm" button on the dashboard |
| Preview without hardware | Wokwi | Wokwi + `tools/mock_device.py` (simulated data) |

## 5. System architecture

```
 Solar PV ─► MPPT ─► Battery ─► DC bus ─► DC compressor + fans ─► Insulated chamber
                                  │
 Sensors ──────────────────► ┌────▼─────────────────────────────┐
  DS18B20 (core/product)     │ ESP32                              │ ─► OLED / LCD, buzzer, LED
  DHT22 / SHT3x (air, RH)    │  hysteresis control, energy modes, │
  Reed switch / LDR (door)   │  fault rules, RTC timestamps       │ ─► MicroSD backlog.csv
  INA219 / ACS712 (V, I)     │  LittleFS web dashboard + log      │ ─► LittleFS readings.csv
  DS3231 RTC                 └────┬───────────────────────────────┘
                                  │ Wi-Fi HTTP POST (JSON, API key)  ·  GSM (optional)
                                  ▼        store-and-forward when back online
                     Flask server ─► SQLite ─► ML holdover-time model
                                  └──► Admin dashboard / User dashboard (Chart.js)
```

More detail: [`v2-littlefs/docs/PROJECT_FLOW.md`](v2-littlefs/docs/PROJECT_FLOW.md).

## 6. Hardware used

| Module | Role | Interface / ESP32 pin (v2) |
|---|---|---|
| ESP32 Dev Module | controller, Wi-Fi, web server | — |
| DS18B20 | core/product temperature | OneWire, GPIO 4 (4.7 kΩ pull-up) |
| DHT22 (DHT11 in bring-up tests) | air temperature and humidity | GPIO 27 |
| DS3231 RTC | timestamps while offline | I²C 21/22 |
| SSD1306 OLED | local readout | I²C 21/22 |
| MicroSD module | offline backlog | SPI, CS 5 / SCK 18 / MOSI 23 / MISO 19 |
| Buzzer + LED | local alarm | GPIO 25 / 26 |
| INA219 | voltage/current (battery, load) | I²C (bring-up test) |
| LM393 LDR / reed switch | door / lid-open detection | ADC 34 (bring-up test) |
| NEO-6M GPS, SIM800L GSM | location and SMS/GPRS fallback (optional) | UART (bring-up tests) |

Full wiring table: [`v2-littlefs/firmware/README_WIRING.md`](v2-littlefs/firmware/README_WIRING.md).

## 7. Quick start (Version 2)

**a) Preview the on-board dashboard without hardware**
```bash
cd v2-littlefs
python3 tools/mock_device.py          # open http://localhost:8080  (simulated data)
```

**b) Flash the ESP32**
1. Arduino IDE 2.x → install the *esp32 by Espressif* board package and the libraries listed
   in `README_WIRING.md`.
2. Board: **ESP32 Dev Module**, Partition Scheme: **No OTA (2MB APP/2MB SPIFFS)**.
3. Edit the `USER CONFIG` block in `firmware/cold_chain_esp32/cold_chain_esp32.ino`
   (Wi-Fi SSID/password, `SERVER_HOST` = your laptop's LAN IP).
4. Upload the sketch, then run **Upload LittleFS** to write `data/` (the dashboard) to flash.
5. Browse to `http://coldchain.local`, or join Wi-Fi `ColdChain-BOX01` and open the AP IP.

**c) Run the server, database and ML model**
```bash
cd v2-littlefs/backend
python3 -m venv venv && source venv/bin/activate
pip install -r requirements.txt
python database.py        # creates data/coldchain.db with demo users
python app.py             # http://localhost:5000/login
```
Demo logins: `admin / admin123` (admin) and `viewer / viewer123` (user). **Change these
before any real deployment.**

Step-by-step guide: [`v2-littlefs/docs/README.md`](v2-littlefs/docs/README.md) and the PDF
[`ColdChain_LittleFS_Dashboard_Guide.pdf`](v2-littlefs/docs/ColdChain_LittleFS_Dashboard_Guide.pdf).

## 8. Tech stack

- **Embedded:** ESP32, Arduino C++ (ESP32 core 3.x), LittleFS, WebServer, mDNS, ArduinoJson
- **Backend:** Python, Flask, SQLite, Werkzeug auth (role-based admin/user)
- **ML:** scikit-learn RandomForestRegressor (holdover-time prediction), pandas, NumPy, joblib
- **Frontend:** HTML/CSS/JS, Chart.js
- **Simulation and design:** Wokwi, OpenSCAD (3D enclosure), SVG wiring diagrams

## 9. Current status (what is and isn't validated)

We're stating this plainly so evaluators can judge the work fairly:

- ✅ A breadboard prototype (ESP32 + sensors + GSM + GPS + LCD) was built and streamed live
  temperature/humidity data, shown in `v1/images/prototype_hardware_thingspeak.jpg`.
- ✅ The v2 firmware **compiles** for ESP32 (esp32 core 3.3.10, No-OTA partition). The
  LittleFS dashboard runs against the mock device.
- ⏳ v2 firmware has **not yet been tested on the physical hardware**.
- ⏳ The solar PV / MPPT / DC-compressor refrigeration chamber is the **proposed design**.
  It has not been built yet.
- ⏳ The SHT/holdover model is trained on **physics-informed synthetic data**. It still
  needs field data, and crop-specific set-points still need validating.
- Dashboard mockups and the mock device use **simulated values**, not measurements.
- The firmware thresholds come from the earlier vaccine-carrier (2–8 °C) work. They will be
  replaced with crop-specific vegetable set-points.

## 10. References

1. FAO, *Global Food Losses and Food Waste — Extent, Causes and Prevention*, Rome, 2011.
2. ICAR-CIPHET, *Assessment of Quantitative Harvest and Post-Harvest Losses of Major Crops and Commodities in India*, Ludhiana, 2015.
3. National Centre for Cold-chain Development (NCCD) and National Horticulture Board (NHB): cold-chain infrastructure and horticulture data.
4. MNRE / NABARD: solar-powered cold storage and agri-infrastructure guidelines.

## 11. Security note

All Wi-Fi credentials in this repository are placeholders. The API key, Flask secret key and
demo passwords are for **demo use only**. Move them to environment variables and put the
server behind HTTPS before any real deployment.

---
_Team INVICTUS · Smart India Hackathon 2026_
