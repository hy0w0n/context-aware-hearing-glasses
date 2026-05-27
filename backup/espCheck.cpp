#include <Arduino.h>

// ESP32-S3가 켜진 뒤 한 번만 실행됩니다.
void setup() {
  // 시리얼 통신을 시작합니다.
  Serial.begin(115200);

  // 시리얼 모니터를 열 시간을 벌기 위해 5초 기다립니다.
  delay(5000);

  // 시작 메시지를 출력합니다.
  Serial.println("====================================");
  Serial.println("ESP32-S3 setup started");
  Serial.println("====================================");
}

// 계속 반복 실행됩니다.
void loop() {
  // 동작 확인 메시지입니다.
  Serial.println("ESP32-S3 is running...");

  // 1초 대기합니다.
  delay(1000);
}
