#pragma once
#include <stdint.h>
#include "canary/fleet/fleet_instance.h"

// Dash face (800x480, LVGL "Quiet Glass"): header sentence + witness card
// gallery + event timeline. Single screen — a wall panel is a poster, not
// an app. Input policy lives in main.cpp; this module renders and animates.

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

void dash_ui_create();
void dash_ui_update(const canary::fleet::Fleet& fleet, uint32_t now,
                    const DashState& st);

// Hold-to-acknowledge sweep (same affordance as the watch).
void dash_ui_ack_hold(bool active);

}  // namespace canary::ui
