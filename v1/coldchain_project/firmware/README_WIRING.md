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
| `ArduinoJson` | Benoit Blanchon | building the JSON POST body (v6.x) |
| `SD` | built-in (comes with ESP32 core) | SD card offline logging |

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

## 5. Format the SD card
Use **FAT32** formatting (standard for cards ≤32GB). The firmware will create
`/backlog.csv` automatically on first boot if it doesn't already exist.

## 6. Testing without hardware
See `/wokwi/` — a ready-to-run Wokwi simulation (browser-based, no physical parts needed)
that mirrors this wiring exactly, useful for demoing sensor logic before your PCB/breadboard
is fully assembled.
