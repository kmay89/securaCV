// include/canary/ui/nightstand7_ui.h — the 7" bedside face (Nightstand 7).
//
// The 800x480 glass turned toward the bed instead of the room: a big
// segment-drawn clock hero, the day's weather + comfort + sun lines, a
// quiet fleet strip with the living canary — and after dark, a red-shifted
// clock focus with tomorrow's weather, over a glass that is otherwise
// black-when-safe. The lantern overlay (care/lantern.h) and the rendered
// sunrise (wake alarm) live here too, because the CH422G backlight is
// binary: every kind of "dim" on this board is drawn, not driven.
#pragma once
#include <lvgl.h>
#include <stdint.h>
// fleet_instance.h, not fleet_model.h: the model header is the TEMPLATE, and
// `Fleet` is the per-flavor instantiation the instance header names (same
// include the dash and portrait faces take).
#include "canary/fleet/fleet_instance.h"
#include "canary/ui/canary_mark.h"

namespace canary::ui {

struct Nightstand7State {
  bool night = false;          // night LOOK (red-shift preference applied)
  bool wifi_ok = false;
  const char* wifi_reason = nullptr;   // human cause, when wifi is down
  bool mqtt_ok = false;
  bool acked = false;
  bool time_valid = false;
  int clock_hh = 0;
  int clock_mm = 0;
  // Hours since SNTP last PROVED the wall clock (0xFFFF = never this boot).
  // The face whispers about it once this grows old — a bedside clock must
  // never be silently stale.
  uint16_t sync_age_h = 0;
  CanaryMood bird = CanaryMood::Idle;
};

void nightstand7_ui_create();
void nightstand7_ui_update(const canary::fleet::Fleet& fleet, uint32_t now,
                           const Nightstand7State& st);
void nightstand7_ui_ack_hold(bool active);

// Tap routing (main.cpp owns gestures, as everywhere): returns true when the
// tap was consumed here — the lantern affordance after dark, or a scene
// cycle while the lantern is lit. A false falls through to the generic
// wake handling.
bool nightstand7_ui_handle_tap(int16_t x, int16_t y, uint32_t now);

// Ambient-life moment (care/ambient_life.h): surface the status glance
// briefly — the face brightens its fleet line, then lets it fade back.
void nightstand7_ui_life_glance(uint32_t now);

}  // namespace canary::ui
