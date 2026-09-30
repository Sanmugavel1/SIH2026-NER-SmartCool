/*
 * ============================================================================
 *  VACCINE COLD CHAIN MONITORING UNIT — ESP32 FIRMWARE
 * ============================================================================
 *  Final Year M.Tech/M.E Project — Cold Chain Management System
 *
 *  SENSORS / PERIPHERALS
 *  ----------------------------------------------------------------------
 *   1. DHT22            -> Ambient (box) temperature + humidity   (digital)
 *   2. DS18B20           -> Core/product temperature (waterproof probe,
 *                            submerged in a glycol/water bottle inside the
 *                            unit to simulate the vaccine vial thermal mass)
 *   3. DS3231 RTC        -> Real-time timestamp (I2C)              (battery backed)
 *   4. MicroSD module     -> Offline CSV data logging               (SPI)
 *   5. SSD1306 OLED       -> Local live readout                     (I2C, 128x64)
 *   6. Buzzer + LED       -> Excursion / out-of-range alert
 *
 *  DATA FLOW (core logic requested by project brief)
 *  ----------------------------------------------------------------------
 *      read sensors -> stamp with RTC time
 *          |
 *          v
 *      WiFi connected? ----NO----> append row to /backlog.csv on SD
 *          | YES                          |
 *          v                              v
 *      HTTP POST to Flask server     (wait for next loop / reconnect)
 *          | success                      |
 *          v                              v
 *      done, show "LIVE" on OLED     when WiFi returns -> stream backlog.csv
 *                                     rows to server one by one, delete each
 *                                     line once server ACKs it (200 OK)
 *
 *  Wiring reference: see /docs/README.md and /firmware/README_WIRING.md
 * ============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// USER CONFIG — edit these for your setup
// ---------------------------------------------------------------------------
const char* WIFI_SSID      = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD  = "YOUR_WIFI_PASSWORD";

// Flask backend endpoint (see backend/app.py -> POST /api/data)
const char* SERVER_HOST    = "10.180.131.61";   // your laptop's LAN IP
const uint16_t SERVER_PORT = 5000;
const char* SERVER_PATH    = "/api/data";
const char* DEVICE_ID      = "COLD_BOX_01";
const char* API_KEY        = "esp32-coldchain-key-01";  // must match backend

// Safe storage envelope for the vaccine (WHO cold chain default: 2-8 C)
const float SAFE_TEMP_MIN  = 2.0;
const float SAFE_TEMP_MAX  = 8.0;

const unsigned long READ_INTERVAL_MS   = 30000;  // sensor read + send cycle
const unsigned long WIFI_RETRY_MS      = 15000;  // how often to retry WiFi if down
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;

// ---------------------------------------------------------------------------
// PIN MAP
// ---------------------------------------------------------------------------
#define DHTPIN        27
#define DHTTYPE       DHT22
#define ONE_WIRE_BUS  4        // DS18B20 data pin (needs 4.7k pull-up to 3V3)
#define SD_CS_PIN     5        // SD card module CS
#define BUZZER_PIN    25
#define LED_PIN       26
// I2C (DS3231 + SSD1306) uses default ESP32 pins: SDA=21, SCL=22

// ---------------------------------------------------------------------------
// OBJECTS
// ---------------------------------------------------------------------------
DHT dht(DHTPIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
RTC_DS3231 rtc;
Adafruit_SSD1306 display(128, 64, &Wire, -1);

const char* BACKLOG_FILE = "/backlog.csv";
unsigned long lastReadMillis = 0;
unsigned long lastWifiAttemptMillis = 0;
bool sdReady = false;

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  Wire.begin();               // SDA=21 SCL=22 on ESP32 by default
  dht.begin();
  ds18b20.begin();

  if (!rtc.begin()) {
    Serial.println(F("[RTC] DS3231 not found! Check wiring."));
  } else if (rtc.lostPower()) {
    Serial.println(F("[RTC] Lost power, setting time to compile time."));
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("[OLED] SSD1306 not found!"));
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Cold Chain Monitor"));
    display.println(F("Booting..."));
    display.display();
  }

  sdReady = SD.begin(SD_CS_PIN);
  if (!sdReady) {
    Serial.println(F("[SD] Card mount failed. Offline logging disabled!"));
  } else {
    Serial.println(F("[SD] Card mounted OK."));
    ensureBacklogHeader();
  }

  connectWiFi();
}

// ---------------------------------------------------------------------------
// MAIN LOOP
// ---------------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  // Keep trying WiFi in the background without blocking sensor reads
  if (WiFi.status() != WL_CONNECTED && now - lastWifiAttemptMillis > WIFI_RETRY_MS) {
    lastWifiAttemptMillis = now;
    connectWiFi();
  }

  if (now - lastReadMillis >= READ_INTERVAL_MS) {
    lastReadMillis = now;
    handleSensorCycle();
  }
}

// ---------------------------------------------------------------------------
// ONE FULL SENSOR -> DECIDE -> SEND/LOG CYCLE
// ---------------------------------------------------------------------------
void handleSensorCycle() {
  float ambientTemp = dht.readTemperature();
  float ambientHum   = dht.readHumidity();

  ds18b20.requestTemperatures();
  float coreTemp = ds18b20.getTempCByIndex(0);

  DateTime now = rtc.now();
  char timestamp[25];
  snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d",
           now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());

  bool sensorsOk = !isnan(ambientTemp) && !isnan(ambientHum) && coreTemp > -100.0;
  if (!sensorsOk) {
    Serial.println(F("[SENSOR] Read error, skipping this cycle."));
    return;
  }

  bool excursion = (coreTemp < SAFE_TEMP_MIN || coreTemp > SAFE_TEMP_MAX);
  setAlert(excursion);

  updateOLED(coreTemp, ambientTemp, ambientHum, excursion);

  Serial.printf("[%s] core=%.2fC ambient=%.2fC hum=%.1f%% excursion=%d\n",
                timestamp, coreTemp, ambientTemp, ambientHum, excursion);

  bool sentLive = false;
  if (WiFi.status() == WL_CONNECTED) {
    sentLive = sendToServer(timestamp, coreTemp, ambientTemp, ambientHum, excursion);
  }

  if (!sentLive) {
    // WiFi down OR server unreachable -> fall back to local SD logging
    logToSD(timestamp, coreTemp, ambientTemp, ambientHum, excursion);
  } else {
    // We're online: opportunistically flush any earlier offline backlog
    uploadBacklog();
  }
}

// ---------------------------------------------------------------------------
// WIFI
// ---------------------------------------------------------------------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WiFi] Connecting to %s ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] Not connected — will operate offline.");
  }
}

// ---------------------------------------------------------------------------
// SEND ONE READING TO THE FLASK SERVER (HTTP POST JSON)
// ---------------------------------------------------------------------------
bool sendToServer(const char* timestamp, float coreTemp, float ambientTemp,
                   float ambientHum, bool excursion) {
  HTTPClient http;
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + SERVER_PATH;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  http.setTimeout(5000);

  StaticJsonDocument<256> doc;
  doc["device_id"]     = DEVICE_ID;
  doc["timestamp"]     = timestamp;
  doc["core_temp"]     = coreTemp;
  doc["ambient_temp"]  = ambientTemp;
  doc["ambient_hum"]   = ambientHum;
  doc["excursion"]     = excursion;
  doc["source"]        = "live";

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  bool ok = (code == 200 || code == 201);
  if (!ok) {
    Serial.printf("[HTTP] POST failed, code=%d\n", code);
  }
  http.end();
  return ok;
}

// ---------------------------------------------------------------------------
// OFFLINE LOGGING — append a CSV row on the SD card
// CSV columns: timestamp,core_temp,ambient_temp,ambient_hum,excursion
// ---------------------------------------------------------------------------
void ensureBacklogHeader() {
  if (!SD.exists(BACKLOG_FILE)) {
    File f = SD.open(BACKLOG_FILE, FILE_WRITE);
    if (f) {
      f.println("timestamp,core_temp,ambient_temp,ambient_hum,excursion");
      f.close();
    }
  }
}

void logToSD(const char* timestamp, float coreTemp, float ambientTemp,
             float ambientHum, bool excursion) {
  if (!sdReady) {
    Serial.println(F("[SD] Not ready, reading is LOST for this cycle."));
    return;
  }
  File f = SD.open(BACKLOG_FILE, FILE_APPEND);
  if (!f) {
    Serial.println(F("[SD] Failed to open backlog file for append."));
    return;
  }
  f.printf("%s,%.2f,%.2f,%.1f,%d\n", timestamp, coreTemp, ambientTemp, ambientHum, excursion ? 1 : 0);
  f.close();
  Serial.println(F("[SD] Reading buffered offline (backlog.csv)."));
}

// ---------------------------------------------------------------------------
// UPLOAD BACKLOG — called once WiFi + server are confirmed reachable.
// Reads backlog.csv line-by-line, POSTs each with source="backlog",
// then rewrites the file keeping only the rows that failed to send.
// ---------------------------------------------------------------------------
void uploadBacklog() {
  if (!sdReady || !SD.exists(BACKLOG_FILE)) return;

  File f = SD.open(BACKLOG_FILE, FILE_READ);
  if (!f) return;

  // Skip if file only has the header (nothing buffered)
  if (f.size() < 60) { f.close(); return; }

  Serial.println(F("[SYNC] Uploading offline backlog to server..."));

  const char* TMP_FILE = "/backlog_tmp.csv";
  File tmp = SD.open(TMP_FILE, FILE_WRITE);
  if (!tmp) { f.close(); return; }

  bool isHeader = true;
  int sentCount = 0, keptCount = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (isHeader) {
      tmp.println(line);
      isHeader = false;
      continue;
    }

    // Parse: timestamp,core_temp,ambient_temp,ambient_hum,excursion
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    int c3 = line.indexOf(',', c2 + 1);
    int c4 = line.indexOf(',', c3 + 1);
    if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0) { keptCount++; tmp.println(line); continue; }

    String ts   = line.substring(0, c1);
    float ct    = line.substring(c1 + 1, c2).toFloat();
    float at    = line.substring(c2 + 1, c3).toFloat();
    float ah    = line.substring(c3 + 1, c4).toFloat();
    bool exc    = line.substring(c4 + 1).toInt() == 1;

    bool ok = sendBacklogRow(ts.c_str(), ct, at, ah, exc);
    if (ok) {
      sentCount++;
    } else {
      keptCount++;
      tmp.println(line);   // keep it for the next sync attempt
    }
  }
  f.close();
  tmp.close();

  SD.remove(BACKLOG_FILE);
  SD.rename(TMP_FILE, BACKLOG_FILE);

  Serial.printf("[SYNC] Backlog sync done. Sent=%d Remaining=%d\n", sentCount, keptCount);
}

bool sendBacklogRow(const char* timestamp, float coreTemp, float ambientTemp,
                     float ambientHum, bool excursion) {
  HTTPClient http;
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + SERVER_PATH;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  http.setTimeout(4000);

  StaticJsonDocument<256> doc;
  doc["device_id"]    = DEVICE_ID;
  doc["timestamp"]    = timestamp;
  doc["core_temp"]    = coreTemp;
  doc["ambient_temp"] = ambientTemp;
  doc["ambient_hum"]  = ambientHum;
  doc["excursion"]    = excursion;
  doc["source"]       = "backlog";

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  http.end();
  return (code == 200 || code == 201);
}

// ---------------------------------------------------------------------------
// ALERTS
// ---------------------------------------------------------------------------
void setAlert(bool excursion) {
  digitalWrite(LED_PIN, excursion ? HIGH : LOW);
  if (excursion) {
    tone(BUZZER_PIN, 2000, 300);   // short beep; loop cadence gives natural pulsing
  } else {
    noTone(BUZZER_PIN);
  }
}

// ---------------------------------------------------------------------------
// OLED LOCAL DISPLAY
// ---------------------------------------------------------------------------
void updateOLED(float coreTemp, float ambientTemp, float ambientHum, bool excursion) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(F("WiFi: "));
  display.println(WiFi.status() == WL_CONNECTED ? F("ONLINE") : F("OFFLINE"));

  display.setCursor(0, 14);
  display.setTextSize(2);
  display.print(coreTemp, 1);
  display.println(F(" C"));

  display.setTextSize(1);
  display.setCursor(0, 34);
  display.print(F("Ambient: "));
  display.print(ambientTemp, 1);
  display.print(F("C "));
  display.print(ambientHum, 0);
  display.println(F("%"));

  display.setCursor(0, 48);
  display.println(excursion ? F("STATUS: OUT OF RANGE!") : F("STATUS: SAFE (2-8C)"));

  display.display();
}
