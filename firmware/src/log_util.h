// log_util — 로그/콘솔을 네이티브 USB(CDC)와 UART0(CH343 브리지) 양쪽에 미러링.
// DevKitC-1은 USB 포트가 2개라 어느 쪽에 꽂아도 로그가 보이고 명령이 먹게 한다 (2026-08-11 혼선 재발 방지).
#pragma once
#include <Arduino.h>

class DualSerial : public Print {
 public:
  size_t write(uint8_t c) override {
    Serial.write(c);
    Serial0.write(c);
    return 1;
  }
  size_t write(const uint8_t *buf, size_t n) override {
    Serial.write(buf, n);
    Serial0.write(buf, n);
    return n;
  }
};

extern DualSerial Log;

void log_begin();          // Serial(CDC) + Serial0(UART0) 초기화
int  log_read();           // 두 포트 중 먼저 온 입력 1바이트 (-1 = 없음)
