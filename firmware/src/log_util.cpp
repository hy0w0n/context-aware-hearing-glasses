#include "log_util.h"

DualSerial Log;

void log_begin() {
  Serial.begin(115200);   // 네이티브 USB CDC
  Serial0.begin(115200);  // UART0 (GPIO43/44 → CH343 브리지 포트)
}

int log_read() {
  if (Serial.available()) return Serial.read();
  if (Serial0.available()) return Serial0.read();
  return -1;
}
