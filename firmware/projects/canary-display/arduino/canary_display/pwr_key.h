// include/canary/io/pwr_key.h — the AMOLED 2.41's case PWR key, pure model.
//
// The Waveshare ESP32-S3-Touch-AMOLED-2.41 routes its PWR button to a GPIO
// (pins.h PWR_KEY_PIN, "KEY_BAT") through the board's soft power latch. Our
// pin map was transcribed, not bench-verified, and nothing we can read says
// which level that GPIO rests at: a key diode-isolated from the latch rests
// HIGH under a pull-up, but a sense line taken off the latched node rests
// LOW for as long as the board is on. The old glue assumed active-low. On a
// board whose line rests LOW that assumption reads as "PWR held" from the
// first loop pass, so three seconds later the glass powered itself off —
// and on USB power it then waited forever for a release that never came.
// A dark glass with a perfectly good panel behind it.
//
// So this model never assumes. It LEARNS the resting level: the first level
// that holds, untouched, for LEARN_MS after loop() starts. Until it has
// learned, the key does nothing at all. Once learned, "pressed" is simply
// "not the resting level", and power-off needs a press that BEGAN after
// learning and was held HOLD_MS — a level that was already there when we
// started watching can never power the device off.
//
// Why LEARN_MS is long: on battery the user holds PWR to boot, but loop()
// runs only after the splash, provisioning and the Wi-Fi join, so the
// finger is long gone. A user would have to keep holding PWR for LEARN_MS
// more after the face appeared to mislearn. Two rules make even that safe:
//
//   - CONFIRMED before the deadline fires. Until one full press-and-release
//     has been seen, a hold powers off only on its RELEASE (a release proves
//     the level really was a press). After that first round trip the
//     deadline fires while the key is still down, the "learn the hold by
//     feel" gesture the case button is meant to have.
//   - A press that outlasts STUCK_MS is not a press: it is the resting
//     level we mislearned. The model re-learns it as resting and forgets
//     the press — no event, no power-off.
//
// So a mislearn costs a key that does nothing for STUCK_MS, never a
// surprise power-off.
//
// Pure logic: the glue in main.cpp reads the GPIO and feeds level + now.
// Host-tested in tests_host/test_pwr_key.cpp.
#pragma once
#include <stdint.h>

namespace canary::io {

enum class PwrKeyEvent : uint8_t { None = 0, Tap, PowerOff };

class PwrKeyModel {
 public:
  static constexpr uint32_t LEARN_MS = 4000;     // untouched level = resting
  static constexpr uint32_t DEBOUNCE_MS = 30;
  static constexpr uint32_t STUCK_MS = 15000;    // no human holds this long

  explicit PwrKeyModel(uint32_t hold_ms = 3000) : hold_ms_(hold_ms) {}

  // Feed every poll with the raw GPIO level (true = HIGH).
  PwrKeyEvent feed(bool level_high, uint32_t now) {
    if (!started_) {
      started_ = true;
      raw_ = level_high;
      raw_since_ = now;
      stable_ = level_high;
      stable_since_ = now;
    }
    // Debounce: a raw level must hold DEBOUNCE_MS before it is the stable one.
    if (level_high != raw_) {
      raw_ = level_high;
      raw_since_ = now;
    }
    if (raw_ != stable_ && (uint32_t)(now - raw_since_) >= DEBOUNCE_MS) {
      stable_ = raw_;
      stable_since_ = now;
      if (learned_) return on_edge(now);
    }
    if (!learned_) {
      if ((uint32_t)(now - stable_since_) >= LEARN_MS) {
        learned_ = true;
        idle_high_ = stable_;
      }
      return PwrKeyEvent::None;
    }
    if (down_) {
      const uint32_t held = (uint32_t)(now - down_since_);
      if (held >= STUCK_MS) {
        // Never a press: the level we took for "pressed" is where the pin
        // rests. Re-learn it, drop the press, start confirmation over.
        idle_high_ = stable_;
        down_ = false;
        confirmed_ = false;
        return PwrKeyEvent::None;
      }
      // Hold deadline: fires once, while still held, on a press that began
      // after learning (down_ is only ever set by on_edge) — and only once
      // a full round trip has confirmed which level is "pressed".
      if (confirmed_ && !fired_ && held >= hold_ms_) {
        fired_ = true;
        return PwrKeyEvent::PowerOff;
      }
    }
    return PwrKeyEvent::None;
  }

  bool learned() const { return learned_; }
  // The resting level, once learned. Meaningless before learned().
  bool idle_high() const { return idle_high_; }
  // The level a press drives the pin to — what a wake-on-press arms for.
  bool pressed_high() const { return !idle_high_; }
  bool pressed() const { return learned_ && down_; }
  bool confirmed() const { return confirmed_; }

 private:
  PwrKeyEvent on_edge(uint32_t now) {
    const bool is_pressed = (stable_ != idle_high_);
    if (is_pressed) {
      down_ = true;
      fired_ = false;
      down_since_ = now;
      return PwrKeyEvent::None;
    }
    // Released: the round trip that proves the polarity.
    if (!down_) return PwrKeyEvent::None;
    down_ = false;
    if (fired_) return PwrKeyEvent::None;  // the deadline already acted
    const uint32_t held = (uint32_t)(now - down_since_);
    confirmed_ = true;
    return held >= hold_ms_ ? PwrKeyEvent::PowerOff : PwrKeyEvent::Tap;
  }

  uint32_t hold_ms_;
  bool started_ = false;
  bool raw_ = false;
  uint32_t raw_since_ = 0;
  bool stable_ = false;
  uint32_t stable_since_ = 0;
  bool learned_ = false;
  bool idle_high_ = true;
  bool down_ = false;
  bool fired_ = false;
  bool confirmed_ = false;
  uint32_t down_since_ = 0;
};

}  // namespace canary::io
