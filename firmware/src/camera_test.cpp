#include "camera_test.h"
#include <Arduino.h>
#include "esp_camera.h"
#include "config.h"
#include "log_util.h"

static bool s_inited = false;

bool camera_ensure_init() {
  if (!s_inited) {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0 = CAM_PIN_D0;   // Y2
    c.pin_d1 = CAM_PIN_D1;
    c.pin_d2 = CAM_PIN_D2;
    c.pin_d3 = CAM_PIN_D3;
    c.pin_d4 = CAM_PIN_D4;
    c.pin_d5 = CAM_PIN_D5;
    c.pin_d6 = CAM_PIN_D6;
    c.pin_d7 = CAM_PIN_D7;   // Y9
    c.pin_xclk  = CAM_PIN_XCLK;
    c.pin_pclk  = CAM_PIN_PCLK;
    c.pin_vsync = CAM_PIN_VSYNC;
    c.pin_href  = CAM_PIN_HREF;
    c.pin_sccb_sda = CAM_PIN_SIOD;
    c.pin_sccb_scl = CAM_PIN_SIOC;
    c.pin_pwdn  = CAM_PIN_PWDN;
    c.pin_reset = CAM_PIN_RESET;
    c.xclk_freq_hz = 20000000;                 // 실패 시 10~16MHz로 낮춰볼 것 (CLAUDE.md §4.3)
    c.pixel_format = PIXFORMAT_JPEG;           // 센서 하드웨어 JPEG (CLAUDE.md §3)
    c.frame_size   = FRAMESIZE_VGA;            // 640x480 — 앱 224x224 변환에 충분
    c.jpeg_quality = 12;
    c.fb_count     = 2;
    c.fb_location  = CAMERA_FB_IN_PSRAM;
    c.grab_mode    = CAMERA_GRAB_LATEST;

    const esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
      Log.printf("[cam] init 실패: 0x%x (0x105=센서 미감지 → FPC 접촉/핀맵 확인)\n", err);
      return false;
    }
    s_inited = true;
    sensor_t *s = esp_camera_sensor_get();
    Log.printf("[cam] init OK (센서 PID=0x%x — OV2640=0x26)\n", s ? s->id.PID : 0);
  }
  return true;
}

bool camera_test_capture() {
  if (!camera_ensure_init()) return false;

  const uint32_t t0 = millis();
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Log.println("[cam] 캡처 실패 (fb=null)");
    return false;
  }
  Log.printf("[cam] 캡처 OK: %ux%u JPEG %uKB, %lums | heap=%uKB psram=%uKB\n",
             fb->width, fb->height, (unsigned)(fb->len / 1024),
             (unsigned long)(millis() - t0),
             (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  esp_camera_fb_return(fb);
  return true;
}
