#include <SPI.h>
#include <SD.h>

#define SD_CS_PIN 5

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }
  delay(1000);

  Serial.println("\n--- Testing Micro SD Card Connection ---");

  // Explicitly initialize SPI pins
  SPI.begin(18, 19, 23, 5); // SCK=18, MISO=19, MOSI=23, SS=5

  // Try initializing at standard speed, and if failing, at a lower 4MHz bus speed
  if (!SD.begin(SD_CS_PIN, SPI, 4000000)) {
    Serial.println("❌ SD initialization failed!");
    Serial.println("Troubleshooting Checklist:");
    Serial.println("1. Is VCC plugged into 5V/VIN (not 3.3V)?");
    Serial.println("2. Are MOSI (GPIO 23) and MISO (GPIO 19) swapped?");
    Serial.println("3. Is the card strictly formatted as FAT32?");
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("❌ No SD card attached or detected inside the slot.");
    return;
  }

  Serial.print(" Card Type: ");
  if (cardType == CARD_MMC) Serial.println("MMC");
  else if (cardType == CARD_SD) Serial.println("SDSC");
  else if (cardType == CARD_SDHC) Serial.println("SDHC");
  else Serial.println("UNKNOWN");

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf(" Card Size: %llu MB\n", cardSize);

  // Test File Write
  File testFile = SD.open("/test.txt", FILE_WRITE);
  if (testFile) {
    testFile.println("Cold chain logger initialized successfully!");
    testFile.close();
    Serial.println(" Write Test: SUCCESS (Created /test.txt)");
  } else {
    Serial.println("❌ Write Test: FAILED (Could not open file for writing)");
  }
}

void loop() {
  // Idle
}
