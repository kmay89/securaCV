// firmware/projects/canary-display/tests_host/test_bedside_models.cpp
//
// Host tests for the three pure bedside models the Nightstand 7 wave adds:
//   care/lantern.h       — the honest night light (timeout, attention veto,
//                          the opt-in auto schedule, the scene ring)
//   care/ambient_life.h  — rationed organic check-ins (cadence bounds,
//                          the idle/lit gate, determinism)
//   io/boot_button.h     — short / double / long press classification
//
// All three are header-only integer logic with no Arduino, LVGL or storage
// dependency, so the invariants that make them SAFE (a lamp can never
// outlive its window or survive an alarm; a "moment" can never fire into a
// dark room; a hold is never also a tap) are proven here, before any of it
// reaches a board.
//
// Build/run via the tests_host Makefile.
#include "canary/care/lantern.h"
#include "canary/care/ambient_life.h"
#include "canary/io/boot_button.h"

#include <cstdio>

using canary::care::AmbientLife;
using canary::care::LanternModel;
using canary::care::LifeMoment;
using canary::io::ButtonClassifier;
using canary::io::ButtonEvent;

static int g_fail = 0;
#define CHECK(cond, msg)                                        \
  do {                                                          \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; }     \
  } while (0)

// ── Lantern ───────────────────────────────────────────────────────────────

static void test_lantern_timeout() {
  printf("lantern timeout...\n");
  LanternModel l;
  l.configure(/*scene=*/6, /*minutes=*/15, canary::care::LANTERN_AUTO_OFF);

  CHECK(!l.active(1000, /*night=*/true, /*attention=*/false),
        "dark until summoned");

  l.summon(1000);
  CHECK(l.active(1000, true, false), "lit right after the summon");
  CHECK(l.active(1000 + 14 * 60000, true, false), "still lit at 14 min");
  CHECK(l.summoned(1000 + 14 * 60000), "reports as a summoned window");
  // The window closes exactly at `minutes`, and stays closed.
  CHECK(!l.active(1000 + 15 * 60000, true, false), "out at 15 min");
  CHECK(!l.active(1000 + 20 * 60000, true, false), "stays out after");
  CHECK(!l.summoned(1000 + 20 * 60000), "no longer a summoned window");

  // Re-summoning restarts the window (the natural "keep it on" gesture).
  l.summon(1000000);
  CHECK(l.active(1000000 + 10 * 60000, true, false), "re-summon restarts");
  l.summon(1000000 + 10 * 60000);
  CHECK(l.active(1000000 + 24 * 60000, true, false),
        "second summon extends past the first window");
}

static void test_lantern_attention_veto() {
  printf("lantern attention veto...\n");
  LanternModel l;
  l.configure(6, 15, canary::care::LANTERN_AUTO_OFF);
  l.summon(1000);
  CHECK(l.active(2000, true, false), "lit while calm");

  // The invariant: attention takes the glass back immediately...
  CHECK(!l.active(3000, true, /*attention=*/true), "attention extinguishes");
  // ...and it stays out afterwards. A lamp must not silently resume over an
  // alarm the user just dealt with — re-lighting is a decision, not a
  // rebound.
  CHECK(!l.active(4000, true, false), "does not resume after the alarm");
  l.summon(5000);
  CHECK(l.active(6000, true, false), "an explicit re-summon works");
}

static void test_lantern_auto_schedule() {
  printf("lantern auto schedule...\n");
  LanternModel off;
  off.configure(6, 15, canary::care::LANTERN_AUTO_OFF);
  CHECK(!off.active(1000, /*night=*/true, false),
        "auto OFF: night alone never lights it (dark still means safe)");

  LanternModel on;
  on.configure(6, 15, canary::care::LANTERN_AUTO_NIGHT);
  CHECK(on.active(1000, /*night=*/true, false), "auto NIGHT: lit at night");
  CHECK(!on.active(1000, /*night=*/false, false), "auto NIGHT: dark by day");
  CHECK(!on.summoned(1000), "the schedule is not a summoned window");
  CHECK(on.remaining_s(1000) == 0, "the schedule has no countdown");
  // The veto is identical for the schedule — an opt-in convenience never
  // buys an exemption from honesty.
  CHECK(!on.active(2000, true, /*attention=*/true),
        "auto NIGHT still yields to attention");
}

static void test_lantern_scene_ring() {
  printf("lantern scene ring...\n");
  LanternModel l;
  l.configure(/*scene=*/8, 15, canary::care::LANTERN_AUTO_OFF);
  l.cycle_scene(10);
  CHECK(l.scene() == 9, "cycles forward");
  l.cycle_scene(10);
  CHECK(l.scene() == 0, "wraps to the start");
  // A zero count must not divide by zero.
  const uint8_t before = l.scene();
  l.cycle_scene(0);
  CHECK(l.scene() == before, "zero scene count is a no-op");
  // Minutes are clamped to at least 1 — a zero-minute lamp is a bug, not a
  // preference.
  l.configure(0, 0, canary::care::LANTERN_AUTO_OFF);
  CHECK(l.minutes() >= 1, "minutes clamped to >= 1");
}

// ── Ambient life ──────────────────────────────────────────────────────────

static void test_ambient_life_cadence() {
  printf("ambient life cadence...\n");
  AmbientLife a;
  a.seed(12345);

  // Nothing fires immediately: the first allowed tick only schedules.
  CHECK(a.step(0, true, false) == LifeMoment::None, "first tick schedules");

  // Walk a simulated day of allowed ticks and collect the gaps.
  uint32_t last = 0;
  int moments = 0;
  uint32_t min_gap = 0xFFFFFFFFu, max_gap = 0;
  for (uint32_t t = 1000; t < 6UL * 3600UL * 1000UL; t += 1000) {
    if (a.step(t, /*allowed=*/true, /*night=*/false) != LifeMoment::None) {
      if (moments) {
        const uint32_t gap = t - last;
        if (gap < min_gap) min_gap = gap;
        if (gap > max_gap) max_gap = gap;
      }
      last = t;
      moments++;
    }
  }
  CHECK(moments > 30, "moments do happen over six hours");
  // The budget: minutes apart, never a stream. 1 s tick granularity is the
  // only slack allowed either side.
  CHECK(min_gap >= AmbientLife::DAY_MIN_MS - 1000, "never faster than 3 min");
  CHECK(max_gap <= AmbientLife::DAY_MAX_MS + 1000, "never slower than 7 min");

  // Night is calmer than day.
  AmbientLife n;
  n.seed(999);
  uint32_t nlast = 0, nmin = 0xFFFFFFFFu;
  int nmoments = 0;
  for (uint32_t t = 1000; t < 6UL * 3600UL * 1000UL; t += 1000) {
    if (n.step(t, true, /*night=*/true) != LifeMoment::None) {
      if (nmoments && t - nlast < nmin) nmin = t - nlast;
      nlast = t;
      nmoments++;
    }
  }
  CHECK(nmin >= AmbientLife::NIGHT_MIN_MS - 1000,
        "night moments never faster than 8 min");
  CHECK(nmoments < moments, "night stirs less often than day");
}

static void test_ambient_life_gate() {
  printf("ambient life gate...\n");
  AmbientLife a;
  a.seed(7);
  // Not allowed = never a moment, however long we wait. This is the rule
  // that keeps the layer out of an honest darkness and off an alarm screen.
  for (uint32_t t = 0; t < 2UL * 3600UL * 1000UL; t += 1000) {
    if (a.step(t, /*allowed=*/false, false) != LifeMoment::None) {
      CHECK(false, "a moment fired while not allowed");
      break;
    }
  }
  // And re-allowance does not release a burst of stored-up moments: the
  // first allowed tick after a gap only re-schedules.
  CHECK(a.step(2UL * 3600UL * 1000UL, true, false) == LifeMoment::None,
        "no burst when the gate reopens");

  // Determinism: same seed, same tick sequence, same moments.
  AmbientLife x, y;
  x.seed(4242);
  y.seed(4242);
  bool identical = true;
  for (uint32_t t = 0; t < 3600UL * 1000UL; t += 1000) {
    if (x.step(t, true, false) != y.step(t, true, false)) identical = false;
  }
  CHECK(identical, "same seed reproduces the same cadence");

  // Different devices drift apart (the reason the seed is per-device).
  AmbientLife p, q;
  p.seed(1);
  q.seed(2);
  bool differ = false;
  for (uint32_t t = 0; t < 3600UL * 1000UL; t += 1000) {
    if (p.step(t, true, false) != q.step(t, true, false)) differ = true;
  }
  CHECK(differ, "different seeds do not stir in lockstep");
}

// ── BOOT button ───────────────────────────────────────────────────────────

// Feed a level for `ms` milliseconds at 10 ms granularity, returning the
// first non-None event seen (and consuming the rest of the window).
static ButtonEvent hold(ButtonClassifier& b, bool level, uint32_t& t,
                        uint32_t ms) {
  ButtonEvent found = ButtonEvent::None;
  const uint32_t end = t + ms;
  for (; t < end; t += 10) {
    const ButtonEvent e = b.step(level, t);
    if (e != ButtonEvent::None && found == ButtonEvent::None) found = e;
  }
  return found;
}

static void test_button_short() {
  printf("button short...\n");
  ButtonClassifier b;
  uint32_t t = 1000;
  hold(b, false, t, 200);                       // settle
  CHECK(hold(b, true, t, 120) == ButtonEvent::None, "no event mid-tap");
  // Release, then wait past the double window: the tap matures into Short.
  CHECK(hold(b, false, t, 500) == ButtonEvent::Short, "a lone tap is Short");
}

static void test_button_double() {
  printf("button double...\n");
  ButtonClassifier b;
  uint32_t t = 1000;
  hold(b, false, t, 200);
  hold(b, true, t, 120);                        // press 1
  hold(b, false, t, 100);                       // gap, inside the window
  CHECK(hold(b, true, t, 120) == ButtonEvent::Double,
        "second press inside the window is Double");
  // The release of a Double is silent — it must not also mature into Short.
  CHECK(hold(b, false, t, 600) == ButtonEvent::None,
        "release after a Double emits nothing");

  // Holding the second press must not ALSO acknowledge: a double-then-hold
  // is one gesture, not two.
  ButtonClassifier c;
  uint32_t u = 1000;
  hold(c, false, u, 200);
  hold(c, true, u, 120);
  hold(c, false, u, 100);
  CHECK(hold(c, true, u, 120) == ButtonEvent::Double, "double fires");
  CHECK(hold(c, true, u, 1500) == ButtonEvent::None,
        "keeping the second press held does not acknowledge");

  // Two taps FURTHER apart than the window are two Shorts, not a Double.
  ButtonClassifier d;
  uint32_t v = 1000;
  hold(d, false, v, 200);
  hold(d, true, v, 120);
  CHECK(hold(d, false, v, 500) == ButtonEvent::Short, "first tap is Short");
  hold(d, true, v, 120);
  CHECK(hold(d, false, v, 500) == ButtonEvent::Short, "second tap is Short");
}

static void test_button_long() {
  printf("button long...\n");
  ButtonClassifier b;
  uint32_t t = 1000;
  hold(b, false, t, 200);
  // Long fires WHILE held — it must work without waiting for a release.
  CHECK(hold(b, true, t, 1200) == ButtonEvent::Long, "held press is Long");
  // And the release afterwards is silent: a hold is never also a tap.
  CHECK(hold(b, false, t, 600) == ButtonEvent::None,
        "release after a Long emits nothing");
}

static void test_button_debounce() {
  printf("button debounce...\n");
  ButtonClassifier b;
  uint32_t t = 1000;
  hold(b, false, t, 200);
  // Chatter well under the debounce window must not register as presses.
  for (int i = 0; i < 6; i++) {
    b.step(true, t); t += 5;
    b.step(false, t); t += 5;
  }
  CHECK(hold(b, false, t, 600) == ButtonEvent::None, "chatter is swallowed");
}

int main() {
  printf("== bedside model host tests ==\n");
  test_lantern_timeout();
  test_lantern_attention_veto();
  test_lantern_auto_schedule();
  test_lantern_scene_ring();
  test_ambient_life_cadence();
  test_ambient_life_gate();
  test_button_short();
  test_button_double();
  test_button_long();
  test_button_debounce();
  if (g_fail) { printf("\n%d CHECK(s) FAILED\n", g_fail); return 1; }
  printf("\nAll bedside-model checks passed.\n");
  return 0;
}
