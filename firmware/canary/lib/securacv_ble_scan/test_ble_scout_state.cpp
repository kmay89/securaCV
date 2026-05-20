/**
 * @file test_ble_scout_state.cpp
 * @brief Host-build conformance test for the BLE Scout presence state machine.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_ble_scan/test_ble_scout_state.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scout_state.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scan.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       -I firmware/canary/lib/securacv_ble_scan/src \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_scout && /tmp/test_scout
 */

#include "ble_scout_state.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_ble_scout_state_run() { return 0; }
#else

namespace {

void make_id(uint8_t out[ble_scan::HASHED_ID_LEN], uint8_t seed) {
  for (size_t i = 0; i < ble_scan::HASHED_ID_LEN; ++i)
    out[i] = (uint8_t)(seed + i);
}

/* Feed N adverts at strong rssi spaced 100 ms apart. Returns the
 * first event emitted (NONE/ARRIVED/DEPARTED) across the run. */
ble_scout::PresenceEvent
drive_strong(ble_scout::PresenceTracker* t,
             const uint8_t* id, int8_t rssi, uint32_t start_ms,
             int n_samples, int step_ms) {
  ble_scout::PresenceEvent first = ble_scout::PresenceEvent::NONE;
  for (int i = 0; i < n_samples; ++i) {
    ble_scout::PresenceEvent e =
      ble_scout::presence_on_advert(t, id, rssi, start_ms + (uint32_t)(i * step_ms));
    if (e != ble_scout::PresenceEvent::NONE &&
        first == ble_scout::PresenceEvent::NONE) {
      first = e;
    }
  }
  return first;
}

/* ────────────────────────────────────────────────────────────────────────
 * Tests
 * ──────────────────────────────────────────────────────────────────────── */

void test_arrived_after_dwell() {
  ble_scout::PresenceTracker t;
  ble_scout::presence_init(&t);

  uint8_t id[ble_scan::HASHED_ID_LEN];
  make_id(id, 0x10);

  /* Single packet at -60 (above -75 threshold) should NOT arrive — needs DWELL. */
  auto e0 = ble_scout::presence_on_advert(&t, id, -60, 1000);
  assert(e0 == ble_scout::PresenceEvent::NONE);
  assert(ble_scout::presence_query(&t, id) == ble_scout::PresenceState::PROVISIONAL);

  /* Still in DWELL window after 4 seconds. */
  auto e1 = ble_scout::presence_on_advert(&t, id, -60, 5000);
  assert(e1 == ble_scout::PresenceEvent::NONE);

  /* At 6500ms (DWELL=5000 from t=1000), should ARRIVE. */
  auto e2 = ble_scout::presence_on_advert(&t, id, -60, 6500);
  assert(e2 == ble_scout::PresenceEvent::ARRIVED);
  assert(ble_scout::presence_query(&t, id) == ble_scout::PresenceState::PRESENT);

  std::printf("PASS test_arrived_after_dwell\n");
}

void test_provisional_drops_on_signal_loss() {
  ble_scout::PresenceTracker t;
  ble_scout::presence_init(&t);

  uint8_t id[ble_scan::HASHED_ID_LEN];
  make_id(id, 0x20);

  /* Strong sample → provisional. */
  ble_scout::presence_on_advert(&t, id, -60, 1000);
  assert(ble_scout::presence_query(&t, id) == ble_scout::PresenceState::PROVISIONAL);

  /* Weak sample (below threshold) drops back to AWAY before DWELL. */
  ble_scout::presence_on_advert(&t, id, -90, 2000);
  assert(ble_scout::presence_query(&t, id) == ble_scout::PresenceState::AWAY);

  std::printf("PASS test_provisional_drops_on_signal_loss\n");
}

void test_brief_flyby_does_not_arrive() {
  ble_scout::PresenceTracker t;
  ble_scout::presence_init(&t);

  uint8_t id[ble_scan::HASHED_ID_LEN];
  make_id(id, 0x30);

  /* One strong packet followed by silence for 60s. Should never ARRIVE. */
  auto e = ble_scout::presence_on_advert(&t, id, -55, 1000);
  assert(e == ble_scout::PresenceEvent::NONE);

  /* Tick at +30s — should not fire DEPARTED (we never reached PRESENT). */
  uint8_t departed[ble_scan::HASHED_ID_LEN];
  size_t  n = ble_scout::presence_on_tick(&t, 31000, departed, 1);
  assert(n == 0);

  std::printf("PASS test_brief_flyby_does_not_arrive\n");
}

void test_departed_after_lost() {
  ble_scout::PresenceTracker t;
  ble_scout::presence_init(&t);

  uint8_t id[ble_scan::HASHED_ID_LEN];
  make_id(id, 0x40);

  /* Drive to PRESENT. */
  auto e = drive_strong(&t, id, -55, 0, 80, 100);   /* 8s of 10 Hz strong adverts */
  assert(e == ble_scout::PresenceEvent::ARRIVED);
  assert(ble_scout::presence_query(&t, id) == ble_scout::PresenceState::PRESENT);

  /* Last seen ~= 7900 ms. Tick at 38000 — well past LOST_MS=30000. */
  uint8_t departed[ble_scan::HASHED_ID_LEN];
  size_t  n = ble_scout::presence_on_tick(&t, 38000, departed, 1);
  assert(n == 1);
  assert(std::memcmp(departed, id, ble_scan::HASHED_ID_LEN) == 0);
  assert(ble_scout::presence_query(&t, id) == ble_scout::PresenceState::AWAY);

  /* Second tick — already-departed slot does not re-fire. */
  size_t n2 = ble_scout::presence_on_tick(&t, 40000, departed, 1);
  assert(n2 == 0);

  std::printf("PASS test_departed_after_lost\n");
}

void test_present_survives_brief_silence() {
  ble_scout::PresenceTracker t;
  ble_scout::presence_init(&t);

  uint8_t id[ble_scan::HASHED_ID_LEN];
  make_id(id, 0x50);

  /* Drive to PRESENT. */
  drive_strong(&t, id, -55, 0, 80, 100);
  assert(ble_scout::presence_query(&t, id) == ble_scout::PresenceState::PRESENT);

  /* Tick at +20s (under LOST_MS=30s) — no DEPARTED. */
  size_t n = ble_scout::presence_on_tick(&t, 27000, nullptr, 0);
  assert(n == 0);
  assert(ble_scout::presence_query(&t, id) == ble_scout::PresenceState::PRESENT);

  std::printf("PASS test_present_survives_brief_silence\n");
}

void test_forget_clears_slot() {
  ble_scout::PresenceTracker t;
  ble_scout::presence_init(&t);

  uint8_t id[ble_scan::HASHED_ID_LEN];
  make_id(id, 0x60);

  drive_strong(&t, id, -55, 0, 80, 100);
  assert(ble_scout::presence_query(&t, id) == ble_scout::PresenceState::PRESENT);

  ble_scout::presence_forget(&t, id);
  assert(ble_scout::presence_query(&t, id) == ble_scout::PresenceState::AWAY);

  std::printf("PASS test_forget_clears_slot\n");
}

void test_two_beacons_independent() {
  ble_scout::PresenceTracker t;
  ble_scout::presence_init(&t);

  uint8_t a[ble_scan::HASHED_ID_LEN], b[ble_scan::HASHED_ID_LEN];
  make_id(a, 0x70);
  make_id(b, 0x80);

  /* A arrives. */
  auto ea = drive_strong(&t, a, -55, 0, 80, 100);
  assert(ea == ble_scout::PresenceEvent::ARRIVED);

  /* B is silent throughout — still AWAY. */
  assert(ble_scout::presence_query(&t, b) == ble_scout::PresenceState::AWAY);

  /* B then arrives. A still PRESENT. */
  auto eb = drive_strong(&t, b, -50, 10000, 80, 100);
  assert(eb == ble_scout::PresenceEvent::ARRIVED);
  assert(ble_scout::presence_query(&t, a) == ble_scout::PresenceState::PRESENT);
  assert(ble_scout::presence_query(&t, b) == ble_scout::PresenceState::PRESENT);

  std::printf("PASS test_two_beacons_independent\n");
}

void test_tracker_full_returns_none() {
  ble_scout::PresenceTracker t;
  ble_scout::presence_init(&t);

  /* Fill all slots with distinct ids by driving one strong advert each. */
  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    uint8_t id[ble_scan::HASHED_ID_LEN];
    make_id(id, (uint8_t)(0xA0 + i));
    ble_scout::presence_on_advert(&t, id, -60, 1000);
  }

  /* One more — should not crash, should return NONE (no slot to claim). */
  uint8_t extra[ble_scan::HASHED_ID_LEN];
  make_id(extra, 0xFE);
  auto e = ble_scout::presence_on_advert(&t, extra, -55, 2000);
  assert(e == ble_scout::PresenceEvent::NONE);

  std::printf("PASS test_tracker_full_returns_none\n");
}

}  /* namespace */

int main() {
  test_arrived_after_dwell();
  test_provisional_drops_on_signal_loss();
  test_brief_flyby_does_not_arrive();
  test_departed_after_lost();
  test_present_survives_brief_silence();
  test_forget_clears_slot();
  test_two_beacons_independent();
  test_tracker_full_returns_none();
  std::printf("\nALL BLE_SCOUT_STATE TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
