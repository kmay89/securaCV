// include/canary/io/boot_button.h — BOOT-button press classifier, pure model.
//
// The 1.47"/1.69" nightstand boards have a BOOT/user button (pins.h
// BOOT_BUTTON_PIN) and, on the 1.47s, no touch panel — the button is the
// whole input surface. This classifier turns a debounced level stream into
// the three gestures the bedside grammar needs:
//
//   Short   tap-equivalent  (wake / peek; cycle the lantern scene while lit)
//   Double  summon/extinguish the lantern
//   Long    acknowledge (the same deliberate hold as a touch long-press)
//   Triple  OPT-IN (the nightlight only): take manual control of the
//           orientation and turn the face one step
//
// Pure logic — the glue in main.cpp reads the GPIO and feeds level+now.
// A Short is only emitted after the double-tap window closes without a
// second press, so the two gestures never fire together.
//
// Triple is opt-in (enable_triple) because it costs the Double its
// immediacy: with a triple grammar armed, a second press must WAIT out
// the gap window before it can honestly be a Double — the lantern lights
// up to DOUBLE_GAP_MS later. Only the flavor that needs a third gesture
// pays that; everyone else's classifier is byte-for-byte the one that
// shipped. Host-tested in tests_host/test_boot_button.cpp (both modes).
#pragma once
#include <stdint.h>

namespace canary::io {

enum class ButtonEvent : uint8_t { None = 0, Short, Double, Long, Triple };

class ButtonClassifier {
 public:
  static constexpr uint32_t DEBOUNCE_MS = 30;
  static constexpr uint32_t LONG_MS = 900;      // = CD_LONGPRESS_MS
  static constexpr uint32_t DOUBLE_GAP_MS = 350;

  // Arm the three-tap grammar (see the header note for what it trades).
  void enable_triple(bool on) { triple_enabled_ = on; }

  // Feed every poll: `pressed` is the polarity-resolved level (true =
  // finger on the button), `now` the millisecond clock. Returns at most one
  // event per call.
  ButtonEvent step(bool pressed, uint32_t now) {
    // Debounce: ignore level flips faster than DEBOUNCE_MS.
    if (pressed != raw_) {
      raw_ = pressed;
      edge_at_ = now;
    }
    const bool stable = (uint32_t)(now - edge_at_) >= DEBOUNCE_MS;
    if (stable && raw_ != level_) {
      level_ = raw_;
      if (level_) {                          // press edge
        down_at_ = now;
        long_fired_ = false;
        if (pending_double_ &&
            (uint32_t)(now - double_at_) < DOUBLE_GAP_MS) {
          // Third press inside the window (triple mode only reaches
          // here). Fires on the PRESS, same immediacy rule as Double.
          pending_double_ = false;
          long_fired_ = true;                // this press is spoken for
          return ButtonEvent::Triple;
        }
        if (pending_short_ &&
            (uint32_t)(now - short_at_) < DOUBLE_GAP_MS) {
          pending_short_ = false;
          // This press is spoken for — holding it must not then also
          // acknowledge, and its release must not mature into a Short.
          long_fired_ = true;
          if (triple_enabled_) {
            // A second press might still become a Triple; hold the
            // Double until the window says otherwise.
            pending_double_ = true;
            double_at_ = now;
            return ButtonEvent::None;
          }
          // Stock grammar: fire on the PRESS, not the release — a
          // double-press summons a light, and a light has to come on
          // when you push the button, not when you let go.
          return ButtonEvent::Double;
        }
      } else if (!long_fired_) {             // release edge, not spoken for
        pending_short_ = true;
        short_at_ = now;
      } else {
        long_fired_ = false;                 // Long/Double/Triple reported
      }
    }
    // Long fires while still held — it must work half-asleep, without
    // waiting for a release.
    if (level_ && !long_fired_ && (uint32_t)(now - down_at_) >= LONG_MS) {
      long_fired_ = true;
      pending_short_ = false;                // a hold is never also a tap
      pending_double_ = false;
      return ButtonEvent::Long;
    }
    // A held Double matures the moment the triple window closes — lamp
    // on while the finger is still down, exactly like the stock grammar,
    // just one window later.
    if (pending_double_ &&
        (uint32_t)(now - double_at_) >= DOUBLE_GAP_MS) {
      pending_double_ = false;
      return ButtonEvent::Double;
    }
    // A lone tap matures into Short once the double window closes.
    if (pending_short_ && !level_ &&
        (uint32_t)(now - short_at_) >= DOUBLE_GAP_MS) {
      pending_short_ = false;
      return ButtonEvent::Short;
    }
    return ButtonEvent::None;
  }

 private:
  bool raw_ = false;         // last observed level
  bool level_ = false;       // debounced level
  bool triple_enabled_ = false;
  uint32_t edge_at_ = 0;
  uint32_t down_at_ = 0;
  uint32_t short_at_ = 0;
  uint32_t double_at_ = 0;
  bool pending_short_ = false;
  bool pending_double_ = false;
  bool long_fired_ = false;
};

}  // namespace canary::io
