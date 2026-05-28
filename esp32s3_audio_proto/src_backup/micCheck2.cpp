#include <Arduino.h>

const int MIC_PIN = 4;
const int SAMPLE_COUNT = 500;

void setup() {
  Serial.begin(115200);

  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) {
    delay(10);
  }

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  Serial.println();
  Serial.println("MAX4466 sound level test start");
  Serial.println("Speak or clap near the microphone.");
}

void loop() {
  int values[SAMPLE_COUNT];

  long sum = 0;
  int minValue = 4095;
  int maxValue = 0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    int v = analogRead(MIC_PIN);
    values[i] = v;
    sum += v;

    if (v < minValue) minValue = v;
    if (v > maxValue) maxValue = v;

    delayMicroseconds(125);
  }

  float avg = (float)sum / SAMPLE_COUNT;

  long absSum = 0;
  long squareSum = 0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    int centered = values[i] - avg;
    absSum += abs(centered);
    squareSum += centered * centered;
  }

  float meanAbs = (float)absSum / SAMPLE_COUNT;
  float rms = sqrt((float)squareSum / SAMPLE_COUNT);
  int p2p = maxValue - minValue;

  Serial.print("MIN: ");
  Serial.print(minValue);
  Serial.print("  MAX: ");
  Serial.print(maxValue);
  Serial.print("  AVG: ");
  Serial.print(avg, 1);
  Serial.print("  P2P: ");
  Serial.print(p2p);
  Serial.print("  ABS: ");
  Serial.print(meanAbs, 1);
  Serial.print("  RMS: ");
  Serial.println(rms, 1);

  delay(100);
}
