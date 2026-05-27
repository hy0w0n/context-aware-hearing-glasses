#include <Arduino.h>

#define MIC_PIN 4   // MAX4466 OUT 연결 핀

void setup()
{
    Serial.begin(115200);

    // ADC 해상도 설정 (ESP32 기본 12bit)
    analogReadResolution(12);

    // ADC attenuation 설정
    // 0~3.3V 범위 측정 가능하게 함
    analogSetPinAttenuation(MIC_PIN, ADC_11db);

    Serial.println("MAX4466 Microphone Test Start");
}

void loop()
{
    int micValue = analogRead(MIC_PIN);

    Serial.println(micValue);

    delay(10);
}