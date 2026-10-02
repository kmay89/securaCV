// tests_host/fake_lvgl/Arduino.h — the Arduino core's clock, on the fake
// LVGL's: canary_mark.cpp and onboard_ui.cpp include the core but place
// the bird with LVGL alone; splash.cpp paces its first meeting with
// millis() and delay(). millis() reads the fake tick, and delay(ms) runs
// the anims that long (fake_lvgl::run: 5 ms steps, so a delay rounds up to
// the next 5 ms).
#pragma once
#include <stdint.h>

#include "lvgl.h"

inline uint32_t millis() { return fake_lvgl::tick(); }
inline void delay(uint32_t ms) { fake_lvgl::run(ms); }
