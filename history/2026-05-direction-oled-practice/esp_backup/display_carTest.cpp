#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED 화면 크기 설정이다.
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// ESP32 I2C 핀 설정이다.
#define OLED_SDA 21
#define OLED_SCL 22

// OLED 리셋 핀이 없으므로 -1로 설정한다.
#define OLED_RESET -1

// OLED 객체 생성이다.
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 자동차 모양을 간단한 선과 도형으로 그리는 함수이다.
void drawCarIcon() {
  // 화면을 먼저 지운다.
  display.clearDisplay();

  // 자동차 윗부분, 창문처럼 보이는 부분이다.
  display.drawRect(38, 20, 52, 16, SSD1306_WHITE);

  // 자동차 아랫부분, 차체이다.
  display.drawRect(25, 34, 78, 14, SSD1306_WHITE);

  // 왼쪽 바퀴이다.
  display.drawCircle(42, 50, 6, SSD1306_WHITE);

  // 오른쪽 바퀴이다.
  display.drawCircle(86, 50, 6, SSD1306_WHITE);

  // 경고 문구이다.
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(35, 5);
  display.println("CAR ALERT");

  // 실제 OLED 화면에 반영한다.
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // I2C 통신을 시작한다.
  Wire.begin(OLED_SDA, OLED_SCL);

  // OLED를 초기화한다.
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
    while (true) {
      delay(1000);
    }
  }

  // 자동차 아이콘을 화면에 그린다.
  drawCarIcon();
}

void loop() {
  // 이번 테스트는 자동차 그림을 고정 출력한다.
}
