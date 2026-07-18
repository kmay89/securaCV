// include/canary/ui/settings_ui.h — the on-glass screen settings surface
// (settings wave, docs/hardware/display_settings.md).
//
// One design system, two renderers: the watch gets page-per-setting screens
// with big centered rows (one screen, one decision); the dash renders the
// same tree in a roomier sheet. Both are driven by main.cpp's existing tap
// routing — the module exposes hit-testing, never registers input devices.
//
// While open it owns the glass: main routes every tap here, long-press is
// the quick exit, and the brightness editors + black-point wizard take the
// backlight (settings_ui_owns_backlight) so live preview never fights the
// brightness policy. A real alarm closes it instantly — a settings sheet
// must never stand between a person and a tamper.
#pragma once
#include <stdint.h>

namespace canary::ui {

void settings_ui_open();
void settings_ui_close();
bool settings_ui_active();

// True while a brightness editor or the black-point wizard is live-driving
// the backlight; main.cpp's apply_brightness stands down.
bool settings_ui_owns_backlight();

// Tap routing (panel pixels). Only called while active.
void settings_ui_handle_tap(int16_t x, int16_t y);

// Housekeeping from loop(): idle timeout, urgent-close, live clock in the
// wizard. urgent = unacked Alert/Tamper somewhere in the fleet.
void settings_ui_tick(uint32_t now_ms, bool urgent);

}  // namespace canary::ui
