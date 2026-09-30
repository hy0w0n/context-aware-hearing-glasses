#include "ui_oled.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <math.h>
#include "config.h"
#include "log_util.h"

// HW I2C — 생성자에 (rotation, reset, SCL, SDA). ESP32에서 Wire.begin(SDA, SCL) 리매핑됨
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_oled(U8G2_R0, U8X8_PIN_NONE, I2C_PIN_SCL, I2C_PIN_SDA);

static bool s_ok = false;
static uint8_t s_mode = 0;
static int16_t s_vol = 0;
static bool s_btn = false;
static uint32_t s_alertUntil = 0;
static int16_t s_alertAz = 0;
static uint8_t s_alertIcon = 0;
static bool s_dirty = true;

static const char *MODES[5] = {"INDOOR", "OUTDOOR", "TV", "TALK 1:1", "GROUP"};
static const char *ICONS[5] = {"CAR", "HORN", "SIREN", "BIKE", "WARN"};

void ui_init() {
  s_ok = s_oled.begin();
  if (!s_ok) {
    Log.println("[oled] init 실패 — 배선(SDA=47, SCL=14)/주소(0x3C) 확인");
    return;
  }
  s_oled.setBusClock(400000);
  Log.println("[oled] SSD1306 128x64 init OK (SDA=47, SCL=14)");
  s_dirty = true;
  ui_tick();
}

void ui_set_mode(uint8_t m) {
  s_mode = (m > 4) ? 4 : m;
  s_dirty = true;
  Log.printf("[oled] 모드 = %s\n", MODES[s_mode]);
}

void ui_set_volume(int8_t d) {
  s_vol += d;
  if (s_vol > 20) s_vol = 20;
  if (s_vol < -20) s_vol = -20;
  s_dirty = true;
  Log.printf("[oled] 볼륨 %+d (누적 %+d)\n", d, s_vol);
}

void ui_alert(int16_t az, uint8_t icon, uint16_t durMs) {
  s_alertAz = az;
  s_alertIcon = (icon > 4) ? 4 : icon;
  s_alertUntil = millis() + (durMs ? durMs : 2000);
  s_dirty = true;
  Log.printf("[oled] ALERT az=%.1f° icon=%s dur=%ums\n", az / 10.0, ICONS[s_alertIcon], durMs);
}

void ui_btn(bool pressed) {
  if (pressed != s_btn) {
    s_btn = pressed;
    s_dirty = true;
  }
}

// 나침반 원(중심 96,32 r=26) 안에 방위각 화살표. az 0°=위(정면=mic3 방향), 시계방향+
static void drawArrow(int16_t azDeci) {
  const float th = azDeci / 10.0f * (float)M_PI / 180.0f;
  const float dx = sinf(th), dy = -cosf(th);
  const int cx = 96, cy = 32;
  const int tx = cx + (int)(dx * 22), ty = cy + (int)(dy * 22);
  const int bx = cx - (int)(dx * 14), by = cy - (int)(dy * 14);
  s_oled.drawLine(bx, by, tx, ty);
  const float a1 = th + 2.6f, a2 = th - 2.6f;
  s_oled.drawLine(tx, ty, tx + (int)(sinf(a1) * 8), ty - (int)(cosf(a1) * 8));
  s_oled.drawLine(tx, ty, tx + (int)(sinf(a2) * 8), ty - (int)(cosf(a2) * 8));
}

void ui_tick() {
  if (!s_ok) return;
  const bool alertActive = millis() < s_alertUntil;
  static bool wasAlert = false;
  if (alertActive != wasAlert) {
    wasAlert = alertActive;
    s_dirty = true;
  }
  if (!s_dirty) return;
  s_dirty = false;

  s_oled.clearBuffer();
  if (alertActive) {
    s_oled.setFont(u8g2_font_ncenB14_tr);
    s_oled.drawStr(0, 22, ICONS[s_alertIcon]);
    s_oled.setFont(u8g2_font_6x10_tr);
    char b[16];
    snprintf(b, sizeof(b), "%d deg", (int)(s_alertAz / 10));
    s_oled.drawStr(0, 40, b);
    s_oled.drawStr(0, 56, "<< ALERT >>");
    s_oled.drawCircle(96, 32, 26);
    drawArrow(s_alertAz);
  } else {
    s_oled.setFont(u8g2_font_ncenB14_tr);
    s_oled.drawStr(0, 20, MODES[s_mode]);
    s_oled.setFont(u8g2_font_6x10_tr);
    char b[24];
    snprintf(b, sizeof(b), "VOL %+d %s", s_vol, s_btn ? "[PTT]" : "");
    s_oled.drawStr(0, 40, b);
    s_oled.drawStr(0, 56, "SmartGlass-HA");
  }
  s_oled.sendBuffer();
}
