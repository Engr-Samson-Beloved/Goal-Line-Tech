#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_http_server.h>

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

constexpr char WIFI_SSID[] = "GOAL-LINE-REFEREE";
constexpr char WIFI_PASSWORD[] = "GoalLine2026";
constexpr char WRIST_STATUS_URL[] = "http://192.168.4.1/status";
constexpr uint8_t RECORD_FPS = 5;
constexpr uint32_t FRAME_INTERVAL_MS = 1000 / RECORD_FPS;
constexpr uint32_t STATE_POLL_MS = 750;
constexpr uint32_t STATE_TIMEOUT_MS = 5000;
constexpr uint32_t SD_RETRY_MS = 30000;

enum MatchState : uint8_t { STATE_UNKNOWN, STATE_READY, STATE_ACTIVE, STATE_PAUSED, STATE_STOPPED };

httpd_handle_t pageServer = nullptr;
httpd_handle_t streamServer = nullptr;
SemaphoreHandle_t cameraMutex = nullptr;
MatchState matchState = STATE_UNKNOWN;
bool cameraReady = false;
bool sdReady = false;
bool sessionOpen = false;
File sessionFile;
String sessionPath;
uint32_t sessionFrames = 0;
uint32_t sessionBytes = 0;
uint32_t lastRecordFrame = 0;
uint32_t lastHeaderUpdate = 0;
uint32_t lastStatePoll = 0;
uint32_t lastStateSeen = 0;
uint32_t lastSdAttempt = 0;

#define PART_BOUNDARY "goallinecamboundary"
static const char *STREAM_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

void writeU16(File &file, uint16_t value) {
  uint8_t bytes[] = {(uint8_t)value, (uint8_t)(value >> 8)};
  file.write(bytes, sizeof(bytes));
}

void writeU32(File &file, uint32_t value) {
  uint8_t bytes[] = {(uint8_t)value, (uint8_t)(value >> 8),
                     (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
  file.write(bytes, sizeof(bytes));
}

void writeFourCC(File &file, const char *value) { file.write((const uint8_t *)value, 4); }

void writeAviHeader(File &file) {
  writeFourCC(file, "RIFF"); writeU32(file, 0); writeFourCC(file, "AVI ");
  writeFourCC(file, "LIST"); writeU32(file, 192); writeFourCC(file, "hdrl");
  writeFourCC(file, "avih"); writeU32(file, 56);
  writeU32(file, 1000000UL / RECORD_FPS);
  writeU32(file, 500000); writeU32(file, 0); writeU32(file, 0);
  writeU32(file, 0); writeU32(file, 0); writeU32(file, 1); writeU32(file, 100000);
  writeU32(file, 640); writeU32(file, 480);
  for (uint8_t i = 0; i < 4; ++i) writeU32(file, 0);
  writeFourCC(file, "LIST"); writeU32(file, 116); writeFourCC(file, "strl");
  writeFourCC(file, "strh"); writeU32(file, 56);
  writeFourCC(file, "vids"); writeFourCC(file, "MJPG");
  writeU32(file, 0); writeU16(file, 0); writeU16(file, 0); writeU32(file, 0);
  writeU32(file, 1); writeU32(file, RECORD_FPS); writeU32(file, 0); writeU32(file, 0);
  writeU32(file, 100000); writeU32(file, 0xFFFFFFFF); writeU32(file, 0);
  writeU16(file, 0); writeU16(file, 0); writeU16(file, 640); writeU16(file, 480);
  writeFourCC(file, "strf"); writeU32(file, 40); writeU32(file, 40);
  writeU32(file, 640); writeU32(file, 480); writeU16(file, 1); writeU16(file, 24);
  writeFourCC(file, "MJPG"); writeU32(file, 640UL * 480UL * 3UL);
  writeU32(file, 0); writeU32(file, 0); writeU32(file, 0); writeU32(file, 0);
  writeFourCC(file, "LIST"); writeU32(file, 0); writeFourCC(file, "movi");
}

void patchAviHeader() {
  if (!sessionOpen) return;
  uint32_t fileSize = sessionFile.size();
  sessionFile.seek(4); writeU32(sessionFile, fileSize - 8);
  sessionFile.seek(48); writeU32(sessionFile, sessionFrames);
  sessionFile.seek(140); writeU32(sessionFile, sessionFrames);
  sessionFile.seek(216); writeU32(sessionFile, fileSize - 220);
  sessionFile.seek(fileSize);
  sessionFile.flush();
  lastHeaderUpdate = millis();
}

String nextSessionPath() {
  if (!SD_MMC.exists("/sessions")) SD_MMC.mkdir("/sessions");
  for (uint16_t number = 1; number < 10000; ++number) {
    char path[40];
    snprintf(path, sizeof(path), "/sessions/session_%04u.avi", number);
    if (!SD_MMC.exists(path)) return String(path);
  }
  return String();
}

bool openSession() {
  if (!sdReady || sessionOpen) return sessionOpen;
  sessionPath = nextSessionPath();
  if (sessionPath.isEmpty()) { Serial.println("No free session filename"); return false; }
  sessionFile = SD_MMC.open(sessionPath, FILE_WRITE);
  if (!sessionFile) { Serial.println("Could not create session file"); return false; }
  writeAviHeader(sessionFile);
  sessionFrames = sessionBytes = 0;
  sessionOpen = true;
  lastRecordFrame = lastHeaderUpdate = millis();
  Serial.printf("Recording started: %s\n", sessionPath.c_str());
  return true;
}

void closeSession() {
  if (!sessionOpen) return;
  patchAviHeader();
  sessionFile.close();
  Serial.printf("Recording closed: %s, %lu frames, %lu bytes\n", sessionPath.c_str(),
                (unsigned long)sessionFrames, (unsigned long)sessionBytes);
  sessionOpen = false;
}

bool mountSd() {
  lastSdAttempt = millis();
  SD_MMC.end();
  sdReady = SD_MMC.begin("/sdcard", true);
  if (!sdReady) { Serial.println("microSD unavailable; streaming remains enabled"); return false; }
  Serial.printf("microSD ready: %llu MB free\n",
                (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024ULL * 1024ULL));
  return true;
}

bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  esp_err_t result = esp_camera_init(&config);
  Serial.printf("Camera: %s (0x%X), PSRAM: %s\n", result == ESP_OK ? "ready" : "FAILED",
                result, psramFound() ? "ready" : "missing");
  return result == ESP_OK;
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  IPAddress local(192, 168, 4, 2), gateway(192, 168, 4, 1), subnet(255, 255, 255, 0);
  WiFi.config(local, gateway, subnet);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Connecting to wrist Wi-Fi...");
}

const char *stateName() {
  if (matchState == STATE_ACTIVE) return "ACTIVE";
  if (matchState == STATE_PAUSED) return "PAUSED";
  if (matchState == STATE_STOPPED) return "STOPPED";
  if (matchState == STATE_READY) return "READY";
  return "OFFLINE";
}

void applyState(MatchState next) {
  if (next == matchState) return;
  MatchState previous = matchState;
  matchState = next;
  Serial.printf("Match state: %s\n", stateName());
  if (next == STATE_ACTIVE) openSession();
  if ((next == STATE_READY || next == STATE_STOPPED || next == STATE_UNKNOWN) &&
      previous != STATE_UNKNOWN) closeSession();
  if (next == STATE_PAUSED) patchAviHeader();
}

void pollWristState() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setConnectTimeout(350);
  http.setTimeout(500);
  if (!http.begin(WRIST_STATUS_URL)) return;
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String body = http.getString();
    lastStateSeen = millis();
    if (body.indexOf("\"state\":\"ACTIVE\"") >= 0) applyState(STATE_ACTIVE);
    else if (body.indexOf("\"state\":\"PAUSED\"") >= 0) applyState(STATE_PAUSED);
    else if (body.indexOf("\"state\":\"STOPPED\"") >= 0) applyState(STATE_STOPPED);
    else if (body.indexOf("\"state\":\"READY\"") >= 0) applyState(STATE_READY);
  }
  http.end();
}

void recordFrame() {
  if (!cameraReady || matchState != STATE_ACTIVE || !sessionOpen) return;
  uint32_t now = millis();
  if (now - lastRecordFrame < FRAME_INTERVAL_MS) return;
  lastRecordFrame = now;
  if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(120)) != pdTRUE) return;
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame) {
    writeFourCC(sessionFile, "00dc");
    writeU32(sessionFile, frame->len);
    size_t written = sessionFile.write(frame->buf, frame->len);
    if (frame->len & 1u) sessionFile.write((uint8_t)0);
    if (written == frame->len) { ++sessionFrames; sessionBytes += frame->len; }
    else { Serial.println("microSD write failed; closing recording"); sdReady = false; }
    esp_camera_fb_return(frame);
  }
  xSemaphoreGive(cameraMutex);
  if (!sdReady) closeSession();
  else if (now - lastHeaderUpdate >= 5000) patchAviHeader();
}

static esp_err_t streamHandler(httpd_req_t *request) {
  httpd_resp_set_type(request, STREAM_TYPE);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  esp_err_t result = ESP_OK;
  while (result == ESP_OK) {
    if (matchState != STATE_ACTIVE || !cameraReady) { delay(100); continue; }
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(500)) != pdTRUE) continue;
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) { xSemaphoreGive(cameraMutex); delay(20); continue; }
    result = httpd_resp_send_chunk(request, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (result == ESP_OK) {
      char header[72];
      size_t length = snprintf(header, sizeof(header), STREAM_PART, frame->len);
      result = httpd_resp_send_chunk(request, header, length);
    }
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, (const char *)frame->buf, frame->len);
    esp_camera_fb_return(frame);
    xSemaphoreGive(cameraMutex);
    delay(40);
  }
  return result;
}

static esp_err_t statusHandler(httpd_req_t *request) {
  char json[240];
  snprintf(json, sizeof(json),
           "{\"state\":\"%s\",\"streaming\":%s,\"recording\":%s,\"sdReady\":%s,\"session\":\"%s\",\"frames\":%lu}",
           stateName(), matchState == STATE_ACTIVE ? "true" : "false",
           sessionOpen && matchState == STATE_ACTIVE ? "true" : "false",
           sdReady ? "true" : "false", sessionPath.c_str(), (unsigned long)sessionFrames);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, json);
}

static esp_err_t indexHandler(httpd_req_t *request) {
  const char page[] = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Goal Post Camera</title><style>body{font-family:Arial;background:#101410;color:white;text-align:center;padding:16px}h1{color:#65d46e}img{width:100%;max-width:800px;background:#000;border-radius:12px}.card{max-width:820px;margin:auto;background:#1c251d;padding:14px;border-radius:12px}</style></head><body><div class="card"><h1>Goal Post Camera</h1><p id="state">Loading...</p><img id="video" src="http://192.168.4.2:81/stream"><p><a href="/sessions" style="color:#8ee897">Recorded sessions</a></p></div><script>setInterval(async()=>{try{let s=await(await fetch('/status')).json();state.textContent=s.state+' | '+(s.recording?'RECORDING':s.sdReady?'SD READY':'NO SD')}catch(e){state.textContent='OFFLINE'}},1000)</script></body></html>)HTML";
  httpd_resp_set_type(request, "text/html");
  return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t sessionsHandler(httpd_req_t *request) {
  if (!sdReady) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, "microSD unavailable");
  }
  String html = "<!doctype html><html><body><h1>Recorded sessions</h1><ul>";
  File directory = SD_MMC.open("/sessions");
  if (directory) {
    File file = directory.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String name = String(file.name());
        if (!name.startsWith("/")) name = "/sessions/" + name;
        html += "<li><a href='/file" + name + "'>" + name + "</a> (" + String(file.size() / 1024) + " KB)</li>";
      }
      file = directory.openNextFile();
    }
  }
  html += "</ul><p><a href='/'>Camera</a></p></body></html>";
  httpd_resp_set_type(request, "text/html");
  return httpd_resp_send(request, html.c_str(), html.length());
}

static esp_err_t fileHandler(httpd_req_t *request) {
  if (matchState == STATE_ACTIVE) {
    httpd_resp_set_status(request, "409 Conflict");
    return httpd_resp_sendstr(request, "Download after the match ends");
  }
  String path = String(request->uri).substring(5);
  if (!path.startsWith("/sessions/") || path.indexOf("..") >= 0) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
  File file = SD_MMC.open(path);
  if (!file || file.isDirectory()) return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
  httpd_resp_set_type(request, "video/x-msvideo");
  httpd_resp_set_hdr(request, "Content-Disposition", "attachment");
  uint8_t buffer[4096];
  while (file.available()) {
    size_t count = file.read(buffer, sizeof(buffer));
    if (httpd_resp_send_chunk(request, (const char *)buffer, count) != ESP_OK) break;
  }
  file.close();
  return httpd_resp_send_chunk(request, nullptr, 0);
}

void startServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80; config.ctrl_port = 32760;
  config.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(&pageServer, &config) == ESP_OK) {
    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = nullptr};
    httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = nullptr};
    httpd_uri_t sessions = {.uri = "/sessions", .method = HTTP_GET, .handler = sessionsHandler, .user_ctx = nullptr};
    httpd_uri_t file = {.uri = "/file/*", .method = HTTP_GET, .handler = fileHandler, .user_ctx = nullptr};
    httpd_register_uri_handler(pageServer, &index);
    httpd_register_uri_handler(pageServer, &status);
    httpd_register_uri_handler(pageServer, &sessions);
    httpd_register_uri_handler(pageServer, &file);
  }
  config.server_port = 81; config.ctrl_port = 32761; config.stack_size = 8192;
  if (httpd_start(&streamServer, &config) == ESP_OK) {
    httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = nullptr};
    httpd_register_uri_handler(streamServer, &stream);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  cameraMutex = xSemaphoreCreateMutex();
  connectWifi();
  unsigned long waitStarted = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - waitStarted < 12000) delay(100);
  if (WiFi.status() == WL_CONNECTED) Serial.println("Wi-Fi ready: http://192.168.4.2");
  else Serial.println("Wrist Wi-Fi not found yet; reconnecting in background");
  cameraReady = initCamera();
  mountSd();
  startServers();
  Serial.println("Session camera ready");
}

void loop() {
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastStatePoll >= 8000) { lastStatePoll = now; connectWifi(); }
  } else if (now - lastStatePoll >= STATE_POLL_MS) {
    lastStatePoll = now;
    pollWristState();
    now = millis();
  }
  if (lastStateSeen && (int32_t)(now - lastStateSeen) > (int32_t)STATE_TIMEOUT_MS) applyState(STATE_UNKNOWN);
  if (!sdReady && now - lastSdAttempt >= SD_RETRY_MS) mountSd();
  if (matchState == STATE_ACTIVE && !sessionOpen && sdReady) openSession();
  recordFrame();
  delay(2);
}
