#include <TinyGPSPlus.h>

// ESP32 Hardware Serial 2 pins
#define RXD2 16  // Connect to NEO-6M TX
#define TXD2 17  // Connect to NEO-6M RX

#define GPS_BAUDRATE 9600

// Create TinyGPS++ object
TinyGPSPlus gps;

// Use ESP32 Hardware Serial port 2
HardwareSerial gpsSerial(2);

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  // Initialize hardware serial port for GPS: 9600 baud, 8 data bits, no parity, 1 stop bit
  gpsSerial.begin(GPS_BAUDRATE, SERIAL_8N1, RXD2, TXD2);

  Serial.println("========================================");
  Serial.println("ESP32 + NEO-6M GPS Initializing...");
  Serial.println("Ensure antenna is facing open sky.");
  Serial.println("========================================");
}

void loop() {
  // Feed characters from GPS serial stream into TinyGPS++ parser
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // Display data every 2 seconds
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 2000) {
    lastUpdate = millis();

    Serial.print("Satellites in view: ");
    Serial.println(gps.satellites.value());

    if (gps.location.isValid()) {
      Serial.print("Latitude:  ");
      Serial.println(gps.location.lat(), 6);
      Serial.print("Longitude: ");
      Serial.println(gps.location.lng(), 6);
      Serial.print("Altitude:  ");
      Serial.print(gps.altitude.meters());
      Serial.println(" m");
      Serial.print("Speed:     ");
      Serial.print(gps.speed.kmph());
      Serial.println(" km/h");
    } else {
      Serial.println("Searching for satellite fix... (Position not valid yet)");
    }

    if (gps.date.isValid() && gps.time.isValid()) {
      char timeBuffer[32];
      sprintf(timeBuffer, "UTC: %04d-%02d-%02d %02d:%02d:%02d",
              gps.date.year(), gps.date.month(), gps.date.day(),
              gps.time.hour(), gps.time.minute(), gps.time.second());
      Serial.println(timeBuffer);
    }

    Serial.println("----------------------------------------");

    // Detection check: if 5 seconds pass with no incoming bytes, notify wiring issue
    if (millis() > 5000 && gps.charsProcessed() < 10) {
      Serial.println("No GPS data detected! Check RX/TX wiring and baud rate.");
    }
  }
}
