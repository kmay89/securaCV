// tests_host/fake_fieldbus/lvgl.h — the one LVGL call playground.cpp makes
// itself (its UI lives in playground_ui.cpp, which the test stubs out).
#pragma once
#include <stdint.h>

inline uint32_t lv_timer_handler() { return 0; }
