/* Host tests for camera_gate_logic.h — the peek-surface gate (policy /
 * thermal / broken-camera precedence) and the idle-standby decision.
 * Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_camera_gate_logic.cpp \
 *       -o /tmp/test_camera_gate_logic && /tmp/test_camera_gate_logic
 */

#include <cstdio>
#include <cstring>

#include "camera_gate_logic.h"

using namespace camera_gate;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_peek_gate_precedence() {
  /* Args: (camera_usable, policy_allows, hot_critical). */

  /* Healthy, plugged in, cool: allow. */
  CHECK(peek_gate(true, true, false) == PeekGate::ALLOW);

  /* A broken camera reports as broken even when policy/thermal would
   * also deny — the user should see the real problem first. */
  CHECK(peek_gate(false, true, false) == PeekGate::DENY_NO_CAMERA);
  CHECK(peek_gate(false, false, true) == PeekGate::DENY_NO_CAMERA);

  /* Thermal outranks policy: it is the acute condition. */
  CHECK(peek_gate(true, false, true) == PeekGate::DENY_THERMAL);
  CHECK(peek_gate(true, true, true) == PeekGate::DENY_THERMAL);

  /* Battery policy denial when cool and healthy. */
  CHECK(peek_gate(true, false, false) == PeekGate::DENY_POLICY);

  /* Every deny reason has non-empty user-facing copy. */
  CHECK(strlen(peek_gate_reason(PeekGate::DENY_POLICY)) > 10);
  CHECK(strlen(peek_gate_reason(PeekGate::DENY_THERMAL)) > 10);
  CHECK(strlen(peek_gate_reason(PeekGate::DENY_NO_CAMERA)) > 10);
}

static void test_standby_due() {
  /* Args: (now, last_use, initialized, in_use, policy_allows[, timeout]). */

  /* Not initialized → nothing to park. */
  CHECK(!standby_due(1000000, 0, false, false, true));

  /* In use (stream / QR / seal) → never park, however stale last_use. */
  CHECK(!standby_due(1000000, 0, true, true, true));
  CHECK(!standby_due(1000000, 0, true, true, false));

  /* Policy denies the peek surface → park immediately, idle or not. */
  CHECK(standby_due(1000, 900, true, false, false));

  /* Idle timer: park only after the timeout elapses. */
  CHECK(!standby_due(1000, 900, true, false, true));
  CHECK(!standby_due(CAM_IDLE_TIMEOUT_MS - 1, 0, true, false, true));
  CHECK(standby_due(CAM_IDLE_TIMEOUT_MS, 0, true, false, true));

  /* Wrap safety: last_use just before the millis() wrap, now just after —
   * elapsed is small, so no standby. */
  CHECK(!standby_due(5000, 0xFFFFFF00u, true, false, true));
  /* And a genuinely stale last_use across the wrap still parks. */
  CHECK(standby_due(CAM_IDLE_TIMEOUT_MS + 5000, 0xFFFFFF00u, true, false,
                    true));

  /* Custom timeout parameter is honored. */
  CHECK(standby_due(600, 100, true, false, true, 500));
  CHECK(!standby_due(599, 100, true, false, true, 500));
}

int main() {
  test_peek_gate_precedence();
  test_standby_due();

  if (g_failures != 0) {
    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL camera_gate TESTS PASSED\n");
  return 0;
}
