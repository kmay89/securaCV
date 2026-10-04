#pragma once
#include <stdint.h>
#include "fleet_instance.h"
#include "canary_mark.h"

// Nightstand face (172x320 portrait ST7789, LVGL "Quiet Glass"). Color is
// the language: a full-height living canary over a severity color wash, a
// vertical witness column (worst floats to the top), and a glance line. The
// onboard WS2812 (hal/ambient_led.cpp) is the across-the-room half of the
// same signal — the glass is the detail-on-approach half.
//
// No touch panel on the 1.47" boards, so there is no page model — one
// standing face that breathes. The BOOT button (io/boot_button.h) is the
// whole input surface there: tap = peek, double = the lantern, hold =
// acknowledge. The Touch-1.69 sibling adds a real touch panel and uses the
// same grammar through main.cpp's tap routing.
//
// The LANTERN (care/lantern.h) is drawn here as a full-glass overlay: these
// small displays live in hallways and on nightstands, so the screen itself
// is the night light. It stays honest — user-summoned, timed out, and the
// instant anything reaches Warn the overlay yields the glass back to the
// state it must show. The WS2812 beacon is never part of the lamp: that
// channel stays a pure attention signal (display_nightstand_line.md §4).

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
  // Where we are inside quiet hours, in minutes. Hallway mode's dwell
  // envelope (care/hallway.h) rises over the first minutes and ebbs over the
  // last, so the corridor light arrives and leaves like evening rather than
  // like a timer. Both zero means "no quiet-hours window known" (no RTC, no
  // schedule) and the dwell is skipped — the lamp simply burns at its own
  // brightness rather than guessing.
  uint16_t night_elapsed_min = 0;
  uint16_t night_remaining_min = 0;
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

// The settings doorway (touch glass only: the Touch-1.69 and the AMOLED
// 2.41): a lit tap on the gear corner, by day, opens the settings panel and
// returns true. Every other tap — and every tap on the touch-less 1.47"
// boards — returns false and stays the wake it already was.
bool portrait_ui_handle_tap(int16_t x, int16_t y);

// Ambient-life moment (care/ambient_life.h): brighten the glance line for a
// few seconds so the check-in is visible, then let it settle back.
void portrait_ui_life_glance(uint32_t now);

}  // namespace canary::ui
