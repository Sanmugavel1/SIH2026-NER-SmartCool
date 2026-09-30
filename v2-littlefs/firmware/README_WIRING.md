# Firmware Setup — Arduino IDE + Wiring Guide

## 1. Arduino IDE board setup
1. Install **Arduino IDE 2.x**.
2. File → Preferences → Additional Board URLs, add:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Tools → Board Manager → search **esp32** → install "esp32 by Espressif Systems".
4. Select board: **Tools → Board → ESP32 Arduino → ESP32 Dev Module**.
5. Select the correct COM port under Tools → Port.

## 2. Required libraries (Library Manager, Ctrl+Shift+I)
| Library | Author | Used for |
|---|---|---|
| `DHT sensor library` | Adafruit | DHT22 ambient temp/humidity |
| `Adafruit Unified Sensor` | Adafruit | dependency of DHT library |
| `OneWire` | Paul Stoffregen | DS18B20 bus protocol |
| `DallasTemperature` | Miles Burton | DS18B20 driver |
| `RTClib` | Adafruit | DS3231 RTC |
| `Adafruit SSD1306` | Adafruit | OLED driver |
| `Adafruit GFX Library` | Adafruit | dependency of SSD1306 |
| `ArduinoJson` | Benoit Blanchon | JSON for the Flask POST + dashboard API (**v7.x**) |
| `SD` | built-in (comes with ESP32 core) | SD card offline logging |
| `LittleFS`, `WebServer`, `ESPmDNS` | built-in (ESP32 core) | on-board dashboard website |

## 3. Wiring table

| Component | Pin on component | Connects to ESP32 |
|---|---|---|
| **DHT22** | VCC | 3V3 |
| | GND | GND |
| | DATA | GPIO 27 (add 10kΩ pull-up to 3V3 if your module has no onboard resistor) |
| **DS18B20** | VCC (red) | 3V3 |
| | GND (black) | GND |
| | DATA (yellow) | GPIO 4 (**add a 4.7kΩ resistor between DATA and 3V3**) |
| **DS3231 RTC** | VCC | 3V3 |
| | GND | GND |
| | SDA | GPIO 21 |
| | SCL | GPIO 22 |
| **SSD1306 OLED (I2C, 0x3C)** | VCC | 3V3 |
| | GND | GND |
| | SDA | GPIO 21 (shared bus with RTC) |
| | SCL | GPIO 22 (shared bus with RTC) |
| **MicroSD module (SPI)** | VCC | 5V (most breakout boards have a regulator; check yours — some need 3V3) |
| | GND | GND |
| | CS | GPIO 5 |
| | SCK | GPIO 18 |
| | MOSI | GPIO 23 |
| | MISO | GPIO 19 |
| **Buzzer (active)** | + | GPIO 25 (through a 220Ω resistor if passive buzzer/LED shares the pin) |
| | - | GND |
| **Alert LED** | Anode (+) | GPIO 26 (through a 220Ω resistor) |
| | Cathode (-) | GND |

> I2C devices (RTC + OLED) share the same SDA/SCL bus — that's normal, they have different
> addresses (RTC `0x68`, OLED `0x3C`) so there's no conflict.

## 4. Before uploading
Edit the **USER CONFIG** block at the top of `cold_chain_esp32.ino`:
```cpp
const char* WIFI_SSID      = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD  = "YOUR_WIFI_PASSWORD";
const char* SERVER_HOST    = "192.168.1.100";   // your laptop's IP on the same WiFi
```
To find your laptop's LAN IP: `ipconfig` (Windows) or `ifconfig`/`ip a` (Mac/Linux). Your
ESP32 and laptop must be on the **same WiFi network** for the direct-POST demo to work.

## 4b. Board settings (IMPORTANT for v2)
In **Tools → Partition Scheme** choose **"No OTA (2MB APP/2MB SPIFFS)"**.
The v2 sketch is ~1.27 MB, which is 96 % of the default 1.2 MB app slot; the No-OTA
scheme gives the app 2 MB and gives LittleFS ~1.9 MB for the website + reading log.
(LittleFS uses the partition that Arduino still labels "SPIFFS" — that's normal.)

## 4c. Upload the dashboard website to LittleFS (one-time, and after any web change)
The web page lives in `cold_chain_esp32/data/` (`index.html`, `style.css`, `app.js`).
It is **not** part of the normal Upload — it has to be written to flash separately:

1. Install the uploader plugin once: download the latest `arduino-littlefs-upload-x.x.x.vsix`
   from https://github.com/earlephilhower/arduino-littlefs-upload/releases and copy it to
   `~/.arduinoIDE/plugins/` (Windows: `C:\Users\<you>\.arduinoIDE\plugins\`). Restart the IDE.
2. **Close the Serial Monitor** (it holds the COM port).
3. Press `Ctrl+Shift+P` → type **Upload LittleFS to Pico/ESP8266/ESP32** → Enter.
4. Wait for "Completed upload". Then do the normal **Upload** (→) for the sketch.

Order doesn't matter, and re-uploading the sketch does **not** erase the website.
If you forget this step, the unit shows a "Dashboard files not uploaded yet" help page.

## 4d. Opening the dashboard
| Situation | Connect phone/laptop to | Open in browser |
|---|---|---|
| No router / in the field | WiFi **ColdChain-BOX01**, password `coldchain123` | http://192.168.4.1 |
| Same router as the unit | your normal WiFi | http://coldchain.local or the IP on the OLED |

The OLED top line shows `ONLINE <router IP>` or `OFFLINE 192.168.4.1`, so you always know the address.

Preview without hardware: `python3 tools/mock_device.py` → http://localhost:8080 (simulated data).

## 5. Format the SD card
Use **FAT32** formatting (standard for cards ≤32GB). The firmware will create
`/backlog.csv` automatically on first boot if it doesn't already exist.

## 6. Testing without hardware
See `/wokwi/` — a ready-to-run Wokwi simulation (browser-based, no physical parts needed)
that mirrors this wiring exactly, useful for demoing sensor logic before your PCB/breadboard
is fully assembled.
