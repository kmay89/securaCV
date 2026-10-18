/**
 * @file reset_hold.h
 * @brief The long hold on a Canary's BOOT/user button — factory reset by
 *        touch, classified once for every board.
 *
 * The onboarding state machine (common/network/onboarding_fsm.h) says a
 * 10-second hold returns a node to its box state: saved networks and
 * settings gone, witness identity kept. Before this header one tree did that
 * at 5 s and wiped the identity key too, one reserved the long hold and did
 * nothing, and three had no reset path but re-flashing.
 *
 * Stages, each reported ONCE per press while the button is still held:
 *
 *   Warn   at `warn_ms` (3 s)  — the board may blink / print "keep holding to
 *                                 reset"; a release here does nothing.
 *   Reset  at `reset_ms` (10 s) — credentials + settings. A board with no
 *                                 further stage acts on this event at once
 *                                 (the hand is still on the button, so the
 *                                 feedback lands); a board that ALSO offers
 *                                 the wipe stage acts on Released instead,
 *                                 reading `released_stage`.
 *   Wipe   at `wipe_ms` (opt-in, 0 = disabled) — everything, identity too.
 *                                 Only a tree that has a reason to shed its
 *                                 key (the PIO canary's historical behavior)
 *                                 enables it, and says so on serial first.
 *
 * `Released` is reported on the release edge of ANY press that reached at
 * least Warn, with `released_stage` telling which stage it came from (1 warn,
 * 2 reset, 3 wipe); a press released before Warn is somebody else's gesture
 * (common/io/short_tap.h, the display's ButtonClassifier) and is silent here.
 * The three classifiers share one GPIO level stream and never fire together:
 * a short tap is < 700 ms, the display's Long is 900 ms, its pair-demo hold
 * 5 s; Warn at 3 s sits between them and is advisory only.
 *
 * Pure: no Arduino. Wrap-safe. firmware/tests_host/test_reset_hold.cpp.
 */

#pragma once

#include <stdint.h>

namespace canary {
namespace io {

enum class HoldEvent : uint8_t { None = 0, Warn = 1, Reset = 2, Wipe = 3, Released = 4 };

struct ResetHold {
  uint32_t warn_ms  = 3000;
  uint32_t reset_ms = 10000;
  uint32_t wipe_ms  = 0;        ///< 0 = no wipe stage

  bool     was_down = false;
  uint32_t down_at_ms = 0;
  uint8_t  stage = 0;           ///< 0 none, 1 warned, 2 reset, 3 wiped
  uint8_t  released_stage = 0;  ///< valid right after a Released event

  /// One pass. `pressed` = the polarity-resolved level (true = held down).
  HoldEvent step(bool pressed, uint32_t now_ms) {
    if (pressed) {
      if (!was_down) {
        was_down = true;
        down_at_ms = now_ms;
        stage = 0;
        return HoldEvent::None;
      }
      const uint32_t held = (uint32_t)(now_ms - down_at_ms);
      if (stage < 1 && held >= warn_ms)  { stage = 1; return HoldEvent::Warn; }
      if (stage < 2 && held >= reset_ms) { stage = 2; return HoldEvent::Reset; }
      if (wipe_ms != 0 && stage < 3 && held >= wipe_ms) { stage = 3; return HoldEvent::Wipe; }
      return HoldEvent::None;
    }
    if (!was_down) return HoldEvent::None;
    was_down = false;
    released_stage = stage;
    stage = 0;
    return released_stage >= 1 ? HoldEvent::Released : HoldEvent::None;
  }

  /// True while a press is past Warn — the board can show "keep holding".
  bool warning() const { return was_down && stage >= 1; }
};

/** What a Reset event clears. The identity key survives the default scope. */
enum class ResetScope : uint8_t {
  CredentialsAndSettings = 0,  ///< saved networks, broker, setup flags, device settings
  Everything = 1,              ///< the above plus the witness identity and chain state
};

inline const char* reset_scope_label(ResetScope s) {
  return s == ResetScope::Everything ? "everything (identity too)" : "credentials and settings";
}

}  // namespace io
}  // namespace canary
