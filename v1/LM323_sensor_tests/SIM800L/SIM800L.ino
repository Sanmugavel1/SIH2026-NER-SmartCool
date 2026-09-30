#include <HardwareSerial.h>

// Software UART pins on ESP32 (leaving Serial2/GPIO 16-17 free for your NEO-6M GPS)
#define MODEM_RX 27  // ESP32 RX -> SIM800L TXD
#define MODEM_TX 26  // ESP32 TX -> SIM800L RXD
#define MODEM_BAUDRATE 9600

HardwareSerial sim800(1);

// Helper function to send AT commands and print output
String sendAT(const char* cmd, unsigned long timeout = 1000) {
  sim800.println(cmd);
  String response = "";
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (sim800.available()) {
      char c = sim800.read();
      response += c;
    }
  }
  return response;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  // Initialize hardware serial port 1
  sim800.begin(MODEM_BAUDRATE, SERIAL_8N1, MODEM_RX, MODEM_TX);

  Serial.println("\n===========================================");
  Serial.println("   SIM800L No-SIM Hardware Diagnostics    ");
  Serial.println("===========================================");
  delay(1500);

  // 1. Check basic communication
  Serial.print("[1] Checking UART AT Link: ");
  String res = sendAT("AT");
  if (res.indexOf("OK") != -1) {
    Serial.println("PASSED (Modem Responding)");
  } else {
    Serial.println("FAILED! Check VCC (3.7-4.2V), Common GND, and TX/RX pins.");
  }

  // 2. Disable echo for cleaner output
  sendAT("ATE0");

  // 3. Query Module Information
  Serial.println("\n[2] Module Identification:");
  Serial.println(sendAT("ATI"));

  // 4. Query IMEI
  Serial.println("[3] Chip IMEI:");
  Serial.println(sendAT("AT+GSN"));

  // 5. Query Supply Voltage detected by the module
  Serial.println("[4] Internal Power Supply (AT+CBC):");
  // Output format: +CBC: <bcs>,<bcl>,<voltage_in_mV>
  Serial.println(sendAT("AT+CBC"));

  // 6. Verify SIM card detector
  Serial.println("[5] Checking SIM Card Slot Status (AT+CPIN?):");
  String cpinRes = sendAT("AT+CPIN?");
  Serial.println(cpinRes);
  if (cpinRes.indexOf("SIM not inserted") != -1 || cpinRes.indexOf("+CME ERROR") != -1) {
    Serial.println("-> Expected result: Slot detection logic is functioning properly.");
  }

  // 7. Check RF Functionality Mode
  Serial.println("[6] Radio Functionality (AT+CFUN?):");
  Serial.println(sendAT("AT+CFUN?"));

  Serial.println("===========================================");
  Serial.println("Diagnostics complete. Interactive mode active.");
  Serial.println("Type any AT command in the Serial Monitor (Both NL & CR).");
  Serial.println("===========================================\n");
}

void loop() {
  // Bridge serial between PC and SIM800L
  if (Serial.available()) {
    sim800.write(Serial.read());
  }
  if (sim800.available()) {
    Serial.write(sim800.read());
  }
}
