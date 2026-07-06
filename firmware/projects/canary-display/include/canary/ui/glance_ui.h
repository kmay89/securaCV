#pragma once
#include <stdint.h>
#include "canary/fleet/fleet_instance.h"

class Arduino_GFX;

// Watch Station face (240x240 round). Page model:
//   0            fleet overview — center state + one ring segment per witness
//   1..count     per-witness detail
//   count+1      recent events list
// Input policy lives in main.cpp (tap = wake/next page, long-press = ack);
// this module only draws.

namespace canary::ui {

struct GlanceState {
  int  page = 0;            // clamped by render against the live fleet size
  bool night = false;
  bool wifi_ok = false;
  bool mqtt_ok = false;
  bool acked = false;       // ack residual marker
  bool time_valid = false;
  int  clock_hh = 0;
  int  clock_mm = 0;
};

int glance_page_count();
void glance_render(Arduino_GFX* g, const canary::fleet::Fleet& fleet,
                   uint32_t now, const GlanceState& st);

}  // namespace canary::ui
