#include <OneWire.h>
#include <DallasTemperature.h>

// Data wire is connected to ESP32 GPIO 4
#define ONE_WIRE_BUS 4

// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature sensors(&oneWire);

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("Initializing DS18B20 Temperature Sensor...");
  sensors.begin();
  
  // Check how many devices are connected on the bus
  int deviceCount = sensors.getDeviceCount();
  Serial.print("Devices found on bus: ");
  Serial.println(deviceCount);
  
  if (deviceCount == 0) {
    Serial.println("Warning: No DS18B20 detected. Verify wiring and 4.7k pull-up resistor.");
  }
}

void loop() {
  // Request temperature readings from all sensors on the bus
  sensors.requestTemperatures(); 
  
  // Fetch temperature in Celsius (index 0 gets the first sensor)
  float tempC = sensors.getTempCByIndex(0);
  
  // Verify reading validity
  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("Error: Could not read temperature data.");
  } else {
    float tempF = sensors.getTempFByIndex(0);
    
    Serial.print("Temperature: ");
    Serial.print(tempC);
    Serial.print(" °C  |  ");
    Serial.print(tempF);
    Serial.println(" °F");
  }
  
  delay(2000); // Read every 2 seconds
}
