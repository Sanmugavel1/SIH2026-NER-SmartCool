/*
 * ============================================================================
 *  VACCINE COLD CHAIN MONITORING UNIT — ESP32 FIRMWARE  (v2: LittleFS web UI)
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
 *   4. MicroSD module     -> Offline CSV backlog for the Flask server (SPI)
 *   5. SSD1306 OLED       -> Local live readout                     (I2C, 128x64)
 *   6. Buzzer + LED       -> Excursion / out-of-range alert
 *   7. LittleFS (internal flash)
 *                         -> hosts the animated web dashboard (data/ folder)
 *                            and keeps an on-board reading log that survives
 *                            reboots, so the chart is never empty after reset.
 *
 *  WHAT'S NEW IN v2
 *  ----------------------------------------------------------------------
 *   - The ESP32 runs its OWN web server. Open the dashboard from any phone
 *     or laptop — no Flask server or internet needed:
 *        * Through the unit's own hotspot:  WiFi "ColdChain-BOX01"
 *          (password below) -> browse to http://192.168.4.1
 *        * On the same router WiFi:         http://coldchain.local
 *          (or the IP shown on the OLED / Serial Monitor)
 *   - Sensors are sampled every 5 s for the live dashboard; the 30 s
 *     send/log cycle from v1 is unchanged (same Flask API, same SD backlog).
 *   - The ML holdover prediction returned by Flask is captured and shown
 *     on the on-board dashboard whenever the server is reachable.
 *   - "Silence alarm" button on the dashboard mutes the buzzer for 10 min
 *     (the red LED keeps flashing — the excursion is still visible).
 *   - RTC is auto-corrected from internet time (NTP) whenever WiFi is up.
 *
 *  DATA FLOW (unchanged core logic from v1)
 *  ----------------------------------------------------------------------
 *      read sensors -> stamp with RTC time -> append to LittleFS log
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
 *                                     line once server ACKs it (200/201)
 *
 *  Wiring + upload steps: see /firmware/README_WIRING.md
 * ============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <LittleFS.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
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

// The unit's own hotspot — always on, so the dashboard works with no router.
const char* AP_SSID        = "ColdChain-BOX01";
const char* AP_PASSWORD    = "coldchain123";     // min 8 characters
const char* MDNS_NAME      = "coldchain";        // -> http://coldchain.local

// Flask backend endpoint (see backend/app.py -> POST /api/data)
const char* SERVER_HOST    = "192.168.1.100";   // your laptop's LAN IP
const uint16_t SERVER_PORT = 5000;
const char* SERVER_PATH    = "/api/data";
const char* DEVICE_ID      = "COLD_BOX_01";
const char* API_KEY        = "esp32-coldchain-key-01";  // must match backend

// Safe storage envelope for the vaccine (WHO cold chain default: 2-8 C)
const float SAFE_TEMP_MIN  = 2.0;
const float SAFE_TEMP_MAX  = 8.0;

const unsigned long READ_INTERVAL_MS   = 30000;  // send/log cycle (same as v1)
const unsigned long LIVE_SAMPLE_MS     = 5000;   // dashboard refresh sampling
const unsigned long WIFI_RETRY_MS      = 15000;  // how often to retry WiFi if down
const unsigned long MUTE_DURATION_MS   = 600000; // "silence alarm" = 10 minutes

// Time zone for NTP -> RTC correction (India = UTC+5:30 = 19800 s)
const long GMT_OFFSET_SEC  = 19800;

// ---------------------------------------------------------------------------
// PIN MAP (unchanged from v1)
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
WebServer server(80);

const char* BACKLOG_FILE   = "/backlog.csv";          // on SD card
const char* LOG_FILE       = "/log/readings.csv";     // on LittleFS
const char* LOG_FILE_OLD   = "/log/readings_old.csv"; // on LittleFS (rotated)
const size_t LOG_MAX_BYTES = 96 * 1024;               // rotate after ~96 KB

// ---------------------------------------------------------------------------
// IN-MEMORY HISTORY FOR THE DASHBOARD
// ---------------------------------------------------------------------------
struct Sample {
  uint32_t t;        // unix time (local clock from RTC)
  float    core;
  float    amb;
  float    hum;
  uint8_t  exc;
};

template <size_t N>
struct Ring {
  Sample buf[N];
  size_t head = 0, count = 0;
  void push(const Sample& s) { buf[head] = s; head = (head + 1) % N; if (count < N) count++; }
  const Sample& at(size_t i) const { return buf[(head + N - count + i) % N]; }   // 0 = oldest
};

Ring<120> liveRing;    // 5 s samples  -> last 10 minutes
Ring<240> trendRing;   // 30 s cycles  -> last 2 hours (restored from LittleFS on boot)

enum EventType : uint8_t { EV_INFO, EV_OK, EV_WARN, EV_ALERT };
struct Event { uint32_t t; uint8_t type; char msg[56]; };
Event events[24];
size_t eventHead = 0, eventCount = 0;

// ---------------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------------
unsigned long lastReadMillis = 0;
unsigned long lastSampleMillis = 0;
unsigned long lastWifiAttemptMillis = 0;
unsigned long muteUntilMillis = 0;
unsigned long lastBlinkMillis = 0;
unsigned long lastNtpAttemptMillis = 0;

bool sdReady = false;
bool fsReady = false;
bool rtcReady = false;
bool ntpSynced = false;
bool wasWifiConnected = false;
bool mdnsStarted = false;

Sample latest = {0, NAN, NAN, NAN, 0};
bool   haveSample = false;
bool   sensorFault = false;

bool     lastSendOk = false;
uint32_t lastSendTime = 0;
float    lastHoldoverHours = NAN;
uint32_t lastHoldoverTime = 0;
long     backlogRows = 0;
uint32_t lastSyncTime = 0;
int      lastSyncSent = 0;

// Since-boot statistics (per 30 s cycle)
uint32_t statCycles = 0, statExcCycles = 0, statExcEvents = 0, statReadErrors = 0;
float    statMin = NAN, statMax = NAN;
double   statSum = 0;
bool     excursionActive = false;
uint32_t excursionStart = 0;

// ---------------------------------------------------------------------------
// HELPERS
// ---------------------------------------------------------------------------
uint32_t nowUnix() {
  if (rtcReady) return rtc.now().unixtime();
  return millis() / 1000;   // no RTC: seconds since boot (dashboard shows "uptime" time)
}

void formatIso(uint32_t t, char* out, size_t len) {
  DateTime d(t);
  snprintf(out, len, "%04d-%02d-%02dT%02d:%02d:%02d",
           d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second());
}

void addEvent(uint8_t type, const char* msg) {
  Event& e = events[eventHead];
  e.t = nowUnix();
  e.type = type;
  strncpy(e.msg, msg, sizeof(e.msg) - 1);
  e.msg[sizeof(e.msg) - 1] = '\0';
  eventHead = (eventHead + 1) % 24;
  if (eventCount < 24) eventCount++;
  Serial.printf("[EVENT] %s\n", msg);
}

bool isExcursion(float core) { return core < SAFE_TEMP_MIN || core > SAFE_TEMP_MAX; }
bool alarmMuted() { return muteUntilMillis != 0 && (long)(muteUntilMillis - millis()) > 0; }

// Forward declarations (explicit, so the sketch also builds outside Arduino IDE)
void handleSensorCycle();
bool sampleSensors();
void trackExcursion(bool exc);
void connectWiFi();
void handleWifiState();
void syncRtcFromNtp();
bool sendToServer(const char* timestamp, float coreTemp, float ambientTemp, float ambientHum, bool excursion);
bool sendBacklogRow(const char* timestamp, float coreTemp, float ambientTemp, float ambientHum, bool excursion);
void ensureBacklogHeader();
long countBacklogRows();
void logToSD(const char* timestamp, float coreTemp, float ambientTemp, float ambientHum, bool excursion);
void uploadBacklog();
void appendLocalLog(const char* timestamp, const Sample& s, bool uploaded);
void restoreTrendFromLog();
void setupWebServer();
void setAlert(bool excursion);
void updateOLED(float coreTemp, float ambientTemp, float ambientHum, bool excursion);

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
  ds18b20.setWaitForConversion(false);   // non-blocking: keeps the web server snappy
  ds18b20.requestTemperatures();

  if (!rtc.begin()) {
    Serial.println(F("[RTC] DS3231 not found! Check wiring."));
  } else {
    rtcReady = true;
    if (rtc.lostPower()) {
      Serial.println(F("[RTC] Lost power, setting time to compile time."));
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
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

  // --- LittleFS: web dashboard files + on-board reading log -------------
  // "true" = format the partition if it can't be mounted (first boot only).
  fsReady = LittleFS.begin(true);
  if (!fsReady) {
    Serial.println(F("[FS] LittleFS mount failed! Dashboard unavailable."));
  } else {
    Serial.printf("[FS] LittleFS mounted. Used %u / %u bytes\n",
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    if (!LittleFS.exists("/index.html")) {
      Serial.println(F("[FS] index.html missing -> run 'Upload LittleFS' (see README_WIRING.md)"));
    }
    if (!LittleFS.exists("/log")) LittleFS.mkdir("/log");
    restoreTrendFromLog();
  }

  // --- SD card: offline backlog for Flask (unchanged behaviour) ---------
  sdReady = SD.begin(SD_CS_PIN);
  if (!sdReady) {
    Serial.println(F("[SD] Card mount failed. Offline logging disabled!"));
    addEvent(EV_WARN, "SD card not detected - offline backlog disabled");
  } else {
    Serial.println(F("[SD] Card mounted OK."));
    ensureBacklogHeader();
    backlogRows = countBacklogRows();
  }

  // --- WiFi: own hotspot (always) + router connection (when available) --
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.setAutoReconnect(true);
  Serial.printf("[AP] Hotspot '%s' up. Dashboard: http://%s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());
  connectWiFi();

  setupWebServer();
  addEvent(EV_INFO, "Unit booted - monitoring started");

  sampleSensors();
}

// ---------------------------------------------------------------------------
// MAIN LOOP — never blocks for long, so the dashboard stays responsive
// ---------------------------------------------------------------------------
void loop() {
  server.handleClient();
  unsigned long now = millis();

  handleWifiState();

  if (WiFi.status() != WL_CONNECTED && now - lastWifiAttemptMillis > WIFI_RETRY_MS) {
    lastWifiAttemptMillis = now;
    connectWiFi();
  }

  // NTP needs a few seconds after connecting; retry every minute until it works
  if (WiFi.status() == WL_CONNECTED && !ntpSynced && now - lastNtpAttemptMillis > 60000) {
    lastNtpAttemptMillis = now;
    syncRtcFromNtp();
  }

  if (now - lastSampleMillis >= LIVE_SAMPLE_MS) {
    lastSampleMillis = now;
    sampleSensors();
  }

  if (now - lastReadMillis >= READ_INTERVAL_MS) {
    lastReadMillis = now;
    handleSensorCycle();
  }

  // Keep the alert LED blinking during an excursion (visible even when muted)
  if (excursionActive && now - lastBlinkMillis > 500) {
    lastBlinkMillis = now;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
}

// ---------------------------------------------------------------------------
// FAST SAMPLE (every 5 s) — feeds the live dashboard + OLED + alarm
// ---------------------------------------------------------------------------
bool sampleSensors() {
  float ambientTemp = dht.readTemperature();
  float ambientHum  = dht.readHumidity();

  // Read the conversion started last time, then start the next one.
  float coreTemp = ds18b20.getTempCByIndex(0);
  ds18b20.requestTemperatures();

  // -127 = probe disconnected, 85.0 = power-on value before first conversion
  bool coreOk = coreTemp > -100.0 && coreTemp != 85.0;
  bool ok = !isnan(ambientTemp) && !isnan(ambientHum) && coreOk;

  if (!ok) {
    statReadErrors++;
    if (!sensorFault) {
      sensorFault = true;
      addEvent(EV_WARN, coreOk ? "DHT22 read error" : "DS18B20 probe read error");
    }
    return false;
  }
  if (sensorFault) {
    sensorFault = false;
    addEvent(EV_OK, "Sensors reading normally again");
  }

  latest.t    = nowUnix();
  latest.core = coreTemp;
  latest.amb  = ambientTemp;
  latest.hum  = ambientHum;
  latest.exc  = isExcursion(coreTemp) ? 1 : 0;
  haveSample  = true;
  liveRing.push(latest);

  trackExcursion(latest.exc);
  setAlert(latest.exc);
  updateOLED(coreTemp, ambientTemp, ambientHum, latest.exc);
  return true;
}

void trackExcursion(bool exc) {
  char msg[56];
  if (exc && !excursionActive) {
    excursionActive = true;
    excursionStart = nowUnix();
    statExcEvents++;
    snprintf(msg, sizeof(msg), "EXCURSION: core %.1f C outside %.0f-%.0f C",
             latest.core, SAFE_TEMP_MIN, SAFE_TEMP_MAX);
    addEvent(EV_ALERT, msg);
  } else if (!exc && excursionActive) {
    excursionActive = false;
    uint32_t mins = (nowUnix() - excursionStart) / 60;
    snprintf(msg, sizeof(msg), "Back in safe range after %lu min", (unsigned long)mins);
    addEvent(EV_OK, msg);
    digitalWrite(LED_PIN, LOW);
  }
}

// ---------------------------------------------------------------------------
// ONE FULL 30 s CYCLE -> SEND / BACKLOG / LOG  (v1 logic + LittleFS log)
// ---------------------------------------------------------------------------
void handleSensorCycle() {
  if (!sampleSensors()) {
    Serial.println(F("[SENSOR] Read error, skipping this cycle."));
    return;
  }

  char timestamp[25];
  formatIso(latest.t, timestamp, sizeof(timestamp));
  bool excursion = latest.exc;

  Serial.printf("[%s] core=%.2fC ambient=%.2fC hum=%.1f%% excursion=%d\n",
                timestamp, latest.core, latest.amb, latest.hum, excursion);

  // Stats + trend chart
  statCycles++;
  if (excursion) statExcCycles++;
  statSum += latest.core;
  if (isnan(statMin) || latest.core < statMin) statMin = latest.core;
  if (isnan(statMax) || latest.core > statMax) statMax = latest.core;
  trendRing.push(latest);

  bool sentLive = false;
  if (WiFi.status() == WL_CONNECTED) {
    sentLive = sendToServer(timestamp, latest.core, latest.amb, latest.hum, excursion);
  }
  if (sentLive != lastSendOk && WiFi.status() == WL_CONNECTED) {
    addEvent(sentLive ? EV_OK : EV_WARN,
             sentLive ? "Flask server reachable - live upload" : "Flask server unreachable - buffering to SD");
  }
  lastSendOk = sentLive;
  if (sentLive) lastSendTime = latest.t;

  appendLocalLog(timestamp, latest, sentLive);

  if (!sentLive) {
    // WiFi down OR server unreachable -> fall back to local SD logging
    logToSD(timestamp, latest.core, latest.amb, latest.hum, excursion);
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
  // Non-blocking: result is picked up by handleWifiState() in loop()
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void handleWifiState() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected == wasWifiConnected) return;
  wasWifiConnected = connected;

  if (connected) {
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(WiFi.localIP());
    char msg[56];
    snprintf(msg, sizeof(msg), "WiFi online - %s", WiFi.localIP().toString().c_str());
    addEvent(EV_OK, msg);

    if (!mdnsStarted && MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
    }
    // Correct the RTC from internet time (non-blocking; checked in syncRtcFromNtp)
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    lastNtpAttemptMillis = millis() - 55000;   // first NTP check ~5 s from now
  } else {
    Serial.println("[WiFi] Not connected — will operate offline.");
    addEvent(EV_WARN, "WiFi lost - logging offline to SD card");
  }
}

void syncRtcFromNtp() {
  if (!rtcReady || ntpSynced) return;
  struct tm tinfo;
  if (getLocalTime(&tinfo, 1000)) {
    rtc.adjust(DateTime(tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday,
                        tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec));
    ntpSynced = true;
    addEvent(EV_INFO, "RTC clock corrected from internet time");
  }
}

// ---------------------------------------------------------------------------
// SEND ONE READING TO THE FLASK SERVER (HTTP POST JSON)
// Also captures the ML holdover prediction from the server's reply.
// ---------------------------------------------------------------------------
bool postReading(const char* timestamp, float coreTemp, float ambientTemp,
                 float ambientHum, bool excursion, const char* source, uint16_t timeoutMs) {
  HTTPClient http;
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + SERVER_PATH;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  http.setTimeout(timeoutMs);

  JsonDocument doc;
  doc["device_id"]     = DEVICE_ID;
  doc["timestamp"]     = timestamp;
  doc["core_temp"]     = coreTemp;
  doc["ambient_temp"]  = ambientTemp;
  doc["ambient_hum"]   = ambientHum;
  doc["excursion"]     = excursion;
  doc["source"]        = source;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  bool ok = (code == 200 || code == 201);
  if (ok && strcmp(source, "live") == 0) {
    JsonDocument resp;
    if (!deserializeJson(resp, http.getString()) && resp["predicted_holdover_hours"].is<float>()) {
      lastHoldoverHours = resp["predicted_holdover_hours"].as<float>();
      lastHoldoverTime = nowUnix();
    }
  }
  if (!ok) {
    Serial.printf("[HTTP] POST failed, code=%d\n", code);
  }
  http.end();
  return ok;
}

bool sendToServer(const char* timestamp, float coreTemp, float ambientTemp,
                  float ambientHum, bool excursion) {
  return postReading(timestamp, coreTemp, ambientTemp, ambientHum, excursion, "live", 5000);
}

bool sendBacklogRow(const char* timestamp, float coreTemp, float ambientTemp,
                    float ambientHum, bool excursion) {
  return postReading(timestamp, coreTemp, ambientTemp, ambientHum, excursion, "backlog", 4000);
}

// ---------------------------------------------------------------------------
// OFFLINE LOGGING — append a CSV row on the SD card (unchanged format)
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

long countBacklogRows() {
  File f = SD.open(BACKLOG_FILE, FILE_READ);
  if (!f) return 0;
  long lines = 0;
  while (f.available()) { if (f.read() == '\n') lines++; }
  f.close();
  return lines > 0 ? lines - 1 : 0;   // minus header
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
  backlogRows++;
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
    server.handleClient();   // keep the dashboard responsive during a long sync

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

  backlogRows = keptCount;
  lastSyncTime = nowUnix();
  lastSyncSent = sentCount;
  if (sentCount > 0) {
    char msg[56];
    snprintf(msg, sizeof(msg), "Backlog synced: %d sent, %d pending", sentCount, keptCount);
    addEvent(EV_OK, msg);
  }
  Serial.printf("[SYNC] Backlog sync done. Sent=%d Remaining=%d\n", sentCount, keptCount);
}

// ---------------------------------------------------------------------------
// LITTLEFS ON-BOARD LOG — every 30 s cycle, whether online or not.
// Columns: timestamp,unix,core_temp,ambient_temp,ambient_hum,excursion,uploaded
// Rotates to readings_old.csv at ~96 KB (~2 days of data), so flash never fills.
// ---------------------------------------------------------------------------
void appendLocalLog(const char* timestamp, const Sample& s, bool uploaded) {
  if (!fsReady) return;

  File check = LittleFS.open(LOG_FILE, "r");
  size_t size = check ? check.size() : 0;
  if (check) check.close();
  if (size > LOG_MAX_BYTES) {
    LittleFS.remove(LOG_FILE_OLD);
    LittleFS.rename(LOG_FILE, LOG_FILE_OLD);
    size = 0;
  }

  File f = LittleFS.open(LOG_FILE, "a");
  if (!f) return;
  if (size == 0) f.println("timestamp,unix,core_temp,ambient_temp,ambient_hum,excursion,uploaded");
  f.printf("%s,%lu,%.2f,%.2f,%.1f,%d,%d\n", timestamp, (unsigned long)s.t,
           s.core, s.amb, s.hum, s.exc, uploaded ? 1 : 0);
  f.close();
}

// On boot, reload the last 2 hours of readings so the trend chart isn't blank.
void restoreTrendFromLog() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return;
  size_t restored = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    int c3 = line.indexOf(',', c2 + 1);
    int c4 = line.indexOf(',', c3 + 1);
    int c5 = line.indexOf(',', c4 + 1);
    int c6 = line.indexOf(',', c5 + 1);
    if (c1 < 0 || c6 < 0 || line.startsWith("timestamp")) continue;
    Sample s;
    s.t    = strtoul(line.substring(c1 + 1, c2).c_str(), nullptr, 10);
    s.core = line.substring(c2 + 1, c3).toFloat();
    s.amb  = line.substring(c3 + 1, c4).toFloat();
    s.hum  = line.substring(c4 + 1, c5).toFloat();
    s.exc  = line.substring(c5 + 1, c6).toInt();
    trendRing.push(s);
    restored++;
  }
  f.close();
  Serial.printf("[FS] Restored %u readings from on-board log.\n", (unsigned)restored);
}

// ---------------------------------------------------------------------------
// WEB SERVER — static dashboard from LittleFS + JSON API
// ---------------------------------------------------------------------------
void sendJson(const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleApiLive() {
  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["now"]       = nowUnix();
  doc["uptime_s"]  = millis() / 1000;
  doc["rtc_ok"]    = rtcReady;
  doc["ntp_synced"] = ntpSynced;

  JsonObject r = doc["reading"].to<JsonObject>();
  if (haveSample) {
    r["t"]            = latest.t;
    r["core_temp"]    = serialized(String(latest.core, 2));
    r["ambient_temp"] = serialized(String(latest.amb, 2));
    r["ambient_hum"]  = serialized(String(latest.hum, 1));
    r["excursion"]    = (bool)latest.exc;
  }
  doc["sensor_fault"] = sensorFault;

  JsonObject lim = doc["limits"].to<JsonObject>();
  lim["min"] = SAFE_TEMP_MIN;
  lim["max"] = SAFE_TEMP_MAX;

  JsonObject ex = doc["excursion"].to<JsonObject>();
  ex["active"]    = excursionActive;
  ex["since"]     = excursionActive ? excursionStart : 0;
  ex["events"]    = statExcEvents;
  ex["muted"]     = alarmMuted();
  ex["mute_left_s"] = alarmMuted() ? (muteUntilMillis - millis()) / 1000 : 0;

  JsonObject st = doc["stats"].to<JsonObject>();
  st["cycles"]      = statCycles;
  st["exc_cycles"]  = statExcCycles;
  st["read_errors"] = statReadErrors;
  if (statCycles > 0) {
    st["min"] = serialized(String(statMin, 2));
    st["max"] = serialized(String(statMax, 2));
    st["avg"] = serialized(String(statSum / statCycles, 2));
    st["in_range_pct"] = serialized(String(100.0 * (statCycles - statExcCycles) / statCycles, 1));
  }

  JsonObject w = doc["wifi"].to<JsonObject>();
  bool up = WiFi.status() == WL_CONNECTED;
  w["connected"] = up;
  w["ssid"]      = WIFI_SSID;
  w["ip"]        = up ? WiFi.localIP().toString() : "";
  w["rssi"]      = up ? WiFi.RSSI() : 0;
  w["ap_ssid"]   = AP_SSID;
  w["ap_ip"]     = WiFi.softAPIP().toString();
  w["ap_clients"] = WiFi.softAPgetStationNum();
  w["mdns"]      = String(MDNS_NAME) + ".local";

  JsonObject sv = doc["server"].to<JsonObject>();
  sv["host"]      = String(SERVER_HOST) + ":" + SERVER_PORT;
  sv["last_ok"]   = lastSendOk;
  sv["last_send"] = lastSendTime;
  if (!isnan(lastHoldoverHours)) {
    sv["holdover_h"]    = lastHoldoverHours;
    sv["holdover_time"] = lastHoldoverTime;
  }

  JsonObject sd = doc["sd"].to<JsonObject>();
  sd["ready"]        = sdReady;
  sd["backlog_rows"] = backlogRows;
  sd["last_sync"]    = lastSyncTime;
  sd["last_sync_sent"] = lastSyncSent;

  JsonObject fs = doc["fs"].to<JsonObject>();
  fs["ready"] = fsReady;
  fs["used"]  = fsReady ? LittleFS.usedBytes() : 0;
  fs["total"] = fsReady ? LittleFS.totalBytes() : 0;

  JsonObject sys = doc["sys"].to<JsonObject>();
  sys["heap_free"] = ESP.getFreeHeap();
  sys["heap_total"] = ESP.getHeapSize();
  sys["chip"]      = ESP.getChipModel();

  String out;
  serializeJson(doc, out);
  sendJson(out);
}

// /api/history?range=live  -> last 10 min at 5 s
// /api/history?range=trend -> last 2 h at 30 s (default)
// Compact column format: {"t":[..],"c":[..],"a":[..],"h":[..],"x":[..]}
template <size_t N>
void sendRing(const Ring<N>& ring) {
  String t = "[", c = "[", a = "[", h = "[", x = "[";
  t.reserve(ring.count * 11); c.reserve(ring.count * 7);
  a.reserve(ring.count * 7);  h.reserve(ring.count * 6);
  for (size_t i = 0; i < ring.count; i++) {
    const Sample& s = ring.at(i);
    const char* sep = i ? "," : "";
    t += sep; t += s.t;
    c += sep; c += String(s.core, 2);
    a += sep; a += String(s.amb, 2);
    h += sep; h += String(s.hum, 1);
    x += sep; x += s.exc;
  }
  String out = "{\"t\":" + t + "],\"c\":" + c + "],\"a\":" + a + "],\"h\":" + h + "],\"x\":" + x + "]}";
  sendJson(out);
}

void handleApiHistory() {
  if (server.arg("range") == "live") sendRing(liveRing);
  else sendRing(trendRing);
}

void handleApiEvents() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < eventCount; i++) {      // newest first
    const Event& e = events[(eventHead + 24 - 1 - i) % 24];
    JsonObject o = arr.add<JsonObject>();
    o["t"] = e.t;
    o["type"] = e.type;
    o["msg"] = e.msg;
  }
  String out;
  serializeJson(doc, out);
  sendJson(out);
}

void handleApiMute() {
  if (server.arg("off") == "1") {
    muteUntilMillis = 0;
    addEvent(EV_INFO, "Alarm sound re-enabled from dashboard");
  } else {
    muteUntilMillis = millis() + MUTE_DURATION_MS;
    noTone(BUZZER_PIN);
    addEvent(EV_INFO, "Alarm silenced for 10 min from dashboard");
  }
  sendJson("{\"ok\":true}");
}

void streamFile(fs::FS& fsys, const char* path, const char* downloadName) {
  if (!fsys.exists(path)) { server.send(404, "text/plain", "No data logged yet"); return; }
  File f = fsys.open(path, "r");
  server.sendHeader("Content-Disposition", String("attachment; filename=") + downloadName);
  server.streamFile(f, "text/csv");
  f.close();
}

// Shown only if the LittleFS image was never uploaded.
const char FALLBACK_PAGE[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Cold Chain Monitor</title>
<style>body{font-family:sans-serif;background:#0E1826;color:#E7EEF5;padding:24px;max-width:560px;margin:auto}
code{background:#1B2E48;padding:2px 6px;border-radius:4px}a{color:#43D9C8}</style></head><body>
<h2>Dashboard files not uploaded yet</h2>
<p>The firmware is running, but the LittleFS partition has no <code>index.html</code>.</p>
<p>In Arduino IDE 2: press <code>Ctrl+Shift+P</code> &rarr; <b>Upload LittleFS to Pico/ESP8266/ESP32</b>
(close the Serial Monitor first). See <code>firmware/README_WIRING.md</code>.</p>
<p>Raw data is still available: <a href="/api/live">/api/live</a></p></body></html>)HTML";

void setupWebServer() {
  server.on("/api/live", HTTP_GET, handleApiLive);
  server.on("/api/history", HTTP_GET, handleApiHistory);
  server.on("/api/events", HTTP_GET, handleApiEvents);
  server.on("/api/mute", HTTP_POST, handleApiMute);
  server.on("/download/log.csv", HTTP_GET, []() { streamFile(LittleFS, LOG_FILE, "coldchain_log.csv"); });
  server.on("/download/log_old.csv", HTTP_GET, []() { streamFile(LittleFS, LOG_FILE_OLD, "coldchain_log_old.csv"); });
  server.on("/download/backlog.csv", HTTP_GET, []() {
    if (!sdReady) { server.send(404, "text/plain", "SD card not available"); return; }
    streamFile(SD, BACKLOG_FILE, "sd_backlog.csv");
  });

  // Dashboard: index.html, style.css, app.js served straight from LittleFS
  server.serveStatic("/", LittleFS, "/", "max-age=300");

  server.onNotFound([]() {
    if (server.uri() == "/" || server.uri() == "/index.html") {
      server.send(200, "text/html", FALLBACK_PAGE);
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });

  server.begin();
  Serial.println(F("[WEB] Dashboard server started on port 80."));
}

// ---------------------------------------------------------------------------
// ALERTS
// ---------------------------------------------------------------------------
void setAlert(bool excursion) {
  if (!excursion) {
    digitalWrite(LED_PIN, LOW);
    noTone(BUZZER_PIN);
    return;
  }
  // LED blinking is handled in loop(); buzzer beeps each 5 s sample unless muted
  if (!alarmMuted()) tone(BUZZER_PIN, 2000, 300);
}

// ---------------------------------------------------------------------------
// OLED LOCAL DISPLAY — now also shows where to open the dashboard
// ---------------------------------------------------------------------------
void updateOLED(float coreTemp, float ambientTemp, float ambientHum, bool excursion) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  bool up = WiFi.status() == WL_CONNECTED;
  display.print(up ? F("ONLINE ") : F("OFFLINE "));
  if (up) display.println(WiFi.localIP());
  else    display.println(WiFi.softAPIP());   // hotspot address for the dashboard

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
