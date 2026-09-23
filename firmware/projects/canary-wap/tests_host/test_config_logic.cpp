/* Host tests for config_logic.h — the runtime device-config clamps behind
 * the Device tab's "Save Configuration". Build & run (CI: firmware.yml):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_config_logic.cpp \
 *       -o /tmp/test_config_logic && /tmp/test_config_logic
 */

#include <cstdio>

#include "config_logic.h"

using namespace config_logic;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_time_bucket() {
  // The floor the sketch ships (canary_wap.ino's TIME_BUCKET_MS is defined
  // from this constant). Pinned here so a revert to a finer floor fails the
  // test instead of passing against a local literal.
  const uint32_t FLOOR = kTimeBucketFloorMs;
  CHECK(FLOOR == 600000u);  // the ten-minute grid (Invariant III)
  // The privacy invariant: never finer than the floor. A request below it is
  // raised to the floor, so coarsening can only increase.
  CHECK(clamp_time_bucket_ms(1000, FLOOR) == FLOOR);
  CHECK(clamp_time_bucket_ms(0, FLOOR) == FLOOR);
  CHECK(clamp_time_bucket_ms(FLOOR, FLOOR) == FLOOR);
  // A value persisted under the old 5 s floor is raised when it is loaded at
  // boot (widen-only; config_load_runtime runs this clamp on every boot).
  CHECK(clamp_time_bucket_ms(5000, FLOOR) == FLOOR);
  CHECK(clamp_time_bucket_ms(30000, FLOOR) == FLOOR);
  // At or above the floor, the request is honored (coarser is allowed).
  CHECK(clamp_time_bucket_ms(600001, FLOOR) == 600001);
  CHECK(clamp_time_bucket_ms(3600000, FLOOR) == 3600000);
  // The result is NEVER below the floor for any input.
  for (uint32_t v = 0; v < 700000; v += 137) {
    CHECK(clamp_time_bucket_ms(v, FLOOR) >= FLOOR);
  }
}

static void test_record_interval() {
  const uint32_t MIN = 250, MAX = 60000;
  CHECK(clamp_record_interval_ms(0, MIN, MAX) == MIN);       // no busy-loop
  CHECK(clamp_record_interval_ms(100, MIN, MAX) == MIN);
  CHECK(clamp_record_interval_ms(1000, MIN, MAX) == 1000);
  CHECK(clamp_record_interval_ms(999999, MIN, MAX) == MAX);
  CHECK(clamp_record_interval_ms(MIN, MIN, MAX) == MIN);
  CHECK(clamp_record_interval_ms(MAX, MIN, MAX) == MAX);
}

static void test_log_level() {
  const uint8_t MAXL = 3;  // WARNING — keeps ERROR(4)/CRITICAL(5) always stored
  CHECK(clamp_log_level(0, MAXL) == 0);
  CHECK(clamp_log_level(1, MAXL) == 1);
  CHECK(clamp_log_level(3, MAXL) == 3);
  // A level past WARNING can't be selected — it would silence real faults.
  CHECK(clamp_log_level(4, MAXL) == MAXL);
  CHECK(clamp_log_level(99, MAXL) == MAXL);
}

int main() {
  test_time_bucket();
  test_record_interval();
  test_log_level();
  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL config_logic tests PASSED\n");
  return 0;
}
