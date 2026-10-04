// Mode glue runtime (see include/canary/mode/mode_glue.h). The whole TU is
// empty unless at least one non-fleet gear is compiled in, so the default
// watch/dash/emulator builds stay byte-identical.

#include "canary/config.h"

#if (defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND) ||   \
    (defined(FEATURE_DEVMODE) && FEATURE_DEVMODE) ||         \
    (defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE) ||     \
    (defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE) ||   \
    (defined(FEATURE_ARCADE) && FEATURE_ARCADE)

#include <Arduino.h>
#include <Preferences.h>

#include "canary/mode/mode_glue.h"

#if ((defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND) || \
     (defined(FEATURE_DEVMODE) && FEATURE_DEVMODE)) && defined(CD_FLAVOR_DASH)
#include "canary/playground/playground.h"
#endif
#if defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE
#include "canary/mode/demo_mode.h"
#endif
#if defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE
#include "canary/mode/debug_mode.h"
#endif
#if defined(FEATURE_ARCADE) && FEATURE_ARCADE && defined(CD_FLAVOR_DASH)
#include "canary/mode/arcade_mode.h"
#endif

namespace canary {
namespace mode {

namespace {

// What THIS binary carries — the compile-time truth the pure resolver gates
// against (a stored token for a gear this build lacks resolves to Fleet).
BuildCaps build_caps() {
  BuildCaps c;
#if defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND
  c.dedicated_bench = true;
#endif
#if defined(FEATURE_DEVMODE) && FEATURE_DEVMODE
  c.has_devmode = true;
#endif
#if defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE
  c.has_demo = true;
#endif
#if defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE
  c.has_debug = true;
#endif
  // Arcade is dash-first by spec (the watch's 240 px round glass makes a
  // poor arcade); the cap mirrors what the TU actually compiles.
#if defined(FEATURE_ARCADE) && FEATURE_ARCADE && defined(CD_FLAVOR_DASH)
  c.has_arcade = true;
#endif
  return c;
}

// One writer for both latches. token == nullptr removes the key (Fleet is
// "no latch", so a fleet-bound reboot leaves NVS clean, not tokened).
void write_latches(const char* token, bool legacy_devmode) {
  Preferences p;
  if (p.begin("securacv", /*readOnly=*/false)) {
    if (token && token[0]) {
      p.putString("mode", token);
    } else {
      p.remove("mode");
    }
    if (legacy_devmode) {
      p.putBool("devmode", true);
    } else {
      p.remove("devmode");
    }
    p.end();
  }
}

}  // namespace

Mode boot_mode() {
  char token[16] = {0};
  bool legacy = false;
  {
    Preferences p;
    if (p.begin("securacv", /*readOnly=*/true)) {
      p.getString("mode", token, sizeof(token));
      legacy = p.getBool("devmode", false);
      p.end();
    }
  }
  return resolve_boot_mode(build_caps(), token, legacy);
}

void mode_enter(Mode m) {
  // Migrate a legacy-latched unit to the token grammar on first entry —
  // except in the dedicated bench env, where the FLASH is the mode choice
  // and the latch must stay untouched (a bench unit reflashed to the fleet
  // image must come up as a fleet witness, not re-enter a phantom latch).
#if !(defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND)
  write_latches(mode_token(m), m == Mode::Bench);
#endif

  switch (m) {
    case Mode::Fleet:
      return;  // never dispatched here; setup() owns the fleet path
#if ((defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND) || \
     (defined(FEATURE_DEVMODE) && FEATURE_DEVMODE)) && defined(CD_FLAVOR_DASH)
    case Mode::Bench:
      canary::playground::playground_setup();
      return;
#endif
#if defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE
    case Mode::Demo:
      demo_mode_setup();
      return;
#endif
#if defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE
    case Mode::Debug:
      debug_mode_setup();
      return;
#endif
#if defined(FEATURE_ARCADE) && FEATURE_ARCADE && defined(CD_FLAVOR_DASH)
    case Mode::Arcade:
      arcade_mode_setup();
      return;
#endif
    default:
      // A gear this build doesn't carry can't be dispatched (boot_mode()
      // already resolved it to Fleet); nothing to do.
      return;
  }
}

void mode_loop_step(Mode m) {
  switch (m) {
    case Mode::Fleet:
      return;
#if ((defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND) || \
     (defined(FEATURE_DEVMODE) && FEATURE_DEVMODE)) && defined(CD_FLAVOR_DASH)
    case Mode::Bench:
      canary::playground::playground_loop();
      return;
#endif
#if defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE
    case Mode::Demo:
      demo_mode_loop();
      return;
#endif
#if defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE
    case Mode::Debug:
      debug_mode_loop();
      return;
#endif
#if defined(FEATURE_ARCADE) && FEATURE_ARCADE && defined(CD_FLAVOR_DASH)
    case Mode::Arcade:
      arcade_mode_loop();
      return;
#endif
    default:
      return;
  }
}

void mode_request(Mode m) {
  write_latches(mode_token(m), m == Mode::Bench);
  delay(60);
  ESP.restart();  // does not return
}

void mode_exit_to_fleet() {
  write_latches(nullptr, false);
  delay(60);
  ESP.restart();  // does not return
}

}  // namespace mode
}  // namespace canary

#endif  // any non-fleet gear compiled
