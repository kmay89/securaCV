/**
 * @file test_ble_scout_registry_store.cpp
 * @brief Host-build test for the persisted paired-beacon registry blob (F27).
 *
 * Verifies ble_scout_registry_store.h — the NVS twin of ble_scan's Registry:
 *   1. The blob length and header are the documented contract (magic "SCR",
 *      version 1, 16 slots x 41 bytes).
 *   2. Round trip: hashed_ids and labels survive serialize -> deserialize,
 *      including a full table and a 23-character label.
 *   3. All-or-nothing refusal: bad magic, bad version, truncated or padded
 *      blob, an in_use byte other than 0/1, and a label with no NUL each
 *      refuse the blob and leave the caller's registry untouched.
 *   4. A blob's labels go back through registry_add, so a non-printable
 *      byte smuggled into NVS comes back sanitized like a live pair.
 *   5. The blob holds no MAC: only what the registry holds.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD -Wall -Wextra -Wno-unused-parameter \
 *       firmware/canary/lib/securacv_ble_scan/test_ble_scout_registry_store.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scan.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       -I firmware/canary/lib/securacv_ble_scan/src \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_scout_store && /tmp/test_scout_store
 */

#include "ble_scout_registry_store.h"
#include "ble_scan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_ble_scout_registry_store_run() { return 0; }
#else

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                        \
    }                                                                      \
  } while (0)

namespace {

namespace S = ble_scout::registry_store;
using ble_scan::HASHED_ID_LEN;

void make_id(uint8_t seed, uint8_t out[HASHED_ID_LEN]) {
  for (size_t i = 0; i < HASHED_ID_LEN; ++i) out[i] = (uint8_t)(seed * 31 + i);
}

bool registries_equal(const ble_scan::Registry& a, const ble_scan::Registry& b) {
  if (ble_scan::registry_count(&a) != ble_scan::registry_count(&b)) return false;
  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    if (!a.slots[i].in_use) continue;
    const ble_scan::PairedBeacon* p = ble_scan::registry_find(&b, a.slots[i].hashed_id);
    if (p == nullptr || std::strcmp(p->label, a.slots[i].label) != 0) return false;
  }
  return true;
}

/* A registry with two entries, used as the "untouched" sentinel. */
void sentinel(ble_scan::Registry* r) {
  ble_scan::registry_init(r);
  uint8_t id[HASHED_ID_LEN];
  make_id(200, id);
  CHECK(ble_scan::registry_add(r, id, "sentinel-a"));
  make_id(201, id);
  CHECK(ble_scan::registry_add(r, id, "sentinel-b"));
}

void test_contract_shape() {
  CHECK(S::SLOT_LEN == 41);
  CHECK(S::BLOB_LEN == 4 + 16 * 41);
  CHECK(std::strcmp(S::NVS_NAMESPACE, "securacv") == 0);
  CHECK(std::strcmp(S::NVS_KEY, "scout.reg") == 0);
  CHECK(std::strlen(S::NVS_KEY) <= 15);            /* NVS key budget */

  ble_scan::Registry r;
  ble_scan::registry_init(&r);
  uint8_t blob[S::BLOB_LEN];
  CHECK(S::serialize(&r, blob, sizeof(blob)) == S::BLOB_LEN);
  CHECK(blob[0] == 'S' && blob[1] == 'C' && blob[2] == 'R' && blob[3] == 1);
  for (size_t i = S::HEADER_LEN; i < S::BLOB_LEN; ++i) CHECK(blob[i] == 0);

  CHECK(S::serialize(&r, blob, sizeof(blob) - 1) == 0);   /* cap too small */
  CHECK(S::serialize(nullptr, blob, sizeof(blob)) == 0);
  CHECK(S::serialize(&r, nullptr, sizeof(blob)) == 0);
  std::printf("PASS test_contract_shape\n");
}

void test_round_trip() {
  ble_scan::Registry a;
  ble_scan::registry_init(&a);
  uint8_t id[HASHED_ID_LEN];
  make_id(1, id);
  CHECK(ble_scan::registry_add(&a, id, "keys"));
  make_id(2, id);
  CHECK(ble_scan::registry_add(&a, id, "abcdefghijklmnopqrstuvw"));  /* 23 */
  make_id(3, id);
  CHECK(ble_scan::registry_add(&a, id, ""));
  make_id(2, id);
  CHECK(ble_scan::registry_remove(&a, id));      /* leaves a hole in slot 1 */
  make_id(4, id);
  CHECK(ble_scan::registry_add(&a, id, "front door"));

  uint8_t blob[S::BLOB_LEN];
  CHECK(S::serialize(&a, blob, sizeof(blob)) == S::BLOB_LEN);
  ble_scan::Registry b;
  ble_scan::registry_init(&b);
  CHECK(S::deserialize(&b, blob, sizeof(blob)));
  CHECK(registries_equal(a, b));
  CHECK(ble_scan::registry_count(&b) == 3);

  /* Full table. */
  ble_scan::Registry full;
  ble_scan::registry_init(&full);
  for (uint8_t k = 0; k < ble_scan::MAX_PAIRED_BEACONS; ++k) {
    make_id((uint8_t)(50 + k), id);
    char label[8];
    std::snprintf(label, sizeof(label), "tag%u", (unsigned)k);
    CHECK(ble_scan::registry_add(&full, id, label));
  }
  CHECK(S::serialize(&full, blob, sizeof(blob)) == S::BLOB_LEN);
  ble_scan::Registry full2;
  ble_scan::registry_init(&full2);
  CHECK(S::deserialize(&full2, blob, sizeof(blob)));
  CHECK(registries_equal(full, full2));
  CHECK(ble_scan::registry_count(&full2) == ble_scan::MAX_PAIRED_BEACONS);
  std::printf("PASS test_round_trip\n");
}

void test_refusals_leave_registry_untouched() {
  ble_scan::Registry src;
  ble_scan::registry_init(&src);
  uint8_t id[HASHED_ID_LEN];
  make_id(9, id);
  CHECK(ble_scan::registry_add(&src, id, "keys"));
  uint8_t good[S::BLOB_LEN];
  CHECK(S::serialize(&src, good, sizeof(good)) == S::BLOB_LEN);

  ble_scan::Registry ref;
  sentinel(&ref);
  uint8_t bad[S::BLOB_LEN + 1];

  struct Case { const char* name; size_t len; void (*mutate)(uint8_t*); };
  const Case cases[] = {
    {"bad magic",    S::BLOB_LEN,     [](uint8_t* b) { b[0] = 'X'; }},
    {"bad version",  S::BLOB_LEN,     [](uint8_t* b) { b[3] = 2; }},
    {"truncated",    S::BLOB_LEN - 1, [](uint8_t*) {}},
    {"padded",       S::BLOB_LEN + 1, [](uint8_t*) {}},
    {"in_use = 2",   S::BLOB_LEN,     [](uint8_t* b) { b[S::HEADER_LEN] = 2; }},
    {"label no NUL", S::BLOB_LEN,     [](uint8_t* b) {
        std::memset(b + S::HEADER_LEN + 1 + HASHED_ID_LEN, 'A', S::LABEL_LEN); }},
  };
  for (const Case& c : cases) {
    std::memset(bad, 0, sizeof(bad));
    std::memcpy(bad, good, sizeof(good));
    c.mutate(bad);
    ble_scan::Registry r;
    sentinel(&r);
    if (S::deserialize(&r, bad, c.len)) {
      std::fprintf(stderr, "FAIL: accepted %s\n", c.name);
      std::exit(1);
    }
    CHECK(registries_equal(r, ref));
  }
  ble_scan::Registry r;
  sentinel(&r);
  CHECK(!S::deserialize(&r, nullptr, S::BLOB_LEN));
  CHECK(!S::deserialize(nullptr, good, S::BLOB_LEN));
  CHECK(registries_equal(r, ref));
  std::printf("PASS test_refusals_leave_registry_untouched\n");
}

void test_blob_labels_are_sanitized() {
  ble_scan::Registry src;
  ble_scan::registry_init(&src);
  uint8_t id[HASHED_ID_LEN];
  make_id(7, id);
  CHECK(ble_scan::registry_add(&src, id, "door"));
  uint8_t blob[S::BLOB_LEN];
  CHECK(S::serialize(&src, blob, sizeof(blob)) == S::BLOB_LEN);
  /* Find the in-use slot and plant a control byte in its label. */
  size_t slot = 0;
  while (blob[S::HEADER_LEN + slot * S::SLOT_LEN] != 1) ++slot;
  blob[S::HEADER_LEN + slot * S::SLOT_LEN + 1 + HASHED_ID_LEN + 1] = 0x07;

  ble_scan::Registry out;
  ble_scan::registry_init(&out);
  CHECK(S::deserialize(&out, blob, sizeof(blob)));
  const ble_scan::PairedBeacon* p = ble_scan::registry_find(&out, id);
  CHECK(p != nullptr);
  CHECK(std::strcmp(p->label, "d?or") == 0);
  std::printf("PASS test_blob_labels_are_sanitized\n");
}

void test_blob_holds_no_mac() {
  /* The registry only ever sees hashed ids; build one from a real MAC via
   * the keyed hash and prove the MAC bytes are nowhere in the blob. */
  uint8_t key[ble_scan::PER_DEVICE_KEY_LEN];
  for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(0x40 + i);
  const uint8_t mac[ble_scan::MAC_LEN] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
  uint8_t id[HASHED_ID_LEN];
  CHECK(ble_scan::hash_beacon_id(key, mac, id));
  ble_scan::Registry r;
  ble_scan::registry_init(&r);
  CHECK(ble_scan::registry_add(&r, id, "keys"));
  uint8_t blob[S::BLOB_LEN];
  CHECK(S::serialize(&r, blob, sizeof(blob)) == S::BLOB_LEN);
  for (size_t i = 0; i + ble_scan::MAC_LEN <= sizeof(blob); ++i) {
    CHECK(std::memcmp(blob + i, mac, ble_scan::MAC_LEN) != 0);
  }
  std::printf("PASS test_blob_holds_no_mac\n");
}

}  /* namespace */

int main() {
  test_contract_shape();
  test_round_trip();
  test_refusals_leave_registry_untouched();
  test_blob_labels_are_sanitized();
  test_blob_holds_no_mac();
  std::printf("\nALL BLE_SCOUT_REGISTRY_STORE TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
