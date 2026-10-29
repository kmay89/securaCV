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
// "not the resting level", and only a press that BEGAN after learning
// counts — a level that was already there when we started watching never
// powers the device off.
//
// Power-off is decided AFTER the release, never on a deadline while held:
// a press of at least HOLD_MS, released, and then the resting level held
// for SETTLE_MS. That last clause is what makes a mislearn safe. If the
// user keeps PWR held through LEARN_MS, the model takes the pressed level
// for resting; when they let go, the true resting level looks like an
// endless press. A deadline would power off 3 s later. A round trip cannot
// tell either: the user's next TAP looks exactly like the release of a long
// hold (the review of the first version reproduced a power-off 30 ms into
// a 200 ms tap). But a tap lets go within a moment, so its "rest" never
// settles, and the candidate is dropped. In a mislearned state only a
// deliberate hold of SETTLE_MS on the real key can power off — never a
// tap — and a "press" that outlasts STUCK_MS is re-learned as the resting
// level, which ends the mislearn for good.
//
// The cost on a correctly learned board: the glass goes dark SETTLE_MS
// after the finger lifts, not at the 3 s mark while it is still down.
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
  static constexpr uint32_t SETTLE_MS = 1500;    // rest after a long hold

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
    if (down_ && (uint32_t)(now - down_since_) >= STUCK_MS) {
      // Never a press: the level we took for "pressed" is where the pin
      // rests. Re-learn it and drop the press.
      idle_high_ = stable_;
      down_ = false;
      return PwrKeyEvent::None;
    }
    // A long hold was released: power off once the rest has SETTLED.
    if (off_pending_ && !down_ && (uint32_t)(now - off_since_) >= SETTLE_MS) {
      off_pending_ = false;
      return PwrKeyEvent::PowerOff;
    }
    return PwrKeyEvent::None;
  }

  bool learned() const { return learned_; }
  // The resting level, once learned. Meaningless before learned().
  bool idle_high() const { return idle_high_; }
  // The level a press drives the pin to — what a wake-on-press arms for.
  bool pressed_high() const { return !idle_high_; }
  bool pressed() const { return learned_ && down_; }
  // A long hold was released and the rest is settling toward a power-off.
  bool off_pending() const { return off_pending_; }

 private:
  PwrKeyEvent on_edge(uint32_t now) {
    const bool is_pressed = (stable_ != idle_high_);
    if (is_pressed) {
      // A press during the settle means the "rest" was not one: whatever
      // was pending is dropped (a tap after a mislearn ends up here).
      off_pending_ = false;
      down_ = true;
      down_since_ = now;
      return PwrKeyEvent::None;
    }
    if (!down_) return PwrKeyEvent::None;
    down_ = false;
    if ((uint32_t)(now - down_since_) >= hold_ms_) {
      off_pending_ = true;
      off_since_ = now;
      return PwrKeyEvent::None;
    }
    return PwrKeyEvent::Tap;
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
  uint32_t down_since_ = 0;
  bool off_pending_ = false;
  uint32_t off_since_ = 0;
};

}  // namespace canary::io
