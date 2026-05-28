#include <Arduino.h>

const int MIC_PIN = 4;
const int SAMPLE_RATE = 8000;
const int RECORD_SECONDS = 5;
const int TOTAL_SAMPLES = SAMPLE_RATE * RECORD_SECONDS;
const int SAMPLE_INTERVAL_US = 1000000 / SAMPLE_RATE;

const int GAIN = 30;

int calibrateCenter() {
  const int count = 1000;
  long sum = 0;

  for (int i = 0; i < count; i++) {
    sum += analogRead(MIC_PIN);
    delayMicroseconds(200);
  }

  return sum / count;
}

void setup() {
  Serial.begin(921600);

  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) {
    delay(10);
  }

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  delay(1000);

  Serial.println("READY");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();

    if (c == 'R') {
      int adcCenter = calibrateCenter();

      Serial.print("CENTER:");
      Serial.println(adcCenter);
      Serial.println("START");

      uint32_t nextMicros = micros();

      for (int i = 0; i < TOTAL_SAMPLES; i++) {
        while ((int32_t)(micros() - nextMicros) < 0) {
        }

        nextMicros += SAMPLE_INTERVAL_US;

        int raw = analogRead(MIC_PIN);
        int centered = raw - adcCenter;
        int sample16 = centered * GAIN;

        if (sample16 > 32767) sample16 = 32767;
        if (sample16 < -32768) sample16 = -32768;

        int16_t pcm = (int16_t)sample16;

        Serial.write((uint8_t *)&pcm, 2);
      }

      Serial.println();
      Serial.println("END");
    }
  }
}
