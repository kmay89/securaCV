#pragma once
#include <stdint.h>
#include "canary/playground/playground.h"

// Dev Playground glass (800x480, dash flavor only): a bench instrument
// face, not a fleet face. Left: the always-visible PIN TRACKER — every
// terminal-block line and internal pin group with a used/open icon and
// live state (the "what's wired where" table from the playground doc,
// rendered live). Right: one card per station; tapping a card opens its
// detail panel with step-by-step wiring instructions and the station's
// actions (DO pulse/latch, cap-touch sensitivity preset, ToF trip
// threshold). Input arrives via playground.cpp's tap routing — same
// hit-test pattern as commission_ui, no LVGL indev.

namespace canary::playground {

void playground_ui_create();
void playground_ui_update(const PgState& st);
void playground_ui_handle_tap(int16_t x, int16_t y);

}  // namespace canary::playground
