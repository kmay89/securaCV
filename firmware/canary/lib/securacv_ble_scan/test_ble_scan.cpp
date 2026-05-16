/**
 * @file test_ble_scan.cpp
 * @brief Host-build conformance test for the BLE Scout pure primitives.
 *
 * Verifies:
 *   1. Hashed beacon ID is deterministic (same key + mac → same id).
 *   2. Hashed beacon ID is key-isolated (same mac, different keys
 *      → different ids).
 *   3. Hashed beacon ID is mac-distinguishing (same key, different
 *      macs → different ids).
 *   4. hashed_id_equal returns true / false correctly.
 *   5. Kalman first-sample shortcut: primed=false → returns raw value.
 *   6. Kalman convergence: noisy stream → smoothed → final near
 *      true value.
 *   7. Kalman reset wipes state.
 *   8. Registry add / find / remove round-trip.
 *   9. Registry bounded at MAX_PAIRED_BEACONS — N+1 add returns false.
 *  10. Registry re-add of same id updates the label (idempotent).
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_ble_scan/test_ble_scan.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scan.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       -I firmware/canary/lib/securacv_ble_scan/src \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_ble && /tmp/test_ble
 */

#include "ble_scan.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_ble_scan_run() { return 0; }
#else

namespace {

/* ────────────────────────────────────────────────────────────────────────
 * Hashed beacon ID
 * ──────────────────────────────────────────────────────────────────────── */

void test_hash_deterministic() {
  uint8_t key[ble_scan::PER_DEVICE_KEY_LEN];
  uint8_t mac[ble_scan::MAC_LEN];
  for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(0x10 + i);
  for (size_t i = 0; i < sizeof(mac); ++i) mac[i] = (uint8_t)(0xA0 + i);

  uint8_t a[ble_scan::HASHED_ID_LEN], b[ble_scan::HASHED_ID_LEN];
  assert(ble_scan::hash_beacon_id(key, mac, a));
  assert(ble_scan::hash_beacon_id(key, mac, b));
  assert(std::memcmp(a, b, ble_scan::HASHED_ID_LEN) == 0);
  std::printf("PASS test_hash_deterministic\n");
}

void test_hash_key_isolation() {
  uint8_t key_a[ble_scan::PER_DEVICE_KEY_LEN] = {0};
  uint8_t key_b[ble_scan::PER_DEVICE_KEY_LEN] = {0};
  key_b[0] = 1;   /* single-bit difference */
  uint8_t mac[ble_scan::MAC_LEN] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};

  uint8_t ha[ble_scan::HASHED_ID_LEN], hb[ble_scan::HASHED_ID_LEN];
  assert(ble_scan::hash_beacon_id(key_a, mac, ha));
  assert(ble_scan::hash_beacon_id(key_b, mac, hb));
  assert(std::memcmp(ha, hb, ble_scan::HASHED_ID_LEN) != 0);
  std::printf("PASS test_hash_key_isolation\n");
}

void test_hash_mac_distinguishing() {
  uint8_t key[ble_scan::PER_DEVICE_KEY_LEN] = {0};
  for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)i;
  uint8_t mac1[ble_scan::MAC_LEN] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x01};
  uint8_t mac2[ble_scan::MAC_LEN] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x02};

  uint8_t h1[ble_scan::HASHED_ID_LEN], h2[ble_scan::HASHED_ID_LEN];
  assert(ble_scan::hash_beacon_id(key, mac1, h1));
  assert(ble_scan::hash_beacon_id(key, mac2, h2));
  assert(std::memcmp(h1, h2, ble_scan::HASHED_ID_LEN) != 0);
  std::printf("PASS test_hash_mac_distinguishing\n");
}

void test_hashed_id_equal() {
  uint8_t a[ble_scan::HASHED_ID_LEN], b[ble_scan::HASHED_ID_LEN];
  for (size_t i = 0; i < ble_scan::HASHED_ID_LEN; ++i) {
    a[i] = (uint8_t)i;
    b[i] = (uint8_t)i;
  }
  assert(ble_scan::hashed_id_equal(a, b));
  b[7] ^= 0x01;
  assert(!ble_scan::hashed_id_equal(a, b));
  std::printf("PASS test_hashed_id_equal\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * Kalman
 * ──────────────────────────────────────────────────────────────────────── */

void test_kalman_first_sample_shortcut() {
  ble_scan::KalmanRssi k;
  ble_scan::kalman_reset(&k, 0.0f, 0.0f);
  assert(!k.primed);
  const float out = ble_scan::kalman_update(&k, -55.0f);
  assert(k.primed);
  /* First sample returns as-is (no warm-up). */
  assert(std::fabs(out - (-55.0f)) < 0.001f);
  std::printf("PASS test_kalman_first_sample_shortcut  (out=%.2f)\n", out);
}

void test_kalman_converges_to_true_value() {
  /* Stream of noisy samples around a true value of -60 dB; smoothed
   * estimate should settle near -60. We use the defaults (q=0.5, r=4.0). */
  ble_scan::KalmanRssi k;
  ble_scan::kalman_reset(&k, 0.0f, 0.0f);

  std::srand(42);
  const float true_rssi = -60.0f;
  float last = 0.0f;
  for (int i = 0; i < 200; ++i) {
    /* Gaussian-ish noise via simple uniform sum. */
    float noise = 0.0f;
    for (int j = 0; j < 6; ++j) noise += ((float)std::rand() / RAND_MAX) - 0.5f;
    noise *= 2.0f;   /* roughly ±3 dB swing */
    last = ble_scan::kalman_update(&k, true_rssi + noise);
  }
  /* After 200 samples, smoothed estimate within 1 dB of truth. */
  assert(std::fabs(last - true_rssi) < 1.0f);
  std::printf("PASS test_kalman_converges_to_true_value  (final=%.2f, true=%.2f)\n",
              last, true_rssi);
}

void test_kalman_reset_clears_state() {
  ble_scan::KalmanRssi k;
  ble_scan::kalman_reset(&k, 0.0f, 0.0f);
  ble_scan::kalman_update(&k, -55.0f);
  assert(k.primed);
  ble_scan::kalman_reset(&k, 0.0f, 0.0f);
  assert(!k.primed);
  std::printf("PASS test_kalman_reset_clears_state\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * Registry
 * ──────────────────────────────────────────────────────────────────────── */

void make_id(uint8_t out[ble_scan::HASHED_ID_LEN], uint8_t seed) {
  for (size_t i = 0; i < ble_scan::HASHED_ID_LEN; ++i) out[i] = (uint8_t)(seed + i);
}

void test_registry_add_find_remove_roundtrip() {
  ble_scan::Registry r;
  ble_scan::registry_init(&r);
  assert(ble_scan::registry_count(&r) == 0);

  uint8_t id[ble_scan::HASHED_ID_LEN];
  make_id(id, 0x10);
  assert(ble_scan::registry_add(&r, id, "kitchen-fob"));
  assert(ble_scan::registry_count(&r) == 1);

  const ble_scan::PairedBeacon* p = ble_scan::registry_find(&r, id);
  assert(p != nullptr);
  assert(std::strcmp(p->label, "kitchen-fob") == 0);

  assert(ble_scan::registry_remove(&r, id));
  assert(ble_scan::registry_count(&r) == 0);
  assert(ble_scan::registry_find(&r, id) == nullptr);
  std::printf("PASS test_registry_add_find_remove_roundtrip\n");
}

void test_registry_bounded() {
  ble_scan::Registry r;
  ble_scan::registry_init(&r);
  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    uint8_t id[ble_scan::HASHED_ID_LEN];
    make_id(id, (uint8_t)(0x20 + i));
    assert(ble_scan::registry_add(&r, id, "beacon"));
  }
  assert(ble_scan::registry_count(&r) == ble_scan::MAX_PAIRED_BEACONS);

  /* One more should fail (new id, table full). */
  uint8_t extra[ble_scan::HASHED_ID_LEN];
  make_id(extra, 0xFE);
  assert(!ble_scan::registry_add(&r, extra, "spillover"));
  assert(ble_scan::registry_count(&r) == ble_scan::MAX_PAIRED_BEACONS);
  std::printf("PASS test_registry_bounded\n");
}

void test_registry_idempotent_relabel() {
  ble_scan::Registry r;
  ble_scan::registry_init(&r);

  uint8_t id[ble_scan::HASHED_ID_LEN];
  make_id(id, 0x30);
  assert(ble_scan::registry_add(&r, id, "old-name"));
  assert(ble_scan::registry_count(&r) == 1);

  /* Re-add with new label — should succeed, update label, NOT add a
   * second slot. */
  assert(ble_scan::registry_add(&r, id, "new-name"));
  assert(ble_scan::registry_count(&r) == 1);
  const ble_scan::PairedBeacon* p = ble_scan::registry_find(&r, id);
  assert(p != nullptr);
  assert(std::strcmp(p->label, "new-name") == 0);
  std::printf("PASS test_registry_idempotent_relabel\n");
}

}  /* namespace */

int main() {
  test_hash_deterministic();
  test_hash_key_isolation();
  test_hash_mac_distinguishing();
  test_hashed_id_equal();
  test_kalman_first_sample_shortcut();
  test_kalman_converges_to_true_value();
  test_kalman_reset_clears_state();
  test_registry_add_find_remove_roundtrip();
  test_registry_bounded();
  test_registry_idempotent_relabel();
  std::printf("\nALL BLE_SCAN TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
