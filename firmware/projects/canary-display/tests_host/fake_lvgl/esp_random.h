// tests_host/fake_lvgl/esp_random.h — a fixed sequence for canary_mark.cpp's
// flourish dice. Its timers never fire in the host test (see lvgl.h here),
// so nothing below is ever rolled for a placement.
#pragma once
#include <stdint.h>
inline uint32_t esp_random() {
  static uint32_t s = 0x2545F491u;
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
