// Host test for the demo storyline (include/canary/mode/demo_script.h).
//
// The heart of it is the DRIFT LOCK: every scripted beat's intended severity
// is run through the REAL classifier in src/fleet/fleet_model.cpp — the demo
// can never tell a story the product vocabulary doesn't. Builds standalone
// with g++ (links fleet_model.cpp, exactly like test_fleet_beacon_model).
// Run in CI by the "demo script host test" step in firmware.yml; prints
// "ALL DEMO SCRIPT TESTS PASSED" on success. Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//   firmware/projects/canary-display/tests_host/test_demo_script.cpp
//   firmware/projects/canary-display/src/fleet/fleet_model.cpp -o t && ./t

#include "canary/mode/demo_script.h"

#include <cstdio>
#include <cstring>

using canary::fleet::Sev;
using canary::fleet::classify_event;
using canary::fleet::sev_name;
using namespace canary::mode;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// ── The drift lock ──────────────────────────────────────────────────────────
static void test_intent_matches_real_classifier() {
  for (uint16_t i = 0; i < DEMO_BEAT_COUNT; i++) {
    const auto& b = DEMO_BEATS[i];
    const Sev real = classify_event(b.event);
    if (real != b.intent) {
      std::printf("  beat %u \"%s\": intent %s but classify_event says %s\n",
                  i, b.event, sev_name(b.intent), sev_name(real));
    }
    CHECK(real == b.intent, "storyline severity == real classifier");
  }
}

// ── Structural invariants ───────────────────────────────────────────────────
static void test_timeline_shape() {
  CHECK(DEMO_BEAT_COUNT >= 2, "a story has at least two beats");
  for (uint16_t i = 0; i < DEMO_BEAT_COUNT; i++) {
    const auto& b = DEMO_BEATS[i];
    CHECK(b.at_s > 0 && b.at_s < DEMO_LOOP_S, "beat inside the loop");
    CHECK(b.witness < DEMO_CAST_COUNT, "beat cast index valid");
    if (i > 0) CHECK(b.at_s > DEMO_BEATS[i - 1].at_s, "beats strictly ordered");
  }
}

static void test_cast_is_honest() {
  for (uint8_t i = 0; i < DEMO_CAST_COUNT; i++) {
    const auto& w = DEMO_CAST[i];
    CHECK(std::strncmp(w.id, "demo-", 5) == 0,
          "reserved demo- prefix: can never collide with a real device");
    CHECK(w.name && w.name[0] && w.room && w.room[0], "name + room present");
    CHECK(w.battery <= 100, "battery is a percentage");
  }
}

static void test_covers_every_severity_and_resolves() {
  bool seen[5] = {false, false, false, false, false};
  for (uint16_t i = 0; i < DEMO_BEAT_COUNT; i++) {
    seen[(uint8_t)DEMO_BEATS[i].intent] = true;
  }
  CHECK(seen[(uint8_t)Sev::Ok], "story shows Ok");
  CHECK(seen[(uint8_t)Sev::Notice], "story shows Notice");
  CHECK(seen[(uint8_t)Sev::Warn], "story shows Warn");
  CHECK(seen[(uint8_t)Sev::Alert], "story shows Alert");
  CHECK(seen[(uint8_t)Sev::Tamper], "story shows Tamper");
  CHECK(DEMO_BEATS[DEMO_BEAT_COUNT - 1].intent == Sev::Ok,
        "the loop resolves: no standing alarm across the wrap");
  // The alarm beats leave room to demonstrate hold-to-ack before resolve.
  bool alert_breathes = false;
  for (uint16_t i = 0; i + 1 < DEMO_BEAT_COUNT; i++) {
    if (DEMO_BEATS[i].intent >= Sev::Alert &&
        DEMO_BEATS[i + 1].at_s - DEMO_BEATS[i].at_s >= 15) {
      alert_breathes = true;
    }
  }
  CHECK(alert_breathes, "an alarm beat holds >= 15 s for the ack demo");
}

// ── Loop stepping ───────────────────────────────────────────────────────────
static void test_stepping_simple_window() {
  uint16_t out[DEMO_BEAT_COUNT];
  // (0, 5]: exactly the first beat (at_s == 5, window is half-open right-closed).
  uint8_t n = demo_beats_between(0, 5, out, DEMO_BEAT_COUNT);
  CHECK(n == 1 && out[0] == 0, "boundary beat fires at its second, once");
  // The very next tick must not re-fire it.
  n = demo_beats_between(5, 6, out, DEMO_BEAT_COUNT);
  CHECK(n == 0, "no double-fire on the next tick");
  // A coarse window catches everything inside it.
  n = demo_beats_between(40, 80, out, DEMO_BEAT_COUNT);
  CHECK(n == 3, "window (40,80] holds beats at 45/60/75");
  CHECK(out[0] == 3 && out[1] == 4 && out[2] == 5, "in timeline order");
}

static void test_stepping_no_time_no_beats() {
  uint16_t out[4];
  CHECK(demo_beats_between(45, 45, out, 4) == 0,
        "prev==now means no time passed, never a whole lap");
}

static void test_stepping_wrap() {
  uint16_t out[DEMO_BEAT_COUNT];
  // Wrap from late in the loop to just past the first beat: the tail beat
  // (135) is behind us, so only 5 fires.
  uint8_t n = demo_beats_between(140, 5, out, DEMO_BEAT_COUNT);
  CHECK(n == 1 && out[0] == 0, "wrap window fires the first beat only");
  // Wrap spanning the tail: 135 then 5.
  n = demo_beats_between(120, 10, out, DEMO_BEAT_COUNT);
  CHECK(n == 2 && out[0] == 0 && out[1] == 8, "wrap catches tail + head");
}

static void test_stepping_one_lap_fires_each_beat_once() {
  // Walk a full lap in odd-sized steps; every beat fires exactly once.
  uint8_t fired[DEMO_BEAT_COUNT] = {0};
  uint16_t prev = 0;
  uint16_t clock = 0;
  while (clock < DEMO_LOOP_S) {
    uint16_t next = (uint16_t)(clock + 7);
    if (next >= DEMO_LOOP_S) next = 0;  // the runtime modulo
    uint16_t out[DEMO_BEAT_COUNT];
    const uint8_t n = demo_beats_between(prev, next ? next : 0,
                                         out, DEMO_BEAT_COUNT);
    for (uint8_t i = 0; i < n; i++) fired[out[i]]++;
    prev = next;
    clock += 7;
    if (next == 0) break;  // completed the lap through the wrap
  }
  for (uint16_t i = 0; i < DEMO_BEAT_COUNT; i++) {
    CHECK(fired[i] == 1, "each beat fires exactly once per lap");
  }
}

static void test_stepping_respects_cap() {
  uint16_t out[2];
  const uint8_t n = demo_beats_between(0, DEMO_LOOP_S - 1, out, 2);
  CHECK(n == 2, "cap bounds the write");
}

int main() {
  test_intent_matches_real_classifier();
  test_timeline_shape();
  test_cast_is_honest();
  test_covers_every_severity_and_resolves();
  test_stepping_simple_window();
  test_stepping_no_time_no_beats();
  test_stepping_wrap();
  test_stepping_one_lap_fires_each_beat_once();
  test_stepping_respects_cap();

  if (g_fail == 0) {
    std::printf("ALL DEMO SCRIPT TESTS PASSED\n");
    return 0;
  }
  std::printf("%d DEMO SCRIPT TEST(S) FAILED\n", g_fail);
  return 1;
}
