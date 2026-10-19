#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Wire.h>
#include <HUSKYLENS.h>
#include <esp_arduino_version.h>

constexpr uint8_t CHANNEL = 1;
constexpr uint8_t SENSOR_COUNT = 4;
constexpr uint8_t SENSOR_PINS[SENSOR_COUNT] = {13, 14, 27, 26};
constexpr uint8_t ACTIVE_LEVEL = HIGH;
constexpr uint8_t SENSOR_INPUT_MODE = INPUT_PULLDOWN;
constexpr uint32_t DEBOUNCE_MS = 8;
constexpr uint32_t RELEASE_MS = 60;
constexpr uint32_t HEARTBEAT_MS = 1000;
constexpr uint32_t ACK_TIMEOUT_MS = 250;
constexpr uint8_t MAX_RETRIES = 8;

// Optional HUSKYLENS assistant. Sensor-only operation remains available when
// the camera is absent or stops answering. Leave decision assistance disabled
// until the installed camera and its goal-line zone have been calibrated.
constexpr uint8_t HUSKY_SDA_PIN = 21;
constexpr uint8_t HUSKY_SCL_PIN = 22;
constexpr uint8_t HUSKY_I2C_ADDRESS = 0x32;
constexpr int16_t HUSKY_BALL_ID = 1;
constexpr int16_t HUSKY_GOAL_ZONE_X_MIN = 100;
constexpr int16_t HUSKY_GOAL_ZONE_X_MAX = 220;
constexpr uint32_t HUSKY_RETRY_MS = 5000;
constexpr bool HUSKY_DECISION_ENABLED = false;

uint8_t wristMac[] = {0x1C, 0xDB, 0xD4, 0xD3, 0x6C, 0x4C};
enum Type : uint8_t { PKT_STATUS = 1, COMMAND = 2, ACK = 3 };
enum Cmd : uint8_t { START = 1, PAUSE, RESUME, RESET, CLEAR, END };
struct __attribute__((packed)) Packet {
  uint8_t type, command, mask, count;
  uint32_t sequence, timestamp;
};

volatile bool matchActive = false;
volatile bool eventPending = false;
uint8_t candidateMask = 0;
uint8_t stableMask = 0;
uint8_t eventMask = 0;
uint8_t eventClass = 0;
uint8_t retryCount = 0;
uint32_t candidateSince = 0;
uint32_t clearSince = 0;
uint32_t lastHeartbeat = 0;
uint32_t lastAttempt = 0;
uint32_t nextSequence = 0;
Packet pendingPacket = {};
HUSKYLENS huskyLens;
bool huskyOnline = false;
uint32_t lastHuskyAttempt = 0;

uint8_t readSensorMask() {
  uint8_t mask = 0;
  for (uint8_t index = 0; index < SENSOR_COUNT; ++index) {
    if (digitalRead(SENSOR_PINS[index]) == ACTIVE_LEVEL) mask |= 1u << index;
  }
  return mask;
}

uint8_t bitCount(uint8_t value) {
  uint8_t count = 0;
  while (value) { count += value & 1u; value >>= 1; }
  return count;
}

void transmit(const Packet &packet) {
  esp_err_t result = esp_now_send(wristMac, (const uint8_t *)&packet, sizeof(packet));
  if (result != ESP_OK) Serial.printf("ESP-NOW queue error: %d\n", result);
}

void sendHeartbeat(uint8_t currentMask) {
  Packet packet = {};
  packet.type = PKT_STATUS;
  packet.mask = currentMask;
  packet.count = 0;
  packet.sequence = 0;
  packet.timestamp = millis();
  transmit(packet);
}

bool huskyAddressPresent() {
  Wire.beginTransmission(HUSKY_I2C_ADDRESS);
  return Wire.endTransmission() == 0;
}

void connectHuskyLens(uint32_t now) {
  // Never spend time negotiating an optional camera during active play.
  if (matchActive || huskyOnline || now - lastHuskyAttempt < HUSKY_RETRY_MS) return;
  lastHuskyAttempt = now;
  if (!huskyAddressPresent()) return;
  huskyLens.setTimeOutDuration(25);
  huskyOnline = huskyLens.begin(Wire);
  Serial.printf("HUSKYLENS: %s\n", huskyOnline ? "online" : "detected but not responding");
}

enum VisionResult : uint8_t { VISION_UNAVAILABLE, BALL_CONFIRMED, BALL_NOT_CONFIRMED };

VisionResult checkBallAtGoal() {
  if (!huskyOnline) return VISION_UNAVAILABLE;
  if (!huskyLens.requestBlocks(HUSKY_BALL_ID)) {
    huskyOnline = false;
    lastHuskyAttempt = millis();
    Serial.println("HUSKYLENS: communication lost; sensor-only fallback active");
    return VISION_UNAVAILABLE;
  }

  while (huskyLens.available()) {
    HUSKYLENSResult result = huskyLens.read();
    int16_t left = result.xCenter - result.width / 2;
    int16_t right = result.xCenter + result.width / 2;
    if (right >= HUSKY_GOAL_ZONE_X_MIN && left <= HUSKY_GOAL_ZONE_X_MAX) {
      Serial.printf("HUSKYLENS: ball ID %d in goal zone, x=%d width=%d\n",
                    result.ID, result.xCenter, result.width);
      return BALL_CONFIRMED;
    }
  }
  Serial.println("HUSKYLENS: no learned ball in goal zone");
  return BALL_NOT_CONFIRMED;
}

void beginReliableEvent(uint8_t detectedMask, uint8_t sensorCount) {
  uint8_t decisionCount = sensorCount;
  VisionResult vision = VISION_UNAVAILABLE;
  if (HUSKY_DECISION_ENABLED && sensorCount == 1) {
    vision = checkBallAtGoal();
    if (vision == BALL_NOT_CONFIRMED) {
      decisionCount = 2; // Existing wrist protocol displays CHECK VAR for 2+.
    }
  }

  pendingPacket = {};
  pendingPacket.type = PKT_STATUS;
  pendingPacket.mask = detectedMask;
  pendingPacket.count = decisionCount;
  pendingPacket.sequence = ++nextSequence;
  if (pendingPacket.sequence == 0) pendingPacket.sequence = ++nextSequence;
  pendingPacket.timestamp = millis();
  eventPending = true;
  retryCount = 0;
  lastAttempt = millis();
  transmit(pendingPacket);
  // In monitor-only mode the proven sensor decision is already on the air
  // before the optional camera is queried.
  if (!HUSKY_DECISION_ENABLED) vision = checkBallAtGoal();
  const char *visionText = vision == BALL_CONFIRMED ? "confirmed" :
                           vision == BALL_NOT_CONFIRMED ? "not-confirmed" : "unavailable";
  Serial.printf("EVENT seq=%lu mask=0x%02X sensors=%u decision=%s vision=%s\n",
                (unsigned long)pendingPacket.sequence, pendingPacket.mask,
                sensorCount, pendingPacket.count == 1 ? "GOAL" : "CHECK VAR",
                visionText);
}

void retryEventIfNeeded(uint32_t now) {
  if (!eventPending || now - lastAttempt < ACK_TIMEOUT_MS) return;
  if (retryCount >= MAX_RETRIES) {
    Serial.printf("No wrist ACK for seq=%lu after %u retries\n",
                  (unsigned long)pendingPacket.sequence, retryCount);
    eventPending = false;
    return;
  }
  ++retryCount;
  lastAttempt = now;
  transmit(pendingPacket);
  Serial.printf("Retry %u for seq=%lu\n", retryCount,
                (unsigned long)pendingPacket.sequence);
}

void handleReceive(const uint8_t *sender, const uint8_t *data, int length) {
  if (length != (int)sizeof(Packet) || memcmp(sender, wristMac, 6) != 0) return;
  Packet packet;
  memcpy(&packet, data, sizeof(packet));
  if (packet.type == ACK) {
    if (eventPending && packet.sequence == pendingPacket.sequence) {
      eventPending = false;
      Serial.printf("ACK seq=%lu from wrist\n", (unsigned long)packet.sequence);
    }
    return;
  }
  if (packet.type != COMMAND) return;
  if (packet.command == START || packet.command == RESUME) matchActive = true;
  if (packet.command == PAUSE || packet.command == END || packet.command == RESET) matchActive = false;
  if (packet.command == RESET) {
    eventPending = false;
    eventMask = eventClass = 0;
    clearSince = 0;
  }
  static uint8_t lastLoggedCommand = 0;
  if (packet.command != lastLoggedCommand) {
    lastLoggedCommand = packet.command;
    Serial.printf("Command=%u, match=%s\n", packet.command,
                  matchActive ? "ACTIVE" : "INACTIVE");
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void receivePacket(const esp_now_recv_info *info, const uint8_t *data, int length) {
  handleReceive(info->src_addr, data, length);
}
#else
void receivePacket(const uint8_t *sender, const uint8_t *data, int length) {
  handleReceive(sender, data, length);
}
#endif

void setup() {
  Serial.begin(115200);
  delay(300);
  for (uint8_t pin : SENSOR_PINS) pinMode(pin, SENSOR_INPUT_MODE);
  Wire.begin(HUSKY_SDA_PIN, HUSKY_SCL_PIN);
  Wire.setClock(100000);
  lastHuskyAttempt = millis() - HUSKY_RETRY_MS;
  connectHuskyLens(millis());
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) { Serial.println("FATAL: ESP-NOW initialization failed"); return; }
  esp_now_register_recv_cb(receivePacket);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, wristMac, 6);
  peer.channel = CHANNEL;
  if (esp_now_add_peer(&peer) != ESP_OK) { Serial.println("FATAL: could not add wrist peer"); return; }
  nextSequence = esp_random();
  if (nextSequence == 0) nextSequence = 1;
  Serial.printf("Goal post ready: %s\n", WiFi.macAddress().c_str());
  Serial.println("Sensors: GPIO13, GPIO14, GPIO27, GPIO26 (line sensors active HIGH)");
  Serial.printf("HUSKYLENS: SDA GPIO%u, SCL GPIO%u, decision assist %s\n",
                HUSKY_SDA_PIN, HUSKY_SCL_PIN,
                HUSKY_DECISION_ENABLED ? "enabled" : "monitor-only");
  Serial.println("Waiting for Start Match from wrist");
}

void loop() {
  uint32_t now = millis();
  uint8_t currentMask = readSensorMask();
  if (currentMask != candidateMask) {
    candidateMask = currentMask;
    candidateSince = now;
  } else if (stableMask != candidateMask && now - candidateSince >= DEBOUNCE_MS) {
    stableMask = candidateMask;
    Serial.printf("SENSORS mask=0x%02X active=%u\n", stableMask, bitCount(stableMask));
  }
  connectHuskyLens(now);
  if (now - lastHeartbeat >= HEARTBEAT_MS) { lastHeartbeat = now; sendHeartbeat(currentMask); }
  retryEventIfNeeded(now);
  if (!matchActive) {
    eventMask = eventClass = 0;
    clearSince = 0;
    delay(2);
    return;
  }

  uint8_t activeCount = bitCount(stableMask);
  uint8_t currentClass = activeCount > 1 ? 2 : activeCount;
  if (currentClass == 0) {
    if (clearSince == 0) clearSince = now;
    if (now - clearSince >= RELEASE_MS) {
      eventMask = eventClass = 0;
      clearSince = 0;
    }
  } else {
    clearSince = 0;
    eventMask |= stableMask;
    if (currentClass > eventClass) {
      eventClass = currentClass;
      beginReliableEvent(eventMask, eventClass);
    }
  }
  delay(2);
}
