#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED 화면의 가로, 세로 해상도이다.
// 가장 흔한 0.96인치 SSD1306 OLED는 보통 128x64 해상도이다.
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// ESP32에서 I2C 통신에 사용할 핀 번호이다.
// 보드에는 D21, D22라고 적혀 있을 수 있다.
#define OLED_SDA 21
#define OLED_SCL 22

// SSD1306 OLED의 리셋 핀 설정이다.
// 현재 사용하는 I2C OLED 모듈은 보통 별도 리셋 핀이 없으므로 -1로 둔다.
#define OLED_RESET -1

// OLED 객체를 생성한다.
// &Wire는 ESP32의 I2C 통신 객체를 사용한다는 뜻이다.
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void setup() {
  // 시리얼 모니터 출력용 통신을 시작한다.
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("OLED display test start");

  // ESP32의 I2C 통신을 시작한다.
  // SDA는 GPIO21, SCL은 GPIO22를 사용한다.
  Wire.begin(OLED_SDA, OLED_SCL);

  // OLED 디스플레이 초기화를 시도한다.
  // 0x3C는 SSD1306 OLED에서 가장 흔한 I2C 주소이다.
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation or connection failed");
    Serial.println("Check wiring: VCC, GND, SDA, SCL");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("OLED initialized successfully");

  // 화면에 남아 있을 수 있는 기존 내용을 지운다.
  display.clearDisplay();

  // 글자 색상을 흰색으로 설정한다.
  // OLED는 보통 검은 배경에 흰색 픽셀을 켜는 방식이다.
  display.setTextColor(SSD1306_WHITE);

  // 글자 크기를 설정한다.
  // 1은 작은 글자, 2는 중간 크기, 3은 큰 글자이다.
  display.setTextSize(2);

  // 글자가 시작될 위치를 설정한다.
  // 좌측 상단이 x=0, y=0이다.
  display.setCursor(0, 0);
  display.println("HELLO");

  display.setCursor(0, 22);
  display.println("ESP32");

  display.setTextSize(1);
  display.setCursor(0, 50);
  display.println("DISPLAY OK");

  // 지금까지 메모리에 그린 내용을 실제 OLED 화면에 반영한다.
  // 이 함수를 호출해야 화면에 보인다.
  display.display();
}

void loop() {
  // 이번 테스트에서는 화면에 고정된 글자만 보여주므로 loop에서 할 일은 없다.
}
