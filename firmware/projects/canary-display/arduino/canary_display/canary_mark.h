// include/canary/ui/canary_mark.h — the canary itself, on the glass.
//
// A small geometric canary in brand yellow: round body, round head, orange
// beak, dark eye that blinks, a wing a shade deeper. Pure LVGL objects (no
// image assets, no canvas), so it renders identically on both LVGL majors
// and costs a few hundred bytes.
//
// Living-canary wave: the bird is a GAUGE, not a toy. Its face is driven
// by the mood engine (care/bird_mood.h) — calm when everything is proved,
// visibly worried when something is late, asleep at night (stillness is
// the information), and gone entirely during a real alarm. Idle life is
// rationed Flipper-style: slow breathing with occasional small flourishes,
// and the rarest ones only appear after the system has earned days of
// trust.
#pragma once
#include <lvgl.h>
#include <stdint.h>

namespace canary::ui {

enum class CanaryMood : uint8_t {
  Hidden,      // off stage (alarms, pages without a perch)
  Idle,        // calm: breathe, blink, occasional flourish
  Happy,       // one hop of joy, then settles back to Idle
  Worried,     // eye narrowed, wing half-raised, scanning saccades
  Distressed,  // pacing, fidgeting wing — maintenance overdue
  Asleep,      // eye a line, head tucked, slow breath, no flourishes
  Searching,   // a witness is late: leaning to the edge, looking out
  Calling,     // a witness is lost: beak open, calling for them
};

// One-shot reactions layered over the current mood, then the pose settles
// back. Honesty rule holds: each maps 1:1 to a log-able event. Ignored
// while Hidden or Asleep — never startle a sleeping bird. Tilt/Startle
// are rate-limited; Greeting/Joyful (rare by construction) are not.
enum class CanaryReact : uint8_t {
  Tilt,      // a verified pass just completed: brief curious head-tilt
  Startle,   // the glass was touched while the bird is on stage
  Greeting,  // first wake of the day: slow wing stretch
  Joyful,    // trust milestone (first clean week / month): the song
};

// Builds the bird inside `parent` (size_px square). Recreating (e.g. on a
// new screen) is safe: the module tracks one live bird and cleans up its
// timers when the bird's screen is deleted.
lv_obj_t* canary_mark_create(lv_obj_t* parent, int size_px);

void canary_mark_mood(CanaryMood m);

void canary_mark_react(CanaryReact r);

// Trust ladder (consecutive clean days, from the mood engine): gates the
// rare idle flourishes so a long-healthy system is visibly different from
// a day-one system.
void canary_mark_trust(uint16_t days);

}  // namespace canary::ui
