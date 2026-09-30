#include <Arduino.h>
#include <DHT.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>

// --- Safe Pin Allocations (No Boot Strapping Conflicts) ---
#define SD_CS_PIN       5   // VSPI Chip Select
#define DHTPIN          4   // DHT11 One-Wire Data
#define LDR_ANALOG_PIN  34  // LM393 LDR AO (ADC1_CH6)
#define ACS712_PIN      35  // ACS712 OUT (ADC1_CH7)
#define BUZZER_PIN      13  // Active Buzzer Signal (Moved from GPIO 2 to GPIO 13)

#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// --- Wi-Fi Credentials ---
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Target Sync Endpoint
const char* SYNC_SERVER_URL = "http://httpbin.org/post";

// --- Cold-Chain Safety Specifications (2.0°C - 8.0°C) ---
const float TEMP_MIN_SAFE = 2.0;
const float TEMP_MAX_SAFE = 8.0;
const float HUMID_MAX_SAFE = 65.0;

// LDR Calibration Thresholds
const int THRESHOLD_OPEN  = 2300;
const int THRESHOLD_CLOSE = 2900;

// ACS712 Calibration (5A = 185 mV/A)
const float mVperAmp = 185.0;
float zeroCurrentVoltage_mV = 2500.0;

// --- Timing and State Machine ---
unsigned long cycleStartTime = 0;
const unsigned long CYCLE_DURATION_MS = 30000; // 30-Second Full Cycle

unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

float currentTemp = 0.0;
float currentHum = 0.0;
int currentLdrRaw = 0;
float currentAmps = 0.0;

bool isLidOpen = false;
bool isTempBreach = false;
int lastStage = -1;

bool sdCardAvailable = false;
unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL_MS = 10000; // Log to SD every 10 seconds

// Multi-Sample ADC Filter
int readFilteredADC(int pin, int samples = 20) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(80);
  }
  return sum / samples;
}

// Convert ADC Reading to Real Millivolts
float readMilliVolts(int pin) {
  int raw = readFilteredADC(pin, 30);
  return (raw / 4095.0) * 3300.0;
}

// Write to SD Card file
void appendFile(const char* path, const char* message) {
  if (!sdCardAvailable) return;
  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    Serial.printf("[SD ERROR] Failed to open %s for appending.\n", path);
    return;
  }
  file.print(message);
  file.close();
}

// Store sensor telemetry to local Micro SD card
void logSensorDataToSD() {
  if (!sdCardAvailable) return;

  unsigned long timestampSec = millis() / 1000;
  char logEntry[160];
  snprintf(logEntry, sizeof(logEntry), "%lu,%.2f,%.2f,%d,%.3f,%s,%s\n",
           timestampSec,
           currentTemp,
           currentHum,
           currentLdrRaw,
           currentAmps,
           isLidOpen ? "OPEN" : "CLOSED",
           isTempBreach ? "BREACH" : "NORMAL");

  // 1. Permanent Master Record
  appendFile("/telemetry.csv", logEntry);

  // 2. Pending Queue for Cloud Sync
  appendFile("/pending.csv", logEntry);

  Serial.println("[SD LOG] Snapshot saved to /telemetry.csv and /pending.csv");
}

// Synchronize pending records once Internet connectivity is available
void syncOfflineQueue() {
  if (!sdCardAvailable) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!SD.exists("/pending.csv")) return;

  File pendingFile = SD.open("/pending.csv", FILE_READ);
  if (!pendingFile || pendingFile.size() == 0) {
    if (pendingFile) pendingFile.close();
    return;
  }

  Serial.println("\n[SYNC] Wi-Fi Active. Synchronizing backlog to cloud...");

  HTTPClient http;
  http.begin(SYNC_SERVER_URL);
  http.addHeader("Content-Type", "text/csv");

  String payload = "";
  while (pendingFile.available()) {
    payload += (char)pendingFile.read();
  }
  pendingFile.close();

  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0 && (httpResponseCode == 200 || httpResponseCode == 201)) {
    Serial.printf("[SYNC SUCCESS] Backlog uploaded successfully. (HTTP %d)\n", httpResponseCode);
    SD.remove("/pending.csv"); // Clear queue once acknowledged
  } else {
    Serial.printf("[SYNC POSTPONED] Server unreachable (Code %d). Retaining pending.csv\n", httpResponseCode);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Configure Buzzer on GPIO 13
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // ADC resolution and attenuation
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  dht.begin();

  // Initialize Micro SD Card
  Serial.print("Initializing SD Card on VSPI (CS = GPIO 5)... ");
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("FAILED! Check SPI lines, card format (FAT32), and 5V/GND.");
    sdCardAvailable = false;
  } else {
    Serial.println("SUCCESS! Micro SD mounted.");
    sdCardAvailable = true;

    // Create CSV header if new
    if (!SD.exists("/telemetry.csv")) {
      File file = SD.open("/telemetry.csv", FILE_WRITE);
      if (file) {
        file.println("UptimeSec,Temperature_C,Humidity_Pct,LDR_Raw,Current_A,LidStatus,ThermalStatus");
        file.close();
      }
    }
  }

  // Begin background Wi-Fi connection
  Serial.printf("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Auto-calibrate ACS712 baseline offset (Ensure 0A load during boot)
  float sumMv = 0.0;
  for (int i = 0; i < 40; i++) {
    sumMv += readMilliVolts(ACS712_PIN);
    delay(20);
  }
  zeroCurrentVoltage_mV = sumMv / 40.0;

  cycleStartTime = millis();
}

void loop() {
  unsigned long now = millis();

  // -------------------------------------------------------------
  // 1. Continuous Hardware Safety Monitoring
  // -------------------------------------------------------------
  currentLdrRaw = readFilteredADC(LDR_ANALOG_PIN, 16);
  if (!isLidOpen && currentLdrRaw < THRESHOLD_OPEN) {
    isLidOpen = true;
  } else if (isLidOpen && currentLdrRaw > THRESHOLD_CLOSE) {
    isLidOpen = false;
  }

  static unsigned long lastDhtPoll = 0;
  if (now - lastDhtPoll >= 1500) {
    lastDhtPoll = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) currentTemp = t;
    if (!isnan(h)) currentHum = h;

    if (currentTemp > TEMP_MAX_SAFE || currentTemp < TEMP_MIN_SAFE) {
      isTempBreach = true;
    } else {
      isTempBreach = false;
    }
  }

  float currentPin_mV = readMilliVolts(ACS712_PIN);
  float voltageDelta_mV = currentPin_mV - zeroCurrentVoltage_mV;
  currentAmps = voltageDelta_mV / mVperAmp;
  if (abs(currentAmps) < 0.04) currentAmps = 0.0; // Cut off noise floor

  // Instant Buzzer Warning (Pulsed 150ms on / 150ms off)
  if (isLidOpen || isTempBreach) {
    if (now - lastBuzzerToggle >= 150) {
      lastBuzzerToggle = now;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
  }

  // -------------------------------------------------------------
  // 2. Periodic Local SD Logging & Internet Sync Check
  // -------------------------------------------------------------
  if (now - lastLogTime >= LOG_INTERVAL_MS) {
    lastLogTime = now;
    logSensorDataToSD();
    syncOfflineQueue();
  }

  // -------------------------------------------------------------
  // 3. Sequential 30-Second Diagnostic Display (4x5s + 10s combined)
  // -------------------------------------------------------------
  unsigned long elapsedCycle = (now - cycleStartTime) % CYCLE_DURATION_MS;
  int currentStage = 0;

  if (elapsedCycle < 5000)       currentStage = 1; // Temp (5s)
  else if (elapsedCycle < 10000) currentStage = 2; // Humidity (5s)
  else if (elapsedCycle < 15000) currentStage = 3; // LDR / Lid (5s)
  else if (elapsedCycle < 20000) currentStage = 4; // Current Draw (5s)
  else                           currentStage = 5; // Combined Dashboard (10s)

  if (currentStage != lastStage) {
    lastStage = currentStage;
    switch (currentStage) {
      case 1:
        Serial.println("\n------------------------------------------------");
        Serial.println(" [SCREEN 1/5] TEMPERATURE MONITOR (5 SECONDS)   ");
        Serial.printf(" Temperature: %.1f °C | Target Safe: %.1f - %.1f °C\n", currentTemp, TEMP_MIN_SAFE, TEMP_MAX_SAFE);
        Serial.printf(" Status     : %s\n", isTempBreach ? "CRITICAL BREACH" : "SAFE");
        Serial.println("------------------------------------------------");
        break;

      case 2:
        Serial.println("\n------------------------------------------------");
        Serial.println(" [SCREEN 2/5] HUMIDITY MONITOR (5 SECONDS)      ");
        Serial.printf(" Relative Humidity: %.1f %% (Limit: %.1f %%)\n", currentHum, HUMID_MAX_SAFE);
        Serial.println("------------------------------------------------");
        break;

      case 3:
        Serial.println("\n------------------------------------------------");
        Serial.println(" [SCREEN 3/5] CONTAINER LID & LIGHT (5 SECONDS) ");
        Serial.printf(" Raw LDR: %d | Status: %s\n", currentLdrRaw, isLidOpen ? "LID OPEN (BREACH)" : "SEALED (DARK)");
        Serial.println("------------------------------------------------");
        break;

      case 4:
        Serial.println("\n------------------------------------------------");
        Serial.println(" [SCREEN 4/5] CURRENT CONSUMPTION (5 SECONDS)   ");
        Serial.printf(" Load Current: %.3f A (%.0f mA)\n", currentAmps, currentAmps * 1000.0);
        Serial.println("------------------------------------------------");
        break;

      case 5:
        Serial.println("\n================================================");
        Serial.println(" [SCREEN 5/5] COMBINED TELEMETRY (10 SECONDS)  ");
        Serial.println("================================================");
        Serial.printf(" Temp: %.1f °C | Hum: %.1f %% | Current: %.3f A\n", currentTemp, currentHum, currentAmps);
        Serial.printf(" Lid : %s | Storage: %s\n", isLidOpen ? "OPEN" : "SEALED", sdCardAvailable ? "SD OK" : "SD ERR");
        Serial.printf(" Network Status: %s\n", (WiFi.status() == WL_CONNECTED) ? "ONLINE (SYNC ACTIVE)" : "OFFLINE (CACHING LOCALLY)");
        Serial.println("================================================");
        break;
    }
  }
}
