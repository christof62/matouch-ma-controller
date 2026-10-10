// Minimal serial-output test (no WiFi, no display, nothing else).
// If the monitor shows the banner + tick counter, Serial works and the
// issue is elsewhere. If not, it's the USB cable/port/board mode.

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(2500);  // give the USB stack time to enumerate before printing
  Serial.println("========================================");
  Serial.println("  SERIAL TEST - output works!");
  Serial.println("========================================");
}

void loop() {
  static uint32_t n = 0;
  Serial.printf("tick %lu\n", (unsigned long)++n);
  delay(1000);
}
