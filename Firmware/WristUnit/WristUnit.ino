// ============================================================
// SMART GOAL-LINE DETECTION SYSTEM
// Wrist Unit Firmware
//
// Listens for a GoalMessage over ESP-NOW from either Goal Post
// Controller (POST_ID "A" or "B"), ACKs it immediately, and
// fires a local alert (buzzer + vibration motor + LED).
//
// Bench-test hardware: ESP32-S DevKit #2 (CH340, COM3),
// MAC D4:E9:F4:A1:CF:EC — see ../../tools/known_boards.json.
// This is a stand-in for the eventual ESP32-C3 Super Mini
// wearable; nothing here is chip-specific (plain WiFi STA +
// ESP-NOW + GPIO), so moving to the C3 later is a straight
// recompile with the same source.
//
// Does not hardcode either controller's MAC: the first time a
// GoalMessage arrives from a sender we haven't seen yet, that
// sender is registered as an ESP-NOW peer on the fly so the ACK
// can be sent back. This lets one wrist unit serve both Goal
// Post A and Goal Post B without needing their MACs in advance.
// ============================================================

#include <esp_now.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "wifi_credentials.h"

// ── ALERT OUTPUT PINS ──
const int BUZZER_PIN    = 25;
const int VIBRATION_PIN = 26;
const int LED_PIN       = 27;

const unsigned long ALERT_DURATION = 1500; // ms

// Must match GoalPostController.ino's GoalMessage exactly.
typedef struct {
  char postID[2];
  bool isGoal;
  unsigned long timestamp;
} GoalMessage;

// Fixed ACK payload — GoalPostController's onDataRecv() treats
// receipt of *any* packet as the ACK, so content doesn't matter.
const char ACK_PAYLOAD[] = "ACK";

// ── LIVE MONITOR WEB SERVER ──
// Joins the phone hotspot in wifi_credentials.h so the phone (or anything
// else on that hotspot) can load a live status page from this board.
WebServer server(80);
unsigned long bootMillis = 0;
bool haveMsg = false;
unsigned long lastMsgMillis = 0;
char lastSenderMac[18] = "none";
char lastPostID[3] = "--";
bool lastIsGoal = false;
unsigned long goalCount = 0;

void printMac(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 0x10) Serial.print("0");
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(":");
  }
}

void triggerAlert(const char *postID) {
  Serial.print("GOAL confirmed for Post ");
  Serial.println(postID);
  digitalWrite(BUZZER_PIN, HIGH);
  digitalWrite(VIBRATION_PIN, HIGH);
  digitalWrite(LED_PIN, HIGH);
  delay(ALERT_DURATION);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(VIBRATION_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
}

void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len != sizeof(GoalMessage)) {
    Serial.println("Ignored packet of unexpected size.");
    return;
  }

  GoalMessage msg;
  memcpy(&msg, data, sizeof(msg));

  Serial.print("Message from ");
  printMac(info->src_addr);
  Serial.print(" - postID=");
  Serial.print(msg.postID);
  Serial.print(" isGoal=");
  Serial.println(msg.isGoal ? "true" : "false");

  // Record for the live monitor page.
  snprintf(lastSenderMac, sizeof(lastSenderMac), "%02X:%02X:%02X:%02X:%02X:%02X",
           info->src_addr[0], info->src_addr[1], info->src_addr[2],
           info->src_addr[3], info->src_addr[4], info->src_addr[5]);
  lastPostID[0] = msg.postID[0];
  lastPostID[1] = msg.postID[1];
  lastPostID[2] = '\0';
  lastIsGoal = msg.isGoal;
  lastMsgMillis = millis();
  haveMsg = true;
  if (msg.isGoal) goalCount++;

  // Register the sender as a peer if this is the first message from it,
  // so we're able to send the ACK back (ESP-NOW requires a registered
  // peer before esp_now_send will accept it).
  if (!esp_now_is_peer_exist(info->src_addr)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, info->src_addr, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    Serial.println("  (new sender - registered as peer)");
  }

  esp_now_send(info->src_addr, (uint8_t *)ACK_PAYLOAD, sizeof(ACK_PAYLOAD));

  if (msg.isGoal) {
    triggerAlert(msg.postID);
  }
}

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
    if (MDNS.begin("wristunit")) {
      Serial.println("mDNS responder started: http://wristunit.local");
    }
  } else {
    Serial.println("WiFi connect failed - will keep retrying in the background.");
  }
}

void handleRoot() {
  unsigned long upSec = (millis() - bootMillis) / 1000;
  unsigned long sinceMsg = haveMsg ? (millis() - lastMsgMillis) / 1000 : 0;
  bool wifiOk = WiFi.status() == WL_CONNECTED;

  String html = "<!DOCTYPE html><html><head><title>Wrist Unit Monitor</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;background:#111;color:#eee;padding:20px}"
          "h1{color:#4caf50}.card{background:#222;border-radius:8px;padding:16px;margin-bottom:12px}"
          ".ok{color:#4caf50}.warn{color:#ff9800}</style></head><body>";
  html += "<h1>Wrist Unit - Live Monitor</h1>";
  html += "<div class='card'><b>MAC:</b> " + WiFi.macAddress() + "<br>";
  html += "<b>WiFi:</b> <span class='" + String(wifiOk ? "ok" : "warn") + "'>" +
          String(wifiOk ? "connected" : "disconnected") + "</span><br>";
  html += "<b>IP:</b> " + WiFi.localIP().toString() + "<br>";
  html += "<b>RSSI:</b> " + String(WiFi.RSSI()) + " dBm<br>";
  html += "<b>Uptime:</b> " + String(upSec) + " s</div>";
  html += "<div class='card'><h3>Last ESP-NOW message</h3>";
  if (haveMsg) {
    html += "From: " + String(lastSenderMac) + "<br>";
    html += "Post ID: " + String(lastPostID) + "<br>";
    html += "Goal: " + String(lastIsGoal ? "YES" : "no") + "<br>";
    html += "Received " + String(sinceMsg) + " s ago<br>";
  } else {
    html += "No messages received yet.<br>";
  }
  html += "Total goals seen: " + String(goalCount) + "</div>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(VIBRATION_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(VIBRATION_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  WiFi.mode(WIFI_STA);
  delay(500); // WiFi interface needs a moment to come up before macAddress() is valid
  Serial.print("Wrist unit MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed.");
    return;
  }
  esp_now_register_recv_cb(onDataRecv);

  connectWifi();
  bootMillis = millis();
  server.on("/", handleRoot);
  server.begin();
  Serial.println("Web server started.");

  Serial.println("Wrist unit ready - waiting for goal signals.");
}

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
  delay(2);
}
