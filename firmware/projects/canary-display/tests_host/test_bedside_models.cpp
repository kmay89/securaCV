// firmware/projects/canary-display/tests_host/test_bedside_models.cpp
//
// Host tests for the three pure bedside models the Nightstand 7 wave adds:
//   care/lantern.h       — the honest night light (timeout, attention veto,
//                          the opt-in auto schedule, the scene ring)
//   care/hallway.h       — Hallway mode: the one switch over lantern hours,
//                          and its rise/hold/ebb dwell envelope
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
#include "canary/care/hallway.h"
#include "canary/glass_settings.h"
#include "canary/care/ambient_life.h"
#include "canary/io/boot_button.h"

#include <cstdio>

using canary::care::AmbientLife;
using canary::care::HallwayModel;
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

static void test_button_triple_mode() {
  printf("button triple (opt-in) ...\n");
  // Three taps inside the windows = Triple, fired on the third PRESS.
  ButtonClassifier b;
  b.enable_triple(true);
  uint32_t t = 1000;
  hold(b, false, t, 200);
  hold(b, true, t, 120);                        // press 1
  hold(b, false, t, 100);
  CHECK(hold(b, true, t, 120) == ButtonEvent::None,
        "second press is held back while a Triple is still possible");
  hold(b, false, t, 100);
  CHECK(hold(b, true, t, 120) == ButtonEvent::Triple,
        "third press inside the window is Triple");
  CHECK(hold(b, false, t, 600) == ButtonEvent::None,
        "release after a Triple emits nothing");

  // Two taps then silence: the held-back Double matures at window close —
  // one window later than stock, but it still fires.
  ButtonClassifier c;
  c.enable_triple(true);
  uint32_t u = 1000;
  hold(c, false, u, 200);
  hold(c, true, u, 120);
  hold(c, false, u, 100);
  hold(c, true, u, 120);                        // press 2, held back
  CHECK(hold(c, false, u, 600) == ButtonEvent::Double,
        "two taps mature into Double after the triple window");

  // Two taps then HOLD: the Double must arrive while the finger is still
  // down (a lamp lights under the press), and the hold never acknowledges.
  ButtonClassifier d;
  d.enable_triple(true);
  uint32_t v = 1000;
  hold(d, false, v, 200);
  hold(d, true, v, 120);
  hold(d, false, v, 100);
  CHECK(hold(d, true, v, 600) == ButtonEvent::Double,
        "a held second press still matures into Double");
  CHECK(hold(d, true, v, 1500) == ButtonEvent::None,
        "holding through it does not acknowledge");

  // With triple NOT armed, the grammar is the shipped one: Double fires
  // on the second press edge, immediately.
  ButtonClassifier e;
  uint32_t w = 1000;
  hold(e, false, w, 200);
  hold(e, true, w, 120);
  hold(e, false, w, 100);
  // 120 ms window: enough for the 30 ms debounce, far under the 350 ms
  // gap — so a fire here proves the press-edge immediacy, not maturation.
  CHECK(hold(e, true, w, 120) == ButtonEvent::Double,
        "stock mode keeps the immediate Double");

  // Short and Long are unchanged in triple mode.
  ButtonClassifier f;
  f.enable_triple(true);
  uint32_t x = 1000;
  hold(f, false, x, 200);
  hold(f, true, x, 120);
  CHECK(hold(f, false, x, 500) == ButtonEvent::Short, "lone tap still Short");
  CHECK(hold(f, true, x, 1200) == ButtonEvent::Long, "held press still Long");
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

// ── Hallway mode ──────────────────────────────────────────────────────────

static void test_hallway_off_by_default() {
  printf("hallway off by default...\n");
  HallwayModel h;
  CHECK(!h.enabled(), "a fresh model is off");
  // Off must mean OFF at every point in the night, not merely dim: the
  // dark-means-safe signal is only traded away by an explicit switch.
  bool any = false;
  for (uint16_t m = 0; m < 600; m++)
    if (h.level(m, (uint16_t)(600 - m), false) != 0) any = true;
  CHECK(!any, "no light anywhere in the night while off");
}

static void test_hallway_attention_veto() {
  printf("hallway attention veto...\n");
  HallwayModel h;
  h.configure(true, canary::care::HALLWAY_SOFT,
              HallwayModel::kRiseMinDefault, HallwayModel::kEbbMinDefault,
              /*beacon=*/true);
  // Mid-night, fully risen — the best case for the lamp.
  CHECK(h.level(200, 200, false) == 255, "lit and full mid-night");
  CHECK(h.level(200, 200, true) == 0, "attention extinguishes it outright");
  CHECK(h.brightness_now(200, 200, true) == 0, "and the brightness with it");
}

static void test_hallway_dwell_envelope() {
  printf("hallway dwell envelope...\n");
  HallwayModel h;
  h.configure(true, canary::care::HALLWAY_SOFT, 6, 12, true);
  const uint16_t night = 480;   // an 8-hour window

  // It starts dark and RISES — a lamp that snapped on would read as a timer.
  CHECK(h.level(0, night, false) == 0, "dark at the first minute");
  uint8_t prev = 0;
  bool monotonic = true;
  for (uint16_t m = 0; m <= 6; m++) {
    const uint8_t v = h.level(m, (uint16_t)(night - m), false);
    if (v < prev) monotonic = false;
    prev = v;
  }
  CHECK(monotonic, "the rise never goes backwards");
  CHECK(h.level(6, night - 6, false) == 255, "full by the end of the rise");

  // It holds through the middle.
  CHECK(h.level(120, 360, false) == 255, "holds mid-night");

  // And it EBBS toward morning rather than cutting out.
  CHECK(h.level(night - 12, 12, false) == 255, "still full as the ebb begins");
  CHECK(h.level(night - 6, 6, false) < 255, "dimming halfway through the ebb");
  CHECK(h.level(night, 0, false) == 0, "dark by the end of the window");
}

static void test_hallway_short_night_still_fades() {
  printf("hallway short night...\n");
  // Ramps longer than the whole window: the lamp must still fade in and out
  // (never jump), it just never reaches full.
  HallwayModel h;
  h.configure(true, canary::care::HALLWAY_SOFT, 60, 60, true);
  const uint16_t night = 40;
  CHECK(h.level(0, night, false) == 0, "starts dark");
  CHECK(h.level(night, 0, false) == 0, "ends dark");
  uint8_t peak = 0;
  for (uint16_t m = 0; m <= night; m++) {
    const uint8_t v = h.level(m, (uint16_t)(night - m), false);
    if (v > peak) peak = v;
  }
  CHECK(peak > 0, "it does light up");
  CHECK(peak < 255, "but never reaches full on a night this short");
}

static void test_hallway_levels_are_ordered_and_warm() {
  printf("hallway levels...\n");
  HallwayModel dim, soft, glow;
  dim.configure(true, canary::care::HALLWAY_DIM, 6, 12, true);
  soft.configure(true, canary::care::HALLWAY_SOFT, 6, 12, true);
  glow.configure(true, canary::care::HALLWAY_GLOW, 6, 12, true);
  CHECK(dim.preset().brightness < soft.preset().brightness &&
            soft.preset().brightness < glow.preset().brightness,
        "the three levels are actually ordered");
  // Every level is warm: a corridor at night wants candle light, and warm is
  // the end that disturbs dark-adapted eyes least.
  CHECK(dim.preset().warmth > 0 && soft.preset().warmth > 0 &&
            glow.preset().warmth > 0,
        "every level is warm-shifted");
  // A corridor lamp is noticed, not performed at.
  CHECK(glow.preset().depth < 128, "the song stays a murmur at every level");

  // An out-of-range level must not produce a strange device.
  HallwayModel bogus;
  bogus.configure(true, 99, 6, 12, true);
  CHECK(bogus.level_choice() == canary::care::HALLWAY_SOFT,
        "a bad level falls back to Soft");
  // Zero-length ramps are clamped rather than dividing by zero.
  HallwayModel zero;
  zero.configure(true, canary::care::HALLWAY_SOFT, 0, 0, true);
  CHECK(zero.rise_min() >= 1 && zero.ebb_min() >= 1, "ramps clamp to >= 1 min");
}

static void test_hallway_beacon_needs_the_mode_on() {
  printf("hallway beacon rule...\n");
  // The beacon opt-in is the one thing in this feature that touches the
  // "WS2812 is a pure attention channel" invariant, so the gate is asserted
  // rather than trusted: it can NEVER be effective while the mode is off,
  // whatever the stored preference says.
  HallwayModel h;
  h.configure(false, canary::care::HALLWAY_SOFT, 6, 12, /*beacon=*/true);
  CHECK(!h.beacon(), "beacon is off whenever the mode is off");
  CHECK(h.beacon_pref(), "but the user's choice is remembered");

  h.configure(true, canary::care::HALLWAY_SOFT, 6, 12, /*beacon=*/true);
  CHECK(h.beacon(), "on with the mode on and the opt-in set");

  h.configure(true, canary::care::HALLWAY_SOFT, 6, 12, /*beacon=*/false);
  CHECK(!h.beacon(), "and off when the user declines it");
}

// ── Quiet-hours window position (glass_settings.h) ────────────────────────
// Hallway mode's dwell needs to know WHERE in the window it is, so the helper
// that answers it is pinned here — including the midnight wrap, which is the
// normal case for a night window and the easy one to get wrong.
static void test_hours_window_position() {
  printf("quiet-hours window position...\n");
  uint16_t e = 0, r = 0;

  // 22:00 -> 07:00, the shipped seed: a 9-hour window that wraps midnight.
  canary::glass::hours_window_position(22, 0, 22, 7, &e, &r);
  CHECK(e == 0 && r == 540, "at the very start: 0 in, 540 left");
  canary::glass::hours_window_position(22, 30, 22, 7, &e, &r);
  CHECK(e == 30 && r == 510, "half an hour in");
  canary::glass::hours_window_position(2, 0, 22, 7, &e, &r);
  CHECK(e == 240 && r == 300, "past midnight, still counting from 22:00");
  canary::glass::hours_window_position(6, 30, 22, 7, &e, &r);
  CHECK(e == 510 && r == 30, "half an hour before the end");

  // Outside the window both are zero — the caller's "no dwell" signal.
  canary::glass::hours_window_position(12, 0, 22, 7, &e, &r);
  CHECK(e == 0 && r == 0, "daytime reads as no window");

  // A non-wrapping window works the same way.
  canary::glass::hours_window_position(2, 0, 1, 5, &e, &r);
  CHECK(e == 60 && r == 180, "non-wrapping window");

  // start == end is "never night" and must not divide by anything.
  canary::glass::hours_window_position(3, 0, 4, 4, &e, &r);
  CHECK(e == 0 && r == 0, "start==end is never night");
}

int main() {
  printf("== bedside model host tests ==\n");
  test_lantern_timeout();
  test_lantern_attention_veto();
  test_lantern_auto_schedule();
  test_lantern_scene_ring();
  test_hallway_off_by_default();
  test_hallway_attention_veto();
  test_hallway_dwell_envelope();
  test_hallway_short_night_still_fades();
  test_hallway_levels_are_ordered_and_warm();
  test_hallway_beacon_needs_the_mode_on();
  test_hours_window_position();
  test_ambient_life_cadence();
  test_ambient_life_gate();
  test_button_short();
  test_button_double();
  test_button_long();
  test_button_triple_mode();
  test_button_debounce();
  if (g_fail) { printf("\n%d CHECK(s) FAILED\n", g_fail); return 1; }
  printf("\nAll bedside-model checks passed.\n");
  return 0;
}
