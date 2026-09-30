#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_INA219.h>
#include <TinyGPSPlus.h>

// ================= Pin Assignments =================
#define ONE_WIRE_BUS    15   // DQ pin for both DS18B20 probes (requires 4.7kΩ pullup to 3.3V)
#define GPS_RX_PIN      16   // ESP32 RX2 connected to GPS TX
#define GPS_TX_PIN      17   // ESP32 TX2 connected to GPS RX

// ================= Wi-Fi & Cloud Endpoint =================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* CLOUD_URL     = "http://httpbin.org/post"; // Replace with your cloud webhook/server endpoint

// ================= Cold Chain Operational Thresholds =================
// WHO Guidelines for refrigerated biologics/vaccines (+2°C to +8°C)
const float TEMP_MIN_SAFE = 2.0;
const float TEMP_MAX_SAFE = 8.0;

// Current threshold to determine if cooling element/compressor is actively running
const float COMPRESSOR_MIN_RUN_MA = 150.0; 

// ================= Object Instances =================
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
Adafruit_INA219 ina219;
TinyGPSPlus gps;
HardwareSerial SerialGPS(2); // Using ESP32 Hardware Serial 2

// ================= Telemetry Data Buffers =================
float tempInternal   = -127.0; // Probe 0: Payload core temperature inside cold compartment
float tempColdPlate  = -127.0; // Probe 1: Evaporator / Cold plate surface temperature
float busVoltage_V   = 0.0;
float current_mA     = 0.0;
float loadPower_mW   = 0.0;

double latitude      = 0.0;
double longitude     = 0.0;
float speed_kmph     = 0.0;
int satellites       = 0;

bool isThermalBreach   = false;
bool isCoolingActive   = false;
bool isCompressorFault = false;

unsigned long lastTelemetryUpload = 0;
const unsigned long UPLOAD_INTERVAL_MS = 10000; // 10-second cloud sync interval

// Non-blocking Wi-Fi Reconnection Routine
void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Reconnecting to network...");
    WiFi.disconnect();
    WiFi.reconnect();
  }
}

// Upload formatted telemetry over HTTP
void uploadTelemetryToCloud(const String& payload) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[CLOUD SYNC] Skipped - Wi-Fi disconnected.");
    return;
  }

  HTTPClient http;
  http.begin(CLOUD_URL);
  http.addHeader("Content-Type", "application/json");

  Serial.println("[CLOUD SYNC] Dispatching telemetry packet...");
  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
    Serial.printf("[CLOUD SYNC] HTTP Response: %d\n", httpResponseCode);
  } else {
    Serial.printf("[CLOUD SYNC] POST failed! Error: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=======================================================");
  Serial.println("  COLD CHAIN TELEMETRY ENGINE - SECONDARY ESP32 NODE   ");
  Serial.println("=======================================================");

  // 1. Initialize DS18B20 OneWire Probes
  ds18b20.begin();
  int detectedProbes = ds18b20.getDeviceCount();
  Serial.printf("[1-WIRE] Detected %d DS18B20 probe(s) on GPIO %d\n", detectedProbes, ONE_WIRE_BUS);
  if (detectedProbes < 2) {
    Serial.println("[1-WIRE WARNING] Less than 2 probes detected! Verify 4.7k pull-up resistor.");
  }

  // 2. Initialize INA219 Power Monitor
  Wire.begin(21, 22);
  if (!ina219.begin()) {
    Serial.println("[INA219 ERROR] Module not found on I2C (GPIO 21/22). Verify wiring!");
  } else {
    ina219.setCalibration_32V_2A();
    Serial.println("[INA219] I2C Calibration OK (32V, 2A dynamic range).");
  }

  // 3. Initialize GPS on Hardware Serial 2
  SerialGPS.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] Listening to NEO-6M NMEA stream on UART2 (GPIO 16/17)...");

  // 4. Initialize Wi-Fi Connection
  Serial.printf("[WIFI] Connecting to SSID: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  // Continuously feed incoming GPS characters to TinyGPS++ parser
  while (SerialGPS.available() > 0) {
    gps.encode(SerialGPS.read());
  }

  unsigned long now = millis();

  // Periodic Telemetry Gathering and Publishing
  if (now - lastTelemetryUpload >= UPLOAD_INTERVAL_MS) {
    lastTelemetryUpload = now;

    checkWiFiConnection();

    // 1. Read Dual DS18B20 Temperatures
    ds18b20.requestTemperatures();
    tempInternal  = ds18b20.getTempCByIndex(0); // Inside package chamber
    tempColdPlate = ds18b20.getTempCByIndex(1); // Surface / Cooling element

    // Thermal Breach logic (+2°C to +8°C safety corridor)
    if (tempInternal < TEMP_MIN_SAFE || tempInternal > TEMP_MAX_SAFE) {
      isThermalBreach = true;
    } else {
      isThermalBreach = false;
    }

    // 2. Read Electrical Diagnostics from INA219
    busVoltage_V = ina219.getBusVoltage_V();
    current_mA   = ina219.getCurrent_mA();
    loadPower_mW = ina219.getPower_mW();

    // Filter noise near zero current
    if (abs(current_mA) < 5.0) current_mA = 0.0;
    isCoolingActive = (current_mA > COMPRESSOR_MIN_RUN_MA);

    // Predictive failure detection:
    // If temperature is breaching high AND current is zero, the cooling circuit has failed
    if (tempInternal > TEMP_MAX_SAFE && !isCoolingActive) {
      isCompressorFault = true;
    } else {
      isCompressorFault = false;
    }

    // 3. Extract GPS Location
    if (gps.location.isValid()) {
      latitude   = gps.location.lat();
      longitude  = gps.location.lng();
      speed_kmph = gps.speed.kmph();
      satellites = gps.satellites.value();
    }

    // 4. Build JSON Payload
    String payload = "{";
    payload += "\"node_id\":\"ESP32_NODE_2\",";
    payload += "\"temp_payload_c\":" + String(tempInternal, 2) + ",";
    payload += "\"temp_plate_c\":" + String(tempColdPlate, 2) + ",";
    payload += "\"thermal_breach\":" + String(isThermalBreach ? "true" : "false") + ",";
    payload += "\"compressor_fault\":" + String(isCompressorFault ? "true" : "false") + ",";
    payload += "\"voltage_v\":" + String(busVoltage_V, 2) + ",";
    payload += "\"current_ma\":" + String(current_mA, 1) + ",";
    payload += "\"power_mw\":" + String(loadPower_mW, 1) + ",";
    payload += "\"lat\":" + String(latitude, 6) + ",";
    payload += "\"lng\":" + String(longitude, 6) + ",";
    payload += "\"speed_kmh\":" + String(speed_kmph, 1) + ",";
    payload += "\"satellites\":" + String(satellites);
    payload += "}";

    // 5. Diagnostics Terminal Output
    Serial.println("\n---------------------------------------------------------");
    Serial.printf(" [CORE TEMP]    : %.2f °C (Status: %s)\n", tempInternal, isThermalBreach ? "CRITICAL BREACH" : "SAFE");
    Serial.printf(" [PLATE TEMP]   : %.2f °C\n", tempColdPlate);
    Serial.printf(" [COOLING POWER]: %.2f V | %.1f mA | %.1f mW\n", busVoltage_V, current_mA, loadPower_mW);
    Serial.printf(" [DIAGNOSTICS]  : Cooling Active: %s | Failure Alert: %s\n", 
                  isCoolingActive ? "YES" : "STANDBY", 
                  isCompressorFault ? "COOLER FAILURE DETECTED" : "NOMINAL");
    Serial.printf(" [GPS FIX]      : Lat: %.6f, Lng: %.6f | Sats: %d | Speed: %.1f km/h\n", 
                  latitude, longitude, satellites, speed_kmph);
    Serial.println("---------------------------------------------------------");

    // 6. Push to Server
    uploadTelemetryToCloud(payload);
  }
}
