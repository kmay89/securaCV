// Canary Vision presence FSM (src/state/presence_fsm.cpp): the clocks each
// event row carries. publish_event_json (main.cpp) builds every event from
// fsm.snapshot() taken right after the tick that emitted it, and so does the
// state row published on the same tick, so what the snapshot reports on that
// tick is what the device sends.
//
// Pinned here (sweep F130):
//   * dwell_ended carries the length of the dwell it closed. Before the
//     latch, tick() cleared dwelling_ before the snapshot, so the row said
//     dwell_ms 0 however long the dwell had lasted.
//   * dwell_started carries 0: the dwell starts on that tick.
//   * the latch lasts until the next tick, so a heartbeat between the two
//     reports the same length (not a running clock), and the next frame,
//     whatever it emits, starts from 0 again.
//   * visit_ms keeps its own latch (last_visit_ms_), untouched.
//   * both lengths run to the frame that declared the person gone, so they
//     include the lost timeout (the README's clocks table says so).
//
// presence_fsm.cpp and voxel_tracker.cpp are linked verbatim; the only
// stand-in is canary::cfg::detect(), the NVS-backed tuning, which is
// replaced by a struct the test owns so no Arduino shim is needed.
// Pure hosted C++.

#include <cassert>
#include <cstdio>
#include <cstring>

#include "canary/config.h"
#include "canary/detect_config.h"
#include "canary/state/presence_fsm.h"

namespace canary::cfg {
static DetectConfig g_test_cfg{(uint8_t)PERSON_TARGET, (uint8_t)SCORE_MIN,
                               LOST_TIMEOUT_MS, DWELL_START_MS, 0};
const DetectConfig& detect() { return g_test_cfg; }
}  // namespace canary::cfg

using canary::state::PresenceFSM;

static VisionSample person(int r = 1, int c = 1) {
  VisionSample vs{};
  vs.person_now = true;
  vs.bbox = BBox{96, 88, 64, 128, 91};
  vs.voxel = Voxel(r, c, VOXEL_ROWS, VOXEL_COLS);
  vs.person_count = 1;
  vs.posture = Posture::Upright;
  vs.proximity = Proximity::Mid;
  vs.voxel_mask = (uint16_t)(1u << (r * VOXEL_COLS + c));
  return vs;
}

static VisionSample empty() { return VisionSample{}; }

struct Seen {
  const char* name = nullptr;
  const char* reason = nullptr;
  uint32_t t = 0;
  StateSnapshot snap{};
};

// Drive one frame and return what publish_event_json would have read.
static bool step(PresenceFSM& fsm, const VisionSample& vs, uint32_t t, Seen& out) {
  EventMsg ev{};
  const bool emitted = fsm.tick(vs, t, ev);
  out = Seen{};
  out.t = t;
  if (emitted) {
    out.name = ev.event_name;
    out.reason = ev.reason;
  }
  out.snap = fsm.snapshot(t, out.name ? out.name : "boot");
  return emitted;
}

static bool is(const Seen& s, const char* name) { return s.name && std::strcmp(s.name, name) == 0; }

// A person stays past the dwell start, then leaves. Frames every 100 ms, the
// firmware's INVOKE_PERIOD_MS.
static void test_linger_dwell_ended_reports_its_dwell() {
  PresenceFSM fsm;
  fsm.reset();
  Seen s;
  uint32_t t = 1000;  // not 0: a zero clock would hide a "now - start" bug
  assert(step(fsm, person(), t, s) && is(s, "presence_started"));
  assert(s.snap.presence_ms == 0 && s.snap.dwell_ms == 0 && s.snap.visit_ms == 0);
  const uint32_t present_at = t;

  uint32_t dwell_at = 0;
  for (t += 100; t < present_at + DWELL_START_MS + 5000; t += 100) {
    if (step(fsm, person(), t, s) && is(s, "dwell_started")) {
      dwell_at = t;
      // the dwell starts on this tick: 0 is the honest length
      assert(s.snap.dwelling);
      assert(s.snap.dwell_ms == 0);
      assert(s.snap.presence_ms == t - present_at);
    }
  }
  assert(dwell_at == present_at + DWELL_START_MS);
  // mid-dwell, the snapshot (heartbeat, state row) shows the running dwell
  const uint32_t last_seen = t - 100;
  assert(fsm.snapshot(last_seen, "x").dwell_ms == last_seen - dwell_at);

  // the person leaves: nothing until the lost timeout has passed
  Seen ended{};
  for (; t < last_seen + LOST_TIMEOUT_MS + 1000; t += 100) {
    if (step(fsm, empty(), t, s)) { ended = s; break; }
    assert(s.snap.dwelling && s.snap.dwell_ms == t - dwell_at);  // still running
  }
  assert(is(ended, "dwell_ended"));
  assert(ended.t > last_seen + LOST_TIMEOUT_MS);
  // F130: the row reports how long the dwell lasted, on the clock the
  // running dwell used (the value the last running row would have reached)
  assert(!ended.snap.dwelling);
  assert(ended.snap.presence);  // presence_ended follows on the next tick
  assert(ended.snap.dwell_ms == ended.t - dwell_at);
  assert(ended.snap.dwell_ms > 0);
  // ...which runs to the frame that declared the person gone, not to the
  // last sighting: the length includes the lost timeout (the README says so)
  assert(ended.snap.dwell_ms > (last_seen - dwell_at) + LOST_TIMEOUT_MS);
  assert(ended.snap.presence_ms == ended.t - present_at);
  // a heartbeat before the next frame reads the same length, not a clock
  assert(fsm.snapshot(ended.t + 40, "dwell_ended").dwell_ms == ended.t - dwell_at);
  assert(!fsm.snapshot(ended.t + 40, "dwell_ended").dwelling);

  // the next frame closes the stay; the dwell length is not carried on
  t = ended.t + 100;
  assert(step(fsm, empty(), t, s) && is(s, "presence_ended"));
  assert(!s.snap.presence && !s.snap.dwelling);
  assert(s.snap.dwell_ms == 0);
  assert(s.snap.presence_ms == 0);
  assert(s.snap.visit_ms == t - present_at);  // last_visit_ms_, unchanged
  assert(s.snap.visit_ms > (last_seen - present_at) + LOST_TIMEOUT_MS);  // the tail too
  const uint32_t visit = s.snap.visit_ms;

  t += 100;
  assert(step(fsm, empty(), t, s) && is(s, "interaction_likely"));
  assert(std::strcmp(s.reason, "dwell_then_left") == 0);
  assert(s.snap.dwell_ms == 0);
  assert(s.snap.visit_ms == visit);
  std::printf("  linger: dwell_ended carries %lu ms, presence_ended 0, visit %lu ms\n",
              (unsigned long)ended.snap.dwell_ms, (unsigned long)visit);
}

// The person is back on the frame after dwell_ended (the dwell had ended, the
// stay had not): dwell_started fires again at once, from 0, and the ended
// dwell's length is gone.
static void test_return_after_dwell_ended_starts_from_zero() {
  PresenceFSM fsm;
  fsm.reset();
  Seen s;
  uint32_t t = 500;
  assert(step(fsm, person(), t, s) && is(s, "presence_started"));
  for (t += 100; t <= 500 + DWELL_START_MS; t += 100) step(fsm, person(), t, s);
  assert(s.snap.dwelling);
  const uint32_t last_seen = t - 100;
  for (;; t += 100) {
    if (step(fsm, empty(), t, s)) break;
    assert(t < last_seen + LOST_TIMEOUT_MS + 1000);
  }
  assert(is(s, "dwell_ended") && s.snap.dwell_ms > 0);
  t += 100;
  assert(step(fsm, person(), t, s) && is(s, "dwell_started"));
  assert(s.snap.dwelling && s.snap.dwell_ms == 0);
  assert(s.snap.presence);
}

// A walk-by that never dwells: no dwell event, dwell_ms 0 on every row.
static void test_walk_by_never_dwells() {
  PresenceFSM fsm;
  fsm.reset();
  Seen s;
  uint32_t t = 0;
  int events = 0;
  // 2 s in frame: under ZONE_INTERACTION_MS too, so no interaction_likely
  static_assert(2000 < ZONE_INTERACTION_MS && 2000 < DWELL_START_MS, "a walk-by");
  for (; t < 2000; t += 100) {
    if (step(fsm, person(), t, s)) { events++; assert(is(s, "presence_started")); }
    assert(s.snap.dwell_ms == 0 && !s.snap.dwelling);
  }
  for (; t < 2000 + LOST_TIMEOUT_MS + 5000; t += 100) {
    if (step(fsm, empty(), t, s)) {
      events++;
      assert(!is(s, "dwell_ended") && !is(s, "dwell_started"));
    }
    assert(s.snap.dwell_ms == 0);
  }
  assert(events == 2);  // presence_started, presence_ended
}

// reset() forgets an ended dwell along with everything else.
static void test_reset_clears_the_latch() {
  PresenceFSM fsm;
  fsm.reset();
  Seen s;
  uint32_t t = 0;
  step(fsm, person(), t, s);
  for (t += 100; t <= DWELL_START_MS + 200; t += 100) step(fsm, person(), t, s);
  for (;; t += 100) if (step(fsm, empty(), t, s)) break;
  assert(is(s, "dwell_ended") && s.snap.dwell_ms > 0);
  fsm.reset();
  const StateSnapshot z = fsm.snapshot(t, "boot");
  assert(z.dwell_ms == 0 && z.visit_ms == 0 && !z.presence && !z.dwelling);
}

int main() {
  test_linger_dwell_ended_reports_its_dwell();
  test_return_after_dwell_ended_starts_from_zero();
  test_walk_by_never_dwells();
  test_reset_clears_the_latch();
  std::printf("ALL VISION PRESENCE FSM TESTS PASSED\n");
  return 0;
}
