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
//
// Pure logic — the glue in main.cpp reads the GPIO and feeds level+now.
// A Short is only emitted after the double-tap window closes without a
// second press, so the two gestures never fire together. Host-tested in
// tests_host/test_boot_button.cpp.
#pragma once
#include <stdint.h>

namespace canary::io {

enum class ButtonEvent : uint8_t { None = 0, Short, Double, Long };

class ButtonClassifier {
 public:
  static constexpr uint32_t DEBOUNCE_MS = 30;
  static constexpr uint32_t LONG_MS = 900;      // = CD_LONGPRESS_MS
  static constexpr uint32_t DOUBLE_GAP_MS = 350;

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
        if (pending_short_ &&
            (uint32_t)(now - short_at_) < DOUBLE_GAP_MS) {
          // Second press inside the window. Fire on the PRESS, not the
          // release: a double-press summons a light, and a light has to
          // come on when you push the button, not when you let go.
          pending_short_ = false;
          // This press is spoken for — holding it must not then also
          // acknowledge, and its release must not mature into a Short.
          long_fired_ = true;
          return ButtonEvent::Double;
        }
      } else if (!long_fired_) {             // release edge, not spoken for
        pending_short_ = true;
        short_at_ = now;
      } else {
        long_fired_ = false;                 // Long/Double already reported
      }
    }
    // Long fires while still held — it must work half-asleep, without
    // waiting for a release.
    if (level_ && !long_fired_ && (uint32_t)(now - down_at_) >= LONG_MS) {
      long_fired_ = true;
      pending_short_ = false;                // a hold is never also a tap
      return ButtonEvent::Long;
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
  uint32_t edge_at_ = 0;
  uint32_t down_at_ = 0;
  uint32_t short_at_ = 0;
  bool pending_short_ = false;
  bool long_fired_ = false;
};

}  // namespace canary::io
