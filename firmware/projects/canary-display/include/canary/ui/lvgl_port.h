#pragma once

// LVGL bring-up: display driver bound to the flavor's panel through the
// existing HAL (canary/hal/display.h). Input stays with main.cpp's gesture
// policy — LVGL renders, it does not own touch. Call lvgl_port_init() once
// after display_init(); pump lv_timer_handler() from loop().

namespace canary::ui {

bool lvgl_port_init();

}  // namespace canary::ui
