// ═══════════════════════════════════════════════════════════════════
// SMART GOAL-LINE DETECTION SYSTEM
// Goal Post Controller Firmware — ESP32 DevKit V1
// Change POST_ID below: "A" for Goal Post A, "B" for Goal Post B
// ═══════════════════════════════════════════════════════════════════

#include <esp_now.h>
#include <WiFi.h>
#include <SD.h>
#include <SPI.h>

// ── CONFIGURATION ───────────────────────────────────────────────────
const String POST_ID = "A";          // "A" or "B"

// Wrist unit MAC address — replace with your ESP32-C3 MAC address
// Find it by uploading a blank sketch and reading Serial Monitor
uint8_t WRIST_MAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── PRIMARY BEAM RECEIVER PINS (E3F-10DN signal wires) ──────────────
const int PRIMARY[8] = {13, 12, 14, 27, 26, 25, 33, 32};
//                       R1  R2  R3  R4  R5  R6  R7  R8
//                      10cm 35cm 60cm 90cm 120cm 150cm 180cm 210cm

// ── SECONDARY TCRT5000 PINS ─────────────────────────────────────────
const int SECONDARY[8] = {23, 22, 21, 19, 18, 5, 17, 16};
//                         S1  S2  S3  S4  S5  S6 S7  S8

// ── SD CARD ─────────────────────────────────────────────────────────
const int SD_CS = 4;

// ── STATE VARIABLES ─────────────────────────────────────────────────
bool primaryTriggered  = false;
bool ackReceived       = false;
unsigned long primaryTime = 0;
const unsigned long DIRECTION_WINDOW = 50;   // milliseconds
const unsigned long ACK_TIMEOUT      = 200;  // milliseconds
const int MAX_RETRIES                = 3;
const unsigned long COOLDOWN         = 3000; // milliseconds

// ── MESSAGE STRUCTURE ────────────────────────────────────────────────
typedef struct {
  char  postID[2];
  bool  isGoal;
  unsigned long timestamp;
} GoalMessage;


// ═══════════════════════════════════════════════════════════════════
// LOGGING
// ═══════════════════════════════════════════════════════════════════
void logEvent(String result, int beamTriggered, int tcrtTriggered) {
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


// ═══════════════════════════════════════════════════════════════════
// ESP-NOW — ACK RECEIVED FROM WRIST UNIT
// ═══════════════════════════════════════════════════════════════════
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  ackReceived = true;
  Serial.println("ACK received from wrist unit.");
}


// ═══════════════════════════════════════════════════════════════════
// SEND GOAL SIGNAL TO WRIST UNIT (with retry)
// ═══════════════════════════════════════════════════════════════════
void sendGoalSignal(int beamNum, int tcrtNum) {
  GoalMessage msg;
  POST_ID.toCharArray(msg.postID, 2);
  msg.isGoal    = true;
  msg.timestamp = millis();

  ackReceived = false;

  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    Serial.print("Sending goal signal — attempt ");
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


// ═══════════════════════════════════════════════════════════════════
// CHECK PRIMARY BEAMS — returns index of triggered beam, -1 if none
// ═══════════════════════════════════════════════════════════════════
int checkPrimary() {
  for (int i = 0; i < 8; i++) {
    if (digitalRead(PRIMARY[i]) == LOW) {
      return i;  // return which beam was broken
    }
  }
  return -1;
}


// ═══════════════════════════════════════════════════════════════════
// CHECK SECONDARY TCRT — returns index of triggered sensor, -1 if none
// ═══════════════════════════════════════════════════════════════════
int checkSecondary() {
  for (int i = 0; i < 8; i++) {
    if (digitalRead(SECONDARY[i]) == LOW) {
      return i;
    }
  }
  return -1;
}


// ═══════════════════════════════════════════════════════════════════
// ASK ESP32-CAM TO CAPTURE A FRAME
// ═══════════════════════════════════════════════════════════════════
void requestCameraCapture() {
  Serial2.println("CAPTURE");   // ESP32-CAM listens on its UART
  delay(100);                   // give camera time to respond
}


// ═══════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 16, 17); // UART2 for ESP32-CAM

  // Configure all sensor pins as inputs with pull-up resistors
  for (int i = 0; i < 8; i++) {
    pinMode(PRIMARY[i],   INPUT_PULLUP);
    pinMode(SECONDARY[i], INPUT_PULLUP);
  }

  // Initialise SD card
  if (SD.begin(SD_CS)) {
    Serial.println("SD card ready.");
  } else {
    Serial.println("SD card not found — logging disabled.");
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

  Serial.print("Goal Post ");
  Serial.print(POST_ID);
  Serial.println(" ready and monitoring.");
}


// ═══════════════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════════════
void loop() {

  // ── STEP 1: Check primary beam sensors ──────────────────────────
  int beamIndex = checkPrimary();

  if (beamIndex >= 0 && !primaryTriggered) {
    // A beam was just broken for the first time
    primaryTriggered = true;
    primaryTime      = millis();
    Serial.print("Primary beam broken: R");
    Serial.println(beamIndex + 1);
  }

  // ── STEP 2: If primary triggered, watch for secondary ────────────
  if (primaryTriggered) {
    unsigned long elapsed = millis() - primaryTime;

    if (elapsed <= DIRECTION_WINDOW) {
      int tcrtIndex = checkSecondary();

      if (tcrtIndex >= 0) {
        // Secondary fired within the 50ms window — GOAL
        Serial.println("Direction confirmed: INWARD. GOAL!");
        requestCameraCapture();
        sendGoalSignal(beamIndex, tcrtIndex);
        primaryTriggered = false;
        delay(COOLDOWN);   // prevent double-counting
      }

    } else {
      // 50ms window expired without secondary — REJECT
      Serial.println("Direction window expired. Event rejected.");
      logEvent("REJECTED_NO_DIRECTION", beamIndex, -1);
      primaryTriggered = false;
    }
  }

  delay(1);  // tiny delay to prevent watchdog reset
}
