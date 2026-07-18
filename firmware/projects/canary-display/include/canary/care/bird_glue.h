// include/canary/care/bird_glue.h — wiring for the living canary: feeds
// the pure mood engine (bird_mood.h) from the fleet + link state, keeps
// the trust ladder in flash, and hands the UI a face. See
// docs/hardware/display_living_canary.md.
#pragma once
#include <stdint.h>
#include "canary/ui/canary_mark.h"

namespace canary::care {

// Call once per render pass (internally rate-limited: the engine ticks
// one minute per real minute). Returns the face the glass should wear;
// the UI still decides WHERE the bird may perch (calm-tech: a status-word
// hero or an alarm keeps the stage bird-free).
canary::ui::CanaryMood bird_mood_tick(uint32_t now_ms, bool night,
                                      bool time_valid, int yday);

}  // namespace canary::care
