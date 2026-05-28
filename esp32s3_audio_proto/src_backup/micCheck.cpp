#include <Arduino.h>

const int MIC_PIN = 4;
const int SAMPLE_COUNT = 200;

void setup() {
  Serial.begin(115200);

  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) {
    delay(10);
  }

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  Serial.println();
  Serial.println("MAX4466 ADC min/max test start");
  Serial.println("Speak or clap near the microphone.");
}

void loop() {
  int minValue = 4095;
  int maxValue = 0;
  long sum = 0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    int value = analogRead(MIC_PIN);

    if (value < minValue) minValue = value;
    if (value > maxValue) maxValue = value;

    sum += value;
    delayMicroseconds(200);
  }

  int avgValue = sum / SAMPLE_COUNT;
  int peakToPeak = maxValue - minValue;

  Serial.print("MIN: ");
  Serial.print(minValue);
  Serial.print("  MAX: ");
  Serial.print(maxValue);
  Serial.print("  AVG: ");
  Serial.print(avgValue);
  Serial.print("  P2P: ");
  Serial.println(peakToPeak);

  delay(100);
}
