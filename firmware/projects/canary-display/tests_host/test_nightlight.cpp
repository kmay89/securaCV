// firmware/projects/canary-display/tests_host/test_nightlight.cpp
//
// Host tests for the nightlight's companion-visit model
// (canary/care/nightlight.h) — the pure scheduler that decides WHEN the
// canary takes the stage and WHICH piece of theater the hour calls for.
//
// The invariants that make it SAFE and companion-like, proven off-board:
//   - the `allowed` gate is absolute: no visit begins, and a live visit
//     ENDS, the moment the glass isn't calm+idle (an alarm mid-visit takes
//     the stage back on the very next step)
//   - night visits are Moonwatch only (asleep is asleep — the bird never
//     stirs after bedtime), and the schedule is night-sparse
//   - a clockless device only ever gets the generic Curious visit — the
//     friend keeps no schedule without a clock
//   - the hour picks the theater: morning stretch, midday song/curiosity,
//     evening wind-down
//   - deterministic: same seed, same tick sequence, same visits
//   - the caption surface never says a severity word (the honest lines
//     belong to the glance line, not the friend)
//
// Build/run via the tests_host Makefile.
#include "canary/care/nightlight.h"

#include <cstdio>
#include <cstring>

using canary::care::NightlightVisits;
using canary::care::Visit;
using canary::care::VisitKind;
using canary::care::visit_caption;

static int g_fail = 0;
#define CHECK(cond, msg)                                        \
  do {                                                          \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; }     \
  } while (0)

// Step the model forward in fixed ticks until a visit begins (or the try
// budget runs out). Returns the visit and leaves `now` just after onset.
static Visit run_until_visit(NightlightVisits& v, uint32_t& now, int minute_of_day,
                             bool night, uint32_t step_ms = 1000,
                             uint32_t max_steps = 4 * 60 * 60) {
  for (uint32_t i = 0; i < max_steps; i++) {
    now += step_ms;
    Visit lv = v.step(now, minute_of_day, night, /*allowed=*/true);
    if (lv.kind != VisitKind::None) return lv;
  }
  return Visit{};
}

static void test_gate_blocks_and_ends_visits() {
  printf("the allowed gate...\n");
  NightlightVisits v;
  v.seed(0x1234);
  uint32_t now = 1000;

  // Never allowed -> never a visit, however long we wait.
  for (int i = 0; i < 100000; i++) {
    now += 1000;
    CHECK(v.step(now, 12 * 60, false, false).kind == VisitKind::None,
          "no visit while not allowed");
  }

  // Allowed -> a visit eventually arrives.
  Visit lv = run_until_visit(v, now, 12 * 60, false);
  CHECK(lv.kind != VisitKind::None, "a visit arrives while allowed");

  // The stage is taken back the instant allowance drops (alarm mid-visit).
  now += 1000;
  CHECK(v.step(now, 12 * 60, false, false).kind == VisitKind::None,
        "a live visit ends the moment allowed drops");
  // ...and it does not come back banked when re-allowed.
  now += 1000;
  CHECK(v.step(now, 12 * 60, false, true).kind == VisitKind::None,
        "no stored-up visit fires on re-allowance");
}

static void test_visit_duration() {
  printf("visit duration...\n");
  NightlightVisits v;
  v.seed(0xBEEF);
  uint32_t now = 5000;
  Visit lv = run_until_visit(v, now, 12 * 60, false);
  CHECK(lv.kind != VisitKind::None, "visit began");
  CHECK(lv.until_ms > now, "visit carries its end time");
  CHECK(lv.until_ms - now <= NightlightVisits::VISIT_MS, "visit is ~12 s");
  // Still on stage mid-window; gone after.
  now = lv.until_ms - 1;
  CHECK(v.step(now, 12 * 60, false, true).kind == lv.kind, "still visiting");
  now = lv.until_ms + 1;
  CHECK(v.step(now, 12 * 60, false, true).kind == VisitKind::None,
        "the clock gets the stage back");
}

static void test_hours_pick_the_theater() {
  printf("the hour picks the theater...\n");
  NightlightVisits v;
  v.seed(1);
  CHECK(v.pick(7 * 60, false) == VisitKind::Stretch, "morning = stretch");
  CHECK(v.pick(12 * 60 + 1, false) == VisitKind::Song, "midday odd minute = song");
  CHECK(v.pick(12 * 60, false) == VisitKind::Curious, "midday even minute = curious");
  CHECK(v.pick(19 * 60, false) == VisitKind::Winddown, "evening = wind-down");
  CHECK(v.pick(23 * 60, false) == VisitKind::Curious, "late outside quiet hours = curious");
  CHECK(v.pick(12 * 60, true) == VisitKind::Moonwatch, "night = moonwatch only");
  CHECK(v.pick(-1, false) == VisitKind::Curious, "clockless = curious only");
}

static void test_night_is_sparse_and_still() {
  printf("night is sparse and still...\n");
  NightlightVisits v;
  v.seed(0xA11CE);
  uint32_t now = 0;
  Visit lv = run_until_visit(v, now, 3 * 60, true);
  CHECK(lv.kind == VisitKind::Moonwatch, "the only night visit is moonwatch");
  const uint32_t first_at = now;
  // End it, then measure the gap to the next one: night cadence is the
  // sparse band (>= 25 min), not the daytime one.
  now = lv.until_ms + 1;
  (void)v.step(now, 3 * 60, true, true);
  Visit nxt = run_until_visit(v, now, 3 * 60, true);
  CHECK(nxt.kind == VisitKind::Moonwatch, "next night visit is moonwatch too");
  CHECK(now - first_at >= NightlightVisits::NIGHT_MIN_MS,
        "night visits are at least 25 minutes apart");
}

static void test_deterministic() {
  printf("deterministic across reboots of a bench...\n");
  NightlightVisits a, b;
  a.seed(42);
  b.seed(42);
  uint32_t na = 0, nb = 0;
  for (int round = 0; round < 4; round++) {
    Visit va = run_until_visit(a, na, 11 * 60, false);
    Visit vb = run_until_visit(b, nb, 11 * 60, false);
    CHECK(va.kind == vb.kind && na == nb, "same seed, same visit, same time");
    na = va.until_ms + 1;
    nb = vb.until_ms + 1;
    (void)a.step(na, 11 * 60, false, true);
    (void)b.step(nb, 11 * 60, false, true);
  }
}

static void test_captions_are_kind_words() {
  printf("captions...\n");
  // Every kind speaks; None is silent; nobody says a severity word (those
  // belong to the glance line — the friend never carries the alarm).
  static const char* banned[] = {"tamper", "alert", "alarm", "warn", "lost"};
  const VisitKind kinds[] = {VisitKind::Stretch, VisitKind::Song,
                             VisitKind::Curious, VisitKind::Winddown,
                             VisitKind::Moonwatch};
  for (VisitKind k : kinds) {
    const char* c = visit_caption(k);
    CHECK(c && c[0], "every visit has words");
    for (const char* w : banned)
      CHECK(strstr(c, w) == nullptr, "the friend never says a severity word");
  }
  CHECK(visit_caption(VisitKind::None)[0] == '\0', "no visit, no words");
}

int main() {
  printf("test_nightlight (companion visits model)\n");
  test_gate_blocks_and_ends_visits();
  test_visit_duration();
  test_hours_pick_the_theater();
  test_night_is_sparse_and_still();
  test_deterministic();
  test_captions_are_kind_words();
  if (g_fail) {
    printf("%d FAILURE(S)\n", g_fail);
    return 1;
  }
  printf("all nightlight visit tests passed\n");
  return 0;
}
