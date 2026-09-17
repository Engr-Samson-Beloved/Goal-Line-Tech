// ═══════════════════════════════════════════════════════════════════
// SMART GOAL-LINE DETECTION SYSTEM
// ESP32-CAM Firmware — AI-Thinker Board
// Listens on UART for CAPTURE command, saves JPEG to SD
// ═══════════════════════════════════════════════════════════════════

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"

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

void setup() {
  Serial.begin(115200);   // UART0 — debug
  // UART1 receives commands from ESP32 Controller
  // Uses default RX=GPIO3, TX=GPIO1 on AI-Thinker
  initCamera();
  SD_MMC.begin();
  Serial.println("ESP32-CAM ready. Waiting for CAPTURE command.");
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil("\n");
    cmd.trim();
    if (cmd == "CAPTURE") {
      Serial.println("Capturing...");
      captureAndSave();
    }
  }
}
