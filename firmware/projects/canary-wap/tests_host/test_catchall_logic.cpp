/* Host tests for catchall_logic.h — who owns the shared canary.local
 * catch-all when multiple Canaries share a LAN. Build & run (CI:
 * firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_catchall_logic.cpp \
 *       -o /tmp/test_catchall_logic && /tmp/test_catchall_logic
 */

#include <cstdio>

#include "catchall_logic.h"

using namespace catchall_logic;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_stagger() {
  // Bounds: [500, 3499] ms for every possible fingerprint.
  CHECK(claim_stagger_ms(0x00, 0x00) == 500);
  CHECK(claim_stagger_ms(0xFF, 0xFF) >= 500 && claim_stagger_ms(0xFF, 0xFF) < 3500);
  for (int hi = 0; hi < 256; hi += 17) {
    for (int lo = 0; lo < 256; lo += 13) {
      uint32_t s = claim_stagger_ms((uint8_t)hi, (uint8_t)lo);
      CHECK(s >= 500 && s < 3500);
    }
  }
  // Deterministic: the same device always picks the same slot.
  CHECK(claim_stagger_ms(0xAB, 0xCD) == claim_stagger_ms(0xAB, 0xCD));
  // Different fingerprints usually pick different slots (spot check the two
  // real field devices' pattern: different first fingerprint bytes).
  CHECK(claim_stagger_ms(0x12, 0x34) != claim_stagger_ms(0x56, 0x78));
}

static void test_tie_break() {
  const uint32_t A = 0xC0A8050A;  // 192.168.5.10 (numeric compare only)
  const uint32_t B = 0xC0A8059B;  // 192.168.5.155

  // THE INVARIANT: both devices evaluate the same pair from opposite sides
  // and exactly ONE keeps the claim — otherwise either both keep (the
  // session-killing double-claim persists) or both withdraw (canary.local
  // goes dark until the next claim cycle).
  CHECK(keep_claim_on_conflict(A, B) != keep_claim_on_conflict(B, A));
  CHECK(keep_claim_on_conflict(A, B));   // lower IP wins
  CHECK(!keep_claim_on_conflict(B, A));
}

static void test_probe_conflict() {
  const uint32_t AP  = 0x0104A8C0;
  const uint32_t STA = 0x9B05A8C0;
  const uint32_t PEER = 0x6405A8C0;

  // Silence is not a conflict.
  CHECK(!probe_is_conflict(0, AP, STA));
  // Our own delegated record echoed back is not a conflict.
  CHECK(!probe_is_conflict(AP, AP, STA));
  CHECK(!probe_is_conflict(STA, AP, STA));
  // A different responder is.
  CHECK(probe_is_conflict(PEER, AP, STA));
  // Interfaces that are down (0) never match a real answer — a peer whose
  // address happens to equal our zeroed interface is still a conflict.
  CHECK(probe_is_conflict(PEER, 0, STA));
  CHECK(probe_is_conflict(STA, 0, 0));
}

static void test_due() {
  CHECK(due(1000, 1000));
  CHECK(due(1001, 1000));
  CHECK(!due(999, 1000));
  // millis() wrap: scheduled just before wrap, now just after.
  uint32_t near_wrap = 0xFFFFF000u;
  CHECK(due(near_wrap + 5000u /* wraps */, near_wrap));
  CHECK(!due(near_wrap, near_wrap + 5000u /* wraps */));
}

int main() {
  test_stagger();
  test_tie_break();
  test_probe_conflict();
  test_due();

  if (g_failures != 0) {
    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL catchall_logic TESTS PASSED\n");
  return 0;
}
