#include <Arduino.h>

const int MIC_AO_PIN = 34;

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_AO_PIN, ADC_11db);

  Serial.println("Simple microphone AO raw test");
}

void loop() {
  int value = analogRead(MIC_AO_PIN);

  Serial.print("AO raw = ");
  Serial.println(value);

  delay(100);
}