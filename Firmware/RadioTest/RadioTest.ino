// TEMPORARY bring-up test only — not part of the final firmware.
// Confirms the WiFi and Bluetooth (BLE) radios both work on this
// ESP32 module: WiFi scan for nearby access points, then a BLE scan
// for nearby advertising devices.

#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("=== ESP32 Radio Test ===");

  WiFi.mode(WIFI_STA);
  Serial.print("WiFi MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.println();
  Serial.println("--- WiFi scan ---");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("No WiFi networks found.");
  } else {
    Serial.printf("%d network(s) found:\n", n);
    for (int i = 0; i < n; i++) {
      Serial.printf("  %2d: %-32s RSSI:%4d dBm  Ch:%2d  %s\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i),
                    WiFi.channel(i),
                    (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "SECURED");
    }
  }
  WiFi.scanDelete();

  Serial.println();
  Serial.println("--- Bluetooth (BLE) scan ---");
  BLEDevice::init("GoalPostCam-RadioTest");
  BLEScan *pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  BLEScanResults *results = pBLEScan->start(5, false);
  int count = results->getCount();
  Serial.printf("%d BLE device(s) found:\n", count);
  for (int i = 0; i < count; i++) {
    BLEAdvertisedDevice d = results->getDevice(i);
    Serial.printf("  %2d: %-24s RSSI:%4d  Addr:%s\n",
                  i + 1,
                  d.haveName() ? d.getName().c_str() : "(no name)",
                  d.getRSSI(),
                  d.getAddress().toString().c_str());
  }
  pBLEScan->clearResults();

  Serial.println();
  Serial.println("=== Test complete ===");
}

void loop() {}
