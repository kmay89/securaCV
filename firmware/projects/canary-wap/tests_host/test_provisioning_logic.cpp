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

static void test_deferred_reboot() {
  const uint32_t GRACE = 120000u;
  // Unarmed (deadline 0): never due, no matter the clock.
  CHECK(!deferred_reboot_due(0, 0));
  CHECK(!deferred_reboot_due(0xFFFFFFFFu, 0));
  // Armed: not due inside the grace — this is the window in which the
  // provisioning phone re-associates and reads the success card (rebooting
  // at WL_CONNECTED, the old behavior, made the grace pointless).
  uint32_t deadline = 1000 + GRACE;
  CHECK(!deferred_reboot_due(1000, deadline));
  CHECK(!deferred_reboot_due(deadline - 1, deadline));
  // Due at/after the deadline.
  CHECK(deferred_reboot_due(deadline, deadline));
  CHECK(deferred_reboot_due(deadline + 5000, deadline));
  // Wrap-safe: deadline just past the wrap, now just before it.
  uint32_t near_wrap = 0xFFFFF000u;
  uint32_t wrapped_deadline = near_wrap + GRACE;  // wraps
  CHECK(!deferred_reboot_due(near_wrap, wrapped_deadline));
  CHECK(deferred_reboot_due(wrapped_deadline, wrapped_deadline));
}

static void test_reboot_deadline_extend() {
  const uint32_t MIN = 90000u;  // keep >= 90 s for the user to finish step 5
  // Disarmed stays disarmed.
  CHECK(reboot_deadline_extend(0, 1000, MIN) == 0);
  CHECK(reboot_deadline_extend(0, 0xFFFFFFFFu, MIN) == 0);
  // Deadline sooner than now+MIN gets pushed out to exactly now+MIN.
  CHECK(reboot_deadline_extend(1000 + 5000, 1000, MIN) == 1000 + MIN);
  // A deadline already further out than now+MIN is left alone (never pulled in).
  CHECK(reboot_deadline_extend(1000 + MIN + 10000, 1000, MIN) == 1000 + MIN + 10000);
  // Exactly at the floor: unchanged.
  CHECK(reboot_deadline_extend(1000 + MIN, 1000, MIN) == 1000 + MIN);
  // Wrap-safe: now near the wrap, floor wraps past 0.
  uint32_t near_wrap = 0xFFFFF000u;
  uint32_t soon = near_wrap + 1000u;              // deadline sooner than floor
  CHECK(reboot_deadline_extend(soon, near_wrap, MIN) ==
        (uint32_t)(near_wrap + MIN));
}

static void test_ble_discovery_start() {
  const uint32_t SETTLE = 45000u;
  // STA joined → always due (steady state, AP about to drop → stable STA+BLE),
  // regardless of ap_only or how early in the boot it is.
  CHECK(ble_discovery_start_due(false, true, 0, 0, SETTLE));
  CHECK(ble_discovery_start_due(true, true, 0, 0, SETTLE));
  // Normal (non-AP-only) device still provisioning: STA not up → NOT due, so
  // the active scan never fights the phone's SoftAP handshake. This is the bug
  // being fixed — before, discovery scanned from boot during the join window.
  CHECK(!ble_discovery_start_due(false, false, 1000, 0, SETTLE));
  CHECK(!ble_discovery_start_due(false, false, SETTLE * 10, 0, SETTLE));  // never, without STA
  // AP-only standalone: no STA to wait on. Held during the settle window so the
  // operator's first association lands cleanly, then due.
  CHECK(!ble_discovery_start_due(true, false, SETTLE - 1, 0, SETTLE));
  CHECK(ble_discovery_start_due(true, false, SETTLE, 0, SETTLE));       // boundary: >=
  CHECK(ble_discovery_start_due(true, false, SETTLE + 5000, 0, SETTLE));
  // Settle measured from the boot reference, not absolute time.
  CHECK(!ble_discovery_start_due(true, false, 100000 + SETTLE - 1, 100000, SETTLE));
  CHECK(ble_discovery_start_due(true, false, 100000 + SETTLE, 100000, SETTLE));
  // Wrap-safe: boot reference just before the millis() wrap, now just after.
  uint32_t near_wrap = 0xFFFFF000u;
  CHECK(!ble_discovery_start_due(true, false, near_wrap + 1000u /* wraps */, near_wrap, SETTLE));
  CHECK(ble_discovery_start_due(true, false, near_wrap + SETTLE, near_wrap, SETTLE));
}

int main() {
  test_setup_timeout();
  test_scan_cache();
  test_sta_join();
  test_ap_teardown();
  test_deferred_reboot();
  test_reboot_deadline_extend();
  test_ble_discovery_start();
  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL provisioning_logic tests PASSED\n");
  return 0;
}
