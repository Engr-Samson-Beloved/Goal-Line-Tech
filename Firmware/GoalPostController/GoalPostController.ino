// ============================================================
// SMART GOAL-LINE DETECTION SYSTEM
// Goal Post Controller Firmware — ESP32 DevKit V1 (30-pin)
// Change POST_ID below: "A" for Goal Post A, "B" for Goal Post B
//
// REVISED PIN MAP — see ../../WIRING_NOTES.md for the full reason.
// Short version: the original spec routed 5 of the 8 TCRT5000 pins
// onto the SD card's SPI bus (GPIO 23/19/18) and the ESP32-CAM UART2
// (GPIO 16/17), which would corrupt SD writes and camera commands
// the moment a sensor fired. It also wired each E3F-10DN receiver
// (referenced to 12V) straight into a GPIO through only a series
// resistor — the ESP32 is only 3.3V-tolerant, so an idle beam would
// eventually kill that pin. Both are fixed here:
//   - SPI (SD) and UART2 (CAM) pins are now reserved and untouched
//     by any sensor.
//   - Each E3F-10DN beam receiver goes through a 2N2222 buffer/level
//     shifter (12V signal -> clean 0-3.3V) before reaching a GPIO.
//     That buffer inverts the signal, so "beam broken" now reads
//     HIGH instead of LOW — see checkPrimary() below.
// ============================================================

#include <esp_now.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <SPI.h>
#include "wifi_credentials.h"

// ── CONFIGURATION ──────────────────────────────────────────────
const String POST_ID = "A";          // "A" or "B"

// Wrist unit MAC address — ESP32-S DevKit #2 (CH340, COM3), bench-test
// stand-in for the eventual ESP32-C3 Super Mini wearable. See
// ../../tools/known_boards.json and firmware/WristUnit/WristUnit.ino.
uint8_t WRIST_MAC[] = {0xD4, 0xE9, 0xF4, 0xA1, 0xCF, 0xEC};

// ── PRIMARY BEAM RECEIVER PINS (through 2N2222 level-shift buffer) ──
// GPIO12 is intentionally excluded (must read LOW at boot or the module
// picks the wrong flash voltage); GPIO0 is intentionally excluded (tied
// to the BOOT button — reusing it risks a stuck ball forcing download mode).
const int PRIMARY[8] = {13, 14, 27, 26, 25, 33, 32, 4};
//                       R1  R2  R3  R4  R5  R6  R7  R8
//                      10cm 35cm 60cm 90cm 120cm 150cm 180cm 210cm

// ── SECONDARY TCRT5000 PINS (direct — module has its own comparator,
//    no external pull-up needed, so input-only pins 34/35/36/39 are fine) ──
const int SECONDARY[8] = {21, 22, 15, 34, 35, 36, 39, 2};
//                         S1  S2  S3  S4  S5  S6  S7  S8

// ── RESERVED — do not reassign these to any sensor ──
// SPI (hardware VSPI default) for the SD card: MOSI=23 MISO=19 SCK=18 CS=5
// UART2 for the ESP32-CAM link: RX2=16 TX2=17
const int SD_CS = 5;

// ── STATE VARIABLES ──────────────────────────────────────────────
bool primaryTriggered  = false;
bool ackReceived        = false;
unsigned long primaryTime = 0;

// ── LIVE MONITOR WEB SERVER ──
// Joins the phone hotspot in wifi_credentials.h so the phone (or anything
// else on that hotspot) can load a live status page from this board.
WebServer server(80);
unsigned long bootMillis = 0;
String lastEventResult = "none yet";
unsigned long lastEventMillis = 0;
bool sdReady = false;

const unsigned long DIRECTION_WINDOW = 50;   // milliseconds
const unsigned long ACK_TIMEOUT      = 200;  // milliseconds
const int MAX_RETRIES                = 3;
const unsigned long COOLDOWN         = 3000; // milliseconds

// ── MESSAGE STRUCTURE ──────────────────────────────────────────────
typedef struct {
  char postID[2];
  bool isGoal;
  unsigned long timestamp;
} GoalMessage;

// ════════════════════════════════════════════════════════════════
// LOGGING
// ════════════════════════════════════════════════════════════════
void logEvent(String result, int beamTriggered, int tcrtTriggered) {
  lastEventResult = result;
  lastEventMillis = millis();
  if (!SD.begin(SD_CS)) return;
  File f = SD.open("/goallog.txt", FILE_APPEND);
  if (f) {
    f.print("TIME:");
    f.print(millis());
    f.print(" POST:");
    f.print(POST_ID);
    f.print(" BEAM:");
    f.print(beamTriggered);
    f.print(" TCRT:");
    f.print(tcrtTriggered);
    f.print(" RESULT:");
    f.println(result);
    f.close();
  }
}

// ════════════════════════════════════════════════════════════════
// ESP-NOW — ACK RECEIVED FROM WRIST UNIT
// ════════════════════════════════════════════════════════════════
void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  ackReceived = true;
  Serial.println("ACK received from wrist unit.");
}

// ════════════════════════════════════════════════════════════════
// SEND GOAL SIGNAL TO WRIST UNIT (with retry)
// ════════════════════════════════════════════════════════════════
void sendGoalSignal(int beamNum, int tcrtNum) {
  GoalMessage msg;
  POST_ID.toCharArray(msg.postID, 2);
  msg.isGoal    = true;
  msg.timestamp = millis();
  ackReceived = false;

  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    Serial.print("Sending goal signal - attempt ");
    Serial.println(attempt);
    esp_now_send(WRIST_MAC, (uint8_t*)&msg, sizeof(msg));

    unsigned long sent = millis();
    while (!ackReceived && (millis() - sent) < ACK_TIMEOUT) {
      delay(5);
    }
    if (ackReceived) {
      Serial.println("GOAL confirmed and acknowledged.");
      logEvent("GOAL_CONFIRMED", beamNum, tcrtNum);
      return;
    }
  }
  Serial.println("WARNING: No ACK received after 3 attempts.");
  logEvent("GOAL_NO_ACK", beamNum, tcrtNum);
}

// ════════════════════════════════════════════════════════════════
// CHECK PRIMARY BEAMS — returns index of triggered beam, -1 if none
// NOTE: the 2N2222 buffer inverts the signal, so a BROKEN beam now
// reads HIGH at the GPIO (idle/clear beam reads LOW).
// ════════════════════════════════════════════════════════════════
int checkPrimary() {
  for (int i = 0; i < 8; i++) {
    if (digitalRead(PRIMARY[i]) == HIGH) {
      return i;
    }
  }
  return -1;
}

// ════════════════════════════════════════════════════════════════
// CHECK SECONDARY TCRT — returns index of triggered sensor, -1 if none
// TCRT5000 module drives this directly LOW when the ball is detected.
// ════════════════════════════════════════════════════════════════
int checkSecondary() {
  for (int i = 0; i < 8; i++) {
    if (digitalRead(SECONDARY[i]) == LOW) {
      return i;
    }
  }
  return -1;
}

// ════════════════════════════════════════════════════════════════
// ASK ESP32-CAM TO CAPTURE A FRAME
// ════════════════════════════════════════════════════════════════
void requestCameraCapture() {
  Serial2.println("CAPTURE");   // ESP32-CAM listens on its UART
  delay(100);                   // give camera time to respond
}

// ════════════════════════════════════════════════════════════════
// WIFI + LIVE MONITOR PAGE
// ════════════════════════════════════════════════════════════════
void connectWifi() {
  Serial.print("Connecting to WiFi \"");
  Serial.print(WIFI_SSID);
  Serial.println("\" ...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    String host = "goalpost" + POST_ID;
    if (MDNS.begin(host.c_str())) {
      Serial.print("mDNS responder started: http://");
      Serial.print(host);
      Serial.println(".local");
    }
  } else {
    Serial.println("WiFi connect failed - will keep retrying in the background.");
  }
}

void handleRoot() {
  unsigned long upSec = (millis() - bootMillis) / 1000;
  unsigned long sinceEvent = (millis() - lastEventMillis) / 1000;
  bool wifiOk = WiFi.status() == WL_CONNECTED;

  String html = "<!DOCTYPE html><html><head><title>Goal Post " + POST_ID + " Monitor</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;background:#111;color:#eee;padding:20px}"
          "h1{color:#4caf50}.card{background:#222;border-radius:8px;padding:16px;margin-bottom:12px}"
          ".ok{color:#4caf50}.warn{color:#ff9800}</style></head><body>";
  html += "<h1>Goal Post " + POST_ID + " - Live Monitor</h1>";
  html += "<div class='card'><b>MAC:</b> " + WiFi.macAddress() + "<br>";
  html += "<b>WiFi:</b> <span class='" + String(wifiOk ? "ok" : "warn") + "'>" +
          String(wifiOk ? "connected" : "disconnected") + "</span><br>";
  html += "<b>IP:</b> " + WiFi.localIP().toString() + "<br>";
  html += "<b>RSSI:</b> " + String(WiFi.RSSI()) + " dBm<br>";
  html += "<b>SD card:</b> " + String(sdReady ? "ready" : "not found") + "<br>";
  html += "<b>Uptime:</b> " + String(upSec) + " s</div>";

  html += "<div class='card'><h3>Beam sensors (live)</h3><table style='width:100%'>";
  for (int i = 0; i < 8; i++) {
    html += "<tr><td>R" + String(i + 1) + "</td><td>" +
            String(digitalRead(PRIMARY[i]) == HIGH ? "BROKEN" : "clear") + "</td>"
            "<td>S" + String(i + 1) + "</td><td>" +
            String(digitalRead(SECONDARY[i]) == LOW ? "DETECTED" : "clear") + "</td></tr>";
  }
  html += "</table></div>";

  html += "<div class='card'><h3>Last event</h3>" + lastEventResult +
          " (" + String(sinceEvent) + " s ago)</div>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// ════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 16, 17); // UART2 for ESP32-CAM

  // Both sensor buses are actively driven by their own circuits
  // (buffer transistor for PRIMARY, onboard comparator for SECONDARY),
  // so plain INPUT is correct — no internal pull-up needed or wanted.
  for (int i = 0; i < 8; i++) {
    pinMode(PRIMARY[i],   INPUT);
    pinMode(SECONDARY[i], INPUT);
  }

  // Initialise SD card
  sdReady = SD.begin(SD_CS);
  if (sdReady) {
    Serial.println("SD card ready.");
  } else {
    Serial.println("SD card not found - logging disabled.");
  }

  // Initialise ESP-NOW
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed.");
    return;
  }
  esp_now_register_recv_cb(onDataRecv);

  // Register wrist unit as ESP-NOW peer
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, WRIST_MAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  connectWifi();
  bootMillis = millis();
  server.on("/", handleRoot);
  server.begin();
  Serial.println("Web server started.");

  Serial.print("Goal Post ");
  Serial.print(POST_ID);
  Serial.println(" ready and monitoring.");
}

// ════════════════════════════════════════════════════════════════
// MAIN LOOP
// ════════════════════════════════════════════════════════════════
void loop() {
  static unsigned long lastReconnectAttempt = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastReconnectAttempt > 8000) {
    lastReconnectAttempt = millis();
    Serial.println("WiFi disconnected - reconnecting...");
    // A plain WiFi.reconnect() after a failed/stuck association just errors
    // forever ("sta is connecting, return error") instead of recovering —
    // a full disconnect + begin actually resets the driver's state.
    WiFi.disconnect();
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  server.handleClient();

  // ── STEP 1: Check primary beam sensors ──
  int beamIndex = checkPrimary();
  if (beamIndex >= 0 && !primaryTriggered) {
    primaryTriggered = true;
    primaryTime      = millis();
    Serial.print("Primary beam broken: R");
    Serial.println(beamIndex + 1);
  }

  // ── STEP 2: If primary triggered, watch for secondary ──
  if (primaryTriggered) {
    unsigned long elapsed = millis() - primaryTime;
    if (elapsed <= DIRECTION_WINDOW) {
      int tcrtIndex = checkSecondary();
      if (tcrtIndex >= 0) {
        Serial.println("Direction confirmed: INWARD. GOAL!");
        requestCameraCapture();
        sendGoalSignal(beamIndex, tcrtIndex);
        primaryTriggered = false;
        delay(COOLDOWN);   // prevent double-counting
      }
    } else {
      Serial.println("Direction window expired. Event rejected.");
      logEvent("REJECTED_NO_DIRECTION", beamIndex, -1);
      primaryTriggered = false;
    }
  }

  delay(1);  // tiny delay to prevent watchdog reset
}
