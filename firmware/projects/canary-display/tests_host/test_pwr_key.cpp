// firmware/projects/canary-display/tests_host/test_pwr_key.cpp
//
// Host tests for the AMOLED 2.41's PWR-key model (canary/io/pwr_key.h) —
// the piece that keeps an unverified key polarity from powering a working
// glass off three seconds after boot.
//
// The invariants proven off-board:
//   - nothing fires before the resting level is learned
//   - a key that RESTS LOW (the case the old active-low glue powered off)
//     never fires on its own, however long it sits
//   - both polarities: a short press is a Tap, a held press is PowerOff
//   - before the first round trip, a hold powers off only on release;
//     after it, the deadline fires while the key is still held
//   - a level present when watching began can never power the device off
//   - a mislearn (key held through learning) costs a dead key for
//     STUCK_MS, never a power-off, and then re-learns the true resting level
//   - bounces shorter than the debounce never count
//
// Build/run via the tests_host Makefile.
#include "canary/io/pwr_key.h"

#include <cstdio>

using canary::io::PwrKeyEvent;
using canary::io::PwrKeyModel;

static int g_fail = 0;
#define CHECK(cond, msg)                                        \
  do {                                                          \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; }     \
  } while (0)

static constexpr uint32_t HOLD = 3000;
static constexpr uint32_t STEP = 10;  // the loop's poll cadence, roughly

// Feed `level` from t to t+ms in STEP increments; count each event kind.
struct Run {
  PwrKeyModel m{HOLD};
  uint32_t t = 0;
  int taps = 0;
  int offs = 0;
  uint32_t first_off_at = 0;
  void hold(bool level_high, uint32_t ms) {
    const uint32_t end = t + ms;
    for (; t < end; t += STEP) {
      const PwrKeyEvent e = m.feed(level_high, t);
      if (e == PwrKeyEvent::Tap) taps++;
      if (e == PwrKeyEvent::PowerOff) {
        if (offs == 0) first_off_at = t;
        offs++;
      }
    }
  }
};

static void test_rests_low_never_fires() {
  printf("rests LOW: the old active-low glue's failure never happens\n");
  Run r;
  r.hold(false, 120000);  // two minutes untouched, pin LOW the whole time
  CHECK(r.offs == 0, "a LOW-resting key never powers off on its own");
  CHECK(r.taps == 0, "and never taps on its own");
  CHECK(r.m.learned(), "it learned the resting level");
  CHECK(!r.m.idle_high(), "and the resting level is LOW");
  CHECK(r.m.pressed_high(), "so a press drives the pin HIGH (wake arms HIGH)");
}

static void test_nothing_before_learned() {
  printf("nothing fires before the resting level is learned\n");
  Run r;
  r.hold(true, 1000);
  r.hold(false, 2500);  // a "press" during learning
  r.hold(true, 100);
  CHECK(!r.m.learned(), "still learning: no level has held LEARN_MS");
  CHECK(r.taps == 0 && r.offs == 0, "no event while learning");
}

static void polarity_round_trip(bool idle_high) {
  printf("polarity: rests %s\n", idle_high ? "HIGH" : "LOW");
  const bool rest = idle_high, press = !idle_high;
  Run r;
  r.hold(rest, PwrKeyModel::LEARN_MS + 100);
  CHECK(r.m.learned() && r.m.idle_high() == idle_high, "learned the resting level");

  // First a short press: a Tap, and the round trip confirms the polarity.
  r.hold(press, 200);
  r.hold(rest, 200);
  CHECK(r.taps == 1, "a short press is a Tap");
  CHECK(r.offs == 0, "a short press never powers off");
  CHECK(r.m.confirmed(), "the round trip confirmed the polarity");

  // Then a long hold: confirmed, so the deadline fires while still held.
  const uint32_t pressed_at = r.t;
  r.hold(press, HOLD + 500);
  CHECK(r.offs == 1, "a confirmed hold fires PowerOff once");
  CHECK(r.first_off_at >= pressed_at + HOLD &&
            r.first_off_at < pressed_at + HOLD + 100,
        "it fires at the hold deadline, while the key is still down");
  r.hold(rest, 200);
  CHECK(r.offs == 1, "the release after the deadline adds nothing");
  CHECK(r.taps == 1, "and is not a Tap");
}

static void test_unconfirmed_hold_fires_on_release() {
  printf("before the first round trip, a hold powers off on release\n");
  Run r;
  r.hold(true, PwrKeyModel::LEARN_MS + 100);
  r.hold(false, HOLD + 1000);
  CHECK(r.offs == 0, "no deadline fire before the polarity is confirmed");
  r.hold(true, 100);
  CHECK(r.offs == 1, "the release of a long hold is the PowerOff");
  CHECK(r.taps == 0, "and not a Tap");
}

static void test_mislearn_is_harmless() {
  printf("mislearn: key held through learning never powers off\n");
  // A board that rests HIGH, but the user is still holding PWR (LOW) when
  // loop() starts, and keeps holding past LEARN_MS.
  Run r;
  r.hold(false, PwrKeyModel::LEARN_MS + 500);
  CHECK(r.m.learned() && !r.m.idle_high(), "mislearned LOW as resting");
  // They let go: the true resting level now looks like an endless press.
  r.hold(true, PwrKeyModel::STUCK_MS + 1000);
  CHECK(r.offs == 0, "the endless 'press' never powers off");
  CHECK(r.taps == 0, "and never taps");
  CHECK(r.m.idle_high(), "after STUCK_MS it re-learned HIGH as resting");
  CHECK(!r.m.pressed(), "and dropped the phantom press");
  // From here the key behaves exactly like a correctly learned one.
  r.hold(false, 200);
  r.hold(true, 200);
  CHECK(r.taps == 1, "a real short press now taps");
  r.hold(false, HOLD + 200);
  CHECK(r.offs == 1, "a real confirmed hold now powers off");
}

static void test_level_at_boot_cannot_fire() {
  printf("a level present when watching began can never power off\n");
  Run r;
  // Held at loop start for less than LEARN_MS, then released: learning
  // restarts on the release and settles on the true resting level.
  r.hold(false, 2000);
  r.hold(true, PwrKeyModel::LEARN_MS + 100);
  CHECK(r.offs == 0 && r.taps == 0, "the boot-time hold produced nothing");
  CHECK(r.m.idle_high(), "resting level learned after the release");
}

static void test_bounce_ignored() {
  printf("bounces shorter than the debounce never count\n");
  Run r;
  r.hold(true, PwrKeyModel::LEARN_MS + 100);
  for (int i = 0; i < 20; i++) {
    r.hold(false, STEP);  // 10 ms blips, under DEBOUNCE_MS
    r.hold(true, 50);
  }
  CHECK(r.taps == 0 && r.offs == 0, "contact chatter is not a press");
  CHECK(!r.m.confirmed(), "and confirms nothing");
}

int main() {
  test_rests_low_never_fires();
  test_nothing_before_learned();
  polarity_round_trip(true);
  polarity_round_trip(false);
  test_unconfirmed_hold_fires_on_release();
  test_mislearn_is_harmless();
  test_level_at_boot_cannot_fire();
  test_bounce_ignored();
  if (g_fail) {
    printf("%d FAILED\n", g_fail);
    return 1;
  }
  printf("ALL PWR KEY TESTS PASSED\n");
  return 0;
}
