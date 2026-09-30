#include <Arduino.h>
#include <DHT.h>

// Pin Definitions
#define DHTPIN  4         // GPIO 4 connected to DHT11 Data line
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

// Cold-Chain Thresholds (Standard Vaccine/Pharma Storage: 2C to 8C)
const float TEMP_MIN_SAFE = 2.0;   // Freezing risk threshold
const float TEMP_MAX_SAFE = 8.0;   // Thermal degradation threshold
const float HUMID_MIN_SAFE = 30.0; // Dry packaging threshold
const float HUMID_MAX_SAFE = 65.0; // Condensation/moisture threshold

// Timing
unsigned long lastReadTime = 0;
const unsigned long READ_INTERVAL_MS = 2000; // DHT11 minimum refresh interval

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=================================================");
  Serial.println(" Cold-Chain Temperature & Humidity Sentinel     ");
  Serial.println(" Storage Range: 2.0C - 8.0C | Target RH: 30%-65% ");
  Serial.println("=================================================");

  dht.begin();
}

void loop() {
  unsigned long currentMillis = millis();

  // Rate-limit reads to prevent sensor self-heating and read errors
  if (currentMillis - lastReadTime >= READ_INTERVAL_MS) {
    lastReadTime = currentMillis;

    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature(); // Celsius

    // Check if read failed
    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("[ERROR] Failed to read from DHT11 sensor! Check wiring.");
      return;
    }

    bool tempBreach = false;
    bool humidBreach = false;

    // Display telemetry
    Serial.print("Temp: ");
    Serial.print(temperature, 1);
    Serial.print(" °C | Humidity: ");
    Serial.print(humidity, 1);
    Serial.print(" % | Status: ");

    // Evaluate Temperature Limits
    if (temperature > TEMP_MAX_SAFE) {
      Serial.print("[CRITICAL ALERT: OVER-TEMPERATURE] ");
      tempBreach = true;
    } else if (temperature < TEMP_MIN_SAFE) {
      Serial.print("[CRITICAL ALERT: FREEZING RISK] ");
      tempBreach = true;
    }

    // Evaluate Humidity Limits
    if (humidity > HUMID_MAX_SAFE) {
      Serial.print("[WARNING: HIGH HUMIDITY / CONDENSATION] ");
      humidBreach = true;
    } else if (humidity < HUMID_MIN_SAFE) {
      Serial.print("[WARNING: LOW HUMIDITY] ");
      humidBreach = true;
    }

    // Safe state
    if (!tempBreach && !humidBreach) {
      Serial.print("[SECURE - WITHIN RANGE]");
    }

    Serial.println();
  }
}
