#pragma once
#include <stdint.h>
#include "fleet_instance.h"
#include "canary_mark.h"

// Watch Station face (240x240 round, LVGL "Quiet Glass"). Page model:
//   0            halo — one smooth arc per witness, hero state center
//   1..count     per-witness detail
//   count+1      recent events
//   count+2      proof — QR of the most urgent witness's signed chain head
// Input policy lives in main.cpp (tap = wake/next page, long-press = ack);
// this module renders, animates, and nothing else.

namespace canary::ui {

struct GlanceState {
  int  page = 0;            // clamped by update against the live fleet size
  bool night = false;
  bool wifi_ok = false;
  bool mqtt_ok = false;
  bool acked = false;       // ack residual marker
  bool time_valid = false;
  int  clock_hh = 0;
  int  clock_mm = 0;
  // Living canary: the face the mood engine chose (care/bird_glue.h).
  // The halo still decides whether the bird may perch at all.
  CanaryMood bird = CanaryMood::Hidden;
};

void glance_ui_create();
void glance_ui_update(const canary::fleet::Fleet& fleet, uint32_t now,
                      const GlanceState& st);

// Hold-to-acknowledge affordance: a ring sweeps closed over MOTION_ACK_MS
// while the finger stays down, so the ack feels deliberate, never
// accidental. Start on touch-hold, stop on release/fire.
void glance_ui_ack_hold(bool active);

int glance_page_count();

// Rotation index of the settings doorway page — main.cpp routes a
// long-press there into settings_ui_open() instead of ack/mute.
int glance_settings_page();

}  // namespace canary::ui
