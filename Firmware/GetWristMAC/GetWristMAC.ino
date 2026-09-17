// Flash this to the referee wrist unit's ESP32-C3 first.
// Open Serial Monitor at 115200 baud and copy the printed MAC address
// (format XX:XX:XX:XX:XX:XX) into WRIST_MAC[] in GoalPostController.ino.
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  delay(500);
  Serial.print("Wrist unit MAC address: ");
  Serial.println(WiFi.macAddress());
}

void loop() {}
