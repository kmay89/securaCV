// include/canary/ui/portrait7_ui.h — the 7" glass turned on its side.
//
// One portrait information design for BOTH 7" personalities: the wall-mounted
// Dash 7 stood in a doorway column, and the bedside Nightstand 7 stood tall on
// a nightstand. The 800x480 landscape glass becomes a 480x800 column (LVGL
// software-rotates; the panel keeps scanning landscape — see lvgl_port), so
// this face lays itself out in the logical portrait canvas the port reports.
//
// The column reads top-to-bottom the way a person standing in front of it
// does: a big segment clock hero, the household's one-word state in the
// state's own hue, the living canary as the emotional anchor, then a vertical
// witness list with the worst floated to the top, and an honest glance line
// at the foot. Night red-shifts the whole column and drops everything but the
// clock and the state channel — dark-when-safe still holds.
//
// The bedside flag only nudges emphasis (a bedside column leans on the clock;
// a wall column leans on the fleet). Both are the same honest instrument.
#pragma once
#include <lvgl.h>
#include <stdint.h>
#include "fleet_instance.h"
#include "canary_mark.h"

namespace canary::ui {

struct Portrait7State {
  bool night = false;         // night LOOK (red-shift preference applied)
  bool bedside = false;       // Nightstand 7 vs Dash 7 — emphasis only
  bool wifi_ok = false;
  const char* wifi_reason = nullptr;  // human cause when wifi is down
  bool mqtt_ok = false;
  bool acked = false;
  bool time_valid = false;
  int clock_hh = 0;
  int clock_mm = 0;
  CanaryMood bird = CanaryMood::Idle;
};

void portrait7_ui_create();
void portrait7_ui_update(const canary::fleet::Fleet& fleet, uint32_t now,
                         const Portrait7State& st);
void portrait7_ui_ack_hold(bool active);

// A lit tap the column claims (today: the gear corner, which opens the
// settings surface — day only). Returns false for taps that should stay the
// wake they already were; main.cpp routes accordingly.
bool portrait7_ui_handle_tap(int16_t x, int16_t y);

// Ambient-life moment (care/ambient_life.h): brighten the glance line for a
// few seconds so an idle check-in is visible, then let it settle back.
void portrait7_ui_life_glance(uint32_t now);

}  // namespace canary::ui
