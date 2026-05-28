#include <Arduino.h>

// 마이크의 AO, Analog Output 핀을 ESP32의 GPIO34에 연결했다고 가정한다.
// 보드에는 보통 D34라고 적혀 있다.
// GPIO34는 입력 전용 핀이지만 ADC 입력으로 쓰기 좋다.
const int MIC_AO_PIN = 34;

// 마이크의 DO, Digital Output 핀을 ESP32의 GPIO27에 연결했다고 가정한다.
// DO를 연결하지 않았다면 이 값은 의미 없을 수 있다.
const int MIC_DO_PIN = 27;

// 조용한 상태에서의 기준값을 저장할 변수이다.
// 마이크 AO는 완전히 0이 아니라 보통 중간 전압 근처에서 흔들리는 경우가 많다.
int centerValue = 0;

// 마이크 아날로그 값을 여러 번 읽어서 평균을 내는 함수이다.
// 한 번만 읽으면 노이즈 때문에 값이 튈 수 있어서 평균을 사용한다.
int readMicAverage(int count) {
  long sum = 0;

  for (int i = 0; i < count; i++) {
    // analogRead는 ESP32의 ADC 값을 읽는다.
    // 12비트 해상도에서는 보통 0~4095 범위의 값이 나온다.
    sum += analogRead(MIC_AO_PIN);

    // 너무 빠르게 연속으로 읽지 않도록 아주 짧게 기다린다.
    delayMicroseconds(300);
  }

  // 읽은 값들의 평균을 반환한다.
  return sum / count;
}

void setup() {
  // PC와 ESP32가 시리얼 통신을 시작한다.
  // PlatformIO의 monitor_speed도 115200이어야 한다.
  Serial.begin(115200);
  delay(1000);

  // DO 핀을 디지털 입력으로 설정한다.
  // DO를 연결하지 않았다면 출력값이 불안정할 수 있다.
  pinMode(MIC_DO_PIN, INPUT);

  // ESP32 ADC 해상도를 12비트로 설정한다.
  // 12비트이면 analogRead 값이 0~4095 범위로 나온다.
  analogReadResolution(12);

  // ADC 입력 범위를 넓게 설정한다.
  // ADC_11db는 대략 3.3V 근처까지 읽기 위해 자주 사용한다.
  analogSetPinAttenuation(MIC_AO_PIN, ADC_11db);

  Serial.println();
  Serial.println("ESP32 microphone AO test start");
  Serial.println("Calibrating background level...");
  Serial.println("Please stay quiet for 2 seconds.");

  // 처음 2초 정도는 조용히 있다고 가정하고 기준값을 측정한다.
  long sum = 0;
  int samples = 200;

  for (int i = 0; i < samples; i++) {
    // 한 번에 5개씩 평균 낸 값을 다시 누적한다.
    sum += readMicAverage(5);
    delay(10);
  }

  // 조용한 상태의 평균값을 centerValue로 저장한다.
  centerValue = sum / samples;

  Serial.print("Calibration complete. centerValue = ");
  Serial.println(centerValue);
  Serial.println("Now make sound near the microphone.");
}

void loop() {
  // 현재 마이크 AO 값을 평균으로 읽는다.
  int rawValue = readMicAverage(10);

  // 기준값과 현재값의 차이를 소리 크기처럼 사용한다.
  // 실제 음압 dB는 아니고, 단순 변화량이다.
  int soundLevel = abs(rawValue - centerValue);

  // DO 핀의 HIGH/LOW 값을 읽는다.
  // 모듈의 가변저항 설정에 따라 값이 바뀐다.
  int digitalValue = digitalRead(MIC_DO_PIN);

  // 시리얼 모니터에 원본값, 기준값, 변화량, DO 값을 출력한다.
  Serial.print("raw=");
  Serial.print(rawValue);

  Serial.print("  center=");
  Serial.print(centerValue);

  Serial.print("  level=");
  Serial.print(soundLevel);

  Serial.print("  DO=");
  Serial.println(digitalValue);

  // 너무 빠르게 출력하면 보기 힘들기 때문에 0.1초마다 출력한다.
  delay(100);
}
