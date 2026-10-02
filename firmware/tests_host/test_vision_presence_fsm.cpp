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
// Pinned here (sweep F152): each visit starts its own voxel tracker. Before,
// PresenceFSM reset the tracker only in reset(), at boot, so a later visit's
// interaction clock (now - stable_enter_ms >= ZONE_INTERACTION_MS) started in
// an earlier visit and almost every visit but the first ended in
// interaction_likely (zone_interaction_then_left), however short it was; and
// its presence_started named the earlier visit's cell. A 1 s revisit and a
// 0.5 s pass in another cell after a 4 s visit end without it now, a long
// settled visit still ends with it, and presence_started names the cell the
// visit began in.
//
// Pinned here (F152's review): a visit that qualified still reports
// interaction_likely when someone is seen on the frame right after its
// presence_ended. interaction_likely goes out on that frame only if it is
// empty; a sighting there starts the next visit, whose presence_started
// cleared the ended visit's latches. Before F152 the stale interaction
// clock usually made the next fragment report one late (with its own
// length and the zone reason); with F152 alone the visit reported nothing.
// The FSM now owes it: it is sent on the frame after that presence_started,
// inside the same window, with the ended visit's reason and visit_ms.
//
// Pinned here (sweep F154), in a second build of this file with
// -DVISION_DWELL_END_GRACE_MS=4000 (longer than the 1.5 s lost timeout):
// the dwell end grace holds a dweller present and dwelling past the lost
// timeout, a dweller back within it keeps the dwell, and once it has passed
// dwell_ended still fires with the dwell's length before presence_ended.
// Before, the FSM cleared the dwell silently and sent presence_ended at the
// lost timeout, so the dwell's end was never reported. The grace is for
// dwellers only, and a lost timeout longer than the grace still governs.
// The shipped build (grace 0) runs the rest of the file.
//
// presence_fsm.cpp and voxel_tracker.cpp are linked verbatim; the only
// stand-in is canary::cfg::detect(), the NVS-backed tuning, which is
// replaced by a struct the test owns so no Arduino shim is needed.
// Pure hosted C++.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// One visit: `ms` of person frames in cell (r, c) from `t`, then empty frames
// until the post-leave window has closed. Returns every event, in order, as
// "name" or "name:reason", and leaves `t` after the last empty frame.
struct Visit {
  std::string events;
  StateSnapshot started{};  // presence_started's snapshot
  bool interaction = false;
  const char* reason = nullptr;
};
static Visit visit(PresenceFSM& fsm, uint32_t& t, int r, int c, uint32_t ms) {
  Visit v;
  Seen s;
  const auto note = [&]() {
    if (!v.events.empty()) v.events += ' ';
    v.events += s.name;
    if (s.reason) { v.events += ':'; v.events += s.reason; }
    if (is(s, "presence_started")) v.started = s.snap;
    if (is(s, "interaction_likely")) { v.interaction = true; v.reason = s.reason; }
  };
  for (const uint32_t end = t + ms; t < end; t += 100)
    if (step(fsm, person(r, c), t, s)) note();
  const uint32_t quiet = t + canary::cfg::detect().lost_timeout_ms + DWELL_END_GRACE_MS +
                         INTERACTION_AFTER_LEAVE_WINDOW_MS + 1000;
  for (; t < quiet; t += 100)
    if (step(fsm, empty(), t, s)) note();
  return v;
}

// The shipped build's dwell tests (grace 0: the lost timeout ends every stay).
#if VISION_DWELL_END_GRACE_MS == 0

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
  // sent from an empty frame after the person has gone: no box, so its
  // confidence is 0 (the HA alerts no longer print it, sweep HA26)
  assert(s.snap.confidence == 0 && !s.snap.presence);
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

#endif  // VISION_DWELL_END_GRACE_MS == 0

// ---- sweep F152: each visit starts its own interaction clock ----

// The visits below never dwell (each is far under DWELL_START_MS), so the
// only way they qualify is zone_interaction_then_left: seen in one settled
// cell for ZONE_INTERACTION_MS of this visit.
static_assert(4000 >= ZONE_INTERACTION_MS + 1000 && 4000 < DWELL_START_MS, "a long settled visit");
static_assert(1000 < ZONE_INTERACTION_MS, "a short visit");

// A fresh FSM's 1 s visit: no interaction (this held before F152 too).
static void test_fresh_short_visit_is_not_an_interaction() {
  PresenceFSM fsm;
  fsm.reset();
  uint32_t t = 1000;
  const Visit v = visit(fsm, t, 1, 1, 1000);
  assert(v.events == "presence_started presence_ended");
  assert(!v.interaction);
}

// A 4 s visit settled in one cell is an interaction (and still is); a 1 s
// revisit in the same cell after it is not. Before F152 the revisit's
// interaction clock was the first visit's settle time, so its second frame
// qualified and it ended in zone_interaction_then_left.
static void test_revisit_starts_its_own_clock() {
  PresenceFSM fsm;
  fsm.reset();
  uint32_t t = 1000;
  const Visit first = visit(fsm, t, 1, 1, 4000);
  assert(first.events == "presence_started presence_ended interaction_likely:zone_interaction_then_left");
  t += 30000;  // any later time
  const Visit again = visit(fsm, t, 1, 1, 1000);
  assert(again.events == "presence_started presence_ended");
  assert(!again.interaction);
  std::printf("  1 s revisit after a 4 s visit: %s\n", again.events.c_str());
}

// A 0.5 s pass through another cell after a 4 s visit: no interaction, and
// its presence_started names the cell the pass began in. Before F152 it named
// the last visit's cell (the tracker kept it until the new one settled, three
// frames in) and the pass ended in interaction_likely.
static void test_pass_in_another_cell_names_its_own_cell() {
  PresenceFSM fsm;
  fsm.reset();
  uint32_t t = 1000;
  const Visit first = visit(fsm, t, 2, 0, 4000);
  assert(first.interaction);
  assert(first.started.voxel.r == 2 && first.started.voxel.c == 0);
  const Visit pass = visit(fsm, t, 0, 2, 500);
  assert(pass.events == "presence_started presence_ended");
  assert(pass.started.voxel.r == 0 && pass.started.voxel.c == 2);  // not (2,0)
  assert(pass.started.voxel.rows == VOXEL_ROWS && pass.started.voxel.cols == VOXEL_COLS);
  std::printf("  0.5 s pass in (0,2) after a 4 s visit in (2,0): %s, opened on (%d,%d)\n",
              pass.events.c_str(), pass.started.voxel.r, pass.started.voxel.c);
}

// After short visits, a long settled one still qualifies, on its own clock:
// not before ZONE_INTERACTION_MS of this visit in one cell.
static void test_long_settled_visit_still_qualifies() {
  PresenceFSM fsm;
  fsm.reset();
  uint32_t t = 1000;
  visit(fsm, t, 1, 1, 1000);
  visit(fsm, t, 0, 0, 500);
  const Visit stay = visit(fsm, t, 1, 1, 4000);
  assert(stay.events == "presence_started presence_ended interaction_likely:zone_interaction_then_left");
  assert(std::strcmp(stay.reason, "zone_interaction_then_left") == 0);
  // a visit that leaves just short of the zone window does not
  const Visit nearly = visit(fsm, t, 1, 1, ZONE_INTERACTION_MS - 200);
  assert(!nearly.interaction);
  // and one that stays just past it does
  const Visit just = visit(fsm, t, 1, 1, ZONE_INTERACTION_MS + 200);
  assert(just.interaction);
}

// The first sighting after boot seeds the settled cell, as before.
static void test_first_visit_after_boot_opens_on_its_cell() {
  PresenceFSM fsm;
  fsm.reset();
  const StateSnapshot idle = fsm.snapshot(0, "boot");
  assert(idle.voxel.r == -1 && idle.voxel.c == -1);
  uint32_t t = 1000;
  const Visit v = visit(fsm, t, 2, 1, 300);
  assert(v.started.voxel.r == 2 && v.started.voxel.c == 1);
  // and the cell stays after the visit ends (presence_ended names where they were)
  const StateSnapshot after = fsm.snapshot(t, "presence_ended");
  assert(after.voxel.r == 2 && after.voxel.c == 1);
}

// ---- F152's review: a visit seen again on the frame after presence_ended ----

struct Log {
  std::string events;
  std::vector<Seen> seen;
};
static void note(Log& log, const Seen& s) {
  if (!log.events.empty()) log.events += ' ';
  log.events += s.name;
  if (s.reason) { log.events += ':'; log.events += s.reason; }
  log.seen.push_back(s);
}
// `ms` of frames showing `vs`, every 100 ms from t; t ends after the last.
static void frames(PresenceFSM& fsm, uint32_t& t, Log& log, const VisionSample& vs, uint32_t ms) {
  for (const uint32_t end = t + ms; t < end; t += 100) {
    Seen s;
    if (step(fsm, vs, t, s)) note(log, s);
  }
}
// Empty frames until `name` is emitted; t ends on the frame after it.
static void empty_until(PresenceFSM& fsm, uint32_t& t, Log& log, const char* name) {
  for (const uint32_t give_up = t + 60000; t < give_up; t += 100) {
    Seen s;
    if (step(fsm, empty(), t, s)) {
      note(log, s);
      if (is(s, name)) { t += 100; return; }
    }
  }
  assert(false && "never emitted");
}
static const Seen& nth(const Log& log, const char* name, int n = 0) {
  for (const Seen& s : log.seen)
    if (is(s, name) && n-- == 0) return s;
  assert(false && "no such event");
  return log.seen.front();
}
static uint32_t settle_ms() {
  return canary::cfg::detect().lost_timeout_ms + DWELL_END_GRACE_MS + INTERACTION_AFTER_LEAVE_WINDOW_MS + 1000;
}

// A 4 s visit settled in (1,1) qualifies; someone is seen in (0,2) on the
// frame right after its presence_ended, for 1 s (not a qualifying visit).
// The ended visit's interaction_likely goes out on the frame after that
// presence_started, with its own reason and length; the 1 s visit's end
// brings none. On the FSM before this, nothing was sent for either.
static void test_back_to_back_visit_keeps_the_ended_visits_interaction() {
  PresenceFSM fsm;
  fsm.reset();
  Log log;
  uint32_t t = 1000;
  frames(fsm, t, log, person(1, 1), 4000);
  empty_until(fsm, t, log, "presence_ended");
  frames(fsm, t, log, person(0, 2), 1000);
  frames(fsm, t, log, empty(), settle_ms());
  std::printf("  seen again on the frame after presence_ended: %s\n", log.events.c_str());
  assert(log.events == "presence_started presence_ended presence_started "
                       "interaction_likely:zone_interaction_then_left presence_ended");
  const Seen& ended = nth(log, "presence_ended");
  const Seen& again = nth(log, "presence_started", 1);
  const Seen& late = nth(log, "interaction_likely");
  assert(again.t == ended.t + 100);  // the very next frame
  assert(late.t == again.t + 100);   // and the one after it
  assert(late.snap.visit_ms == ended.snap.visit_ms && late.snap.visit_ms > 4000);  // the ended visit's
  // the row is the new visit's frame: present, its box, its cell
  assert(late.snap.presence && late.snap.confidence == 91);
  assert(late.snap.voxel.r == 0 && late.snap.voxel.c == 2);
  assert(nth(log, "presence_ended", 1).snap.visit_ms < 4000);  // the 1 s visit's own length
}

// The same for a visit that dwelled: dwell_then_left, owed and sent.
static void test_back_to_back_after_a_dwell() {
  PresenceFSM fsm;
  fsm.reset();
  Log log;
  uint32_t t = 1000;
  frames(fsm, t, log, person(1, 1), DWELL_START_MS + 2000);
  empty_until(fsm, t, log, "presence_ended");
  frames(fsm, t, log, person(1, 1), 300);
  frames(fsm, t, log, empty(), settle_ms());
  std::printf("  a dweller seen again on the frame after presence_ended: %s\n", log.events.c_str());
  assert(log.events == "presence_started dwell_started dwell_ended presence_ended presence_started "
                       "interaction_likely:dwell_then_left presence_ended");
  assert(nth(log, "interaction_likely").snap.visit_ms == nth(log, "presence_ended").snap.visit_ms);
}

// Seen on that one frame only: the owed event goes out on the next, empty,
// frame (the new visit is still held present through its lost timeout).
static void test_one_frame_back_still_sends_it() {
  PresenceFSM fsm;
  fsm.reset();
  Log log;
  uint32_t t = 1000;
  frames(fsm, t, log, person(1, 1), 4000);
  empty_until(fsm, t, log, "presence_ended");
  frames(fsm, t, log, person(1, 1), 100);
  frames(fsm, t, log, empty(), settle_ms());
  assert(log.events == "presence_started presence_ended presence_started "
                       "interaction_likely:zone_interaction_then_left presence_ended");
  const Seen& late = nth(log, "interaction_likely");
  assert(late.t == nth(log, "presence_started", 1).t + 100);
  assert(late.snap.presence && late.snap.confidence == 0);  // an empty frame
}

// The visit back on that frame is judged on its own as well: a qualifying
// one ends in its own interaction_likely, with its own length.
static void test_back_to_back_visits_each_report() {
  PresenceFSM fsm;
  fsm.reset();
  Log log;
  uint32_t t = 1000;
  frames(fsm, t, log, person(1, 1), 4000);
  empty_until(fsm, t, log, "presence_ended");
  frames(fsm, t, log, person(2, 2), 6000);
  frames(fsm, t, log, empty(), settle_ms());
  assert(log.events == "presence_started presence_ended presence_started "
                       "interaction_likely:zone_interaction_then_left presence_ended "
                       "interaction_likely:zone_interaction_then_left");
  assert(nth(log, "interaction_likely", 0).snap.visit_ms == nth(log, "presence_ended", 0).snap.visit_ms);
  assert(nth(log, "interaction_likely", 1).snap.visit_ms == nth(log, "presence_ended", 1).snap.visit_ms);
  assert(nth(log, "presence_ended", 1).snap.visit_ms > 6000);
}

// Guards: nothing is owed for a visit that did not qualify, and an owed
// event whose window has closed by the next frame is dropped, not sent late.
static void test_nothing_owed_unless_qualified_and_in_the_window() {
  {
    PresenceFSM fsm;
    fsm.reset();
    Log log;
    uint32_t t = 1000;
    frames(fsm, t, log, person(1, 1), 1000);
    empty_until(fsm, t, log, "presence_ended");
    frames(fsm, t, log, person(1, 1), 1000);
    frames(fsm, t, log, empty(), settle_ms());
    assert(log.events == "presence_started presence_ended presence_started presence_ended");
  }
  {
    PresenceFSM fsm;
    fsm.reset();
    Log log;
    uint32_t t = 1000;
    frames(fsm, t, log, person(1, 1), 4000);
    empty_until(fsm, t, log, "presence_ended");
    const uint32_t left = nth(log, "presence_ended").t;
    frames(fsm, t, log, person(1, 1), 100);  // the next frame
    // a stalled camera: the frame after that lands past the window
    t = left + INTERACTION_AFTER_LEAVE_WINDOW_MS + 100;
    frames(fsm, t, log, empty(), settle_ms());
    assert(log.events == "presence_started presence_ended presence_started presence_ended");
  }
}

// ---- sweep F154: the dwell end grace (the build with a grace) ----
#if VISION_DWELL_END_GRACE_MS > 0

// Stay past the dwell start in (1,1); returns the dwell_started tick and
// leaves `t` on the frame after the last sighting.
static uint32_t dwell_then_leave(PresenceFSM& fsm, uint32_t& t, uint32_t& present_at,
                                 uint32_t& last_seen) {
  Seen s;
  present_at = t;
  assert(step(fsm, person(), t, s) && is(s, "presence_started"));
  uint32_t dwell_at = 0;
  for (t += 100; t <= present_at + DWELL_START_MS + 1000; t += 100)
    if (step(fsm, person(), t, s) && is(s, "dwell_started")) dwell_at = t;
  assert(dwell_at == present_at + DWELL_START_MS);
  last_seen = t - 100;
  return dwell_at;
}

static void test_grace_holds_the_dweller_then_dwell_ended_fires() {
  static_assert(DWELL_END_GRACE_MS > LOST_TIMEOUT_MS, "the grace build");
  PresenceFSM fsm;
  fsm.reset();
  Seen s;
  uint32_t t = 1000, present_at = 0, last_seen = 0;
  const uint32_t dwell_at = dwell_then_leave(fsm, t, present_at, last_seen);
  // past the lost timeout and up to the grace: still present and dwelling,
  // the dwell running, nothing sent
  for (; t - last_seen <= DWELL_END_GRACE_MS; t += 100) {
    assert(!step(fsm, empty(), t, s));
    assert(s.snap.presence && s.snap.dwelling);
    assert(s.snap.dwell_ms == t - dwell_at);
    assert(s.snap.confidence == 0);  // nobody in this frame
  }
  // the first frame past the grace ends the dwell, with its length
  assert(t - last_seen > DWELL_END_GRACE_MS);
  assert(step(fsm, empty(), t, s) && is(s, "dwell_ended"));
  assert(s.snap.presence && !s.snap.dwelling);
  assert(s.snap.dwell_ms == t - dwell_at);
  assert(s.snap.dwell_ms > (last_seen - dwell_at) + DWELL_END_GRACE_MS);
  const uint32_t ended = t;
  // then the stay, on the next frame
  t += 100;
  assert(step(fsm, empty(), t, s) && is(s, "presence_ended"));
  assert(!s.snap.presence && s.snap.dwell_ms == 0);
  assert(s.snap.visit_ms == t - present_at);
  t += 100;
  assert(step(fsm, empty(), t, s) && is(s, "interaction_likely"));
  assert(std::strcmp(s.reason, "dwell_then_left") == 0);
  std::printf("  grace %lu ms: dwell_ended %lu ms after the last sighting, dwell %lu ms\n",
              (unsigned long)DWELL_END_GRACE_MS, (unsigned long)(ended - last_seen),
              (unsigned long)(ended - dwell_at));
}

// A dweller who drops out of frame for longer than the lost timeout but less
// than the grace keeps the dwell: no event at all, and the dwell runs on from
// where it started.
static void test_dweller_back_within_the_grace_keeps_the_dwell() {
  PresenceFSM fsm;
  fsm.reset();
  Seen s;
  uint32_t t = 1000, present_at = 0, last_seen = 0;
  const uint32_t dwell_at = dwell_then_leave(fsm, t, present_at, last_seen);
  const uint32_t gap = LOST_TIMEOUT_MS + (DWELL_END_GRACE_MS - LOST_TIMEOUT_MS) / 2;
  for (; t - last_seen < gap; t += 100) assert(!step(fsm, empty(), t, s));
  assert(t - last_seen > LOST_TIMEOUT_MS);
  for (int i = 0; i < 20; ++i, t += 100) {
    assert(!step(fsm, person(), t, s));  // no presence_started, no dwell_started
    assert(s.snap.presence && s.snap.dwelling);
    assert(s.snap.dwell_ms == t - dwell_at);
    assert(s.snap.presence_ms == t - present_at);
  }
}

// The grace is for dwellers: a walk-by is let go at the lost timeout.
static void test_grace_is_for_dwellers_only() {
  PresenceFSM fsm;
  fsm.reset();
  Seen s;
  uint32_t t = 1000;
  for (const uint32_t end = t + 2000; t < end; t += 100) step(fsm, person(), t, s);
  const uint32_t last_seen = t - 100;
  for (;; t += 100) {
    if (step(fsm, empty(), t, s)) break;
    assert(t - last_seen <= LOST_TIMEOUT_MS);
  }
  assert(is(s, "presence_ended"));
  assert(t - last_seen > LOST_TIMEOUT_MS && t - last_seen <= LOST_TIMEOUT_MS + 100);
}

// A lost timeout longer than the grace governs a dweller too.
static void test_longer_lost_timeout_governs() {
  canary::cfg::g_test_cfg.lost_timeout_ms = DWELL_END_GRACE_MS + 2000;
  PresenceFSM fsm;
  fsm.reset();
  Seen s;
  uint32_t t = 1000, present_at = 0, last_seen = 0;
  dwell_then_leave(fsm, t, present_at, last_seen);
  for (;; t += 100) {
    if (step(fsm, empty(), t, s)) break;
    assert(t - last_seen <= DWELL_END_GRACE_MS + 2000);
  }
  assert(is(s, "dwell_ended"));
  assert(t - last_seen > DWELL_END_GRACE_MS + 2000);
  canary::cfg::g_test_cfg.lost_timeout_ms = LOST_TIMEOUT_MS;
}
#endif  // VISION_DWELL_END_GRACE_MS > 0

int main() {
  test_fresh_short_visit_is_not_an_interaction();
  test_revisit_starts_its_own_clock();
  test_pass_in_another_cell_names_its_own_cell();
  test_long_settled_visit_still_qualifies();
  test_first_visit_after_boot_opens_on_its_cell();
  test_back_to_back_visit_keeps_the_ended_visits_interaction();
  test_back_to_back_after_a_dwell();
  test_one_frame_back_still_sends_it();
  test_back_to_back_visits_each_report();
  test_nothing_owed_unless_qualified_and_in_the_window();
#if VISION_DWELL_END_GRACE_MS > 0
  test_grace_holds_the_dweller_then_dwell_ended_fires();
  test_dweller_back_within_the_grace_keeps_the_dwell();
  test_grace_is_for_dwellers_only();
  test_longer_lost_timeout_governs();
  std::printf("ALL VISION PRESENCE FSM TESTS PASSED (dwell end grace %lu ms)\n",
              (unsigned long)DWELL_END_GRACE_MS);
#else
  // the shipped grace: 0, the lost timeout ends every stay
  static_assert(DWELL_END_GRACE_MS == 0, "the shipped build");
  test_linger_dwell_ended_reports_its_dwell();
  test_return_after_dwell_ended_starts_from_zero();
  test_walk_by_never_dwells();
  test_reset_clears_the_latch();
  std::printf("ALL VISION PRESENCE FSM TESTS PASSED\n");
#endif
  return 0;
}
