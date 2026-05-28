#include <Arduino.h>

// 왼쪽, 가운데, 오른쪽 마이크의 AO 핀을 연결한 ESP32 GPIO 번호이다.
// 보드에는 D34, D35, D32처럼 적혀 있을 수 있다.
const int MIC_LEFT_PIN = 34;
const int MIC_CENTER_PIN = 35;
const int MIC_RIGHT_PIN = 32;

// 각 마이크의 조용한 상태 기준값을 저장하는 변수이다.
// 마이크 모듈마다 출력 기준 전압이 다를 수 있으므로 각각 따로 보정한다.
int centerLeft = 0;
int centerCenter = 0;
int centerRight = 0;

// 이 값보다 소리 변화량이 작으면 방향을 판단하지 않고 QUIET으로 처리한다.
// 주변 환경에 따라 30~200 정도로 조절하면 된다.
const int SOUND_THRESHOLD = 5;

// 가장 큰 마이크와 두 번째로 큰 마이크의 차이가 너무 작으면 방향 판단이 애매하다.
// 예를 들어 왼쪽과 오른쪽 값이 비슷하면 특정 방향이라고 말하기 어렵다.
const int DIRECTION_MARGIN = 2;

// ADC 값을 여러 번 읽어서 평균을 내는 함수이다.
// 한 번만 읽으면 노이즈가 많기 때문에 여러 번 읽고 평균을 사용한다.
int readAverageAnalog(int pin, int count) {
  long sum = 0;

  for (int i = 0; i < count; i++) {
    sum += analogRead(pin);
    delayMicroseconds(300);
  }

  return sum / count;
}

// 특정 마이크 핀의 조용한 상태 기준값을 측정하는 함수이다.
// 전원이 켜진 직후 2초 정도 조용히 있어야 기준값이 안정적으로 잡힌다.
int calibrateMic(int pin) {
  long sum = 0;
  const int samples = 200;

  for (int i = 0; i < samples; i++) {
    sum += readAverageAnalog(pin, 5);
    delay(5);
  }

  return sum / samples;
}

// 세 개 값 중 가장 큰 값의 방향을 문자열로 반환하는 함수이다.
// 단, 전체 소리 크기가 너무 작으면 QUIET으로 판단한다.
// 가장 큰 값과 두 번째 값의 차이가 작으면 UNKNOWN으로 판단한다.
String estimateDirection(int leftLevel, int centerLevel, int rightLevel) {
  int maxLevel = leftLevel;
  int secondLevel = centerLevel;
  String direction = "LEFT";

  if (centerLevel > maxLevel) {
    secondLevel = maxLevel;
    maxLevel = centerLevel;
    direction = "CENTER";
  } else {
    secondLevel = centerLevel;
  }

  if (rightLevel > maxLevel) {
    secondLevel = maxLevel;
    maxLevel = rightLevel;
    direction = "RIGHT";
  } else if (rightLevel > secondLevel) {
    secondLevel = rightLevel;
  }

  // 가장 큰 소리 변화량이 기준보다 작으면 조용한 상태로 판단한다.
  if (maxLevel < SOUND_THRESHOLD) {
    return "QUIET";
  }

  // 가장 큰 값과 두 번째 값의 차이가 작으면 방향이 애매한 상태로 판단한다.
  if ((maxLevel - secondLevel) < DIRECTION_MARGIN) {
    return "UNKNOWN";
  }

  return direction;
}

void setup() {
  // 시리얼 모니터 통신 속도이다.
  // platformio.ini의 monitor_speed도 115200으로 맞춰야 한다.
  Serial.begin(115200);
  delay(1000);

  // ESP32 ADC 해상도를 12비트로 설정한다.
  // 그러면 analogRead 값이 대략 0~4095 범위로 나온다.
  analogReadResolution(12);

  // ADC 입력 범위를 3.3V 근처까지 읽을 수 있도록 설정한다.
  // 마이크 AO 출력이 어느 정도 넓게 움직일 수 있으므로 ADC_11db를 사용한다.
  analogSetPinAttenuation(MIC_LEFT_PIN, ADC_11db);
  analogSetPinAttenuation(MIC_CENTER_PIN, ADC_11db);
  analogSetPinAttenuation(MIC_RIGHT_PIN, ADC_11db);

  Serial.println();
  Serial.println("3 microphone direction prototype start");
  Serial.println("Please stay quiet for calibration.");

  // 각 마이크마다 조용한 상태의 기준값을 따로 측정한다.
  centerLeft = calibrateMic(MIC_LEFT_PIN);
  centerCenter = calibrateMic(MIC_CENTER_PIN);
  centerRight = calibrateMic(MIC_RIGHT_PIN);

  Serial.println("Calibration complete.");
  Serial.print("centerLeft = ");
  Serial.println(centerLeft);

  Serial.print("centerCenter = ");
  Serial.println(centerCenter);

  Serial.print("centerRight = ");
  Serial.println(centerRight);

  Serial.println("Make sound near LEFT, CENTER, or RIGHT microphone.");
}

void loop() {
  // 각 마이크의 현재 아날로그 값을 읽는다.
  int rawLeft = readAverageAnalog(MIC_LEFT_PIN, 10);
  int rawCenter = readAverageAnalog(MIC_CENTER_PIN, 10);
  int rawRight = readAverageAnalog(MIC_RIGHT_PIN, 10);

  // 기준값과 현재값의 차이를 소리 크기 변화량으로 사용한다.
  // 실제 dB 값은 아니고, 단순한 상대적 크기이다.
  int levelLeft = abs(rawLeft - centerLeft);
  int levelCenter = abs(rawCenter - centerCenter);
  int levelRight = abs(rawRight - centerRight);

  // 세 마이크의 변화량을 비교해서 방향을 추정한다.
  String direction = estimateDirection(levelLeft, levelCenter, levelRight);

  // 시리얼 모니터에 원본값과 변화량, 추정 방향을 출력한다.
  Serial.print("raw L=");
  Serial.print(rawLeft);

  Serial.print(" C=");
  Serial.print(rawCenter);

  Serial.print(" R=");
  Serial.print(rawRight);

  Serial.print(" | level L=");
  Serial.print(levelLeft);

  Serial.print(" C=");
  Serial.print(levelCenter);

  Serial.print(" R=");
  Serial.print(levelRight);

  Serial.print(" | direction=");
  Serial.println(direction);

  delay(100);
}
