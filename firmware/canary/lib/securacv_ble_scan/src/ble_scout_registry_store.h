/*
 * SecuraCV Canary — BLE Scout paired-beacon registry, persisted form
 *
 * The NVS twin ble_scan.h's Registry promised (repo sweep F27): one blob
 * under the "securacv" namespace, key "scout.reg", written by the loop task
 * whenever a pairing or unpairing lands and read once at ble_scout_init().
 * It holds exactly what the in-RAM registry holds — the 16-byte hashed_id
 * (a per-device keyed hash, never the MAC) and the owner's label — so
 * persisting it adds no identifier the device did not already keep.
 *
 * Layout (a new persisted contract, so it is versioned):
 *   [0..2]  magic "SCR"
 *   [3]     version (1)
 *   then MAX_PAIRED_BEACONS slots of SLOT_LEN bytes each:
 *     [0]      in_use (0 or 1; anything else refuses the blob)
 *     [1..16]  hashed_id
 *     [17..40] label, NUL-padded (MAX_LABEL_LEN + 1 bytes, NUL required)
 *
 * deserialize() is all-or-nothing: a wrong magic, version or length, an
 * in_use byte other than 0/1, or a label with no NUL refuses the whole blob
 * and leaves the caller's registry untouched (the Scout then boots with an
 * empty registry — honest, and re-pairing is one gesture). Accepted slots go
 * back in through ble_scan::registry_add, so the printable-ASCII label rule
 * and the no-duplicates rule hold for a blob exactly as for a live pair.
 *
 * Pure: no Arduino, no Preferences. Host test:
 * firmware/canary/lib/securacv_ble_scan/test_ble_scout_registry_store.cpp.
 */

#ifndef SECURACV_BLE_SCOUT_REGISTRY_STORE_H
#define SECURACV_BLE_SCOUT_REGISTRY_STORE_H

#include "ble_scan.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace ble_scout {
namespace registry_store {

constexpr uint8_t MAGIC[3]   = {'S', 'C', 'R'};
constexpr uint8_t VERSION    = 1;
constexpr size_t  HEADER_LEN = 4;
constexpr size_t  LABEL_LEN  = ble_scan::MAX_LABEL_LEN + 1;
constexpr size_t  SLOT_LEN   = 1 + ble_scan::HASHED_ID_LEN + LABEL_LEN;
constexpr size_t  BLOB_LEN   = HEADER_LEN + ble_scan::MAX_PAIRED_BEACONS * SLOT_LEN;

/* NVS placement — the same namespace ble_scout_key.cpp uses. */
constexpr const char* NVS_NAMESPACE = "securacv";
constexpr const char* NVS_KEY       = "scout.reg";

/* Write the registry into out[cap]. Returns BLOB_LEN, or 0 when cap is too
 * small or an argument is null (nothing written that could be mistaken for
 * a blob). */
inline size_t serialize(const ble_scan::Registry* r, uint8_t* out, size_t cap) {
  if (r == nullptr || out == nullptr || cap < BLOB_LEN) return 0;
  memset(out, 0, BLOB_LEN);
  memcpy(out, MAGIC, sizeof(MAGIC));
  out[3] = VERSION;
  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    const ble_scan::PairedBeacon& s = r->slots[i];
    uint8_t* p = out + HEADER_LEN + i * SLOT_LEN;
    if (!s.in_use) continue;           /* free slot stays all-zero */
    p[0] = 1;
    memcpy(p + 1, s.hashed_id, ble_scan::HASHED_ID_LEN);
    const size_t n = strnlen(s.label, ble_scan::MAX_LABEL_LEN);
    memcpy(p + 1 + ble_scan::HASHED_ID_LEN, s.label, n);  /* rest stays NUL */
  }
  return BLOB_LEN;
}

/* Rebuild a registry from a blob. All-or-nothing (see the header comment):
 * returns false and leaves *r untouched on any malformed input. */
inline bool deserialize(ble_scan::Registry* r, const uint8_t* blob, size_t len) {
  if (r == nullptr || blob == nullptr || len != BLOB_LEN) return false;
  if (memcmp(blob, MAGIC, sizeof(MAGIC)) != 0 || blob[3] != VERSION) return false;

  /* Validate every slot before touching the output. */
  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    const uint8_t* p = blob + HEADER_LEN + i * SLOT_LEN;
    if (p[0] > 1) return false;
    if (p[0] == 1 &&
        memchr(p + 1 + ble_scan::HASHED_ID_LEN, '\0', LABEL_LEN) == nullptr) {
      return false;
    }
  }

  ble_scan::Registry tmp;
  ble_scan::registry_init(&tmp);
  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    const uint8_t* p = blob + HEADER_LEN + i * SLOT_LEN;
    if (p[0] != 1) continue;
    const char* label = (const char*)(p + 1 + ble_scan::HASHED_ID_LEN);
    if (!ble_scan::registry_add(&tmp, p + 1, label)) return false;
  }
  *r = tmp;
  return true;
}

}  /* namespace registry_store */
}  /* namespace ble_scout */

#endif  /* SECURACV_BLE_SCOUT_REGISTRY_STORE_H */
