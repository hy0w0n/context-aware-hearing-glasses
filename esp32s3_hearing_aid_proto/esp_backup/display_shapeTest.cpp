#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED 해상도 설정이다.
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// ESP32 I2C 핀 설정이다.
#define OLED_SDA 21
#define OLED_SCL 22

// OLED 리셋 핀이 없으므로 -1로 설정한다.
#define OLED_RESET -1

// OLED 제어 객체를 생성한다.
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void setup() {
  Serial.begin(115200);
  delay(1000);

  // I2C 통신을 GPIO21, GPIO22로 시작한다.
  Wire.begin(OLED_SDA, OLED_SCL);

  // OLED 초기화, 주소는 대부분 0x3C이다.
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
    while (true) {
      delay(1000);
    }
  }

  // 화면을 지운다.
  display.clearDisplay();

  // 흰색 선으로 사각형을 그린다.
  display.drawRect(5, 5, 40, 30, SSD1306_WHITE);

  // 흰색 선으로 원을 그린다.
  display.drawCircle(80, 20, 15, SSD1306_WHITE);

  // 대각선을 그린다.
  display.drawLine(0, 63, 127, 0, SSD1306_WHITE);

  // 작은 글자를 출력한다.
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(5, 50);
  display.println("Shape Test");

  // 그린 내용을 실제 화면에 표시한다.
  display.display();
}

void loop() {
  // 고정 그림만 출력하므로 반복 동작은 없다.
}
