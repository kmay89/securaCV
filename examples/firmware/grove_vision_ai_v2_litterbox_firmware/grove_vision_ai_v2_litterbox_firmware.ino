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

static float run_cat_inference() {
  // TODO: Replace this stub with your model inference using the
  // Seeed Grove Vision AI V2 SDK. Return a confidence score in [0, 1].
  return 0.0f;
}

void setup() {
  Serial.begin(115200);
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
