#include <Arduino.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.begin(115200);

  delay(5000);

  Serial.println();
  Serial.println("ESP32-S3 Serial Monitor Test");
  Serial.println("Serial output OK");
  Serial.print("Millis: ");
  Serial.println(millis());
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("LED ON");
  delay(1000);

  digitalWrite(LED_BUILTIN, LOW);
  Serial.println("LED OFF");
  delay(1000);
}
