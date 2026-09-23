/**
 * @file test_ble_scout_pairing.cpp
 * @brief Host-build test for the BLE Scout proximity pairing window (F27).
 *
 * Two halves:
 *   A. The pure window FSM (ble_scout_pairing.h): arm / offer / finish /
 *      expire / cancel, the clamps, the label rule, wrap-safe timing, the
 *      status copy and the hashed_id hex codec the HTTP API speaks.
 *   B. The integration in ble_scout.cpp: an armed window pairs the FIRST
 *      unpaired advert at/above the threshold from inside
 *      ble_scout_on_advert(), weaker adverts and already-paired beacons do
 *      not consume it, the window is single-shot, a canceled or expired
 *      window pairs nothing, the registry snapshot carries hashed_id +
 *      label and never the MAC, and every change marks the registry dirty
 *      until the loop-task tick writes it.
 *
 * Build (the firmware.yml "Mesh + Scout Host Tests" link set):
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD -Wall -Wextra -Wno-unused-parameter \
 *       firmware/canary/lib/securacv_ble_scan/test_ble_scout_pairing.cpp \
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
 *       -o /tmp/test_scout_pairing && /tmp/test_scout_pairing
 */

#include "ble_scout.h"
#include "ble_scout_pairing.h"
#include "ble_scan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_ble_scout_pairing_run() { return 0; }
#else

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                        \
    }                                                                      \
  } while (0)

namespace {

namespace P = ble_scout::pairing;
using ble_scan::HASHED_ID_LEN;
using ble_scan::MAC_LEN;

/* ────────────────────────────────────────────────────────────────────────
 * A. Pure window FSM
 * ──────────────────────────────────────────────────────────────────────── */

void test_init_is_idle() {
  P::Window w;
  P::init(&w);
  CHECK(w.state == P::State::IDLE);
  const P::Status st = P::status(&w, 1234);
  CHECK(st.state == P::State::IDLE);
  CHECK(st.remaining_ms == 0);
  CHECK(std::strcmp(P::state_name(st.state), "idle") == 0);
  std::printf("PASS test_init_is_idle\n");
}

void test_clamps() {
  CHECK(P::clamp_window_ms(0) == P::DEFAULT_WINDOW_MS);
  CHECK(P::clamp_window_ms(1000) == P::MIN_WINDOW_MS);
  CHECK(P::clamp_window_ms(120000) == P::MAX_WINDOW_MS);
  CHECK(P::clamp_window_ms(30000) == 30000);
  CHECK(P::clamp_rssi_min(-100) == P::RSSI_MIN_FLOOR);
  CHECK(P::clamp_rssi_min(0) == P::RSSI_MIN_CEIL);
  CHECK(P::clamp_rssi_min(-45) == -45);
  CHECK(P::MAX_WINDOW_MS == 60000);          /* the API's window_s <= 60 */
  CHECK(P::DEFAULT_RSSI_MIN == -45);

  P::Window w;
  P::init(&w);
  CHECK(P::arm(&w, "keys", 999999, -120, 0) == P::ArmResult::OK);
  CHECK(w.window_ms == 60000);
  CHECK(w.rssi_min == -70);
  std::printf("PASS test_clamps\n");
}

void test_label_rule() {
  CHECK(!P::label_ok(nullptr));
  CHECK(!P::label_ok(""));
  CHECK(!P::label_ok("   "));
  CHECK(P::label_ok("Keys"));
  CHECK(P::label_ok("Front door tag #2"));
  CHECK(P::label_ok("abcdefghijklmnopqrstuvw"));    /* 23 = MAX_LABEL_LEN */
  CHECK(!P::label_ok("abcdefghijklmnopqrstuvwx"));  /* 24 */
  CHECK(!P::label_ok("tab\there"));
  CHECK(!P::label_ok("K\xc3\xbc" "che"));           /* UTF-8: refused, not rewritten */

  P::Window w;
  P::init(&w);
  CHECK(P::arm(&w, "", 0, -45, 0) == P::ArmResult::BAD_LABEL);
  CHECK(w.state == P::State::IDLE);
  std::printf("PASS test_label_rule\n");
}

void test_offer_threshold_and_single_shot() {
  P::Window w;
  P::init(&w);
  CHECK(P::arm(&w, "keys", 30000, -45, 1000) == P::ArmResult::OK);
  CHECK(P::arm(&w, "other", 30000, -45, 2000) == P::ArmResult::BUSY);

  CHECK(!P::offer(&w, -46, 2000));        /* one dB short */
  CHECK(w.state == P::State::ARMED);
  CHECK(P::offer(&w, -45, 3000));         /* at threshold: claims */
  CHECK(w.state == P::State::CLAIMED);
  CHECK(std::strcmp(P::state_name(w.state), "pairing") == 0);
  CHECK(!P::offer(&w, -30, 3001));        /* single-shot */
  CHECK(P::arm(&w, "other", 30000, -45, 3002) == P::ArmResult::BUSY);
  CHECK(!P::cancel(&w, 3002));            /* claimed: finishes, not canceled */

  uint8_t id[HASHED_ID_LEN];
  for (size_t i = 0; i < HASHED_ID_LEN; ++i) id[i] = (uint8_t)(0xA0 + i);
  P::finish(&w, true, id);
  CHECK(w.state == P::State::PAIRED);
  const P::Status st = P::status(&w, 4000);
  CHECK(st.state == P::State::PAIRED);
  CHECK(std::memcmp(st.paired_id, id, HASHED_ID_LEN) == 0);
  CHECK(std::strcmp(st.label, "keys") == 0);

  P::finish(&w, false, nullptr);          /* not CLAIMED: ignored */
  CHECK(w.state == P::State::PAIRED);

  /* A finished window re-arms. */
  CHECK(P::arm(&w, "wallet", 30000, -50, 5000) == P::ArmResult::OK);
  CHECK(P::status(&w, 5000).state == P::State::ARMED);
  const P::Status fresh = P::status(&w, 5000);
  for (size_t i = 0; i < HASHED_ID_LEN; ++i) CHECK(fresh.paired_id[i] == 0);
  std::printf("PASS test_offer_threshold_and_single_shot\n");
}

void test_failed_pair() {
  P::Window w;
  P::init(&w);
  CHECK(P::arm(&w, "keys", 30000, -45, 0) == P::ArmResult::OK);
  CHECK(P::offer(&w, -40, 10));
  P::finish(&w, false, nullptr);
  CHECK(w.state == P::State::FAILED);
  CHECK(std::strcmp(P::state_name(w.state), "failed") == 0);
  std::printf("PASS test_failed_pair\n");
}

void test_expiry() {
  P::Window w;
  P::init(&w);
  CHECK(P::arm(&w, "keys", 10000, -45, 1000) == P::ArmResult::OK);
  CHECK(!P::expire(&w, 10999));
  CHECK(P::status(&w, 6000).remaining_ms == 5000);
  /* status() reports an elapsed window as expired before the tick does. */
  CHECK(P::status(&w, 11000).state == P::State::EXPIRED);
  CHECK(w.state == P::State::ARMED);
  CHECK(P::expire(&w, 11000));
  CHECK(w.state == P::State::EXPIRED);
  CHECK(!P::expire(&w, 12000));
  CHECK(!P::offer(&w, -30, 12000));

  /* An offer past the window expires it even if no tick ran. */
  CHECK(P::arm(&w, "keys", 10000, -45, 20000) == P::ArmResult::OK);
  CHECK(!P::offer(&w, -30, 30000));
  CHECK(w.state == P::State::EXPIRED);

  /* An armed-but-elapsed window no longer blocks a new arm. */
  CHECK(P::arm(&w, "keys", 10000, -45, 40000) == P::ArmResult::OK);
  CHECK(P::arm(&w, "keys2", 10000, -45, 50000) == P::ArmResult::OK);
  std::printf("PASS test_expiry\n");
}

void test_cancel() {
  P::Window w;
  P::init(&w);
  CHECK(!P::cancel(&w, 0));               /* idle */
  CHECK(P::arm(&w, "keys", 10000, -45, 0) == P::ArmResult::OK);
  CHECK(P::cancel(&w, 100));
  CHECK(w.state == P::State::CANCELED);
  CHECK(std::strcmp(P::state_name(w.state), "canceled") == 0);
  CHECK(!P::offer(&w, -30, 200));
  CHECK(P::arm(&w, "keys", 10000, -45, 300) == P::ArmResult::OK);
  CHECK(!P::cancel(&w, 10300));           /* elapsed: expires instead */
  CHECK(w.state == P::State::EXPIRED);
  std::printf("PASS test_cancel\n");
}

void test_millis_wrap() {
  P::Window w;
  P::init(&w);
  const uint32_t t0 = 0xFFFFF000u;        /* 4096 ms before the wrap */
  CHECK(P::arm(&w, "keys", 10000, -45, t0) == P::ArmResult::OK);
  CHECK(!P::expire(&w, 0x00000100u));     /* 4352 ms in: still armed */
  CHECK(P::status(&w, 0x00000100u).remaining_ms == 10000 - 4352);
  CHECK(P::offer(&w, -40, 0x00000100u));
  P::Window w2;
  P::init(&w2);
  CHECK(P::arm(&w2, "keys", 10000, -45, t0) == P::ArmResult::OK);
  CHECK(P::expire(&w2, t0 + 10000u));     /* wraps past zero, expires */
  std::printf("PASS test_millis_wrap\n");
}

void test_hex_codec() {
  uint8_t id[HASHED_ID_LEN];
  for (size_t i = 0; i < HASHED_ID_LEN; ++i) id[i] = (uint8_t)(i * 17);
  char hex[2 * HASHED_ID_LEN + 1];
  P::id_to_hex(id, hex);
  CHECK(std::strlen(hex) == 32);
  CHECK(std::strcmp(hex, "00112233445566778899aabbccddeeff") == 0);
  uint8_t back[HASHED_ID_LEN];
  CHECK(P::id_from_hex(hex, back));
  CHECK(std::memcmp(back, id, HASHED_ID_LEN) == 0);
  CHECK(P::id_from_hex("00112233445566778899AABBCCDDEEFF", back));
  CHECK(std::memcmp(back, id, HASHED_ID_LEN) == 0);

  uint8_t untouched[HASHED_ID_LEN];
  std::memset(untouched, 0x5A, sizeof(untouched));
  CHECK(!P::id_from_hex(nullptr, untouched));
  CHECK(!P::id_from_hex("", untouched));
  CHECK(!P::id_from_hex("00112233445566778899aabbccddeef", untouched));    /* 31 */
  CHECK(!P::id_from_hex("00112233445566778899aabbccddeeff0", untouched));  /* 33 */
  CHECK(!P::id_from_hex("00112233445566778899aabbccddeefg", untouched));
  CHECK(!P::id_from_hex("0011223344556677 899aabbccddeeff", untouched));
  for (size_t i = 0; i < HASHED_ID_LEN; ++i) CHECK(untouched[i] == 0x5A);
  std::printf("PASS test_hex_codec\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * B. Integration through ble_scout.cpp (one process, shared state — each
 *    test builds on the registry the previous ones left).
 * ──────────────────────────────────────────────────────────────────────── */

/* The host build's deterministic per-device key (ble_scout_key.cpp). */
void expected_id(const uint8_t mac[MAC_LEN], uint8_t out[HASHED_ID_LEN]) {
  uint8_t key[ble_scan::PER_DEVICE_KEY_LEN];
  for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)i;
  CHECK(ble_scan::hash_beacon_id(key, mac, out));
}

bool snapshot_has(const uint8_t id[HASHED_ID_LEN], const char* label) {
  ble_scan::PairedBeacon snap[ble_scan::MAX_PAIRED_BEACONS];
  const size_t n = ble_scout::ble_scout_registry_snapshot(snap, ble_scan::MAX_PAIRED_BEACONS);
  for (size_t i = 0; i < n; ++i) {
    if (std::memcmp(snap[i].hashed_id, id, HASHED_ID_LEN) == 0) {
      return label == nullptr || std::strcmp(snap[i].label, label) == 0;
    }
  }
  return false;
}

/* No byte sequence equal to the MAC anywhere in the snapshot. */
bool snapshot_free_of(const uint8_t mac[MAC_LEN]) {
  ble_scan::PairedBeacon snap[ble_scan::MAX_PAIRED_BEACONS];
  std::memset(snap, 0, sizeof(snap));
  const size_t n = ble_scout::ble_scout_registry_snapshot(snap, ble_scan::MAX_PAIRED_BEACONS);
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(snap);
  const size_t len = n * sizeof(snap[0]);
  for (size_t i = 0; i + MAC_LEN <= len; ++i) {
    if (std::memcmp(bytes + i, mac, MAC_LEN) == 0) return false;
  }
  return true;
}

void test_window_before_init_is_not_ready() {
  CHECK(ble_scout::ble_scout_pair_window_start("keys", 0, -45, 0) ==
        P::ArmResult::NOT_READY);
  std::printf("PASS test_window_before_init_is_not_ready\n");
}

void test_window_pairs_first_strong_unpaired_advert() {
  CHECK(ble_scout::ble_scout_init());
  CHECK(ble_scout::ble_scout_count() == 0);
  CHECK(!ble_scout::ble_scout_registry_dirty());

  CHECK(ble_scout::ble_scout_pair_window_start("keys", 30000, -45, 1000) ==
        P::ArmResult::OK);
  CHECK(ble_scout::ble_scout_pair_window_start("x", 30000, -45, 1001) ==
        P::ArmResult::BUSY);

  const uint8_t far_mac[MAC_LEN]  = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  const uint8_t near_mac[MAC_LEN] = {0xC0, 0xFF, 0xEE, 0x00, 0x11, 0x22};
  ble_scout::ble_scout_on_advert(far_mac, -60, 2000);   /* across the room */
  CHECK(ble_scout::ble_scout_count() == 0);
  CHECK(ble_scout::ble_scout_pair_window_status(2000).state == P::State::ARMED);

  ble_scout::ble_scout_on_advert(near_mac, -38, 2500);  /* held against it */
  CHECK(ble_scout::ble_scout_count() == 1);

  uint8_t want[HASHED_ID_LEN];
  expected_id(near_mac, want);
  const P::Status st = ble_scout::ble_scout_pair_window_status(2600);
  CHECK(st.state == P::State::PAIRED);
  CHECK(std::memcmp(st.paired_id, want, HASHED_ID_LEN) == 0);
  CHECK(snapshot_has(want, "keys"));
  CHECK(snapshot_free_of(near_mac));

  /* Single-shot: a second strong stranger does not pair. */
  const uint8_t other_mac[MAC_LEN] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
  ble_scout::ble_scout_on_advert(other_mac, -30, 2700);
  CHECK(ble_scout::ble_scout_count() == 1);

  /* The loop-task tick owes, then writes, the NVS blob. */
  CHECK(ble_scout::ble_scout_registry_dirty());
  ble_scout::ble_scout_tick(3000);
  CHECK(!ble_scout::ble_scout_registry_dirty());
  std::printf("PASS test_window_pairs_first_strong_unpaired_advert\n");
}

void test_paired_beacon_does_not_consume_window() {
  CHECK(ble_scout::ble_scout_pair_window_start("wallet", 30000, -45, 10000) ==
        P::ArmResult::OK);
  const uint8_t keys_mac[MAC_LEN]   = {0xC0, 0xFF, 0xEE, 0x00, 0x11, 0x22};
  const uint8_t wallet_mac[MAC_LEN] = {0x77, 0x66, 0x55, 0x44, 0x33, 0x22};
  ble_scout::ble_scout_on_advert(keys_mac, -30, 10100);  /* already paired */
  CHECK(ble_scout::ble_scout_pair_window_status(10100).state == P::State::ARMED);
  CHECK(ble_scout::ble_scout_count() == 1);
  uint8_t keys_id[HASHED_ID_LEN];
  expected_id(keys_mac, keys_id);
  CHECK(snapshot_has(keys_id, "keys"));                   /* not relabeled */

  ble_scout::ble_scout_on_advert(wallet_mac, -44, 10200);
  CHECK(ble_scout::ble_scout_count() == 2);
  uint8_t wallet_id[HASHED_ID_LEN];
  expected_id(wallet_mac, wallet_id);
  CHECK(snapshot_has(wallet_id, "wallet"));
  CHECK(snapshot_free_of(wallet_mac));
  ble_scout::ble_scout_tick(10300);
  std::printf("PASS test_paired_beacon_does_not_consume_window\n");
}

void test_canceled_and_expired_windows_pair_nothing() {
  const uint8_t mac[MAC_LEN] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

  CHECK(ble_scout::ble_scout_pair_window_start("bag", 30000, -45, 20000) ==
        P::ArmResult::OK);
  CHECK(ble_scout::ble_scout_pair_window_cancel(20100));
  CHECK(!ble_scout::ble_scout_pair_window_cancel(20150));
  CHECK(ble_scout::ble_scout_pair_window_status(20100).state == P::State::CANCELED);
  ble_scout::ble_scout_on_advert(mac, -30, 20200);
  CHECK(ble_scout::ble_scout_count() == 2);

  CHECK(ble_scout::ble_scout_pair_window_start("bag", 10000, -45, 30000) ==
        P::ArmResult::OK);
  ble_scout::ble_scout_tick(40000);                       /* loop-task expiry */
  CHECK(ble_scout::ble_scout_pair_window_status(40000).state == P::State::EXPIRED);
  ble_scout::ble_scout_on_advert(mac, -30, 40100);
  CHECK(ble_scout::ble_scout_count() == 2);
  CHECK(!ble_scout::ble_scout_registry_dirty());
  std::printf("PASS test_canceled_and_expired_windows_pair_nothing\n");
}

void test_bad_label_refused_at_start() {
  CHECK(ble_scout::ble_scout_pair_window_start("", 0, -45, 50000) ==
        P::ArmResult::BAD_LABEL);
  CHECK(ble_scout::ble_scout_pair_window_start("K\xc3\xbc" "che", 0, -45, 50000) ==
        P::ArmResult::BAD_LABEL);
  std::printf("PASS test_bad_label_refused_at_start\n");
}

void test_unpair_marks_dirty() {
  const uint8_t wallet_mac[MAC_LEN] = {0x77, 0x66, 0x55, 0x44, 0x33, 0x22};
  uint8_t wallet_id[HASHED_ID_LEN];
  expected_id(wallet_mac, wallet_id);
  CHECK(ble_scout::ble_scout_unpair(wallet_id));
  CHECK(ble_scout::ble_scout_count() == 1);
  CHECK(!snapshot_has(wallet_id, nullptr));
  CHECK(ble_scout::ble_scout_registry_dirty());
  ble_scout::ble_scout_tick(60000);
  CHECK(!ble_scout::ble_scout_registry_dirty());
  CHECK(!ble_scout::ble_scout_unpair(wallet_id));         /* already gone */
  CHECK(!ble_scout::ble_scout_registry_dirty());          /* nothing changed */
  std::printf("PASS test_unpair_marks_dirty\n");
}

void test_full_registry_refuses_to_arm() {
  /* Fill the remaining slots through the window itself. */
  uint32_t t = 70000;
  while (ble_scout::ble_scout_count() < ble_scan::MAX_PAIRED_BEACONS) {
    const uint8_t n = (uint8_t)ble_scout::ble_scout_count();
    const uint8_t mac[MAC_LEN] = {0xEE, 0xEE, 0xEE, 0x00, 0x00, n};
    CHECK(ble_scout::ble_scout_pair_window_start("tag", 10000, -45, t) ==
          P::ArmResult::OK);
    ble_scout::ble_scout_on_advert(mac, -40, t + 10);
    CHECK(ble_scout::ble_scout_pair_window_status(t + 20).state == P::State::PAIRED);
    t += 100;
  }
  CHECK(ble_scout::ble_scout_pair_window_start("one more", 10000, -45, t) ==
        P::ArmResult::REGISTRY_FULL);
  ble_scan::PairedBeacon snap[ble_scan::MAX_PAIRED_BEACONS + 4];
  CHECK(ble_scout::ble_scout_registry_snapshot(snap, 3) == 3);
  CHECK(ble_scout::ble_scout_registry_snapshot(snap, ble_scan::MAX_PAIRED_BEACONS + 4) ==
        ble_scan::MAX_PAIRED_BEACONS);
  CHECK(ble_scout::ble_scout_registry_snapshot(nullptr, 4) == 0);
  std::printf("PASS test_full_registry_refuses_to_arm\n");
}

}  /* namespace */

int main() {
  test_init_is_idle();
  test_clamps();
  test_label_rule();
  test_offer_threshold_and_single_shot();
  test_failed_pair();
  test_expiry();
  test_cancel();
  test_millis_wrap();
  test_hex_codec();

  test_window_before_init_is_not_ready();
  test_window_pairs_first_strong_unpaired_advert();
  test_paired_beacon_does_not_consume_window();
  test_canceled_and_expired_windows_pair_nothing();
  test_bad_label_refused_at_start();
  test_unpair_marks_dirty();
  test_full_registry_refuses_to_arm();
  std::printf("\nALL BLE_SCOUT_PAIRING TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
