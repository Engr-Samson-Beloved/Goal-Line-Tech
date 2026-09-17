// TEMPORARY bring-up test only — not part of the final firmware.
// Captures one frame and streams it back over USB serial as base64,
// so a photo can be verified with no microSD card inserted.
// Send the single word CAPTURE (newline-terminated) to trigger it.

#include "esp_camera.h"

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

static const char B64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void printBase64(const uint8_t *data, size_t len) {
  size_t i = 0;
  char out[4];
  while (i + 3 <= len) {
    out[0] = B64_TABLE[(data[i] >> 2) & 0x3F];
    out[1] = B64_TABLE[((data[i] & 0x3) << 4) | ((data[i + 1] & 0xF0) >> 4)];
    out[2] = B64_TABLE[((data[i + 1] & 0xF) << 2) | ((data[i + 2] & 0xC0) >> 6)];
    out[3] = B64_TABLE[data[i + 2] & 0x3F];
    Serial.write((uint8_t*)out, 4);
    i += 3;
  }
  size_t rem = len - i;
  if (rem == 1) {
    out[0] = B64_TABLE[(data[i] >> 2) & 0x3F];
    out[1] = B64_TABLE[(data[i] & 0x3) << 4];
    out[2] = '=';
    out[3] = '=';
    Serial.write((uint8_t*)out, 4);
  } else if (rem == 2) {
    out[0] = B64_TABLE[(data[i] >> 2) & 0x3F];
    out[1] = B64_TABLE[((data[i] & 0x3) << 4) | ((data[i + 1] & 0xF0) >> 4)];
    out[2] = B64_TABLE[(data[i + 1] & 0xF) << 2];
    out[3] = '=';
    Serial.write((uint8_t*)out, 4);
  }
}

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
  config.frame_size   = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count     = 1;
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("CAMERA INIT FAILED: 0x%x\n", err);
  } else {
    Serial.println("Camera init OK.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  initCamera();
  Serial.println("Photo test ready. Send CAPTURE.");
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "CAPTURE") {
      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) {
        Serial.println("Camera capture failed.");
        return;
      }
      Serial.print("BEGIN_JPEG ");
      Serial.println(fb->len);
      printBase64(fb->buf, fb->len);
      Serial.println();
      Serial.println("END_JPEG");
      esp_camera_fb_return(fb);
    }
  }
}
