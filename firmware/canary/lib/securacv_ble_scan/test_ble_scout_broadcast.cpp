/**
 * @file test_ble_scout_broadcast.cpp
 * @brief Host-build test for the BLE Scout mesh-broadcast hook (PR 5c).
 *
 * Verifies:
 *   1. With no callback registered (default), ble_scout never broadcasts.
 *   2. With a callback installed, emit_arrived fires the callback with
 *      (arrived=true, label="kitchen") when the presence state machine
 *      transitions AWAY → PRESENT.
 *   3. The same callback fires with (arrived=false, label="kitchen")
 *      on the PRESENT → AWAY (LOST_MS) timeout transition.
 *   4. Unhooking with nullptr silences subsequent transitions.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_ble_scan/test_ble_scout_broadcast.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scan.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scout.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scout_state.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scout_key.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       firmware/common/csi/src/csi_event.cpp \
 *       firmware/common/csi/src/csi_module.cpp \
 *       firmware/common/csi/src/csi_bundler.cpp \
 *       firmware/common/csi/src/csi_witness_payload.cpp \
 *       -I firmware/canary/lib/securacv_ble_scan/src \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -I firmware/common/csi/src \
 *       -o /tmp/test_scout_bcast && /tmp/test_scout_bcast
 */

#include "ble_scout.h"
#include "ble_scan.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_ble_scout_broadcast_run() { return 0; }
#else

namespace {

/* Captured invocation state. The test callback appends each call so
 * tests can assert label-specific behaviour without being thrown off
 * by callbacks for other paired beacons that happen to be timing out
 * in the same tick. There is no full Scout reset between tests; the
 * registry, tracker, and key persist across the run. */
struct CallRecord {
  bool arrived;
  char label[32];
};
constexpr size_t MAX_CALLS = 8;
CallRecord g_calls[MAX_CALLS] = {};
size_t     g_call_count = 0;

void reset_captured() {
  g_call_count = 0;
  std::memset(g_calls, 0, sizeof(g_calls));
}

void test_callback(bool arrived, const char* label) {
  if (g_call_count >= MAX_CALLS) return;
  CallRecord* r = &g_calls[g_call_count++];
  r->arrived = arrived;
  std::strncpy(r->label, label ? label : "", sizeof(r->label) - 1);
  r->label[sizeof(r->label) - 1] = '\0';
}

/* True iff any captured call matches (arrived, label). */
bool saw_call(bool arrived, const char* label) {
  for (size_t i = 0; i < g_call_count; ++i) {
    if (g_calls[i].arrived == arrived &&
        std::strcmp(g_calls[i].label, label) == 0) {
      return true;
    }
  }
  return false;
}

/* Helper: feed strong adverts at 100ms cadence until the presence
 * state machine commits to PRESENT (DWELL_MS+ from start). */
void drive_to_arrived(const uint8_t mac[ble_scan::MAC_LEN]) {
  for (int i = 0; i < 80; ++i) {
    ble_scout::ble_scout_on_advert(mac, /*rssi_dbm=*/-55,
                                   /*now_ms=*/(uint32_t)(1000 + i * 100));
  }
}

/* ────────────────────────────────────────────────────────────────────────
 * Tests
 * ──────────────────────────────────────────────────────────────────────── */

void test_no_callback_is_silent() {
  /* Default state: no callback installed. Drive an arrived transition
   * and verify nothing was called. */
  ble_scout::set_broadcast_callback(nullptr);
  ble_scout::ble_scout_init();
  reset_captured();

  uint8_t mac[ble_scan::MAC_LEN] = {0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03};
  assert(ble_scout::ble_scout_pair(mac, "kitchen"));

  drive_to_arrived(mac);

  assert(g_call_count == 0);
  std::printf("PASS test_no_callback_is_silent\n");
}

void test_arrived_fires_callback() {
  ble_scout::set_broadcast_callback(test_callback);
  ble_scout::ble_scout_init();
  reset_captured();

  uint8_t mac[ble_scan::MAC_LEN] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  assert(ble_scout::ble_scout_pair(mac, "office"));

  drive_to_arrived(mac);

  /* Exactly one ARRIVED transition for the office beacon. (The kitchen
   * beacon from the prior test is still PROVISIONAL / PRESENT but is
   * not being driven here, so its state is unchanged.) */
  assert(saw_call(/*arrived=*/true, "office"));
  std::printf("PASS test_arrived_fires_callback  (calls=%zu)\n", g_call_count);
}

void test_departed_fires_callback() {
  /* Both kitchen and office are now PRESENT (from prior tests). Bump
   * time well past LOST_MS (30s) since each beacon's last advert and
   * call tick() — both should fire DEPARTED, and saw_call() lets us
   * assert specifically that "office" was reported without caring
   * about other beacons being timed out in the same tick. */
  reset_captured();
  ble_scout::ble_scout_tick(50000);

  assert(saw_call(/*arrived=*/false, "office"));
  /* Sanity: the kitchen beacon should also have fired since it too
   * was PRESENT and silent past LOST_MS. */
  assert(saw_call(/*arrived=*/false, "kitchen"));
  std::printf("PASS test_departed_fires_callback  (calls=%zu)\n", g_call_count);
}

void test_unhook_silences_callback() {
  /* Pair a fresh beacon, unhook the callback, drive to arrived. */
  ble_scout::set_broadcast_callback(test_callback);
  ble_scout::ble_scout_init();

  uint8_t mac[ble_scan::MAC_LEN] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x99};
  assert(ble_scout::ble_scout_pair(mac, "garage"));

  /* Unhook BEFORE driving the transition. */
  ble_scout::set_broadcast_callback(nullptr);
  reset_captured();

  /* Use timestamps past the prior tests' LOST window so we don't
   * accidentally interleave with their departed transitions (those
   * already fired in the previous test and won't refire now). */
  for (int i = 0; i < 80; ++i) {
    ble_scout::ble_scout_on_advert(mac, /*rssi_dbm=*/-55,
                                   /*now_ms=*/(uint32_t)(100000 + i * 100));
  }

  assert(g_call_count == 0);
  std::printf("PASS test_unhook_silences_callback\n");
}

}  /* namespace */

int main() {
  test_no_callback_is_silent();
  test_arrived_fires_callback();
  test_departed_fires_callback();
  test_unhook_silences_callback();
  std::printf("\nALL BLE_SCOUT_BROADCAST TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
