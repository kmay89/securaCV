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
// latch of its own. The settled cell differs from the frame's: it moves on
// the third consecutive frame away from it (voxel_tracker.cpp's
// VOXEL_STABLE_N), and stays on the last cell once the frame is empty. And (sweep F130) dwell_ended's dwell_ms is the
// dwell it closed, through the same ABI.
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
  for (;; t += 100) {
    k = frame(t);
    if (event_is(k, "dwell_ended")) break;
    assert(t < 4000);
  }
  assert(has(k.fsm, "\"dwelling\":false"));
  assert(int_at(k.fsm, "dwell_ms") == (long)(t - dwell_at));
  k = frame(t += 100);
  assert(event_is(k, "presence_ended"));
  assert(int_at(k.fsm, "dwell_ms") == 0);
}

}  // namespace

int main() {
  // the core this suite links is the one the contract names
  assert(has(vision_emu_contract_json(), "\"schema\":\"securacv.canary-vision.core/v1\""));
  test_settled_cell_and_visit();
  test_dwell_ended_through_the_abi();
  std::printf("ALL VISION CORE BINDING TESTS PASSED\n");
  return 0;
}
