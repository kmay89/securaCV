#pragma once
#include <stdint.h>
#include "canary/fleet/fleet_instance.h"

class Arduino_GFX;

// Dash face (800x480): header strip + witness card grid + event timeline
// column. Single screen — a wall panel is a poster, not an app. Input
// policy lives in main.cpp; this module only draws.

namespace canary::ui {

struct DashState {
  bool night = false;     // dark theme (the backlight-off part is the HAL's job)
  bool wifi_ok = false;
  bool mqtt_ok = false;
  bool acked = false;
  bool time_valid = false;
  int  clock_hh = 0;
  int  clock_mm = 0;
};

void dash_render(Arduino_GFX* g, const canary::fleet::Fleet& fleet,
                 uint32_t now, const DashState& st);

}  // namespace canary::ui
