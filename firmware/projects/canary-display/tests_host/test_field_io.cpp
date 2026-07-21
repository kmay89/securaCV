// Host test for the pure field-I/O logic (include/canary/io/field_io_logic.h).
//
// Builds standalone with g++ — no Arduino, no board. Run in CI by the
// "field-I/O logic host test" step in .github/workflows/firmware.yml. Prints
// "ALL FIELD IO TESTS PASSED" on success (the CI grep makes a silent pass
// impossible to fake). Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//   firmware/projects/canary-display/tests_host/test_field_io.cpp -o t && ./t

#include "canary/io/field_io_logic.h"

#include <cstdio>

namespace fio = canary::io::field;
using Edge = fio::Edge;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                     \
    }                                                              \
  } while (0)

// ── Debounce ────────────────────────────────────────────────────────────────
static void test_debounce_needs_persistence() {
  fio::Debounce d;
  // One sample of the new level is not enough (need=2).
  CHECK(d.update(true, 2) == Edge::None, "1 sample: no edge yet");
  CHECK(d.update(true, 2) == Edge::Rising, "2 samples: rising edge");
  // Holding stays stable, no repeat edges.
  CHECK(d.update(true, 2) == Edge::None, "held active: no repeat rising");
  CHECK(d.update(false, 2) == Edge::None, "1 release sample: no edge yet");
  CHECK(d.update(false, 2) == Edge::Falling, "2 release samples: falling edge");
  CHECK(d.update(false, 2) == Edge::None, "held clear: no repeat falling");
}

static void test_debounce_rejects_glitch() {
  fio::Debounce d;
  d.update(true, 3);
  d.update(true, 3);
  CHECK(d.update(true, 3) == Edge::Rising, "settles active after 3");
  // A single-sample glitch to the other level must not flip the stable state.
  CHECK(d.update(false, 3) == Edge::None, "1-sample glitch ignored");
  CHECK(d.update(true, 3) == Edge::None, "back to active, still stable, no edge");
  CHECK(d.update(true, 3) == Edge::None, "no spurious edge from the glitch");
}

static void test_debounce_need_zero_treated_as_one() {
  fio::Debounce d;
  CHECK(d.update(true, 0) == Edge::Rising, "need=0 behaves as need=1");
}

// ── Siren controller ────────────────────────────────────────────────────────
static void test_siren_drives_only_on_unacked_alert() {
  fio::SirenController s;
  const uint32_t MAX = 1000;
  CHECK(s.update(0, /*alerting=*/false, /*acked=*/false, MAX) == false,
        "quiet: no drive");
  CHECK(s.update(10, true, false, MAX) == true, "unacked alert: drive");
  CHECK(s.update(20, true, true, MAX) == false, "acked: release");
  // Re-arms after ack: a fresh (still-alerting, unacked) episode drives again.
  CHECK(s.update(30, true, false, MAX) == true, "fresh unacked alert: drive again");
  CHECK(s.update(40, false, false, MAX) == false, "cleared: release");
}

static void test_siren_bounded_max_on() {
  fio::SirenController s;
  const uint32_t MAX = 100;
  CHECK(s.update(1000, true, false, MAX) == true, "starts driving");
  CHECK(s.update(1050, true, false, MAX) == true, "still within cap");
  CHECK(s.update(1100, true, false, MAX) == false, "hits cap -> off");
  CHECK(s.update(1200, true, false, MAX) == false,
        "stays capped while the same alert stands");
  // Only clearing (or an ack) re-arms it.
  CHECK(s.update(1300, false, false, MAX) == false, "clear releases");
  CHECK(s.update(1400, true, false, MAX) == true, "new alert after clear re-arms");
}

int main() {
  test_debounce_needs_persistence();
  test_debounce_rejects_glitch();
  test_debounce_need_zero_treated_as_one();
  test_siren_drives_only_on_unacked_alert();
  test_siren_bounded_max_on();

  if (g_fail == 0) {
    std::printf("ALL FIELD IO TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FIELD IO TEST(S) FAILED\n", g_fail);
  return 1;
}
