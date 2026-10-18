// tests_host/fake_lvgl/esp_random.h — a fixed sequence for canary_mark.cpp's
// flourish dice. Its timers never fire on their own in the host test (see
// lvgl.h here); a test that fires the flourish timer by hand scripts the
// rolls it needs through fake_esp::script(), which esp_random() drains first.
#pragma once
#include <stdint.h>

#include <deque>

namespace fake_esp {
inline std::deque<uint32_t>& script() {
  static std::deque<uint32_t> q;
  return q;
}
}  // namespace fake_esp

inline uint32_t esp_random() {
  if (!fake_esp::script().empty()) {
    const uint32_t v = fake_esp::script().front();
    fake_esp::script().pop_front();
    return v;
  }
  static uint32_t s = 0x2545F491u;
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
