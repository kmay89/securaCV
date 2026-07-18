// include/canary/ui/splash.h — boot splash: the canary hops in over the
// wordmark, holds a beat, cross-fades into the UI. Runs BEFORE the main
// screens are built (it owns its own screen and deletes it on exit), so
// the one-live-bird rule in canary_mark hands off cleanly to whichever
// face creates its bird next.
#pragma once
#include <stdint.h>

namespace canary::ui {

// Blocking, like the onboarding Hello beat: pumps LVGL for hold_ms plus a
// short fade-out. Call after lvgl_port_init(), before the face is created.
void splash_play(uint32_t hold_ms);

}  // namespace canary::ui
