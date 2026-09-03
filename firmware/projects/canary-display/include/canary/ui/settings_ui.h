// include/canary/ui/settings_ui.h — the on-glass settings panel
// (settings wave, docs/hardware/display_settings.md).
//
// One design system on every touch glass: a navigation bar, a scrolling list
// of grouped rows — a value and a chevron where a tap opens an editor, a
// switch where the decision is on/off, a check where it is one-of-few, a
// slider where the screen is the preview — with the group's one-line footer
// saying what the setting costs. The panel lays itself out from the live
// canvas (round 240 puck, 240x280 and 450x600 portraits, the 800x480 dash as
// a centered sheet and its 480x800 column), so there is one tree, not one
// per board.
//
// Input is LVGL-native: main.cpp keeps polling the touch controller and its
// gesture policy stays untouched, but while the panel is open it hands every
// sample to settings_ui_handle_touch, which feeds LVGL's pointer device
// (rows click, lists scroll with momentum, switches flip, sliders drag) and
// keeps the two exits the docs promise: a stationary long-press anywhere
// that is not a control, and the idle timeout. A real alarm closes it
// instantly — a settings sheet must never stand between a person and a
// tamper. The brightness editors + black-point wizard take the backlight
// (settings_ui_owns_backlight) so live preview never fights the policy.
#pragma once
#include <stdint.h>

namespace canary::ui {

void settings_ui_open();
void settings_ui_close();
bool settings_ui_active();

// True while a brightness editor or the black-point wizard is live-driving
// the backlight; main.cpp's apply_brightness stands down.
bool settings_ui_owns_backlight();

// The raw touch sample, every loop pass, while active (panel pixels, the
// HAL's logical frame). Feeds LVGL and runs the panel's own exits.
void settings_ui_handle_touch(bool down, int16_t x, int16_t y,
                              uint32_t now_ms);

// Housekeeping from loop(): idle timeout, urgent-close, live pages.
// urgent = unacked Alert/Tamper somewhere in the fleet.
void settings_ui_tick(uint32_t now_ms, bool urgent);

}  // namespace canary::ui
