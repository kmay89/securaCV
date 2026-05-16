/*
 * SecuraCV Canary — BLE Scout (pure-logic primitives)
 * Version 0.1.0
 *
 * PR 5a of the BLE Scout from docs/esp32_mesh_sensing_design.md. Lands
 * only the deterministic, side-effect-free pieces of the Scout role —
 * NO NimBLE dependency, no actual BLE scanning. The full Scout (passive
 * scan loop, mesh-side beacon events, FEATURE_BLE_SCAN flag, NimBLE
 * lib_deps) lands in PR 5b once the primitives below have been reviewed
 * in isolation.
 *
 * Three primitives:
 *
 *   1. ble_scan::hash_beacon_id() — turns a BLE MAC into a 16-byte
 *      hashed beacon ID using a per-device key. The raw MAC NEVER
 *      leaves the device; hashed IDs are what go into events, the
 *      paired-beacon registry, and any mesh broadcast.
 *
 *   2. ble_scan::KalmanRssi — 1-D Kalman smoother over noisy RSSI.
 *      Converts the heavy-jittered raw RSSI stream into a stable
 *      estimate, so a phone briefly walking through doorway noise
 *      doesn't flap "room A / room B / room A" every second.
 *
 *   3. ble_scan::Registry — bounded paired-beacon table. Each slot
 *      stores a hashed_id + a short user-supplied label (no MACs).
 *      In-RAM for v1; NVS persistence is integration-layer work.
 *
 * Privacy posture (matches design doc §"BLE Scout"):
 *   • Passive scan only — never advertises (PR 5b enforces).
 *   • Tracks user-paired beacons by hashed MAC (HMAC-style with a
 *     per-device key, never the raw MAC).
 *   • Events rate-limited and bucketed at the event chokepoint.
 *   • Kalman-filtered RSSI → coarse "room=this Scout's room" decision.
 *     No trilateration (multipath makes the math unreliable in homes).
 *
 * This library compiles with zero Bluetooth deps. Host build + device
 * build both build the same code; PR 5b adds the NimBLE scan loop
 * behind FEATURE_BLE_SCAN=1 in a separate translation unit.
 */

#ifndef SECURACV_BLE_SCAN_H
#define SECURACV_BLE_SCAN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace ble_scan {

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t MAC_LEN              = 6;   /* BLE address */
constexpr size_t HASHED_ID_LEN        = 16;  /* truncated SHA-256 over key||mac */
constexpr size_t PER_DEVICE_KEY_LEN   = 32;  /* per-device secret */
constexpr size_t MAX_LABEL_LEN        = 23;  /* user-friendly name; +1 for NUL */

/* Domain string for the hashed-id derivation. Public — security comes
 * from the per-device key, not domain secrecy. */
constexpr const char* DOMAIN_HASHED_ID = "securacv:ble_scan:hashed_id:v1";

/* Max paired beacons. Small intentionally — Scout role is for a
 * household's phones/watches/tags, not a building. */
constexpr size_t MAX_PAIRED_BEACONS = 16;

/* ──────────────────────────────────────────────────────────────────────────
 * HASHED BEACON ID
 *
 * SHA-256("securacv:ble_scan:hashed_id:v1" || per_device_key || mac),
 * truncated to 16 bytes. Wire-format property: a hashed_id from this
 * device cannot be linked to the same beacon's hashed_id on a
 * different device (because each device has its own per_device_key).
 * This is intentional — Scouts share AGGREGATED room-attribution
 * events, never raw beacon identifiers.
 *
 * `per_device_key` is generated once on first boot via
 * esp_fill_random() in PR 5b's integration and persisted to NVS
 * (under flash-encryption gate, mirroring opera_secret's protection).
 * This module does NOT generate or store the key — caller-supplied.
 *
 * Returns false on null pointer inputs; otherwise fills `out` and
 * returns true.
 * ────────────────────────────────────────────────────────────────────────── */

bool hash_beacon_id(const uint8_t per_device_key[PER_DEVICE_KEY_LEN],
                    const uint8_t mac[MAC_LEN],
                    uint8_t       out[HASHED_ID_LEN]);

/* Constant-time equality for hashed IDs. Returns true if the 16-byte
 * IDs match. */
bool hashed_id_equal(const uint8_t a[HASHED_ID_LEN],
                     const uint8_t b[HASHED_ID_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * KALMAN RSSI SMOOTHER (1-D)
 *
 * Single-state Kalman filter over a noisy RSSI sample stream. State:
 * the current best-estimate RSSI. The process noise q is the per-tick
 * drift we expect; measurement noise r is how much we trust each raw
 * sample.
 *
 * Defaults are tuned for indoor BLE-tag tracking: q=0.5 (RSSI drifts
 * slowly over seconds), r=4.0 (raw samples have ~±2 dB single-sample
 * noise). Callers may override via reset().
 *
 * In float — these are tiny per-call costs and float on ESP32-S3 has
 * an FPU. Allocation-free; one KalmanRssi instance per beacon.
 * ────────────────────────────────────────────────────────────────────────── */

struct KalmanRssi {
  float x;          /* state estimate (current smoothed RSSI) */
  float p;          /* state variance */
  float q;          /* process noise */
  float r;          /* measurement noise */
  bool  primed;     /* false until first sample seen */
};

/* Initialize / re-initialize the filter. q and r are the noise
 * parameters; pass 0 for either to use the indoor-BLE defaults. */
void kalman_reset(KalmanRssi* k, float q, float r);

/* Feed one raw RSSI sample. Returns the smoothed estimate. The first
 * call (primed=false) accepts the sample as-is so the filter doesn't
 * need a long convergence warm-up on a freshly-seen beacon. */
float kalman_update(KalmanRssi* k, float raw_rssi);

/* ──────────────────────────────────────────────────────────────────────────
 * PAIRED-BEACON REGISTRY
 *
 * Bounded fixed-size table of beacons the user has paired through the
 * Scout setup flow. Each slot: 16-byte hashed_id + a short text label
 * (no MAC, no raw identifiers). Lookup is by hashed_id only.
 *
 * In-RAM. PR 5b adds the NVS-persisted twin so paired beacons survive
 * reboots (under the same flash-encryption gate as opera_secret).
 *
 * The label is fixed-length, ASCII only (the chokepoint enforces).
 * It is NEVER user-typed free text from a remote source; it comes
 * from the local setup UI and is sanitized there.
 * ────────────────────────────────────────────────────────────────────────── */

struct PairedBeacon {
  uint8_t  hashed_id[HASHED_ID_LEN];
  char     label[MAX_LABEL_LEN + 1];
  bool     in_use;
};

struct Registry {
  PairedBeacon slots[MAX_PAIRED_BEACONS];
};

/* Zero-initialize. */
void registry_init(Registry* r);

/* Register a paired beacon. Returns:
 *   true   — added (or already present; label updated)
 *   false  — null pointer OR table full AND beacon not already present
 *
 * `label` is truncated to MAX_LABEL_LEN; the slot's label is always
 * NUL-terminated. */
bool registry_add(Registry* r,
                  const uint8_t hashed_id[HASHED_ID_LEN],
                  const char*   label);

/* Look up by hashed_id. Returns a pointer to the slot (do not free)
 * or nullptr if absent. */
const PairedBeacon* registry_find(const Registry* r,
                                  const uint8_t hashed_id[HASHED_ID_LEN]);

/* Remove a paired beacon. Returns true if it was present. */
bool registry_remove(Registry* r,
                     const uint8_t hashed_id[HASHED_ID_LEN]);

/* Current paired-beacon count. */
size_t registry_count(const Registry* r);

}  /* namespace ble_scan */

#endif  /* SECURACV_BLE_SCAN_H */
