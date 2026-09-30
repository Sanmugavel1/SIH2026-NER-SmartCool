#include <Wire.h>
#include <Adafruit_INA219.h>

// Create an instance of the INA219 sensor (default I2C address is 0x40)
Adafruit_INA219 ina219;

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10); // Wait for native USB Serial to open
  }

  Serial.println("Initializing INA219 Current Sensor...");

  // Initialize I2C on ESP32 default pins: SDA (GPIO 21), SCL (GPIO 22)
  Wire.begin(21, 22);

  // Initialize sensor
  if (!ina219.begin()) {
    Serial.println("Failed to find INA219 chip. Check your wiring and I2C address!");
    while (1) {
      delay(10);
    }
  }

  // By default, the library calibrates for 32V, 2A range.
  // For higher precision on smaller loads (up to 32V, 1A):
  // ina219.setCalibration_32V_1A();
  // Or for lower voltage (up to 16V, 400mA):
  // ina219.setCalibration_16V_400mA();

  Serial.println("INA219 initialized successfully!");
}

void loop() {
  float shuntVoltage_mV = 0.0;
  float busVoltage_V    = 0.0;
  float current_mA      = 0.0;
  float loadVoltage_V   = 0.0;
  float power_mW        = 0.0;

  // Read raw parameters
  shuntVoltage_mV = ina219.getShuntVoltage_mV();
  busVoltage_V    = ina219.getBusVoltage_V();
  current_mA      = ina219.getCurrent_mA();
  power_mW        = ina219.getPower_mW();
  
  // Total load voltage is the bus voltage + voltage drop across the shunt resistor
  loadVoltage_V = busVoltage_V + (shuntVoltage_mV / 1000.0);

  // Output measurements to Serial Monitor
  Serial.print("Bus Voltage:   "); Serial.print(busVoltage_V);    Serial.println(" V");
  Serial.print("Shunt Voltage: "); Serial.print(shuntVoltage_mV);  Serial.println(" mV");
  Serial.print("Load Voltage:  "); Serial.print(loadVoltage_V);   Serial.println(" V");
  Serial.print("Current:       "); Serial.print(current_mA);      Serial.println(" mA");
  Serial.print("Power:         "); Serial.print(power_mW);        Serial.println(" mW");
  Serial.println("----------------------------------------");

  delay(2000); // 2-second interval
}
