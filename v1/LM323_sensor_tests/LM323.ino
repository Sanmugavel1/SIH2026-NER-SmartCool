#include <Arduino.h>

// Pin Configuration
#define LDR_ANALOG_PIN   34  // Connect to 4-Pin Module AO
#define LDR_DIGITAL_PIN  19  // Connect to 4-Pin Module DO (Optional reference)

// Threshold Calibration
// In dark/closed box: value typically > 3000 - 3800
// In ambient room light: value typically < 1800 - 2500
const int THRESHOLD_CLOSE = 2900; // Above this = Sealed (Dark)
const int THRESHOLD_OPEN  = 2300; // Below this = Lid Open (Light detected)

// Cold chain tracking state
bool isLidOpen = false;
unsigned long lidOpenedTimestamp = 0;

int readFilteredADC(int pin) {
  long sum = 0;
  const int samples = 16;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(100);
  }
  return sum / samples;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LDR_DIGITAL_PIN, INPUT);

  // Configure ADC to read full 0 - 3.3V range
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12); // 12-bit: 0 - 4095

  Serial.println("============================================");
  Serial.println(" Cold-Chain Container Lid Monitor Active    ");
  Serial.println("============================================");
}

void loop() {
  int rawLdr = readFilteredADC(LDR_ANALOG_PIN);
  int digitalState = digitalRead(LDR_DIGITAL_PIN);

  // State machine with hysteresis
  if (!isLidOpen && rawLdr < THRESHOLD_OPEN) {
    isLidOpen = true;
    lidOpenedTimestamp = millis();
    Serial.println("\n[ALERT] Container Lid OPENED! Thermal breach risk.");
  } 
  else if (isLidOpen && rawLdr > THRESHOLD_CLOSE) {
    unsigned long durationOpenSec = (millis() - lidOpenedTimestamp) / 1000;
    isLidOpen = false;
    Serial.print("\n[NORMAL] Container Lid CLOSED. Total exposure time: ");
    Serial.print(durationOpenSec);
    Serial.println(" seconds.");
  }

  // Live status stream
  Serial.print("LDR Raw: ");
  Serial.print(rawLdr);
  Serial.print(" | Lid Status: ");
  if (isLidOpen) {
    unsigned long elapsed = (millis() - lidOpenedTimestamp) / 1000;
    Serial.print("OPEN (Exposed: ");
    Serial.print(elapsed);
    Serial.print("s)");
  } else {
    Serial.print("CLOSED (Secure)");
  }
  Serial.print(" | DO Pin: ");
  Serial.println(digitalState == HIGH ? "DARK" : "LIGHT");

  delay(500);
}
