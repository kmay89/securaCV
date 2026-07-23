// Host test for the arcade QA core (include/canary/mode/arcade_logic.h).
//
// Arcade mode's factory claim — "a completed round has exercised every touch
// zone, and the score screen is the QA report" — is only as good as this
// core, so the claim is pinned here without a board: the seeded shuffle
// visits every zone exactly once and replays from its seed; targets never
// stray outside their zone cell (else the per-zone claim is void); and the
// verdict fails on a missed zone, a slow zone, or a stray tap. Run in CI by
// the "arcade logic host test" step in firmware.yml; prints
// "ALL ARCADE LOGIC TESTS PASSED" on success. Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//   firmware/projects/canary-display/tests_host/test_arcade_logic.cpp -o t && ./t

#include "canary/mode/arcade_logic.h"

#include <cstdio>
#include <initializer_list>

using namespace canary::mode::arcade;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// The dash geometry the runtime uses: 800x480, 8x5 zones.
static constexpr uint8_t COLS = 8, ROWS = 5, ZONES = COLS * ROWS;
static constexpr int16_t W = 800, H = 480;

// ── RoundPlan ───────────────────────────────────────────────────────────────
static void test_plan_covers_every_zone_once() {
  for (uint32_t seed : {1u, 12345u, 0xDEADBEEFu, 0u}) {
    RoundPlan p;
    p.build(ZONES, seed);
    CHECK(p.count == ZONES, "plan holds every zone");
    uint8_t seen[ZONES] = {0};
    for (uint8_t i = 0; i < p.count; i++) {
      CHECK(p.order[i] < ZONES, "zone index in range");
      seen[p.order[i]]++;
    }
    for (uint8_t z = 0; z < ZONES; z++) {
      CHECK(seen[z] == 1, "every zone exactly once");
    }
  }
}

static void test_plan_is_seed_deterministic() {
  RoundPlan a, b, c;
  a.build(ZONES, 42);
  b.build(ZONES, 42);
  c.build(ZONES, 43);
  bool same_ab = true, same_ac = true;
  for (uint8_t i = 0; i < ZONES; i++) {
    if (a.order[i] != b.order[i]) same_ab = false;
    if (a.order[i] != c.order[i]) same_ac = false;
  }
  CHECK(same_ab, "same seed replays the same round (factory replay)");
  CHECK(!same_ac, "different seed shuffles differently");
}

static void test_plan_clamps_to_cap() {
  RoundPlan p;
  p.build(255, 7);
  CHECK(p.count == MAX_ZONES, "zone count clamps to the bitmap's 64");
}

// ── Target placement ────────────────────────────────────────────────────────
static void test_target_stays_inside_its_cell() {
  const int16_t cell_w = W / COLS, cell_h = H / ROWS;
  uint32_t seed = 0xC0FFEE;
  for (uint8_t z = 0; z < ZONES; z++) {
    for (int rep = 0; rep < 8; rep++) {
      const Target t = target_for_zone(z, COLS, ROWS, W, H, 96, seed);
      const int16_t x0 = (int16_t)((z % COLS) * cell_w);
      const int16_t y0 = (int16_t)((z / COLS) * cell_h);
      CHECK(t.size > 0, "target has size");
      CHECK(t.x >= x0 && t.x + t.size <= x0 + cell_w,
            "target inside its cell horizontally");
      CHECK(t.y >= y0 && t.y + t.size <= y0 + cell_h,
            "target inside its cell vertically");
      CHECK(t.x >= 0 && t.x + t.size <= W && t.y >= 0 && t.y + t.size <= H,
            "target inside the screen");
    }
  }
}

static void test_oversize_target_clamps_to_cell() {
  uint32_t seed = 5;
  const Target t = target_for_zone(0, COLS, ROWS, W, H, 500, seed);
  CHECK(t.size == H / ROWS, "500 px ask clamps to the 96 px cell height");
}

static void test_target_hit_test() {
  uint32_t seed = 9;
  const Target t = target_for_zone(12, COLS, ROWS, W, H, 80, seed);
  CHECK(target_contains(t, t.x, t.y), "top-left corner hits");
  CHECK(target_contains(t, (int16_t)(t.x + t.size - 1),
                        (int16_t)(t.y + t.size - 1)),
        "bottom-right inside edge hits");
  CHECK(!target_contains(t, (int16_t)(t.x + t.size), t.y),
        "one past the edge misses");
  CHECK(!target_contains(t, (int16_t)(t.x - 1), t.y), "left of target misses");
}

// ── Stats + verdict ─────────────────────────────────────────────────────────
static void test_stats_accounting() {
  RoundStats st;
  st.on_hit(0, 120);
  st.on_hit(1, 300);
  st.on_hit(2, 90);
  st.on_miss();
  CHECK(st.hits == 3 && st.misses == 1, "hit/miss counts");
  CHECK(st.worst_ms == 300, "worst latency tracked");
  CHECK(st.avg_ms() == 170, "average latency (510/3)");
  CHECK(st.zones_hit(ZONES) == 3, "coverage bitmap counts");
}

static void test_verdict() {
  const uint16_t BAR = 400;
  RoundStats good;
  for (uint8_t z = 0; z < ZONES; z++) good.on_hit(z, 150);
  CHECK(qa_pass(good, ZONES, BAR), "all zones, fast, clean -> pass");

  RoundStats missed_zone = good;
  missed_zone.zone_bits &= ~1ULL;  // zone 0 never answered
  CHECK(!qa_pass(missed_zone, ZONES, BAR), "a dead zone fails the panel");

  RoundStats slow;
  for (uint8_t z = 0; z < ZONES; z++) slow.on_hit(z, 150);
  slow.on_hit(7, 900);  // one sluggish zone
  CHECK(!qa_pass(slow, ZONES, BAR), "a slow zone fails the panel");

  RoundStats stray;
  for (uint8_t z = 0; z < ZONES; z++) stray.on_hit(z, 150);
  stray.on_miss();  // phantom/stray tap
  CHECK(!qa_pass(stray, ZONES, BAR), "a stray tap fails the panel");
}

static void test_empty_round_is_a_fail() {
  RoundStats st;
  CHECK(st.avg_ms() == 0, "no hits: avg is 0, not a divide");
  CHECK(!qa_pass(st, ZONES, 400), "an empty round never passes");
}

int main() {
  test_plan_covers_every_zone_once();
  test_plan_is_seed_deterministic();
  test_plan_clamps_to_cap();
  test_target_stays_inside_its_cell();
  test_oversize_target_clamps_to_cell();
  test_target_hit_test();
  test_stats_accounting();
  test_verdict();
  test_empty_round_is_a_fail();

  if (g_fail == 0) {
    std::printf("ALL ARCADE LOGIC TESTS PASSED\n");
    return 0;
  }
  std::printf("%d ARCADE LOGIC TEST(S) FAILED\n", g_fail);
  return 1;
}
