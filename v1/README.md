# Version 1 — First Working Build & Review Material

This is Team INVICTUS's first working version of the cold-chain monitoring unit, together
with everything built for the zeroth, first and second project reviews.

> The newer version is in [`../v2-littlefs/`](../v2-littlefs/). v1 is kept so evaluators
> can see how the project developed.

## Contents

| Folder | What it is |
|---|---|
| [`coldchain_project/`](coldchain_project/) | **v1 system.** ESP32 firmware (sensors → Wi-Fi HTTP POST to the server, with an SD-card backlog while offline), Flask + SQLite backend, admin and user dashboards, a RandomForest holdover-time model, and a Wokwi simulation. Full run guide: [`coldchain_project/docs/README.md`](coldchain_project/docs/README.md) |
| [`LM323_sensor_tests/`](LM323_sensor_tests/) | Sketches that test each sensor/module on its own, used to bring up the hardware one piece at a time (see the table below) |
| [`3d_prototype/`](3d_prototype/) | OpenSCAD 3D model of the complete unit (enclosure, lid, electronics, sensors, power, wiring), an STL export and the full hardware connection diagram (SVG) |
| [`first_review_package/`](first_review_package/) | First-review package: 3D model, STL, rendered view, sensor wiring diagram (SVG), dashboard mockup and progress notes |
| [`dashboard_mockup/`](dashboard_mockup/) | `ColdTrace_Dashboard.html`, an offline dashboard mockup (open it in a browser) |
| [`images/`](images/) | `prototype_hardware_thingspeak.jpg` is a photo of our breadboard prototype on the cooler box (ESP32, SIM800L, NEO-6M GPS, 16×2 LCD) with its live ThingSpeak temperature/humidity charts. `concept_overview_AI_generated.png` is an **AI-generated** concept illustration (ChatGPT) of the target design, not a photo |
| [`reviews/`](reviews/) | Ideation deck and the team's zeroth, first and second review presentations |
| [`docs/`](docs/) | Problem-statement research, component list, source list and the shortlisted SIH IoT/Embedded problem statements |

## LM323 sensor bring-up sketches (ESP32, Arduino IDE)

| Sketch | Purpose |
|---|---|
| `LM323.ino` | LM393 LDR module used as a **lid-open detector** (light in the box = lid open), with ADC averaging and hysteresis thresholds |
| `DHT11/` | DHT11 temperature/humidity reading against the 2–8 °C cold-chain limits |
| `DS18B20/` | DS18B20 probe (OneWire, GPIO 4) core-temperature reading |
| `INA219/` | INA219 voltage/current/power monitoring (I²C 21/22) for battery and power tracking |
| `NEO6M/`, `NEO6M_FULL/` | NEO-6M GPS on UART2 (GPIO 16/17) using TinyGPS++; basic fix and full field dump |
| `SIM800L/` | SIM800L GSM modem AT-command test (for an SMS/GPRS fallback link) |
| `SDC/` | MicroSD card over SPI: mount, write and read test |
| `TRISENSOR/` | DHT11 + LDR lid detector + buzzer combined, with 2–8 °C alarm |
| `QUADSENSOR/` | Four-input combination of the same sensor set, with alarm thresholds |
| `STORAGE/` | DHT11 + LDR + buzzer, logged to SD and posted over Wi-Fi |
| `ESP2/` | DS18B20 + INA219 + GPS together, sending JSON to a cloud endpoint over Wi-Fi |

Before uploading, set `WIFI_SSID` / `WIFI_PASSWORD` in `STORAGE.ino` and `ESP2.ino`.

## Honest status

- A breadboard hardware prototype was built and streamed temperature/humidity to ThingSpeak
  (see `images/prototype_hardware_thingspeak.jpg`).
- Values in the dashboard mockups and the first-review package are **simulated**, as
  their own README files state.
- The holdover-time model is trained on **physics-informed synthetic data**, not on field
  measurements (see `coldchain_project/backend/ml_model/train_model.py`).
- The 3D model is a presentation visual. Check the dimensions before fabrication.
- Wi-Fi credentials were replaced with placeholders in this public repository.
