/**
 * @file short_tap.h
 * @brief A short press on a button the firmware reads — debounced, and
 *        told apart from the long holds other gestures own.
 *
 * The Bluetooth setup door (common/network/improv_core.h) opens for a
 * minute on a physical tap, so a device that already has an owner can be
 * re-pointed at a new network only by someone touching it. Every Canary
 * board with a BOOT button names it in its pins.h (BOOT_BUTTON_PIN, active
 * low); the family's loop() feeds this stepper the raw level each pass and
 * acts on the one `true` it returns per tap.
 *
 * What counts as a tap: the button goes down, stays down for at least
 * `debounce_ms` (contact bounce and a brushed finger do not) and comes up
 * before `max_ms` (a hold past that is someone else's gesture — the WAP's
 * two-second BOOT hold, a factory reset — and is not reported here).
 *
 * Pure: no Arduino. The loop passes the level and millis(); wrap-safe.
 */

#pragma once

#include <stdint.h>

namespace canary {
namespace io {

struct ShortTap {
  /// A press shorter than this is noise.
  uint32_t debounce_ms = 40;
  /// A press at or past this is a hold, not a tap.
  uint32_t max_ms = 700;

  bool     was_down = false;
  uint32_t down_at_ms = 0;
  /// Set once the press has outlasted max_ms, so the release is not a tap.
  bool     held = false;

  /// One pass. `pressed` is the debounced-by-us raw level (true = down).
  /// Returns true exactly once per completed short press, on the release.
  bool step(bool pressed, uint32_t now_ms) {
    if (pressed) {
      if (!was_down) {
        was_down = true;
        down_at_ms = now_ms;
        held = false;
      } else if (!held && (uint32_t)(now_ms - down_at_ms) >= max_ms) {
        held = true;
      }
      return false;
    }
    if (!was_down) return false;
    was_down = false;
    if (held) return false;
    const uint32_t down_for = (uint32_t)(now_ms - down_at_ms);
    return down_for >= debounce_ms && down_for < max_ms;
  }
};

}  // namespace io
}  // namespace canary
