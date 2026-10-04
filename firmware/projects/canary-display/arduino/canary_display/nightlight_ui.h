// include/canary/ui/nightlight_ui.h — the Canary Nightlight face
// (ESP32-C3-LCD-1.47, 180x320 portrait — geometry-parametric via pins.h).
//
// A kid's bedside clock with a friend living in it: a big 7-segment clock
// over the lamp's look-engine wash, and the canary companion perched below
// — visiting the stage a few seconds at a time on the household's rhythm
// (care/nightlight.h), asleep beside the clock through quiet hours.
//
// The layer order IS the product's honesty:
//   - the LAMP (bottom): a look-engine scene — Lantern's warm orange,
//     Rainbow, Moonbeam white. Decor. Never a semantic color, so light
//     never means "safe" (display_nightstand_line.md §4 applies upside
//     down here: this flavor simply never renders safety as light).
//   - the CLOCK (middle): information. Blank-with-ghosts until SNTP gives
//     a real wall time — a clock that guesses is worse than none.
//   - the GLANCE LINE (top): the honest channel. Link trouble, an unsynced
//     clock, or a fleet condition is said in words, always, over any lamp.
//
// The companion's FACE stays the mood engine's (the bird is a gauge, not a
// toy — canary_mark.h); visits move the bird and dress the scene, they
// never repaint its expression.
#pragma once
#include <stdint.h>
#include "canary/care/nightlight.h"
#include "canary/fleet/fleet_instance.h"
#include "canary/ui/canary_mark.h"

namespace canary::ui {

struct NightlightState {
  bool night = false;               // night LOOK (red-shift applied)
  bool wifi_ok = false;
  const char* wifi_reason = nullptr;  // human cause, when wifi is down
  bool mqtt_ok = false;
  bool standalone = false;          // no broker ever configured — the mqtt
                                    // leg is not a truth this device owes
  bool acked = false;
  bool time_valid = false;
  int  clock_hh = 0;
  int  clock_mm = 0;
  int  wday = -1;                   // 0=sunday … 6=saturday; -1 unknown
  int  mday = 0;                    // 1..31; 0 unknown
  int  mon = 0;                     // 1..12; 0 unknown
  CanaryMood bird = CanaryMood::Idle;
  canary::care::Visit visit;        // the live companion visit (or None)
};

void nightlight_ui_create();
void nightlight_ui_update(const canary::fleet::Fleet& fleet, uint32_t now,
                          const NightlightState& st);

// The tumble: play the re-orientation entrance on a freshly rebuilt face —
// the companion drops in from the edge that WAS up, bounces on its perch,
// and the clock breathes back in behind it. `delta_cw` is how many quarter
// turns clockwise the world just made (1..3); the entry direction derives
// from it so the motion always reads as the real turn the device felt.
// Call right after nightlight_ui_create() on an orientation commit.
void nightlight_ui_tumble(uint8_t delta_cw, uint32_t now);

// Symmetry with the other faces (main.cpp calls ui_ack_hold uniformly);
// no touch panel on this board, so it is a no-op today.
void nightlight_ui_ack_hold(bool active);

// Ambient-life moment: brighten the caption line briefly.
void nightlight_ui_life_glance(uint32_t now);

}  // namespace canary::ui
