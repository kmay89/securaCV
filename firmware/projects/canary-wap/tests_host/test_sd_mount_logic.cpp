/* Host tests for sd_mount_logic.h — the pure decisions around the SD mount
 * worker. Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_sd_mount_logic.cpp \
 *       -o /tmp/test_sd_mount_logic && /tmp/test_sd_mount_logic
 */

#include <cstdio>

#include "sd_mount_logic.h"

using namespace sd_mount_logic;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_periodic_action() {
  const uint32_t IV = 30000u;  // SD_RECHECK_INTERVAL_MS
  // Args: (safe_mode, mounted, in_flight, now, last_check, interval)

  // Interval not elapsed: nothing, regardless of state.
  CHECK(periodic_action(false, false, false, 1000, 0, IV) == PeriodicAction::NONE);
  CHECK(periodic_action(false, true,  false, IV - 1, 0, IV) == PeriodicAction::NONE);

  // Normal mode, interval elapsed: absent card → REMOUNT, mounted → VERIFY.
  CHECK(periodic_action(false, false, false, IV, 0, IV) == PeriodicAction::REMOUNT);
  CHECK(periodic_action(false, true,  false, IV, 0, IV) == PeriodicAction::VERIFY);

  // SAFE MODE: never touch the SD from the loop. This is the crash-loop fix —
  // boot skipped SD init, but the periodic remount used to re-run the blocking
  // mount from loop() and blow the task watchdog, so safe mode itself
  // crash-looped (observed: "consecutive crash count 7/3" climbing forever).
  CHECK(periodic_action(true, false, false, IV, 0, IV) == PeriodicAction::NONE);
  CHECK(periodic_action(true, false, false, IV * 100, 0, IV) == PeriodicAction::NONE);
  CHECK(periodic_action(true, true,  false, IV, 0, IV) == PeriodicAction::NONE);

  // A previous mount attempt still in flight (worker possibly stuck inside a
  // blocking SD.begin): never pile on another request.
  CHECK(periodic_action(false, false, true, IV, 0, IV) == PeriodicAction::NONE);
  CHECK(periodic_action(false, true,  true, IV, 0, IV) == PeriodicAction::NONE);

  // Wrap-safe interval math: last check just before millis() wrap.
  uint32_t near_wrap = 0xFFFFF000u;
  CHECK(periodic_action(false, false, false, near_wrap + 1000u /* wraps */,
                        near_wrap, IV) == PeriodicAction::NONE);
  CHECK(periodic_action(false, false, false, near_wrap + IV,
                        near_wrap, IV) == PeriodicAction::REMOUNT);
}

static void test_mount_wait() {
  const uint32_t BUDGET = 4000u;
  // Inside the budget: keep waiting (and feeding the watchdog).
  CHECK(!mount_wait_expired(0, 0, BUDGET));
  CHECK(!mount_wait_expired(BUDGET - 1, 0, BUDGET));
  // At/after the budget: give up, report absent, leave the worker detached.
  CHECK(mount_wait_expired(BUDGET, 0, BUDGET));
  CHECK(mount_wait_expired(BUDGET + 5000, 0, BUDGET));
  // Wrap-safe.
  uint32_t near_wrap = 0xFFFFFF00u;
  CHECK(!mount_wait_expired(near_wrap + 100u /* wraps */, near_wrap, BUDGET));
  CHECK(mount_wait_expired(near_wrap + BUDGET, near_wrap, BUDGET));
}

int main() {
  test_periodic_action();
  test_mount_wait();
  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL sd_mount_logic tests PASSED\n");
  return 0;
}
