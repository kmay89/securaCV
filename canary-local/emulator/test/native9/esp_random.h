// canary-local/emulator/test/native9/esp_random.h — ESP-IDF's hardware RNG
// for glass_splash_lvgl9_test.cpp (F246) and its LVGL 8.4 sibling,
// glass_splash_test.cpp (F185): canary_mark.cpp draws its idle
// flourishes' timing and sides from esp_random(). Here it is a fixed
// xorshift, so every run of the splash plays the same frames.
#pragma once
#include <stdint.h>

inline uint32_t esp_random(void) {
  static uint32_t s = 0x2545F491u;
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
