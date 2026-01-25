/*
  Grove Vision AI V2 - Minimal AT responder (litter box demo)

  This sketch provides a tiny AT interface for the ESP32C3 bridge:

    AT+INFER?
      -> cat:<score>

  Replace `run_cat_inference()` with calls from the Seeed Grove Vision AI V2
  SDK (Himax/SSCMA) to run your cat/no-cat model.

  Conformance note:
  - This firmware emits only a label+score. The ESP32C3 constructs the
    event payload, ensuring no extra metadata leaves the device boundary.
*/

#include <Arduino.h>
#include <Seeed_Arduino_SSCMA.h>

#ifdef ESP32
#include <HardwareSerial.h>
HardwareSerial aiSerial(1);
#else
#define aiSerial Serial1
#endif

SSCMA ai;

static float run_cat_inference() {
  // Run one inference cycle via SSCMA and return a confidence score in [0, 1].
  if (ai.invoke(1, false, false) != 0) {
    return 0.0f;
  }

  float score = 0.0f;
  for (size_t i = 0; i < ai.classes().size(); ++i) {
    float candidate = static_cast<float>(ai.classes()[i].score) / 100.0f;
    if (candidate > score) {
      score = candidate;
    }
  }

  for (size_t i = 0; i < ai.boxes().size(); ++i) {
    float candidate = static_cast<float>(ai.boxes()[i].score) / 100.0f;
    if (candidate > score) {
      score = candidate;
    }
  }

  if (score > 1.0f) {
    score = 1.0f;
  }

  return score;
}

void setup() {
  Serial.begin(115200);
  ai.begin(&aiSerial);
}

void loop() {
  if (!Serial.available()) {
    delay(5);
    return;
  }

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) {
    return;
  }

  if (line.equalsIgnoreCase("AT+INFER?")) {
    float score = run_cat_inference();
    Serial.print("cat:");
    Serial.println(score, 3);
    return;
  }

  if (line.equalsIgnoreCase("AT")) {
    Serial.println("OK");
    return;
  }

  Serial.println("ERROR");
}
