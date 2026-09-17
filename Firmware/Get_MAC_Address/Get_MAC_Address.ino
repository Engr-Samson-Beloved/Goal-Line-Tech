// ═══════════════════════════════════════════════════════════════════
// Utility sketch — upload to the ESP32-C3 (referee wrist unit) to read
// its MAC address from the Serial Monitor. Copy the printed address
// into WRIST_MAC[] in GoalPost_Controller.ino.
// ═══════════════════════════════════════════════════════════════════

#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  Serial.println(WiFi.macAddress());
}

void loop() {}
