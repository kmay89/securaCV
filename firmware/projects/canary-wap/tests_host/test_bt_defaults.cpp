/* Host tests for bt_defaults.h — the Bluetooth channel's factory defaults.
 * Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_bt_defaults.cpp \
 *       -o /tmp/test_bt_defaults && /tmp/test_bt_defaults
 */

#include <cstdio>

#include "bt_defaults.h"

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

int main() {
  // The radio is ON out of the box — this is the user-requested change
  // ("set the flag by default") and the fix for the dead Bluetooth panel.
  CHECK(bt_defaults::ENABLED == true);
  CHECK(bt_defaults::AUTO_ADVERTISE == true);

  // On is NOT open: a PIN confirmation is still required, so an
  // advertised-but-unpaired device exposes nothing without a human step.
  // If a future edit turns the radio on but also drops REQUIRE_PIN, this
  // fails — enabling by default must never silently open pairing.
  CHECK(bt_defaults::REQUIRE_PIN == true);
  CHECK(!(bt_defaults::ENABLED && !bt_defaults::REQUIRE_PIN));

  // Long-range stays opt-in (power/regulatory).
  CHECK(bt_defaults::LONG_RANGE == false);

  // BLE init heap guard: below the minimum contiguous internal block, the
  // stack must NOT be brought up — a no-PSRAM build otherwise OOM-panics the
  // controller into a boot loop that even safe mode can't escape. At/above, ok.
  CHECK(!bt_defaults::init_has_headroom(0));
  CHECK(!bt_defaults::init_has_headroom(bt_defaults::MIN_INIT_FREE_BLOCK - 1));
  CHECK(bt_defaults::init_has_headroom(bt_defaults::MIN_INIT_FREE_BLOCK));
  CHECK(bt_defaults::init_has_headroom(bt_defaults::MIN_INIT_FREE_BLOCK + 200000UL));
  // The threshold must clear the controller's ~30 KB largest allocation with
  // margin, so passing the guard is a real guarantee rather than a coin-flip.
  CHECK(bt_defaults::MIN_INIT_FREE_BLOCK >= 30UL * 1024UL);

  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL bt_defaults tests PASSED\n");
  return 0;
}
