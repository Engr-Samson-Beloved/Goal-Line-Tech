// ============================================================
// SMART GOAL-LINE DETECTION SYSTEM
// ESP32-CAM Firmware — AI-Thinker Board
// Listens on UART for CAPTURE command, saves JPEG to SD
//
// Wiring note: GPIO1/3 (this board's only exposed UART) is used both
// to flash this sketch (via a USB-to-TTL adapter) and, afterwards, to
// receive "CAPTURE" from the Goal Post Controller's UART2. Disconnect
// the USB-TTL adapter before wiring GPIO1/3 to the controller — do not
// have both attached at once.
// ============================================================

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_http_server.h>
#include "wifi_credentials.h"

// AI-Thinker ESP32-CAM pin definition
#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      21
#define Y4_GPIO_NUM      19
#define Y3_GPIO_NUM      18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

int imageCount = 0;

// ── LIVE MONITOR / STREAM WEB SERVER ──
// Joins the phone hotspot in wifi_credentials.h. Page + snapshot serve on
// port 80; the MJPEG stream runs on its own server on port 81 (its handler
// loops forever per client, so it can't share a server with other routes).
httpd_handle_t pageServer   = NULL;
httpd_handle_t streamServer = NULL;

#define PART_BOUNDARY "goallinecamboundary"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;   // 640x480
  config.jpeg_quality = 12;
  config.fb_count     = 1;
  esp_camera_init(&config);
}

void captureAndSave() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed.");
    return;
  }
  String path = "/goal_" + String(millis()) + ".jpg";
  fs::FS &fs = SD_MMC;
  File file = fs.open(path.c_str(), FILE_WRITE);
  if (file) {
    file.write(fb->buf, fb->len);
    file.close();
    Serial.print("Image saved: ");
    Serial.println(path);
  }
  esp_camera_fb_return(fb);
}

void connectWifi() {
  Serial.print("Connecting to WiFi \"");
  Serial.print(WIFI_SSID);
  Serial.println("\" ...");
  WiFi.mode(WIFI_STA);
  // This board's TX current spikes at default (~19.5dBm) power are a known
  // trigger for association failures when the CH340 programmer base's
  // regulator can't keep up — dropping TX power trades a little range for
  // a much better shot at actually associating.
  WiFi.setTxPower(WIFI_POWER_11dBm);
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
    if (MDNS.begin("goalpostcam")) {
      Serial.println("mDNS responder started: http://goalpostcam.local");
    }
  } else {
    Serial.println("WiFi connect failed - will keep retrying in the background.");
  }
}

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
      if (res == ESP_OK) {
        char header[64];
        size_t hlen = snprintf(header, sizeof(header), STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, header, hlen);
      }
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
      }
      esp_camera_fb_return(fb);
    }
    if (res != ESP_OK) break;
  }
  return res;
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  String ip = WiFi.localIP().toString();
  String html = "<!DOCTYPE html><html><head><title>Goal Post Cam</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:20px}"
    "h1{color:#4caf50}img{max-width:100%;border-radius:8px}</style></head>"
    "<body><h1>Goal Post Cam - Live</h1>"
    "<img src='http://" + ip + ":81/stream'></body></html>";
  return httpd_resp_send(req, html.c_str(), html.length());
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port    = 32760;

  httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
  if (httpd_start(&pageServer, &config) == ESP_OK) {
    httpd_register_uri_handler(pageServer, &index_uri);
  }

  config.server_port = 81;
  config.ctrl_port    = 32761;
  config.stack_size   = 8192;
  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
  if (httpd_start(&streamServer, &config) == ESP_OK) {
    httpd_register_uri_handler(streamServer, &stream_uri);
  }
}

void setup() {
  Serial.begin(115200);   // UART0 — debug AND link to controller's UART2

  // WiFi joins BEFORE the camera starts clocking: the camera's 20MHz XCLK
  // sits close to the 2.4GHz WiFi antenna on this board and its harmonics
  // are a well-known cause of association failures if it's already running
  // when WiFi.begin() is called.
  connectWifi();
  initCamera();
  SD_MMC.begin();
  startCameraServer();
  Serial.println("ESP32-CAM ready. Waiting for CAPTURE command.");
}

void loop() {
  static unsigned long lastReconnectAttempt = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastReconnectAttempt > 8000) {
    lastReconnectAttempt = millis();
    // A plain WiFi.reconnect() after a failed/stuck association just errors
    // forever ("sta is connecting, return error") instead of recovering —
    // a full disconnect + begin actually resets the driver's state.
    WiFi.disconnect();
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "CAPTURE") {
      Serial.println("Capturing...");
      captureAndSave();
    }
  }
}
