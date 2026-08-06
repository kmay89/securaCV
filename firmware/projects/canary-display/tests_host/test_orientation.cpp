// firmware/projects/canary-display/tests_host/test_orientation.cpp
//
// Host tests for the gravity-settled orientation model
// (canary/io/orientation.h) — the piece that makes the nightlight's
// auto-rotate feel connected to REAL movement without false positives.
//
// The invariants proven off-board:
//   - a flip commits only from SETTLED gravity (a shake, a carry, a bump
//     never flips the clock — their samples carry no opinion)
//   - lying flat (face up/down) keeps the last orientation
//   - diagonal holds have no opinion (no 45° flapping)
//   - the dwell means a real turn commits once, at rest — and the
//     cooldown means a wobbling hand cannot double-flip
//   - all four orientations resolve from their gravity vectors
//
// Build/run via the tests_host Makefile.
#include "canary/io/orientation.h"

#include <cstdio>

using canary::io::Orient;
using canary::io::OrientationModel;

static int g_fail = 0;
#define CHECK(cond, msg)                                        \
  do {                                                          \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; }     \
  } while (0)

static constexpr int32_t G = 8192;  // +-4g scale: 8192 LSB per g

// Feed a constant sample at 20 Hz for `ms`; report whether a commit fired.
static bool feed(OrientationModel& m, int32_t ax, int32_t ay, int32_t az,
                 uint32_t& t, uint32_t ms) {
  bool committed = false;
  const uint32_t end = t + ms;
  for (; t < end; t += 50) {
    if (m.step(ax, ay, az, t)) committed = true;
  }
  return committed;
}

static void test_four_orientations() {
  printf("all four orientations resolve...\n");
  OrientationModel m;
  m.begin(G, Orient::R0);
  uint32_t t = 10000;

  // +y is down the glass: upright = gravity along +y. Turn it CW onto its
  // left edge: gravity now pulls along +x (screen right edge down) = R90.
  CHECK(feed(m, G, 0, 0, t, 2000), "turn to R90 commits");
  CHECK(m.current() == Orient::R90, "R90 seen");
  t += 2000;  // idle past the cooldown
  CHECK(feed(m, 0, -G, 0, t, 2000), "upside down commits");
  CHECK(m.current() == Orient::R180, "R180 seen");
  t += 2000;
  CHECK(feed(m, -G, 0, 0, t, 2000), "the other landscape commits");
  CHECK(m.current() == Orient::R270, "R270 seen");
  t += 2000;
  CHECK(feed(m, 0, G, 0, t, 2000), "back upright commits");
  CHECK(m.current() == Orient::R0, "home again");
}

static void test_shake_carries_no_opinion() {
  printf("a shake never flips it...\n");
  OrientationModel m;
  m.begin(G, Orient::R0);
  uint32_t t = 10000;

  // A vigorous shake: magnitudes far off 1 g, axes flying everywhere.
  // Alternate big up-down-sideways samples for three full seconds.
  bool committed = false;
  for (int i = 0; i < 60; i++, t += 50) {
    const int32_t big = (i % 2) ? 3 * G : G / 4;
    const int32_t ax = (i % 3 == 0) ? big : -big;
    const int32_t ay = (i % 3 == 1) ? big : 0;
    const int32_t az = (i % 3 == 2) ? -big : big / 2;
    if (m.step(ax, ay, az, t)) committed = true;
  }
  CHECK(!committed, "no commit during the shake");
  CHECK(m.current() == Orient::R0, "orientation held through the shake");

  // And a shake that ENDS upside down commits exactly once, at rest.
  CHECK(feed(m, 0, -G, 0, t, 2000), "settling after the shake commits");
  CHECK(m.current() == Orient::R180, "the rest position wins");
}

static void test_flat_keeps_last() {
  printf("lying flat keeps the last orientation...\n");
  OrientationModel m;
  m.begin(G, Orient::R90);
  uint32_t t = 10000;
  // Face-up on a table: gravity is all out-of-plane.
  CHECK(!feed(m, 0, 0, G, t, 5000), "face-up carries no opinion");
  CHECK(m.current() == Orient::R90, "orientation held while flat");
  CHECK(!feed(m, 0, 0, -G, t, 5000), "face-down carries no opinion");
  CHECK(m.current() == Orient::R90, "still held");
}

static void test_diagonal_has_no_opinion() {
  printf("diagonals never flap...\n");
  OrientationModel m;
  m.begin(G, Orient::R0);
  uint32_t t = 10000;
  // 45°: both in-plane axes equal — under the 1.5x dominance margin.
  const int32_t d = (int32_t)(G / 1.4142f);
  CHECK(!feed(m, d, -d, 0, t, 5000), "a 45-degree hold commits nothing");
  CHECK(m.current() == Orient::R0, "orientation held on the diagonal");
}

static void test_dwell_and_cooldown() {
  printf("dwell + cooldown...\n");
  OrientationModel m;
  m.begin(G, Orient::R0);
  uint32_t t = 10000;

  // Shorter than the dwell: a glance-flip that comes right back does
  // nothing (picked it up, looked at the back, put it down).
  CHECK(!feed(m, 0, -G, 0, t, OrientationModel::DWELL_MS - 200),
        "briefly inverted does not commit");
  CHECK(!feed(m, 0, G, 0, t, 2000), "returning home commits nothing");
  CHECK(m.current() == Orient::R0, "still upright");

  // A real turn commits once; the cooldown then absorbs the wobble.
  CHECK(feed(m, 0, -G, 0, t, 2000), "held inversion commits");
  CHECK(m.current() == Orient::R180, "now upside down");
  // Wobble back within the cooldown: nothing may fire.
  CHECK(!feed(m, 0, G, 0, t, OrientationModel::COOLDOWN_MS / 2),
        "cooldown absorbs the immediate wobble");
  CHECK(m.current() == Orient::R180, "held through the wobble");
}

static void test_gravity_mapping_sanity() {
  printf("tilted-but-decisive still resolves...\n");
  OrientationModel m;
  m.begin(G, Orient::R0);
  uint32_t t = 10000;
  // Leaning back 30° on a stand: y carries cos(30)~0.87g in plane, z 0.5g
  // out of plane — decisively upright, well inside every gate.
  const int32_t y = (int32_t)(G * 0.866f);
  const int32_t z = G / 2;
  CHECK(!feed(m, 0, y, z, t, 3000), "already upright: no commit");
  m.begin(G, Orient::R180);
  t += 1000;
  CHECK(feed(m, 0, y, z, t, 3000), "leaning-back upright commits from R180");
  CHECK(m.current() == Orient::R0, "reads as upright");
}

static void test_sync_lets_a_hand_turned_glass_right_itself() {
  printf("sync(): a manual turn re-baselines the model...\n");
  OrientationModel m;
  m.begin(G, Orient::R0);
  uint32_t t = 10000;

  // The unit stands UPRIGHT the whole time. A triple-press turns the
  // glass to R90 and parks auto; the glue syncs the model to the glass.
  // Re-armed, gravity (upright) now DISAGREES with the reference — the
  // display must right itself. Without sync, gravity would agree with
  // the model's stale R0 and the glass would stay sideways forever.
  m.sync(Orient::R90);
  CHECK(m.current() == Orient::R90, "model adopts the shown rotation");
  CHECK(feed(m, 0, G, 0, t, 3000), "upright gravity rights a hand-turned glass");
  CHECK(m.current() == Orient::R0, "back upright");

  // And a sync TO where gravity already points stays quiet.
  m.sync(Orient::R0);
  CHECK(!feed(m, 0, G, 0, t, 3000), "sync to the true up commits nothing");
}

static void test_gap_in_samples_is_not_dwell() {
  printf("a sampling gap earns no dwell credit...\n");
  OrientationModel m;
  m.begin(G, Orient::R0);
  uint32_t t = 10000;

  // One settled R180 sample opens a candidate...
  CHECK(!m.step(0, -G, 0, t), "single sample cannot commit");
  // ...then the feed stalls (modal surface, blocking reconnect, I2C
  // outage) far past the dwell window. The next matching sample must NOT
  // cash in the unsampled time as evidence.
  t += 10000;
  CHECK(!m.step(0, -G, 0, t), "sample after a long gap starts over");
  CHECK(m.current() == Orient::R0, "no commit off two samples and a gap");
  // Continuous evidence from here commits normally.
  CHECK(feed(m, 0, -G, 0, t, 3000), "fresh continuous dwell still commits");
  CHECK(m.current() == Orient::R180, "and lands the flip");
}

int main() {
  printf("test_orientation (gravity-settled auto-rotate)\n");
  test_four_orientations();
  test_shake_carries_no_opinion();
  test_flat_keeps_last();
  test_diagonal_has_no_opinion();
  test_dwell_and_cooldown();
  test_gravity_mapping_sanity();
  test_sync_lets_a_hand_turned_glass_right_itself();
  test_gap_in_samples_is_not_dwell();
  if (g_fail) {
    printf("%d FAILURE(S)\n", g_fail);
    return 1;
  }
  printf("all orientation tests passed\n");
  return 0;
}
