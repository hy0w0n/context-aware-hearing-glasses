#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED 화면 해상도 설정이다.
// 일반적인 0.96인치 SSD1306 OLED는 128x64 해상도를 많이 사용한다.
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// OLED I2C 핀 설정이다.
// ESP32 보드에는 D21, D22라고 적혀 있을 수 있다.
#define OLED_SDA 21
#define OLED_SCL 22

// OLED 리셋 핀이 없는 I2C 모듈이므로 -1로 설정한다.
#define OLED_RESET -1

// OLED 제어 객체를 생성한다.
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 왼쪽, 가운데, 오른쪽 마이크 AO 핀이 연결된 ESP32 GPIO 번호이다.
// 보드에는 각각 D34, D35, D32라고 적혀 있을 수 있다.
const int MIC_LEFT_PIN = 34;
const int MIC_CENTER_PIN = 35;
const int MIC_RIGHT_PIN = 32;

// 각 마이크의 조용한 상태 기준값이다.
// 마이크마다 기본 출력 전압과 감도가 다를 수 있으므로 각각 따로 보정한다.
int centerLeft = 0;
int centerCenter = 0;
int centerRight = 0;

// 이 값보다 소리 변화량이 작으면 조용한 상태로 판단한다.
// 마이크 감도가 낮다고 했으므로 처음에는 너무 높이지 않는 것이 좋다.
const int SOUND_THRESHOLD = 6;

// 가장 큰 마이크 값과 두 번째로 큰 마이크 값의 차이가 이 값보다 작으면 방향이 애매하다고 판단한다.
// 너무 자주 UNKNOWN이 나오면 값을 낮추고, 너무 쉽게 방향이 바뀌면 값을 높이면 된다.
const int DIRECTION_MARGIN = 2;

// OLED 화면을 너무 자주 갱신하면 깜빡임이 생길 수 있으므로 마지막 표시 상태를 기억한다.
String lastDirection = "";

// 마이크 ADC 값을 여러 번 읽어서 평균을 내는 함수이다.
// 한 번만 읽으면 노이즈가 섞일 수 있으므로 여러 번 읽고 평균을 사용한다.
int readAverageAnalog(int pin, int count) {
  long sum = 0;

  for (int i = 0; i < count; i++) {
    // ESP32 ADC에서 현재 아날로그 값을 읽는다.
    // 12비트 해상도 기준으로 대략 0~4095 범위가 나온다.
    sum += analogRead(pin);

    // 너무 빠른 연속 읽기를 피하기 위해 아주 짧게 기다린다.
    delayMicroseconds(300);
  }

  // 읽은 값의 평균을 반환한다.
  return sum / count;
}

// 특정 마이크의 조용한 상태 기준값을 측정하는 함수이다.
// 시작 직후 주변이 조용해야 기준값이 안정적으로 잡힌다.
int calibrateMic(int pin) {
  long sum = 0;
  const int samples = 200;

  for (int i = 0; i < samples; i++) {
    // 한 번에 5번 읽어서 평균 낸 값을 다시 누적한다.
    sum += readAverageAnalog(pin, 5);
    delay(5);
  }

  // 조용한 상태의 평균 ADC 값을 반환한다.
  return sum / samples;
}

// 세 마이크의 소리 변화량을 비교해서 방향을 추정하는 함수이다.
// 반환값은 LEFT, CENTER, RIGHT, QUIET, UNKNOWN 중 하나이다.
String estimateDirection(int leftLevel, int centerLevel, int rightLevel) {
  int maxLevel = leftLevel;
  int secondLevel = centerLevel;
  String direction = "LEFT";

  // 가운데 마이크가 현재 최대값보다 크면 CENTER를 1등으로 설정한다.
  if (centerLevel > maxLevel) {
    secondLevel = maxLevel;
    maxLevel = centerLevel;
    direction = "CENTER";
  } else {
    secondLevel = centerLevel;
  }

  // 오른쪽 마이크가 현재 최대값보다 크면 RIGHT를 1등으로 설정한다.
  if (rightLevel > maxLevel) {
    secondLevel = maxLevel;
    maxLevel = rightLevel;
    direction = "RIGHT";
  } else if (rightLevel > secondLevel) {
    secondLevel = rightLevel;
  }

  // 전체 소리 변화량이 작으면 조용한 상태로 판단한다.
  if (maxLevel < SOUND_THRESHOLD) {
    return "QUIET";
  }

  // 1등과 2등 차이가 작으면 어느 방향인지 애매하므로 UNKNOWN으로 판단한다.
  if ((maxLevel - secondLevel) < DIRECTION_MARGIN) {
    return "UNKNOWN";
  }

  return direction;
}

// 방향 문자열에 따라 OLED에 간단한 화살표 그림을 그리는 함수이다.
void drawDirectionIcon(String direction) {
  if (direction == "LEFT") {
    // 왼쪽 방향 화살표를 그린다.
    display.fillTriangle(20, 32, 45, 15, 45, 49, SSD1306_WHITE);
    display.fillRect(45, 25, 55, 14, SSD1306_WHITE);
  } else if (direction == "RIGHT") {
    // 오른쪽 방향 화살표를 그린다.
    display.fillTriangle(108, 32, 83, 15, 83, 49, SSD1306_WHITE);
    display.fillRect(28, 25, 55, 14, SSD1306_WHITE);
  } else if (direction == "CENTER") {
    // 가운데 방향을 표시하는 원형 아이콘을 그린다.
    display.drawCircle(64, 32, 20, SSD1306_WHITE);
    display.fillCircle(64, 32, 8, SSD1306_WHITE);
  } else if (direction == "QUIET") {
    // 조용한 상태를 표시하는 작은 점 세 개를 그린다.
    display.fillCircle(50, 32, 3, SSD1306_WHITE);
    display.fillCircle(64, 32, 3, SSD1306_WHITE);
    display.fillCircle(78, 32, 3, SSD1306_WHITE);
  } else {
    // UNKNOWN 상태를 물음표로 표시한다.
    display.setTextSize(4);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(52, 20);
    display.println("?");
  }
}

// OLED에 현재 방향과 각 마이크 레벨을 표시하는 함수이다.
void showDirectionOnDisplay(String direction, int leftLevel, int centerLevel, int rightLevel) {
  // 화면 전체를 지운다.
  display.clearDisplay();

  // 상단에 제목을 표시한다.
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Sound Direction");

  // 방향 텍스트를 크게 표시한다.
  display.setTextSize(2);

  if (direction == "CENTER") {
    display.setCursor(25, 14);
  } else if (direction == "UNKNOWN") {
    display.setCursor(18, 14);
  } else {
    display.setCursor(30, 14);
  }

  display.println(direction);

  // 방향에 맞는 간단한 아이콘을 그린다.
  drawDirectionIcon(direction);

  // 하단에는 각 마이크의 level 값을 작게 표시한다.
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("L:");
  display.print(leftLevel);
  display.print(" C:");
  display.print(centerLevel);
  display.print(" R:");
  display.print(rightLevel);

  // 메모리에 그린 내용을 실제 OLED 화면에 반영한다.
  display.display();
}

void setup() {
  // 시리얼 모니터 통신을 시작한다.
  Serial.begin(115200);
  delay(1000);

  // ESP32 ADC 해상도를 12비트로 설정한다.
  // analogRead 값이 대략 0~4095 범위로 나온다.
  analogReadResolution(12);

  // 각 ADC 핀의 입력 전압 범위를 넓게 설정한다.
  analogSetPinAttenuation(MIC_LEFT_PIN, ADC_11db);
  analogSetPinAttenuation(MIC_CENTER_PIN, ADC_11db);
  analogSetPinAttenuation(MIC_RIGHT_PIN, ADC_11db);

  // I2C 통신을 시작한다.
  Wire.begin(OLED_SDA, OLED_SCL);

  // OLED 초기화를 시도한다.
  // 대부분의 SSD1306 OLED 주소는 0x3C이다.
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed. Check VCC, GND, SDA, SCL.");
    while (true) {
      delay(1000);
    }
  }

  // 시작 화면을 표시한다.
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Mic Direction Demo");
  display.setCursor(0, 20);
  display.println("Calibrating...");
  display.setCursor(0, 40);
  display.println("Stay quiet");
  display.display();

  Serial.println();
  Serial.println("Mic direction display demo start");
  Serial.println("Please stay quiet for calibration.");

  // 각 마이크의 조용한 상태 기준값을 측정한다.
  centerLeft = calibrateMic(MIC_LEFT_PIN);
  centerCenter = calibrateMic(MIC_CENTER_PIN);
  centerRight = calibrateMic(MIC_RIGHT_PIN);

  Serial.println("Calibration complete.");
  Serial.print("centerLeft=");
  Serial.println(centerLeft);
  Serial.print("centerCenter=");
  Serial.println(centerCenter);
  Serial.print("centerRight=");
  Serial.println(centerRight);

  // 보정 완료 화면을 잠깐 보여준다.
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Calibration OK");
  display.setCursor(0, 20);
  display.println("Make sound near");
  display.setCursor(0, 35);
  display.println("LEFT / CENTER / RIGHT");
  display.display();

  delay(1500);
}

void loop() {
  // 각 마이크의 현재 아날로그 값을 평균으로 읽는다.
  int rawLeft = readAverageAnalog(MIC_LEFT_PIN, 10);
  int rawCenter = readAverageAnalog(MIC_CENTER_PIN, 10);
  int rawRight = readAverageAnalog(MIC_RIGHT_PIN, 10);

  // 현재값과 조용한 상태 기준값의 차이를 계산한다.
  int levelLeft = abs(rawLeft - centerLeft);
  int levelCenter = abs(rawCenter - centerCenter);
  int levelRight = abs(rawRight - centerRight);

  // 세 마이크의 레벨을 비교해서 방향을 추정한다.
  String direction = estimateDirection(levelLeft, levelCenter, levelRight);

  // 시리얼 모니터에도 상태를 출력한다.
  Serial.print("L=");
  Serial.print(levelLeft);
  Serial.print(" C=");
  Serial.print(levelCenter);
  Serial.print(" R=");
  Serial.print(levelRight);
  Serial.print(" direction=");
  Serial.println(direction);

  // 방향이 바뀌었거나, 레벨 표시를 갱신하기 위해 OLED를 업데이트한다.
  showDirectionOnDisplay(direction, levelLeft, levelCenter, levelRight);

  // 너무 빠르게 화면을 갱신하면 깜빡일 수 있으므로 약간 기다린다.
  delay(150);
}
