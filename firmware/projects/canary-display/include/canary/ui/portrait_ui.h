#pragma once
#include <stdint.h>
#include "canary/fleet/fleet_instance.h"
#include "canary/ui/canary_mark.h"

// Nightstand face (172x320 portrait ST7789, LVGL "Quiet Glass"). Color is
// the language: a full-height living canary over a severity color wash, a
// vertical witness column (worst floats to the top), and a glance line. The
// onboard WS2812 (hal/ambient_led.cpp) is the across-the-room half of the
// same signal — the glass is the detail-on-approach half.
//
// No touch panel on the 1.47" boards, so there is no page model — one
// standing face that breathes. Input (BOOT button peek / summon-nightlight)
// is a follow-up; today the face renders, animates, and nothing else.

namespace canary::ui {

struct PortraitState {
  bool night = false;
  bool wifi_ok = false;
  // When the uplink is down, WHY — the shared label from
  // common/network/wifi_join_policy.h ("Wrong password", "Network not found").
  // A bare "No WiFi" tells an operator standing in front of the glass nothing
  // they can act on; the cause is the entire diagnostic a screen this size can
  // give. Null while the link is up.
  const char* wifi_reason = nullptr;
  bool mqtt_ok = false;
  bool acked = false;       // ack residual marker
  bool time_valid = false;
  int  clock_hh = 0;
  int  clock_mm = 0;
  // Living canary: the face the mood engine chose (care/bird_glue.h).
  CanaryMood bird = CanaryMood::Hidden;
};

void portrait_ui_create();
void portrait_ui_update(const canary::fleet::Fleet& fleet, uint32_t now,
                        const PortraitState& st);

// Symmetry with the touch flavors (main.cpp calls ui_ack_hold uniformly).
// The 1.47" boards have no touch, so this is a no-op today; it stays in the
// interface so the BOOT-button acknowledge can light the same affordance
// when it lands.
void portrait_ui_ack_hold(bool active);

}  // namespace canary::ui
