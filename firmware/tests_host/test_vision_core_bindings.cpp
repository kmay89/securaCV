// The Canary Vision core the Lab runs (canary-local/emulator/dist/
// canary-vision-core.js), built natively: the same five sources build.sh's
// vision flavor hands em++ (detect_config.cpp, presence_fsm.cpp,
// voxel_tracker.cpp, and the emulator's vision_core_bindings.cpp and
// vision_core_shim.cpp, with the emulator's Arduino shim), linked here with
// g++ and a one-macro <emscripten.h>. It holds the tick JSON the Vision
// page's MQTT pane reads to what the device publishes.
//
// Pinned (sweep A39): the "fsm" object carries the voxel tracker's settled
// cell (snapshot.voxel, the cell publish_event_json writes) and visit_ms
// (last_visit_ms_), so the pane no longer shows the frame's cell or keeps a
// latch of its own. The settled cell differs from the frame's: it moves
// only once the person has been seen away from it three times in a row
// (voxel_tracker.cpp's VOXEL_STABLE_N), and stays on the last cell once the
// frame is empty. Each visit starts its own tracker (sweep F152: PresenceFSM
// resets it on the frame that starts a visit), so a later visit's
// presence_started names the cell that visit began in, and a short later
// visit no longer inherits an earlier one's interaction clock: pinned below
// because the README, the pane's note and the bindings comment say so.
// (Before F152 the tracker was reset only in reset(), at boot, and a later
// visit opened on the previous visit's cell; A39 pinned that.) A visit that
// qualified still reports interaction_likely when someone is seen on the
// frame right after its presence_ended (F152's review): on the frame after
// the next visit's presence_started, with the ended visit's visit_ms. And
// (sweep F130) dwell_ended's dwell_ms is the dwell it closed, through the same
// ABI, counted to the frame that declared the person gone, so it includes
// the lost timeout. And (sweep F186) a dweller seen on the frame after
// dwell_ended starts no second dwell: that frame sends presence_ended and the
// next visit opens on the frame after, as the device does, from the next
// frame's own sighting when it has one; and vision_emu_reset, which the Lab
// calls on every scene change and camera start, forgets an owed
// presence_ended and a held sighting (F186's review). And (sweep F202) a
// person last seen on the frame that sends dwell_started leaves a stay whose
// interaction_likely says dwell_then_left, as the device's does.
//
// Before A39 the fsm object had neither key, and this suite fails on it.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "canary/config.h"

extern "C" {
const char* vision_emu_contract_json();
void vision_emu_reset();
void vision_emu_set_config(int target, int score, unsigned int lost_ms, unsigned int dwell_ms);
void vision_emu_begin_frame();
int vision_emu_push_box(int x, int y, int w, int h, int score, int target);
const char* vision_emu_tick_json(unsigned int now_ms);
}

namespace {

// The object that follows "<name>": in the tick JSON, braces balanced.
std::string object_at(const std::string& json, const char* name) {
  const std::string key = std::string("\"") + name + "\":{";
  const size_t at = json.find(key);
  if (at == std::string::npos) {
    std::fprintf(stderr, "no %s object in %s\n", name, json.c_str());
    std::abort();
  }
  size_t i = at + key.size() - 1;
  int depth = 0;
  for (size_t j = i; j < json.size(); ++j) {
    if (json[j] == '{') depth++;
    if (json[j] == '}' && --depth == 0) return json.substr(i, j - i + 1);
  }
  std::abort();
}

// The integer after "<key>": in a flat or nested object (first match).
long int_at(const std::string& obj, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const size_t at = obj.find(k);
  if (at == std::string::npos) {
    std::fprintf(stderr, "no %s in %s\n", key, obj.c_str());
    std::abort();
  }
  return std::strtol(obj.c_str() + at + k.size(), nullptr, 10);
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

struct Tick {
  std::string json, sample, fsm, sample_voxel, fsm_voxel;
};

// A person whose box center sits in cell (r, c) of the 3x3 grid, or an
// empty frame when r < 0.
Tick frame(unsigned int now_ms, int r = -1, int c = -1) {
  vision_emu_begin_frame();
  if (r >= 0) {
    const int cw = FRAME_W / VOXEL_COLS, ch = FRAME_H / VOXEL_ROWS, w = 40, h = 70;
    // detection_pipeline.h's cell is the one under x + w/2, y + h/2
    assert(vision_emu_push_box(c * cw + cw / 2 - w / 2, r * ch + ch / 2 - h / 2, w, h, 90,
                               PERSON_TARGET) == 1);
  }
  Tick t;
  t.json = vision_emu_tick_json(now_ms);
  t.sample = object_at(t.json, "sample");
  t.fsm = object_at(t.json, "fsm");
  t.sample_voxel = object_at(t.sample, "voxel");
  t.fsm_voxel = object_at(t.fsm, "voxel");
  return t;
}

bool event_is(const Tick& t, const char* name) {
  return has(t.json, (std::string("\"event\":\"") + name + "\"").c_str());
}

void test_settled_cell_and_visit() {
  vision_emu_reset();
  vision_emu_set_config(PERSON_TARGET, SCORE_MIN, LOST_TIMEOUT_MS, DWELL_START_MS);
  unsigned int t = 1000;

  // Nobody yet: the tracker has no cell, the reset Voxel{-1,-1,0,0} the
  // device's first state row prints.
  Tick k = frame(t);
  assert(int_at(k.fsm_voxel, "r") == -1 && int_at(k.fsm_voxel, "c") == -1);
  assert(int_at(k.fsm_voxel, "rows") == 0 && int_at(k.fsm_voxel, "cols") == 0);
  assert(int_at(k.fsm, "visit_ms") == 0);

  // Someone in cell (2,0): the first sighting seeds the settled cell.
  t += 100;
  k = frame(t, 2, 0);
  assert(event_is(k, "presence_started"));
  const unsigned int present_at = t;
  assert(int_at(k.fsm_voxel, "r") == 2 && int_at(k.fsm_voxel, "c") == 0);
  assert(int_at(k.fsm_voxel, "rows") == VOXEL_ROWS && int_at(k.fsm_voxel, "cols") == VOXEL_COLS);
  for (int i = 0; i < 5; ++i) frame(t += 100, 2, 0);

  // They step into (0,2): the frame's cell moves at once, the settled cell
  // holds for the first frames (voxel_tracker.cpp's VOXEL_STABLE_N = 3).
  k = frame(t += 100, 0, 2);
  assert(int_at(k.sample_voxel, "r") == 0 && int_at(k.sample_voxel, "c") == 2);
  assert(int_at(k.fsm_voxel, "r") == 2 && int_at(k.fsm_voxel, "c") == 0);
  k = frame(t += 100, 0, 2);
  assert(int_at(k.fsm_voxel, "r") == 2 && int_at(k.fsm_voxel, "c") == 0);
  k = frame(t += 100, 0, 2);
  assert(int_at(k.fsm_voxel, "r") == 0 && int_at(k.fsm_voxel, "c") == 2);  // settled

  // They leave. The empty frame has no cell; the settled one stays.
  const unsigned int last_seen = t;
  Tick ended{};
  bool found = false;
  for (t += 100; t < last_seen + LOST_TIMEOUT_MS + 1000; t += 100) {
    k = frame(t);
    assert(int_at(k.sample_voxel, "r") == -1);
    assert(int_at(k.fsm_voxel, "r") == 0 && int_at(k.fsm_voxel, "c") == 2);
    if (event_is(k, "presence_ended")) { ended = k; found = true; break; }
    assert(int_at(k.fsm, "visit_ms") == 0);  // no stay has ended yet
  }
  assert(found);
  // visit_ms is last_visit_ms_: the stay presence_ended closed
  assert(int_at(ended.fsm, "visit_ms") == (long)(t - present_at));
  assert(int_at(ended.fsm, "presence_ms") == 0);
  // and it is kept until the next stay ends
  k = frame(t + 5000);
  assert(int_at(k.fsm, "visit_ms") == (long)(t - present_at));
  std::printf("  settled cell held 2 frames, visit %ld ms\n", int_at(ended.fsm, "visit_ms"));
}

// A second visit after the first has ended, in another cell. PresenceFSM
// resets its tracker on the frame that starts a visit (sweep F152), so the
// new visit opens on its own cell, which the first sighting seeds, and the
// cell the last visit settled in is kept only until then. Before F152 the
// new visit opened on the old visit's cell and kept it until the person had
// been seen in the new one three times in a row, and its interaction clock
// started in the old visit, so this short visit ended in interaction_likely.
void test_each_visit_opens_on_its_own_cell() {
  vision_emu_reset();
  vision_emu_set_config(PERSON_TARGET, SCORE_MIN, LOST_TIMEOUT_MS, DWELL_START_MS);
  unsigned int t = 1000;
  Tick k = frame(t, 2, 0);
  assert(event_is(k, "presence_started"));
  // long enough in one cell to qualify (ZONE_INTERACTION_MS), short of a dwell
  for (int i = 0; i < 30; ++i) frame(t += 100, 2, 0);
  bool first_interaction = false;
  for (t += 100;; t += 100) {
    k = frame(t);
    if (event_is(k, "interaction_likely")) { first_interaction = true; break; }
    assert(t < 10000);
  }
  assert(first_interaction && has(k.json, "\"reason\":\"zone_interaction_then_left\""));
  // long enough later that no leave-side event is pending; the idle rows
  // still name where the last visit settled
  t += INTERACTION_AFTER_LEAVE_WINDOW_MS + 1000;
  k = frame(t);
  assert(int_at(k.fsm_voxel, "r") == 2 && int_at(k.fsm_voxel, "c") == 0);

  // the next visit, a short pass in (0,2)
  k = frame(t += 100, 0, 2);
  assert(event_is(k, "presence_started"));
  assert(int_at(k.sample_voxel, "r") == 0 && int_at(k.sample_voxel, "c") == 2);
  assert(int_at(k.fsm_voxel, "r") == 0 && int_at(k.fsm_voxel, "c") == 2);  // its own cell
  assert(int_at(k.fsm_voxel, "rows") == VOXEL_ROWS && int_at(k.fsm_voxel, "cols") == VOXEL_COLS);
  for (int i = 0; i < 4; ++i) {
    k = frame(t += 100, 0, 2);
    assert(int_at(k.fsm_voxel, "r") == 0 && int_at(k.fsm_voxel, "c") == 2);
    assert(has(k.json, "\"event\":null"));
  }
  // it leaves after 0.5 s: presence_ended, and no interaction_likely
  bool ended = false;
  for (const unsigned int stop = t + LOST_TIMEOUT_MS + INTERACTION_AFTER_LEAVE_WINDOW_MS + 1000;
       t < stop;) {
    k = frame(t += 100);
    if (event_is(k, "presence_ended")) ended = true;
    assert(!event_is(k, "interaction_likely"));
  }
  assert(ended);
  std::printf("  next visit opened on its own cell; a 0.5 s pass ended without interaction_likely\n");
}

void test_back_to_back_visit_through_the_abi() {
  vision_emu_reset();
  vision_emu_set_config(PERSON_TARGET, SCORE_MIN, LOST_TIMEOUT_MS, DWELL_START_MS);
  unsigned int t = 1000;
  Tick k = frame(t, 1, 1);
  assert(event_is(k, "presence_started"));
  for (int i = 0; i < 39; ++i) frame(t += 100, 1, 1);  // 4 s in (1,1): it qualifies
  for (;;) {
    k = frame(t += 100);
    if (event_is(k, "presence_ended")) break;
    assert(!has(k.json, "\"event\":\"interaction_likely\"") && t < 10000);
  }
  const long visit = int_at(k.fsm, "visit_ms");
  assert(visit > 4000);
  // seen again on the very next frame, in (0,2): the next visit starts...
  k = frame(t += 100, 0, 2);
  assert(event_is(k, "presence_started"));
  // ...and the ended visit's interaction_likely goes out on the frame after
  k = frame(t += 100, 0, 2);
  assert(event_is(k, "interaction_likely"));
  assert(has(k.json, "\"reason\":\"zone_interaction_then_left\""));
  assert(int_at(k.fsm, "visit_ms") == visit);  // the ended visit's length
  assert(has(k.fsm, "\"presence\":true") && int_at(k.fsm, "confidence") == 90);  // the new visit's frame
  assert(int_at(k.fsm_voxel, "r") == 0 && int_at(k.fsm_voxel, "c") == 2);
  // the 1 s visit then ends without one of its own
  for (int i = 0; i < 8; ++i) frame(t += 100, 0, 2);
  bool ended = false;
  for (const unsigned int stop = t + LOST_TIMEOUT_MS + INTERACTION_AFTER_LEAVE_WINDOW_MS + 1000; t < stop;) {
    k = frame(t += 100);
    if (event_is(k, "presence_ended")) ended = true;
    assert(!event_is(k, "interaction_likely"));
  }
  assert(ended);
  std::printf("  a visit seen again on the next frame still reported interaction_likely (%ld ms)\n", visit);
}

void test_dwell_ended_through_the_abi() {
  vision_emu_reset();
  vision_emu_set_config(PERSON_TARGET, SCORE_MIN, 500, 1000);
  unsigned int t = 0;
  Tick k = frame(t, 1, 1);
  assert(event_is(k, "presence_started"));
  unsigned int dwell_at = 0;
  for (t += 100; t <= 2000; t += 100) {
    k = frame(t, 1, 1);
    if (event_is(k, "dwell_started")) {
      dwell_at = t;
      assert(int_at(k.fsm, "dwell_ms") == 0);
    }
  }
  assert(dwell_at == 1000);
  const unsigned int last_seen = t - 100;
  for (;; t += 100) {
    k = frame(t);
    if (event_is(k, "dwell_ended")) break;
    assert(t < 4000);
  }
  assert(has(k.fsm, "\"dwelling\":false"));
  assert(int_at(k.fsm, "dwell_ms") == (long)(t - dwell_at));
  // counted to the frame that declared the person gone, not to the last
  // sighting: the length includes the lost timeout (500 ms here)
  assert(t - last_seen > 500);
  assert(int_at(k.fsm, "dwell_ms") > (long)(last_seen - dwell_at) + 500);
  k = frame(t += 100);
  assert(event_is(k, "presence_ended"));
  assert(int_at(k.fsm, "dwell_ms") == 0);
}

// Sweep F186, through the ABI: a dweller seen on the frame after dwell_ended
// does not start a second dwell in the same stay. That frame ends the stay
// (presence_ended, the stay's cell), the next visit opens on the frame after
// on the sighting's cell, and the ended stay's interaction_likely follows.
// Before F186 the sighting took the dwell_started branch again at once.
void test_seen_after_dwell_ended_through_the_abi() {
  vision_emu_reset();
  vision_emu_set_config(PERSON_TARGET, SCORE_MIN, 500, 1000);
  unsigned int t = 0;
  Tick k = frame(t, 1, 1);
  assert(event_is(k, "presence_started"));
  int dwells = 0;
  for (t += 100; t <= 1900; t += 100) dwells += event_is(frame(t, 1, 1), "dwell_started") ? 1 : 0;
  assert(dwells == 1);
  for (;; t += 100) {
    k = frame(t);
    if (event_is(k, "dwell_ended")) break;
    assert(t < 4000);
  }
  const long dwell = int_at(k.fsm, "dwell_ms");
  // seen on the very next frame, in (0,2): it ends the stay...
  k = frame(t += 100, 0, 2);
  assert(event_is(k, "presence_ended"));
  assert(has(k.fsm, "\"presence\":false") && int_at(k.fsm, "visit_ms") == (long)t);
  assert(int_at(k.fsm_voxel, "r") == 1 && int_at(k.fsm_voxel, "c") == 1);
  const long visit = int_at(k.fsm, "visit_ms");
  // ...the next visit opens on the frame after, on its own cell...
  k = frame(t += 100, 0, 2);
  assert(event_is(k, "presence_started"));
  assert(has(k.fsm, "\"presence\":true") && int_at(k.fsm, "presence_ms") == 0);
  assert(int_at(k.fsm_voxel, "r") == 0 && int_at(k.fsm_voxel, "c") == 2);
  // ...and the ended stay's interaction_likely follows, with its length
  k = frame(t += 100, 0, 2);
  assert(event_is(k, "interaction_likely") && has(k.json, "\"reason\":\"dwell_then_left\""));
  assert(int_at(k.fsm, "visit_ms") == visit);
  // the short visit then ends with no dwell of its own
  frame(t += 100, 0, 2);
  bool ended = false;
  for (const unsigned int stop = t + 500 + INTERACTION_AFTER_LEAVE_WINDOW_MS + 1000; t < stop;) {
    k = frame(t += 100);
    assert(!event_is(k, "dwell_started") && !event_is(k, "dwell_ended"));
    if (event_is(k, "presence_ended")) ended = true;
  }
  assert(ended);
  std::printf("  seen on the frame after dwell_ended (dwell %ld ms): presence_ended, then the next visit\n",
              dwell);
}

// Seen on the frame after dwell_ended in one cell and on the next frame in
// another: the next frame's own sighting opens the visit, so its
// presence_started names the cell the person is in on that frame.
void test_seen_in_the_gap_then_elsewhere_through_the_abi() {
  vision_emu_reset();
  vision_emu_set_config(PERSON_TARGET, SCORE_MIN, 500, 1000);
  unsigned int t = 0;
  Tick k = frame(t, 1, 1);
  for (t += 100; t <= 1900; t += 100) frame(t, 1, 1);
  for (;; t += 100) {
    k = frame(t);
    if (event_is(k, "dwell_ended")) break;
    assert(t < 4000);
  }
  k = frame(t += 100, 0, 2);
  assert(event_is(k, "presence_ended"));
  k = frame(t += 100, 2, 0);
  assert(event_is(k, "presence_started"));
  assert(int_at(k.fsm_voxel, "r") == 2 && int_at(k.fsm_voxel, "c") == 0);
  const unsigned int last_seen = t;
  k = frame(t += 100);
  assert(event_is(k, "interaction_likely"));
  // held present through the lost timeout (500 ms here) from that sighting
  for (;; ) {
    k = frame(t += 100);
    if (event_is(k, "presence_ended")) break;
    assert(t - last_seen <= 500);
  }
  assert(t - last_seen > 500 && t - last_seen <= 600);
  assert(int_at(k.fsm_voxel, "r") == 2 && int_at(k.fsm_voxel, "c") == 0);
  std::printf("  seen in the gap in (0,2), then in (2,0): the visit opens on (2,0)\n");
}

// The Lab resets this core on every scene change and camera start
// (vision-ui.js and eyes-bench.js call vision_emu_reset), so a reset can
// land right after dwell_ended, while the stay's presence_ended is owed, or
// right after a sighting on the frame after it, while that sighting is held
// for the next visit (sweep F186). Through the ABI, in both places: the
// empty frames after the reset send nothing, and the next sighting opens a
// visit on its own frame and cell.
void test_reset_after_dwell_ended_through_the_abi() {
  for (const bool seen_in_the_gap : {false, true}) {
    vision_emu_reset();
    vision_emu_set_config(PERSON_TARGET, SCORE_MIN, 500, 1000);
    unsigned int t = 0;
    Tick k = frame(t, 1, 1);
    for (t += 100; t <= 1900; t += 100) frame(t, 1, 1);
    for (;; t += 100) {
      k = frame(t);
      if (event_is(k, "dwell_ended")) break;
      assert(t < 4000);
    }
    if (seen_in_the_gap) {
      k = frame(t += 100, 0, 2);
      assert(event_is(k, "presence_ended"));
    }
    vision_emu_reset();
    for (const unsigned int stop = t + 500 + INTERACTION_AFTER_LEAVE_WINDOW_MS + 1000; t < stop;) {
      k = frame(t += 100);
      if (!has(k.json, "\"event\":null")) {
        std::fprintf(stderr, "reset after dwell_ended%s, then an empty frame sent: %s\n",
                     seen_in_the_gap ? " and a sighting" : "", k.json.c_str());
        std::abort();
      }
      assert(has(k.fsm, "\"presence\":false"));
    }
    k = frame(t += 100, 2, 0);
    assert(event_is(k, "presence_started"));
    assert(has(k.fsm, "\"presence\":true") && int_at(k.fsm, "presence_ms") == 0);
    assert(int_at(k.fsm_voxel, "r") == 2 && int_at(k.fsm_voxel, "c") == 0);
  }
  std::printf("  reset after dwell_ended, and after a sighting on the frame after it: nothing owed\n");
}

// Sweep F202, through the ABI: a person last seen on the frame that sends
// dwell_started leaves a stay that dwelled, and its interaction_likely says
// so (dwell_then_left), settled in one cell or with the settled cell moving
// every 0.5 s. Before F202 the dwell was latched for the leave only on a
// later sighted frame, so the settled stay reported zone_interaction_then_left
// and the moving one no interaction_likely at all.
void test_last_seen_on_the_dwell_started_frame_through_the_abi() {
  static const int ring[8][2] = {{0, 0}, {0, 1}, {0, 2}, {1, 2}, {2, 2}, {2, 1}, {2, 0}, {1, 0}};
  for (const bool moving : {false, true}) {
    vision_emu_reset();
    vision_emu_set_config(PERSON_TARGET, SCORE_MIN, LOST_TIMEOUT_MS, DWELL_START_MS);
    unsigned int t = 1000;
    bool dwelled = false;
    for (int i = 0; !dwelled; ++i, t += 100) {
      const int r = moving ? ring[(i / 5) % 8][0] : 1, c = moving ? ring[(i / 5) % 8][1] : 1;
      const Tick k = frame(t, r, c);
      if (i == 0) assert(event_is(k, "presence_started"));
      dwelled = event_is(k, "dwell_started");
      assert(t < 1000 + DWELL_START_MS + 100);
    }
    // gone from the frame after it: dwell_ended, presence_ended, then the report
    std::string events;
    Tick late{};
    for (const unsigned int stop = t + LOST_TIMEOUT_MS + INTERACTION_AFTER_LEAVE_WINDOW_MS + 1000; t < stop;
         t += 100) {
      const Tick k = frame(t);
      for (const char* name : {"dwell_ended", "presence_ended", "interaction_likely"}) {
        if (event_is(k, name)) {
          events += events.empty() ? "" : " ";
          events += name;
          if (std::strcmp(name, "interaction_likely") == 0) late = k;
        }
      }
    }
    std::printf("  last seen on the dwell_started frame (%s): %s, %s\n", moving ? "settled cell moving" : "settled",
                events.c_str(), late.json.empty() ? "no report" : late.json.c_str() + late.json.find("\"event\""));
    std::fflush(stdout);
    assert(events == "dwell_ended presence_ended interaction_likely");
    assert(has(late.json, "\"reason\":\"dwell_then_left\""));
  }
}

}  // namespace

int main() {
  // the core this suite links is the one the contract names
  assert(has(vision_emu_contract_json(), "\"schema\":\"securacv.canary-vision.core/v1\""));
  test_settled_cell_and_visit();
  test_each_visit_opens_on_its_own_cell();
  test_back_to_back_visit_through_the_abi();
  test_dwell_ended_through_the_abi();
  test_seen_after_dwell_ended_through_the_abi();
  test_seen_in_the_gap_then_elsewhere_through_the_abi();
  test_reset_after_dwell_ended_through_the_abi();
  test_last_seen_on_the_dwell_started_frame_through_the_abi();
  std::printf("ALL VISION CORE BINDING TESTS PASSED\n");
  return 0;
}
