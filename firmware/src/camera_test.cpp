#include "camera_test.h"
#include <Arduino.h>
#include <Preferences.h>
#include "esp_camera.h"
#include "config.h"
#include "log_util.h"

static bool s_inited = false;
static bool s_qvga = false;   // NVS 유지 — 혼잡 환경 속도 개선 A/B (proto.md §7.5)
static int s_quality = 12;
static bool s_gray = false;   // 모노크롬 전송 — 이미지 모델이 흑백 학습(2026-10-01 확정), 색 성분 제거로 추가 절감
static char s_presetStr[32] = "VGA q12";

static void presetStr() {
  snprintf(s_presetStr, sizeof(s_presetStr), "%s q%d%s",
           s_qvga ? "QVGA" : "VGA", s_quality, s_gray ? " gray" : "");
}

static void presetLoad() {
  Preferences p;
  p.begin("cfg", true);
  s_qvga = p.getBool("imgqvga", false);
  s_quality = (int)p.getInt("imgq", 12);
  s_gray = p.getBool("imggray", false);
  p.end();
  presetStr();
}

static void presetApply() {  // init 이후 런타임 변경 (esp32-camera 센서 API)
  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr) return;
  s->set_framesize(s, s_qvga ? FRAMESIZE_QVGA : FRAMESIZE_VGA);
  s->set_quality(s, s_quality);
  s->set_special_effect(s, s_gray ? 2 : 0);   // 2 = OV2640 grayscale 효과 (JPEG 크로마 제거)
  presetStr();
  Log.printf("[cam] 프리셋: %s (NVS 유지)\n", s_presetStr);
}

bool camera_ensure_init() {
  if (!s_inited) {
    presetLoad();
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
    c.frame_size   = s_qvga ? FRAMESIZE_QVGA : FRAMESIZE_VGA;  // 프리셋 (NVS, §7.5 속도 A/B)
    c.jpeg_quality = s_quality;
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
    Log.printf("[cam] init OK (센서 PID=0x%x — OV2640=0x26) 프리셋=%s\n",
               s ? s->id.PID : 0, s_presetStr);
    if (s_gray) presetApply();   // 모노크롬은 config 구조체에 없어 init 후 센서 API로 적용
  }
  return true;
}

void camera_set_gray(bool gray) {
  s_gray = gray;
  Preferences p;
  p.begin("cfg", false);
  p.putBool("imggray", gray);
  p.end();
  if (s_inited) presetApply();
  else { presetStr(); Log.printf("[cam] 프리셋 저장: %s (다음 init 적용)\n", s_presetStr); }
}

void camera_set_qvga(bool qvga) {
  s_qvga = qvga;
  Preferences p;
  p.begin("cfg", false);
  p.putBool("imgqvga", qvga);
  p.end();
  if (s_inited) presetApply();
  else Log.printf("[cam] 프리셋 저장: %s (다음 init 적용)\n", qvga ? "QVGA" : "VGA");
}

void camera_set_quality(int q) {
  if (q < 10) q = 10;
  if (q > 40) q = 40;
  s_quality = q;
  Preferences p;
  p.begin("cfg", false);
  p.putInt("imgq", q);
  p.end();
  if (s_inited) presetApply();
  else Log.printf("[cam] 품질 저장: q%d (다음 init 적용)\n", q);
}

const char *camera_preset_str() { return s_presetStr; }

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
