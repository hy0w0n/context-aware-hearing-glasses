// ui_oled — M5: SSD1306 0.96" 128x64 I2C OLED (SDA=47, SCL=14, 주소 0x3C)
// 대기 화면: 모드/볼륨/PTT 상태. DISPLAY_ALERT 수신 시: 방향 화살표 + 아이콘 (지속시간 후 자동 복귀)
#pragma once
#include <Arduino.h>

void ui_init();                                                   // I2C+OLED 초기화 (실패해도 안전)
void ui_set_mode(uint8_t mode);                                   // 0=실내 1=야외 2=TV 3=일대일 4=다인
void ui_set_volume(int8_t delta);                                 // 누적 표시 (-20~+20 클램프)
void ui_alert(int16_t azimuthDeciDeg, uint8_t icon, uint16_t durMs);  // §6.1 DISPLAY_ALERT
void ui_btn(bool pressed);                                        // PTT 표시
void ui_tick();                                                   // loop()에서 주기 호출 (렌더/알림 만료)
