/* Host tests for selftest_logic.h — the pure verdicts behind the pre-flight
 * health check. Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_selftest_logic.cpp \
 *       -o /tmp/test_selftest_logic && /tmp/test_selftest_logic
 */

#include <cstdio>

#include "selftest_logic.h"

using selftest_logic::Status;
using selftest_logic::WifiKind;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_bluetooth() {
  using selftest_logic::bluetooth_status;
  // Not built in → ABSENT, never FAIL.
  CHECK(bluetooth_status(false, false, false, false, false) == Status::ABSENT);

  // The two historical false-FAILs are gone:
  //  (a) boot window before init ran → SKIP, not FAIL.
  CHECK(bluetooth_status(true, /*init_attempted=*/false, false, false, false)
        == Status::SKIP);
  //  (b) safe mode (radios intentionally skipped) → SKIP, not FAIL,
  //      even though init never ran and the stack isn't up.
  CHECK(bluetooth_status(true, false, false, /*safe_mode=*/true, false)
        == Status::SKIP);

  // Genuine fault: init ran, stack did not come up → FAIL (the one real case).
  CHECK(bluetooth_status(true, /*init_attempted=*/true, /*available=*/false,
                         false, false) == Status::FAIL);

  // Up with a live feature → PASS.
  CHECK(bluetooth_status(true, true, true, false, /*any_active=*/true)
        == Status::PASS);
  // Up but every feature gated off → honest SKIP, not FAIL.
  CHECK(bluetooth_status(true, true, true, false, /*any_active=*/false)
        == Status::SKIP);

  // A pairing-channel-only build (compiled_in true via FEATURE_BLUETOOTH
  // even when discovery is off) is treated the same — it is NOT ABSENT.
  CHECK(bluetooth_status(/*compiled_in=*/true, true, true, false, true)
        == Status::PASS);
}

static void test_wifi() {
  using selftest_logic::wifi_kind;
  using selftest_logic::wifi_status;
  // Joined home Wi-Fi.
  CHECK(wifi_kind(true, false, false) == WifiKind::JOINED);
  CHECK(wifi_status(WifiKind::JOINED) == Status::PASS);
  // Hotspot only (wizard steps 1-3) → SKIP, not FAIL.
  CHECK(wifi_kind(false, true, false) == WifiKind::HOTSPOT);
  CHECK(wifi_status(WifiKind::HOTSPOT) == Status::SKIP);
  // Radio on, link dropped → LINK_DOWN (distinct detail from radio-off).
  CHECK(wifi_kind(false, false, false) == WifiKind::LINK_DOWN);
  CHECK(wifi_status(WifiKind::LINK_DOWN) == Status::FAIL);
  // Radio genuinely off.
  CHECK(wifi_kind(false, false, true) == WifiKind::RADIO_OFF);
  CHECK(wifi_status(WifiKind::RADIO_OFF) == Status::FAIL);
}

static void test_all_passed() {
  using selftest_logic::all_passed;
  CHECK(all_passed(0));
  CHECK(!all_passed(1));
  CHECK(!all_passed(3));
}

int main() {
  test_bluetooth();
  test_wifi();
  test_all_passed();
  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL selftest_logic tests PASSED\n");
  return 0;
}
