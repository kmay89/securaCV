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
//   - both polarities: a short press is a Tap; a held press powers off
//     only after its release, once the rest has settled for SETTLE_MS
//   - a press during the settle drops the pending power-off
//   - a level present when watching began can never power the device off
//   - a mislearn (key held through learning) never powers off on a tap —
//     the review's reproduction (LOW 4.5 s, HIGH 5 s, a 200 ms tap) is
//     pinned — and re-learns the true resting level after STUCK_MS
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

  r.hold(press, 200);
  r.hold(rest, 200);
  CHECK(r.taps == 1, "a short press is a Tap");
  CHECK(r.offs == 0, "a short press never powers off");

  r.hold(press, HOLD + 500);
  CHECK(r.offs == 0, "nothing fires while the key is still held");
  const uint32_t released_at = r.t;
  r.hold(rest, PwrKeyModel::SETTLE_MS + 500);
  CHECK(r.offs == 1, "a long hold, released, powers off once");
  CHECK(r.first_off_at >= released_at + PwrKeyModel::SETTLE_MS &&
            r.first_off_at < released_at + PwrKeyModel::SETTLE_MS + 100,
        "it fires when the rest has settled for SETTLE_MS");
  CHECK(r.taps == 1, "and the long hold is not a Tap");
}

static void test_press_during_settle_cancels() {
  printf("a press during the settle drops the pending power-off\n");
  Run r;
  r.hold(true, PwrKeyModel::LEARN_MS + 100);
  r.hold(false, HOLD + 200);
  r.hold(true, 500);   // released, but not settled yet
  CHECK(r.m.off_pending(), "the power-off is pending");
  r.hold(false, 200);  // pressed again inside the settle
  r.hold(true, PwrKeyModel::SETTLE_MS + 500);
  CHECK(r.offs == 0, "the interrupted rest never powers off");
  CHECK(r.taps == 1, "the second short press is its own Tap");
}

static void test_mislearn_tap_never_powers_off() {
  printf("mislearn: the review's reproduction no longer powers off\n");
  // Board rests HIGH; PWR (LOW) held through learning; released; then a
  // 200 ms tap 5 s later. The first version fired 30 ms into that tap.
  Run r;
  r.hold(false, 4500);
  r.hold(true, 5000);
  r.hold(false, 200);
  r.hold(true, 2000);
  CHECK(r.offs == 0, "a tap after a mislearn is never a power-off");
  // Any number of taps, at any spacing inside STUCK_MS.
  for (int i = 0; i < 6; i++) {
    r.hold(false, 150);
    r.hold(true, 900 + 400 * i);
  }
  CHECK(r.offs == 0, "nor are repeated taps");
}

static void test_mislearn_is_harmless() {
  printf("mislearn: untouched, it re-learns and then behaves\n");
  Run r;
  r.hold(false, PwrKeyModel::LEARN_MS + 500);
  CHECK(r.m.learned() && !r.m.idle_high(), "mislearned LOW as resting");
  r.hold(true, PwrKeyModel::STUCK_MS + 1000);
  CHECK(r.offs == 0 && r.taps == 0, "the endless 'press' does nothing");
  CHECK(r.m.idle_high(), "after STUCK_MS it re-learned HIGH as resting");
  CHECK(!r.m.pressed(), "and dropped the phantom press");
  r.hold(false, 200);
  r.hold(true, 200);
  CHECK(r.taps == 1, "a real short press now taps");
  r.hold(false, HOLD + 200);
  r.hold(true, PwrKeyModel::SETTLE_MS + 200);
  CHECK(r.offs == 1, "a real long hold now powers off");
}

static void test_mislearn_needs_a_deliberate_hold() {
  printf("mislearn: only a deliberate SETTLE_MS hold on the real key powers off\n");
  // The one residual, pinned so it cannot quietly grow: after a mislearn,
  // the real key held for SETTLE_MS reads as the settled rest after a long
  // phantom hold. A deliberate hold, never a tap.
  Run r;
  r.hold(false, 4500);
  r.hold(true, 5000);
  r.hold(false, PwrKeyModel::SETTLE_MS - 200);
  CHECK(r.offs == 0, "a hold shorter than SETTLE_MS does not");
  r.hold(true, 5000);
  r.hold(false, PwrKeyModel::SETTLE_MS + 200);
  CHECK(r.offs == 1, "a SETTLE_MS hold does: the documented residual");
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
}

int main() {
  test_rests_low_never_fires();
  test_nothing_before_learned();
  polarity_round_trip(true);
  polarity_round_trip(false);
  test_press_during_settle_cancels();
  test_mislearn_tap_never_powers_off();
  test_mislearn_is_harmless();
  test_mislearn_needs_a_deliberate_hold();
  test_level_at_boot_cannot_fire();
  test_bounce_ignored();
  if (g_fail) {
    printf("%d FAILED\n", g_fail);
    return 1;
  }
  printf("ALL PWR KEY TESTS PASSED\n");
  return 0;
}
