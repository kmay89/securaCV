/* Host tests for power_gate_logic.h — the round-two power-policy gate
 * decisions: advisory-feature run/skip and routine MQTT heartbeat cadence
 * stretch. Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_power_gate_logic.cpp \
 *       -o /tmp/test_power_gate_logic && /tmp/test_power_gate_logic
 */

#include <cstdio>
#include <cstdint>

#include "power_gate_logic.h"

using namespace power_gate;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_feature_runs() {
  /* No policy engine compiled in → the feature always runs (compile-time
   * flags remain the only gate). */
  CHECK(feature_runs(false, false));
  CHECK(feature_runs(false, true));
  /* Policy present → the runtime bit decides. */
  CHECK(feature_runs(true, true));
  CHECK(!feature_runs(true, false));
}

static void test_stretch_factor_by_mode() {
  CHECK(routine_stretch_factor(MODE_PLUGGED_IN) == 1);
  CHECK(routine_stretch_factor(MODE_BATTERY_NORMAL) == 1);
  CHECK(routine_stretch_factor(MODE_USB_ONLY) == 1);
  CHECK(routine_stretch_factor(MODE_BATTERY_SAVER) == 4);
  CHECK(routine_stretch_factor(MODE_LOW_POWER) == 8);
  CHECK(routine_stretch_factor(MODE_SHUTDOWN) == 8);
  /* Unknown mode → fail-responsive (1x, never faster). */
  CHECK(routine_stretch_factor(200) == 1);
}

static void test_routine_interval() {
  /* Mains/normal: unchanged. */
  CHECK(routine_interval_ms(30000, MODE_PLUGGED_IN) == 30000);
  CHECK(routine_interval_ms(60000, MODE_BATTERY_NORMAL) == 60000);
  /* Saver: 4x; low/shutdown: 8x. */
  CHECK(routine_interval_ms(30000, MODE_BATTERY_SAVER) == 120000);
  CHECK(routine_interval_ms(60000, MODE_LOW_POWER) == 480000);
  CHECK(routine_interval_ms(30000, MODE_SHUTDOWN) == 240000);
  /* Zero base stays zero. */
  CHECK(routine_interval_ms(0, MODE_LOW_POWER) == 0);
  /* Saturation rather than wrap on an absurd base. */
  CHECK(routine_interval_ms(0xFFFFFFFFu, MODE_LOW_POWER) == 0xFFFFFFFFu);
  CHECK(routine_interval_ms(0x30000000u, MODE_LOW_POWER) == 0xFFFFFFFFu);
  /* A base right at the edge for a given factor. */
  CHECK(routine_interval_ms(0x3FFFFFFFu, MODE_BATTERY_SAVER) == 0xFFFFFFFCu);
}

int main() {
  test_feature_runs();
  test_stretch_factor_by_mode();
  test_routine_interval();

  if (g_failures != 0) {
    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL power_gate_logic TESTS PASSED\n");
  return 0;
}
