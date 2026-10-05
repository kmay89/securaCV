// Host tests for common/doorbell/doorbell_logic.h — the Vision Doorbell's
// button and glow ring. Each button case is a way the doorbell could ring
// the house when nobody rang (bounce, a boot with the button held, a
// jammed plunger) or stay silent when somebody did (a short tap seen only
// as ISR edges, a millis() wrap). Each glow case is a promise the ring
// makes to the visitor: it breathes while the witness is on, breathes
// differently when the hub is gone, swells once when a ring is sealed,
// never goes dark while the doorbell is on, and never flashes.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdio>
#include <cstdlib>

#include "../common/doorbell/doorbell_logic.h"

using doorbell::ButtonEvent;
using doorbell::ButtonState;
using doorbell::GlowMode;
using doorbell::GlowState;

static int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      return 1;                                                              \
    }                                                                        \
    ++g_checks;                                                              \
  } while (0)

// Poll the level every `step` ms for `dur` ms; count each event kind.
struct Tally {
  int ring = 0, repeat = 0, stuck = 0, cleared = 0;
};
static void poll(ButtonState* s, bool pressed, uint32_t* t, uint32_t dur, uint32_t step, Tally* n) {
  for (uint32_t e = 0; e < dur; e += step) {
    switch (doorbell::sample(s, pressed, *t)) {
      case ButtonEvent::RING: ++n->ring; break;
      case ButtonEvent::REPEAT: ++n->repeat; break;
      case ButtonEvent::STUCK: ++n->stuck; break;
      case ButtonEvent::STUCK_CLEARED: ++n->cleared; break;
      case ButtonEvent::NONE: break;
    }
    *t += step;
  }
}

static int test_one_press_one_ring() {
  uint32_t t = 1000;
  ButtonState s = doorbell::begin(false, t);
  Tally n;
  poll(&s, false, &t, 200, 5, &n);
  poll(&s, true, &t, 400, 5, &n);   // a normal press
  poll(&s, false, &t, 200, 5, &n);
  CHECK(n.ring == 1 && n.repeat == 0 && n.stuck == 0);
  // held for 5 s: still one ring (and not stuck)
  t += 5000;
  poll(&s, true, &t, 5000, 5, &n);
  poll(&s, false, &t, 200, 5, &n);
  CHECK(n.ring == 2 && n.stuck == 0);
  return 0;
}

static int test_bounce_is_not_a_ring() {
  uint32_t t = 0;
  ButtonState s = doorbell::begin(false, t);
  Tally n;
  // a 20 ms burst of chatter (a knock, a cheap contact) never settles
  for (int i = 0; i < 10; ++i) {
    poll(&s, (i & 1) == 0, &t, 2, 1, &n);
  }
  poll(&s, false, &t, 500, 5, &n);
  CHECK(n.ring == 0);
  // a press whose leading edge bounces still rings exactly once
  for (int i = 0; i < 6; ++i) poll(&s, (i & 1) == 0, &t, 3, 1, &n);
  poll(&s, true, &t, 300, 5, &n);
  poll(&s, false, &t, 300, 5, &n);
  CHECK(n.ring == 1);
  return 0;
}

static int test_holdoff_repeats_are_counted_not_rung() {
  uint32_t t = 0;
  ButtonState s = doorbell::begin(false, t);
  Tally n;
  for (int i = 0; i < 5; ++i) {  // five jabs, 400 ms apart
    poll(&s, true, &t, 150, 5, &n);
    poll(&s, false, &t, 250, 5, &n);
  }
  CHECK(n.ring == 1 && n.repeat == 4 && s.repeats == 4);
  // after the holdoff, the next press rings again
  t += doorbell::kDefaultPolicy.holdoff_ms;
  poll(&s, true, &t, 150, 5, &n);
  poll(&s, false, &t, 150, 5, &n);
  CHECK(n.ring == 2);
  return 0;
}

static int test_held_at_boot_never_rings() {
  uint32_t t = 0;
  ButtonState s = doorbell::begin(true, t);  // jammed (or wired closed) at power-on
  Tally n;
  poll(&s, true, &t, 30000, 10, &n);
  CHECK(n.ring == 0 && n.stuck == 0);  // not armed: not a ring, not a "new" fault
  poll(&s, false, &t, 200, 5, &n);     // released: now armed, silently
  CHECK(n.ring == 0);
  poll(&s, true, &t, 200, 5, &n);
  poll(&s, false, &t, 200, 5, &n);
  CHECK(n.ring == 1);
  return 0;
}

static int test_stuck_reported_once_and_cleared() {
  uint32_t t = 0;
  ButtonState s = doorbell::begin(false, t);
  Tally n;
  poll(&s, false, &t, 100, 5, &n);
  poll(&s, true, &t, 40000, 10, &n);  // ice in the plunger
  CHECK(n.ring == 1 && n.stuck == 1 && s.stuck);
  poll(&s, false, &t, 200, 5, &n);
  CHECK(n.cleared == 1 && !s.stuck);
  return 0;
}

static int test_isr_edges_catch_a_tap_between_polls() {
  // The main loop may be inside a 300 ms NPU invoke for the whole press.
  // The ISR stamped the two edges; feeding them in order rings.
  uint32_t t = 5000;
  ButtonState s = doorbell::begin(false, t);
  int rings = 0;
  if (doorbell::sample(&s, true, 5100) == ButtonEvent::RING) ++rings;   // edge: down
  if (doorbell::sample(&s, false, 5180) == ButtonEvent::RING) ++rings;  // edge: up (80 ms later)
  if (doorbell::sample(&s, false, 5400) == ButtonEvent::RING) ++rings;  // the loop's own poll
  CHECK(rings == 1);
  // a 10 ms spike stamped by the ISR is bounce, not a tap
  ButtonState s2 = doorbell::begin(false, t);
  int r2 = 0;
  if (doorbell::sample(&s2, true, 6000) == ButtonEvent::RING) ++r2;
  if (doorbell::sample(&s2, false, 6010) == ButtonEvent::RING) ++r2;
  if (doorbell::sample(&s2, false, 6500) == ButtonEvent::RING) ++r2;
  CHECK(r2 == 0);
  return 0;
}

static int test_millis_wrap() {
  uint32_t t = 0xFFFFFF00u;  // 256 ms before the wrap
  ButtonState s = doorbell::begin(false, t);
  Tally n;
  poll(&s, false, &t, 100, 5, &n);
  poll(&s, true, &t, 300, 5, &n);  // crosses zero mid-press
  poll(&s, false, &t, 300, 5, &n);
  CHECK(n.ring == 1 && n.stuck == 0);
  return 0;
}

// ---------------------------------------------------------------------------

static int test_glow_never_flashes() {
  // Every mode change, every swell, every setting change: frame to frame
  // (20 ms, the device's cadence) the duty moves at most the slew limit.
  const uint32_t limit = 20 * doorbell::kSlewPerMs;
  uint32_t t = 0;
  GlowState g = doorbell::glow_begin(100, t);
  uint16_t last = doorbell::glow_step(&g, t);
  const GlowMode script[] = {GlowMode::AWAKE, GlowMode::OFF, GlowMode::UNSURE,
                             GlowMode::FAULT, GlowMode::AWAKE, GlowMode::OFF};
  for (GlowMode m : script) {
    g.mode = m;
    for (int f = 0; f < 600; ++f) {  // 12 s per mode
      t += 20;
      if (f == 100) doorbell::glow_swell(&g, t);
      if (f == 300) g.pct = (g.pct == 100) ? doorbell::kGlowPctMin : 100;
      const uint16_t d = doorbell::glow_step(&g, t);
      const uint32_t move = d > last ? d - last : last - d;
      CHECK(move <= limit);
      CHECK(d <= doorbell::kDutyMax);
      last = d;
    }
  }
  return 0;
}

static int test_glow_on_is_never_dark_off_is_dark() {
  uint32_t t = 0;
  GlowState g = doorbell::glow_begin(0, t);  // a setting of 0 clamps up
  CHECK(g.pct == doorbell::kGlowPctMin);
  const GlowMode on_modes[] = {GlowMode::AWAKE, GlowMode::UNSURE, GlowMode::FAULT};
  for (GlowMode m : on_modes) {
    g.mode = m;
    for (int f = 0; f < 1000; ++f) {  // 20 s, past the slew-in and two full breaths
      t += 20;
      const uint16_t d = doorbell::glow_step(&g, t);
      if (f > 50) CHECK(d > 0);
    }
  }
  g.mode = GlowMode::OFF;
  doorbell::glow_swell(&g, t);  // even a swell cannot light a switched-off ring
  for (int f = 0; f < 300; ++f) {
    t += 20;
    doorbell::glow_step(&g, t);
  }
  CHECK(g.duty == 0);
  return 0;
}

static int test_unsure_breathes_differently() {
  // UNSURE is slower and dimmer than AWAKE, so "lost the hub" can be told
  // apart from "fine" by looking: its peak sits below AWAKE's.
  uint32_t t = 0;
  GlowState a = doorbell::glow_begin(100, t), u = doorbell::glow_begin(100, t);
  a.mode = GlowMode::AWAKE;
  u.mode = GlowMode::UNSURE;
  uint16_t a_hi = 0, u_hi = 0;
  for (uint32_t ms = 0; ms < 24000; ms += 20) {
    const uint16_t da = doorbell::glow_target(&a, ms), du = doorbell::glow_target(&u, ms);
    if (da > a_hi) a_hi = da;
    if (du > u_hi) u_hi = du;
  }
  CHECK(u_hi < a_hi);
  CHECK(doorbell::kUnsureBreath.period_ms > doorbell::kAwakeBreath.period_ms);
  return 0;
}

static int test_swell_reaches_full_and_returns() {
  uint32_t t = 10000;
  GlowState g = doorbell::glow_begin(100, t);
  g.mode = GlowMode::AWAKE;
  for (int f = 0; f < 100; ++f) { t += 20; doorbell::glow_step(&g, t); }  // settle
  doorbell::glow_swell(&g, t);
  uint16_t peak = 0;
  for (uint32_t e = 0; e < doorbell::kSwellTotalMs + 2000; e += 20) {
    t += 20;
    const uint16_t d = doorbell::glow_step(&g, t);
    if (d > peak) peak = d;
  }
  CHECK(peak == doorbell::kDutyMax);  // full, at a 100 % setting
  CHECK(!g.swelling);
  // back on the breath: below the breath's own ceiling
  CHECK(g.duty <= doorbell::permille_to_duty(doorbell::kAwakeBreath.hi_permille, 100) + 20 * doorbell::kSlewPerMs);
  return 0;
}

static int test_mode_for() {
  CHECK(doorbell::glow_mode_for(false, true, true) == GlowMode::OFF);
  CHECK(doorbell::glow_mode_for(true, true, true) == GlowMode::FAULT);
  CHECK(doorbell::glow_mode_for(true, false, true) == GlowMode::AWAKE);
  CHECK(doorbell::glow_mode_for(true, false, false) == GlowMode::UNSURE);
  CHECK(doorbell::clamp_glow_pct(-5) == doorbell::kGlowPctMin);
  CHECK(doorbell::clamp_glow_pct(250) == doorbell::kGlowPctMax);
  CHECK(doorbell::wave_permille(0, 6000) == 0 && doorbell::wave_permille(3000, 6000) == 1000);
  return 0;
}

int main() {
  int (*tests[])() = {test_one_press_one_ring, test_bounce_is_not_a_ring,
                      test_holdoff_repeats_are_counted_not_rung, test_held_at_boot_never_rings,
                      test_stuck_reported_once_and_cleared, test_isr_edges_catch_a_tap_between_polls,
                      test_millis_wrap, test_glow_never_flashes, test_glow_on_is_never_dark_off_is_dark,
                      test_unsure_breathes_differently, test_swell_reaches_full_and_returns, test_mode_for};
  for (auto t : tests) {
    if (t() != 0) return 1;
  }
  std::printf("doorbell logic: %d checks passed\n", g_checks);
  return 0;
}
