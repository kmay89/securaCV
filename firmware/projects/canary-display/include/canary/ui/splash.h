// include/canary/ui/splash.h — boot splash. Two splashes live behind one
// call: the FIRST boot ever is a meeting — the bird hops in, notices you,
// and introduces itself in a typed speech bubble (tap always advances) —
// and every later boot is the short familiar splash with a quiet "hello
// again". First-meeting state persists in NVS ("scv-hello"). Runs BEFORE
// the main screens are built (it owns its own screen and deletes it on
// exit), so the one-live-bird rule in canary_mark hands off cleanly to
// whichever face creates its bird next.
#pragma once
#include <stdint.h>

namespace canary::ui {

// Blocking, like the onboarding Hello beat: pumps LVGL for hold_ms (the
// first meeting runs its own storyboard instead), then drops an opaque
// curtain over the glass and returns. Call after lvgl_port_init(), before
// the face is created; build and paint the face, then call splash_reveal()
// to wipe the curtain up off it (the arcade exit — no flash frame, ever).
void splash_play(uint32_t hold_ms);
void splash_reveal();

}  // namespace canary::ui
