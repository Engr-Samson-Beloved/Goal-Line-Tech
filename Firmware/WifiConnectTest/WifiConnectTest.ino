// TEMPORARY bring-up test only — not part of the final firmware.
// Joins a real WiFi network and prints the assigned IP address,
// proving the radio can do more than passively scan.

#include <WiFi.h>

const char *SSID = "TECNO SPARK 40";
const char *PASSWORD = "00000000";

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("CONNECTED.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway:    ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("RSSI:       ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.print("MAC:        ");
    Serial.println(WiFi.macAddress());
  } else {
    Serial.print("FAILED to connect. WiFi.status() = ");
    Serial.println(WiFi.status());
  }
}

void loop() {}
