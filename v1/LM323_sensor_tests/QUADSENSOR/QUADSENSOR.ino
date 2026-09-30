#include <Arduino.h>
#include <DHT.h>

// --- Pin Assignments ---
#define DHTPIN          4   // DHT11 One-Wire data line
#define LDR_ANALOG_PIN  34  // LM393 LDR AO pin (ADC1)
#define ACS712_PIN      35  // ACS712 Current OUT pin (ADC1)
#define BUZZER_PIN      18  // Active Buzzer Signal pin

#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// --- Cold-Chain Operating Thresholds ---
const float TEMP_MIN_SAFE = 2.0;   // Freezing risk (°C)
const float TEMP_MAX_SAFE = 8.0;   // Thermal degradation threshold (°C)
const float HUMID_MAX_SAFE = 65.0; // Moisture threshold (%RH)

// --- LDR Calibration ---
const int THRESHOLD_OPEN  = 2300;  // Below this = Lid Open (Light present)
const int THRESHOLD_CLOSE = 2900;  // Above this = Lid Closed (Pitch dark)

// --- ACS712 Calibration (5A Variant = 185 mV/A) ---
const float mVperAmp = 185.0;
float zeroCurrentVoltage_mV = 2500.0;

// --- Timing and State Machine ---
unsigned long cycleStartTime = 0;
const unsigned long CYCLE_DURATION_MS = 30000; // 30-Second Full Cycle

// Non-blocking Buzzer Timing
unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

// Cached Telemetry Data
float currentTemp = 0.0;
float currentHum = 0.0;
int currentLdrRaw = 0;
float currentAmps = 0.0;

bool isLidOpen = false;
bool isTempBreach = false;

// Track active stage to print only once per transition
int lastStage = -1;

// Multi-Sample ADC Filter
int readFilteredADC(int pin, int samples = 20) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(80);
  }
  return sum / samples;
}

// Convert ADC to Millivolts
float readMilliVolts(int pin) {
  int raw = readFilteredADC(pin, 30);
  return (raw / 4095.0) * 3300.0;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  dht.begin();

  Serial.println("=================================================");
  Serial.println(" Cold-Chain Box Monitoring System Initialized     ");
  Serial.println(" Mode: Sequential 30-Second Diagnostic Display   ");
  Serial.println(" Calibrating Current Sensor (Ensure No Load)... ");

  // Calibrate 0A baseline over 50 samples
  float sumMv = 0.0;
  for (int i = 0; i < 50; i++) {
    sumMv += readMilliVolts(ACS712_PIN);
    delay(20);
  }
  zeroCurrentVoltage_mV = sumMv / 50.0;

  Serial.print(" ACS712 Zero-Point: ");
  Serial.print(zeroCurrentVoltage_mV, 1);
  Serial.println(" mV");
  Serial.println("=================================================\n");

  cycleStartTime = millis();
}

void loop() {
  unsigned long now = millis();

  // -------------------------------------------------------------
  // 1. Continuous Background Sensor Acquisition & Safety Checks
  // -------------------------------------------------------------
  // LDR Check
  currentLdrRaw = readFilteredADC(LDR_ANALOG_PIN, 16);
  if (!isLidOpen && currentLdrRaw < THRESHOLD_OPEN) {
    isLidOpen = true;
  } else if (isLidOpen && currentLdrRaw > THRESHOLD_CLOSE) {
    isLidOpen = false;
  }

  // Read DHT11 (Only every 1-2 sec to respect hardware duty cycle)
  static unsigned long lastDhtPoll = 0;
  if (now - lastDhtPoll >= 1500) {
    lastDhtPoll = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) currentTemp = t;
    if (!isnan(h)) currentHum = h;

    // Evaluate Cold Chain Excursion
    if (currentTemp > TEMP_MAX_SAFE || currentTemp < TEMP_MIN_SAFE) {
      isTempBreach = true;
    } else {
      isTempBreach = false;
    }
  }

  // Current Measurement
  float currentPin_mV = readMilliVolts(ACS712_PIN);
  float voltageDelta_mV = currentPin_mV - zeroCurrentVoltage_mV;
  currentAmps = voltageDelta_mV / mVperAmp;
  if (abs(currentAmps) < 0.04) currentAmps = 0.0; // Cut off noise floor

  // -------------------------------------------------------------
  // 2. Immediate Buzzer Annunciation (Independent of display)
  // -------------------------------------------------------------
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
  // 3. 30-Second Sequential Display State Machine
  // -------------------------------------------------------------
  unsigned long elapsedCycle = (now - cycleStartTime) % CYCLE_DURATION_MS;
  int currentStage = 0;

  if (elapsedCycle < 5000) {
    currentStage = 1; // 0 - 5s: Temperature
  } else if (elapsedCycle < 10000) {
    currentStage = 2; // 5 - 10s: Humidity
  } else if (elapsedCycle < 15000) {
    currentStage = 3; // 10 - 15s: LDR / Lid Status
  } else if (elapsedCycle < 20000) {
    currentStage = 4; // 15 - 20s: Current Consumption
  } else {
    currentStage = 5; // 20 - 30s: Combined Dashboard
  }

  // Print output only when moving into a new window
  if (currentStage != lastStage) {
    lastStage = currentStage;

    switch (currentStage) {
      case 1: // Temperature (5 Seconds)
        Serial.println("\n------------------------------------------------");
        Serial.println(" [SCREEN 1/5] TEMPERATURE MONITOR (5 SECONDS)   ");
        Serial.print(" Current Temperature : ");
        Serial.print(currentTemp, 1);
        Serial.println(" °C");
        Serial.print(" Target Safe Window  : ");
        Serial.print(TEMP_MIN_SAFE, 1);
        Serial.print(" °C to ");
        Serial.print(TEMP_MAX_SAFE, 1);
        Serial.println(" °C");
        Serial.print(" Status              : ");
        if (currentTemp > TEMP_MAX_SAFE) {
          Serial.println("CRITICAL BREACH (OVER-TEMPERATURE)");
        } else if (currentTemp < TEMP_MIN_SAFE) {
          Serial.println("CRITICAL BREACH (FREEZING RISK)");
        } else {
          Serial.println("NORMAL / SECURE");
        }
        Serial.println("------------------------------------------------");
        break;

      case 2: // Humidity (5 Seconds)
        Serial.println("\n------------------------------------------------");
        Serial.println(" [SCREEN 2/5] HUMIDITY MONITOR (5 SECONDS)      ");
        Serial.print(" Relative Humidity   : ");
        Serial.print(currentHum, 1);
        Serial.println(" %");
        Serial.print(" Max Safe Limit      : ");
        Serial.print(HUMID_MAX_SAFE, 1);
        Serial.println(" %");
        Serial.print(" Status              : ");
        Serial.println(currentHum > HUMID_MAX_SAFE ? "HIGH HUMIDITY WARNING" : "NORMAL");
        Serial.println("------------------------------------------------");
        break;

      case 3: // LDR / Lid Status (5 Seconds)
        Serial.println("\n------------------------------------------------");
        Serial.println(" [SCREEN 3/5] CONTAINER LID & LIGHT (5 SECONDS) ");
        Serial.print(" LDR Raw ADC Reading : ");
        Serial.println(currentLdrRaw);
        Serial.print(" Box Integrity       : ");
        if (isLidOpen) {
          Serial.println("UNSEALED / LID OPEN (LIGHT DETECTED)");
        } else {
          Serial.println("SEALED / SECURE (DARK)");
        }
        Serial.println("------------------------------------------------");
        break;

      case 4: // Current Consumption (5 Seconds)
        Serial.println("\n------------------------------------------------");
        Serial.println(" [SCREEN 4/5] SYSTEM CURRENT DRAW (5 SECONDS)   ");
        Serial.print(" Load Current Draw   : ");
        Serial.print(currentAmps, 3);
        Serial.print(" A  (");
        Serial.print(currentAmps * 1000.0, 0);
        Serial.println(" mA)");
        Serial.print(" Calibrated Baseline : ");
        Serial.print(zeroCurrentVoltage_mV, 1);
        Serial.println(" mV");
        Serial.println("------------------------------------------------");
        break;

      case 5: // Combined Dashboard (10 Seconds)
        Serial.println("\n================================================");
        Serial.println(" [SCREEN 5/5] COMBINED TELEMETRY (10 SECONDS)  ");
        Serial.println("================================================");
        Serial.print(" Temperature : ");
        Serial.print(currentTemp, 1);
        Serial.print(" °C  [");
        Serial.print(isTempBreach ? "BREACH" : "SAFE");
        Serial.println("]");

        Serial.print(" Humidity    : ");
        Serial.print(currentHum, 1);
        Serial.println(" %");

        Serial.print(" Lid Status  : ");
        Serial.println(isLidOpen ? "OPEN (TAMPER DETECTED)" : "CLOSED (DARK)");

        Serial.print(" Current     : ");
        Serial.print(currentAmps, 3);
        Serial.print(" A (");
        Serial.print(currentAmps * 1000.0, 0);
        Serial.println(" mA)");

        Serial.print(" Alarm State : ");
        Serial.println((isLidOpen || isTempBreach) ? "ACTIVE BUZZING" : "ALL NORMAL");
        Serial.println("================================================");
        break;
    }
  }
}
