/* Host tests for provisioning_logic.h — the pure decisions behind the
 * canary-wap first-run wizard. Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_provisioning_logic.cpp \
 *       -o /tmp/test_provisioning_logic && /tmp/test_provisioning_logic
 */

#include <cstdio>
#include <cstdlib>

#include "provisioning_logic.h"

using namespace provisioning_logic;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_setup_timeout() {
  const uint32_t WIN = 15u * 60u * 1000u;
  // Inactive wizard never times out.
  CHECK(!setup_timeout_due(false, WIN * 2, 0, WIN));
  // Fresh activity: not due.
  CHECK(!setup_timeout_due(true, 1000, 1000, WIN));
  CHECK(!setup_timeout_due(true, WIN - 1, 0, WIN));
  // Window elapsed: due.
  CHECK(setup_timeout_due(true, WIN, 0, WIN));
  // touch() semantics: activity late in the window restarts the countdown.
  uint32_t touched = WIN - 5000;
  CHECK(!setup_timeout_due(true, WIN + 1000, touched, WIN));
  CHECK(setup_timeout_due(true, touched + WIN, touched, WIN));
  // millis() wraparound: started just before wrap, now just after.
  uint32_t near_wrap = 0xFFFFF000u;
  CHECK(!setup_timeout_due(true, near_wrap + 60000u /* wraps */, near_wrap, WIN));
  CHECK(setup_timeout_due(true, near_wrap + WIN, near_wrap, WIN));
}

static void test_scan_cache() {
  const uint32_t TTL = 5u * 60u * 1000u;
  // Empty cache is never fresh, however recent.
  CHECK(!scan_cache_fresh(1000, 1000, false, TTL));
  // Recent + non-empty: fresh.
  CHECK(scan_cache_fresh(1000, 500, true, TTL));
  CHECK(scan_cache_fresh(TTL - 1, 0, true, TTL));
  // Aged out.
  CHECK(!scan_cache_fresh(TTL, 0, true, TTL));
  // Wrap-safe.
  uint32_t near_wrap = 0xFFFFFF00u;
  CHECK(scan_cache_fresh(near_wrap + 1000u /* wraps */, near_wrap, true, TTL));
}

static void test_sta_join() {
  // AP-only wins over everything: the user chose standalone.
  CHECK(!sta_join_allowed(true, true, true));
  // Normal configured+enabled device joins.
  CHECK(sta_join_allowed(false, true, true));
  // Unconfigured or disabled: no join.
  CHECK(!sta_join_allowed(false, false, true));
  CHECK(!sta_join_allowed(false, true, false));
}

static void test_ap_teardown() {
  const uint32_t GRACE = 120000u;
  // AP-only: never torn down, even long after a (stale) connect stamp.
  CHECK(!ap_teardown_due(true, true, GRACE * 10, 0, GRACE));
  // STA not connected: nothing to trade the AP for.
  CHECK(!ap_teardown_due(false, false, GRACE * 10, 0, GRACE));
  // Inside the grace window the AP must survive — this is the window in
  // which the provisioning phone re-associates and reads the success card.
  CHECK(!ap_teardown_due(false, true, GRACE, 0, GRACE));  // strict >
  CHECK(!ap_teardown_due(false, true, 8000, 0, GRACE));   // the old 8 s bug
  // Past the grace: teardown for radio stability.
  CHECK(ap_teardown_due(false, true, GRACE + 1, 0, GRACE));
  // Wrap-safe.
  uint32_t near_wrap = 0xFFFF0000u;
  CHECK(!ap_teardown_due(false, true, near_wrap + 60000u, near_wrap, GRACE));
  CHECK(ap_teardown_due(false, true, near_wrap + GRACE + 1, near_wrap, GRACE));
}

int main() {
  test_setup_timeout();
  test_scan_cache();
  test_sta_join();
  test_ap_teardown();
  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL provisioning_logic tests PASSED\n");
  return 0;
}
