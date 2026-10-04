#pragma once
#include <stdint.h>
#include "canary/fleet/fleet_instance.h"
#include "canary/ui/canary_mark.h"

// Dash face (800x480, LVGL "Quiet Glass"): header sentence + witness card
// gallery + event timeline. Single screen — a wall panel is a poster, not
// an app. Input policy lives in main.cpp; this module renders and animates.

namespace canary::ui {

struct DashState {
  bool night = false;     // dark theme (the backlight-off part is the HAL's job)
  bool wifi_ok = false;
  // When the uplink is down, WHY — the shared label from
  // common/network/wifi_join_policy.h ("Wrong password", "Network not found").
  // A bare "No WiFi" tells an operator standing in front of the glass nothing
  // they can act on; the cause is the entire diagnostic a screen this size can
  // give. Null while the link is up.
  const char* wifi_reason = nullptr;
  bool mqtt_ok = false;
  bool acked = false;
  bool time_valid = false;
  int  clock_hh = 0;
  int  clock_mm = 0;
  // Living canary: the face the mood engine chose (care/bird_glue.h).
  CanaryMood bird = CanaryMood::Hidden;
};

void dash_ui_create();
void dash_ui_update(const canary::fleet::Fleet& fleet, uint32_t now,
                    const DashState& st);

// Hold-to-acknowledge sweep (same affordance as the watch).
void dash_ui_ack_hold(bool active);

// Tap routing (Proof-on-Glass, spec §1): tap a witness card -> proof sheet
// with a scannable QR of its signed chain head; tap again -> dismiss.
// Returns true when the tap was consumed (main then skips its wake-only
// handling). Coordinates are panel pixels.
bool dash_ui_handle_tap(int16_t x, int16_t y);

// Which witness card sits under panel pixel (x, y): fleet index, or -1.
// main routes long-press by it (on a card = mute that witness; anywhere
// else = the household acknowledge).
int dash_ui_card_at(int16_t x, int16_t y);

// Cleaning-mode lockout (armed from the transparency sheet): while true,
// main swallows ALL touch input — a wet cloth must not ack an alarm.
bool dash_ui_touch_locked(uint32_t now_ms);

}  // namespace canary::ui
