#include <Arduino.h>
#include <DHT.h>

// --- Pin Assignments ---
#define DHTPIN          4   // DHT11 Data line
#define LDR_ANALOG_PIN  34  // 4-pin LM393 LDR AO pin
#define BUZZER_PIN      18  // Active Buzzer I/O (Signal) pin

#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// --- Cold-Chain Safety Limits (2.0°C to 8.0°C) ---
const float TEMP_MIN_SAFE = 2.0;   // Freezing risk threshold
const float TEMP_MAX_SAFE = 8.0;   // Over-temperature threshold
const float HUMID_MAX_SAFE = 65.0; // High relative humidity limit

// --- LDR Calibration Thresholds ---
// Light detected (Lid OPEN)  : Raw ADC falls below THRESHOLD_OPEN
// Pitch dark (Lid CLOSED)    : Raw ADC rises above THRESHOLD_CLOSE
const int THRESHOLD_OPEN  = 2300; 
const int THRESHOLD_CLOSE = 2900; 

// --- Timing & Intervals ---
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 2000; // 2 sec DHT11 sampling rate

unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

// Global State Flags
bool isLidOpen = false;
bool isTempBreach = false;

// Noise filtering for ESP32 ADC
int readFilteredADC(int pin) {
  long sum = 0;
  const int samples = 16;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(80);
  }
  return sum / samples;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Configure ADC for full 0 - 3.3V measurement
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  dht.begin();

  Serial.println("====================================================");
  Serial.println(" Cold-Chain Box Monitor: LDR + DHT11 + Buzzer Active");
  Serial.println("====================================================");
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Check LDR Lid Status Continuously (High Responsiveness)
  int rawLdr = readFilteredADC(LDR_ANALOG_PIN);

  if (!isLidOpen && rawLdr < THRESHOLD_OPEN) {
    isLidOpen = true;
    Serial.println("\n[CRITICAL EVENT] Container Lid OPENED (Light Detected)!");
  } else if (isLidOpen && rawLdr > THRESHOLD_CLOSE) {
    isLidOpen = false;
    Serial.println("\n[STATUS] Container Lid CLOSED (Darkness Restored).");
  }

  // 2. Sample DHT11 at Rated 2-Second Frequency
  if (currentMillis - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = currentMillis;

    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();

    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("[ERROR] Failed to read from DHT11 sensor! Check wiring.");
    } else {
      // Evaluate Temperature Excursions
      if (temperature > TEMP_MAX_SAFE) {
        isTempBreach = true;
      } else if (temperature < TEMP_MIN_SAFE) {
        isTempBreach = true;
      } else {
        isTempBreach = false;
      }

      // Print telemetry stream
      Serial.print("Temp: ");
      Serial.print(temperature, 1);
      Serial.print(" °C | Humidity: ");
      Serial.print(humidity, 1);
      Serial.print(" % | LDR ADC: ");
      Serial.print(rawLdr);
      Serial.print(" | Lid: ");
      Serial.print(isLidOpen ? "OPEN (BREACH)" : "CLOSED");
      Serial.print(" | Temp Status: ");
      if (temperature > TEMP_MAX_SAFE) {
        Serial.print("OVER-TEMP ALERT");
      } else if (temperature < TEMP_MIN_SAFE) {
        Serial.print("FREEZE RISK ALERT");
      } else {
        Serial.print("NORMAL (2-8°C)");
      }
      Serial.println();
    }
  }

  // 3. Buzzer Alarm State Machine
  // Condition: Beep if lid is open OR temperature is outside safe boundaries
  if (isLidOpen || isTempBreach) {
    // Non-blocking pulsed beep: 150ms ON / 150ms OFF
    if (currentMillis - lastBuzzerToggle >= 150) {
      lastBuzzerToggle = currentMillis;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
  } else {
    // Silence buzzer under normal conditions
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
  }
}
