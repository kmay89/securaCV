/*
 * SecuraCV Canary — BLE Scout (pure-logic primitives) — Implementation
 *
 * All three primitives compile with zero Bluetooth dependency. The
 * NimBLE scan loop is a separate translation unit (PR 5b) gated by
 * FEATURE_BLE_SCAN=1.
 */

#include "ble_scan.h"
#include "mesh_crypto.h"   /* sha256_domain — vendored host impl + mbedtls on device */

#include <string.h>

namespace ble_scan {

/* ──────────────────────────────────────────────────────────────────────────
 * HASHED BEACON ID
 * ────────────────────────────────────────────────────────────────────────── */

bool hash_beacon_id(const uint8_t per_device_key[PER_DEVICE_KEY_LEN],
                    const uint8_t mac[MAC_LEN],
                    uint8_t       out[HASHED_ID_LEN]) {
  if (per_device_key == nullptr || mac == nullptr || out == nullptr) return false;

  /* Build the data buffer: key || mac. The key is the secret that
   * prevents an external observer from linking a hashed_id back to
   * the original MAC (or to the same beacon's hashed_id on another
   * device with a different key). */
  uint8_t buf[PER_DEVICE_KEY_LEN + MAC_LEN];
  memcpy(buf,                       per_device_key, PER_DEVICE_KEY_LEN);
  memcpy(buf + PER_DEVICE_KEY_LEN,  mac,            MAC_LEN);

  uint8_t hash[mesh_crypto::SHA256_OUT_LEN];
  mesh_crypto::sha256_domain(DOMAIN_HASHED_ID, buf, sizeof(buf), hash);

  memcpy(out, hash, HASHED_ID_LEN);

  /* Wipe the scratch buffer; key bytes lived briefly in it. */
  volatile uint8_t* p = (volatile uint8_t*)buf;
  for (size_t i = 0; i < sizeof(buf); ++i) p[i] = 0;
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" ::: "memory");
#endif
  return true;
}

bool hashed_id_equal(const uint8_t a[HASHED_ID_LEN],
                     const uint8_t b[HASHED_ID_LEN]) {
  if (a == nullptr || b == nullptr) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < HASHED_ID_LEN; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * KALMAN RSSI SMOOTHER
 * ────────────────────────────────────────────────────────────────────────── */

void kalman_reset(KalmanRssi* k, float q, float r) {
  if (k == nullptr) return;
  k->x      = 0.0f;
  k->p      = 1.0f;
  /* Indoor-BLE defaults: drift ~0.5 dB/s, single-sample noise ~±2 dB. */
  k->q      = (q > 0.0f) ? q : 0.5f;
  k->r      = (r > 0.0f) ? r : 4.0f;
  k->primed = false;
}

float kalman_update(KalmanRssi* k, float raw_rssi) {
  if (k == nullptr) return raw_rssi;
  if (!k->primed) {
    /* First sample: skip the warm-up, take it as-is. The filter
     * converges fast enough on subsequent samples that we don't need
     * a multi-sample preamble. */
    k->x      = raw_rssi;
    k->p      = k->r;          /* start variance at measurement noise */
    k->primed = true;
    return k->x;
  }

  /* Predict step. */
  k->p = k->p + k->q;

  /* Update step. */
  const float kgain = k->p / (k->p + k->r);
  k->x = k->x + kgain * (raw_rssi - k->x);
  k->p = (1.0f - kgain) * k->p;
  return k->x;
}

/* ──────────────────────────────────────────────────────────────────────────
 * REGISTRY
 * ────────────────────────────────────────────────────────────────────────── */

void registry_init(Registry* r) {
  if (r == nullptr) return;
  memset(r, 0, sizeof(*r));
}

static PairedBeacon* registry_find_mut(Registry* r,
                                       const uint8_t id[HASHED_ID_LEN]) {
  if (r == nullptr || id == nullptr) return nullptr;
  for (size_t i = 0; i < MAX_PAIRED_BEACONS; ++i) {
    if (r->slots[i].in_use && hashed_id_equal(r->slots[i].hashed_id, id)) {
      return &r->slots[i];
    }
  }
  return nullptr;
}

/* Copy `src` into `dst[cap]`, replacing any non-printable-ASCII byte
 * with '?' and truncating to cap-1. dst is always NUL-terminated.
 *
 * The label is a single boundary input — once we paired with it, it
 * flows to the csi_event note field AND the PR 5c broadcast callback
 * AND eventually any mesh wire payload. The csi_event chokepoint
 * sanitizes strings at emit time (test_strings_are_sanitized), but
 * the broadcast callback bypasses that path. Sanitizing here, once,
 * means EVERY consumer sees the same printable-only string and the
 * mesh wire format can't carry control bytes smuggled through the
 * pairing UI. Mirrors the chokepoint's "printable ASCII only" rule
 * (codepoints 0x20..0x7E inclusive). */
static void sanitize_label(char* dst, size_t cap, const char* src) {
  if (cap == 0) return;
  size_t j = 0;
  for (size_t i = 0; src != nullptr && src[i] != '\0' && j + 1 < cap; ++i) {
    const unsigned char c = (unsigned char)src[i];
    dst[j++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
  }
  dst[j] = '\0';
}

bool registry_add(Registry* r,
                  const uint8_t hashed_id[HASHED_ID_LEN],
                  const char*   label) {
  if (r == nullptr || hashed_id == nullptr) return false;

  /* If already present, update the label and return true (idempotent
   * pairing — useful when the user re-names a beacon). */
  PairedBeacon* existing = registry_find_mut(r, hashed_id);
  if (existing != nullptr) {
    if (label != nullptr) {
      sanitize_label(existing->label, sizeof(existing->label), label);
    }
    return true;
  }

  /* Find a free slot. */
  for (size_t i = 0; i < MAX_PAIRED_BEACONS; ++i) {
    if (!r->slots[i].in_use) {
      memcpy(r->slots[i].hashed_id, hashed_id, HASHED_ID_LEN);
      if (label != nullptr) {
        sanitize_label(r->slots[i].label, sizeof(r->slots[i].label), label);
      } else {
        r->slots[i].label[0] = '\0';
      }
      r->slots[i].in_use = true;
      return true;
    }
  }
  return false;   /* table full */
}

const PairedBeacon* registry_find(const Registry* r,
                                  const uint8_t hashed_id[HASHED_ID_LEN]) {
  if (r == nullptr || hashed_id == nullptr) return nullptr;
  for (size_t i = 0; i < MAX_PAIRED_BEACONS; ++i) {
    if (r->slots[i].in_use && hashed_id_equal(r->slots[i].hashed_id, hashed_id)) {
      return &r->slots[i];
    }
  }
  return nullptr;
}

bool registry_remove(Registry* r,
                     const uint8_t hashed_id[HASHED_ID_LEN]) {
  PairedBeacon* slot = registry_find_mut(r, hashed_id);
  if (slot == nullptr) return false;
  memset(slot, 0, sizeof(*slot));
  return true;
}

size_t registry_count(const Registry* r) {
  if (r == nullptr) return 0;
  size_t n = 0;
  for (size_t i = 0; i < MAX_PAIRED_BEACONS; ++i) {
    if (r->slots[i].in_use) ++n;
  }
  return n;
}

}  /* namespace ble_scan */
