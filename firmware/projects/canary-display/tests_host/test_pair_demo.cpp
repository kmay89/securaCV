// Host test for the First Light pair demo's pure core
// (include/canary/pair/pair_demo.h).
//
// Builds standalone with g++ — no Arduino, no LVGL, no radio. Run in CI by
// the "pair demo host test" step in .github/workflows/firmware.yml. Prints
// "ALL PAIR DEMO TESTS PASSED" on success (the CI grep makes a silent pass
// impossible to fake). Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//     firmware/projects/canary-display/tests_host/test_pair_demo.cpp -o t && ./t
//
// What is pinned here, and why it matters on the demo table:
//   * adopt-first-heard, foreign witnesses ignored, lock survives forget's
//     inverse — the card can never silently switch cameras mid-demo;
//   * the edge rule matches the sender's (alert rising, or class change
//     while alert holds) — both ends of the wire agree on what "now" means;
//   * a remembered lock re-follows immediately and skips the adopt step;
//   * staleness reads exactly like the fleet model's band freshness;
//   * the HoldGate fires once per hold, debounced, and never early;
//   * auto-open is unprovisioned + router-free band + calm glass, and
//     NOTHING else — a working bedside clock never swaps its face.

#include "canary/pair/pair_demo.h"

#include <cstdio>
#include <cstring>

using canary::fleet::BeaconStatus;
using canary::fleet::Via;
using canary::pair::HoldGate;
using canary::pair::PairDemo;
using canary::pair::PairStage;
using canary::pair::pair_should_auto_open;
using canary::pair::PAIR_HOLD_OPEN_MS;
using canary::pair::PAIR_STALE_MS;

static int g_fail = 0;
#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);  \
      g_fail++;                                                       \
    }                                                                 \
  } while (0)

static BeaconStatus status(bool alert, uint8_t cls, int16_t score) {
  BeaconStatus s;
  s.alert = alert;
  s.detect_class = cls;
  s.detect_score = score;
  return s;
}

static void test_stage_ladder_and_adopt() {
  PairDemo d;
  CHECK(d.stage() == PairStage::Idle, "closed = Idle");

  d.open(1000, nullptr);
  CHECK(d.stage() == PairStage::Listening, "open, nobody heard = Listening");

  // First witness heard is adopted…
  d.observe("3fa2", status(false, 0, -1), true, 1500, Via::Mesh, 111);
  CHECK(d.stage() == PairStage::Found, "first heard = Found");
  CHECK(std::strcmp(d.fp4(), "3fa2") == 0, "candidate is the first heard");

  // …and a second camera in range stays ignored (first wins).
  d.observe("beef", status(true, 1, 90), true, 1600, Via::Mesh, 222);
  CHECK(std::strcmp(d.fp4(), "3fa2") == 0, "foreign witness not adopted");
  CHECK(!d.alert_now(), "foreign witness's alert not absorbed");

  CHECK(d.can_lock(), "candidate on stage = lockable");
  d.lock();
  CHECK(d.stage() == PairStage::Live, "locked = Live");
  CHECK(d.locked(), "locked() reads back");

  d.forget();
  CHECK(d.stage() == PairStage::Listening, "forget returns to Listening");
  CHECK(d.fp4()[0] == '\0', "forget clears the candidate");
}

static void test_remembered_lock_skips_adopt() {
  PairDemo d;
  d.open(1000, "3fa2");
  CHECK(d.stage() == PairStage::Live, "remembered lock opens Live");
  CHECK(!d.heard(), "not heard yet");

  // A different camera cannot claim the followed slot.
  d.observe("beef", status(true, 1, 90), true, 1500, Via::Mesh, 1);
  CHECK(!d.heard(), "foreign witness ignored under a lock");

  d.observe("3fa2", status(false, 0, -1), true, 2000, Via::Mesh, 2);
  CHECK(d.heard(), "the remembered camera is followed");
}

static void test_edge_rule_matches_sender() {
  PairDemo d;
  d.open(0, "3fa2");

  // alert rising = edge
  CHECK(d.observe("3fa2", status(true, 1, 87), true, 100, Via::Mesh, 10),
        "alert rising = edge");
  CHECK(d.edges() == 1 && d.last_edge_ms() == 100 && d.edge_rx_us() == 10,
        "edge bookkeeping");

  // held alert (refresh) = NOT an edge; confidence wobble = NOT an edge
  CHECK(!d.observe("3fa2", status(true, 1, 91), true, 5100, Via::Mesh, 11),
        "held alert refresh is not an edge");
  CHECK(d.detect_score() == 91, "score still tracks the wobble");

  // class change while alert holds = edge (the sender's rule, mirrored)
  CHECK(d.observe("3fa2", status(true, 3, 80), true, 5200, Via::Mesh, 12),
        "class change mid-alert = edge");
  CHECK(d.edges() == 2, "second edge counted");

  // falling = not an edge for the pulse; rising again = edge
  CHECK(!d.observe("3fa2", status(false, 0, -1), true, 6000, Via::Mesh, 13),
        "alert falling is not a pulse edge");
  CHECK(d.observe("3fa2", status(true, 1, 88), true, 7000, Via::Mesh, 14),
        "alert rising again = edge");
  CHECK(d.edges() == 3, "third edge counted");

  // v1 beacon (no status) refreshes freshness only
  CHECK(!d.observe("3fa2", status(true, 1, 88), false, 8000, Via::Mesh, 15),
        "v1 beacon is never an edge");
  CHECK(d.last_seen_ms() == 8000, "v1 beacon still refreshes freshness");

  // react time parks where the UI put it
  d.note_react_us(41000);
  CHECK(d.react_us() == 41000, "react time parked for the card");
}

static void test_staleness_reads_like_the_fleet() {
  PairDemo d;
  d.open(0, "3fa2");
  d.observe("3fa2", status(false, 0, -1), true, 1000, Via::Mesh, 1);
  CHECK(!d.stale(1000 + PAIR_STALE_MS), "at the boundary: not yet stale");
  CHECK(d.stale(1000 + PAIR_STALE_MS + 1), "past three refreshes: stale");
}

static void test_hold_gate() {
  HoldGate g;
  uint32_t t = 1000;
  CHECK(!g.step(false, t), "idle: nothing");

  // A tap never fires it.
  CHECK(!g.step(true, t), "press: clock starts, nothing fires");
  CHECK(!g.step(true, t + 200), "short hold: nothing");
  CHECK(!g.step(false, t + 300), "release: nothing, clock reset");

  // A full hold fires exactly once, and not a millisecond early.
  t = 10000;
  CHECK(!g.step(true, t), "press again");
  CHECK(!g.step(true, t + PAIR_HOLD_OPEN_MS - 1), "one ms early: not fired");
  bool fired = false;
  for (uint32_t dt = PAIR_HOLD_OPEN_MS; dt < PAIR_HOLD_OPEN_MS + 2000;
       dt += 50) {
    if (g.step(true, t + dt)) {
      CHECK(!fired, "fires only once per hold");
      fired = true;
    }
  }
  CHECK(fired, "matured hold fired");

  // A bounce dip restarts the clock — "keep holding" is forgiving, not
  // stateful.
  CHECK(!g.step(false, t + 20000), "dip reads as release");
  t = 40000;
  CHECK(!g.step(true, t), "new hold: clock restarted");
  CHECK(!g.step(true, t + 100), "new hold: not fired early");
  CHECK(g.step(true, t + PAIR_HOLD_OPEN_MS + 40), "new hold fires again");
}

static void test_auto_open_rule() {
  // The one true auto-open: unprovisioned + router-free band + calm.
  CHECK(pair_should_auto_open(true, Via::Mesh, false, false),
        "boxed pair: auto-open");
  // Everything else refuses.
  CHECK(!pair_should_auto_open(false, Via::Mesh, false, false),
        "provisioned clock never auto-opens");
  CHECK(!pair_should_auto_open(true, Via::Wifi, false, false),
        "a LAN datagram never auto-opens (a network already exists)");
  CHECK(!pair_should_auto_open(true, Via::Ble, false, false),
        "ble never auto-opens");
  CHECK(!pair_should_auto_open(true, Via::Mesh, true, false),
        "already open: no re-open");
  CHECK(!pair_should_auto_open(true, Via::Mesh, false, true),
        "an urgent glass is never interrupted");
}

int main() {
  test_stage_ladder_and_adopt();
  test_remembered_lock_skips_adopt();
  test_edge_rule_matches_sender();
  test_staleness_reads_like_the_fleet();
  test_hold_gate();
  test_auto_open_rule();

  if (g_fail == 0) {
    std::printf("ALL PAIR DEMO TESTS PASSED\n");
    return 0;
  }
  std::printf("%d PAIR DEMO TEST(S) FAILED\n", g_fail);
  return 1;
}
