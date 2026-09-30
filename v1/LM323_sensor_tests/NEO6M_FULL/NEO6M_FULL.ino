#include <TinyGPSPlus.h>

#define RXD2 16  // Connect to NEO-6M TX
#define TXD2 17  // Connect to NEO-6M RX
#define GPS_BAUDRATE 9600

TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(GPS_BAUDRATE, SERIAL_8N1, RXD2, TXD2);

  Serial.println("\n--- Cold Chain Asset Tracker: NEO-6M Advanced Telemetry ---");
}

void loop() {
  // Feed the GPS parser
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 3000) {
    lastPrint = millis();

    Serial.println("\n========== TELEMETRY REPORT ==========");

    // 1. Satellite & Accuracy Metrics
    Serial.print("Satellites Tracked : ");
    Serial.println(gps.satellites.value());

    Serial.print("HDOP (Precision)    : ");
    // HDOP < 1.0 = Ideal, 1.0-2.0 = Excellent, > 5.0 = Poor/Unreliable
    float hdop = gps.hdop.hdop();
    Serial.print(hdop);
    if (hdop <= 2.0 && hdop > 0.0) {
      Serial.println(" (High Accuracy)");
    } else if (hdop > 2.0) {
      Serial.println(" (Moderate / Poor Accuracy)");
    } else {
      Serial.println(" (No Fix)");
    }

    // 2. Position & Velocity
    if (gps.location.isValid()) {
      double lat = gps.location.lat();
      double lng = gps.location.lng();

      Serial.print("Latitude           : "); Serial.println(lat, 6);
      Serial.print("Longitude          : "); Serial.println(lng, 6);
      Serial.print("Altitude           : "); Serial.print(gps.altitude.meters()); Serial.println(" m");
      Serial.print("Speed              : "); Serial.print(gps.speed.kmph()); Serial.println(" km/h");
      Serial.print("Course (Heading)   : "); Serial.print(gps.course.deg()); Serial.println(" deg");

      // Direct Link for Mobile Dashboard / Notifications
      Serial.print("Map URL            : https://maps.google.com/?q=");
      Serial.print(lat, 6);
      Serial.print(",");
      Serial.println(lng, 6);
    } else {
      Serial.println("Position           : Searching for lock... (Move antenna near window)");
    }

    // 3. Precision UTC Time
    if (gps.date.isValid() && gps.time.isValid()) {
      char timestamp[32];
      sprintf(timestamp, "%04d-%02d-%02d %02d:%02d:%02d UTC",
              gps.date.year(), gps.date.month(), gps.date.day(),
              gps.time.hour(), gps.time.minute(), gps.time.second());
      Serial.print("UTC Timestamp      : ");
      Serial.println(timestamp);
    }

    Serial.println("=======================================");

    // Diagnostic check
    if (millis() > 5000 && gps.charsProcessed() < 10) {
      Serial.println("WARNING: No serial bytes received from GPS module. Check wiring.");
    }
  }
}
