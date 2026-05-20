/*
 * SecuraCV Canary — BLE Scout per-device key store
 * Version 0.1.0
 *
 * PR 5b. Owns the 32-byte per-device secret that hash_beacon_id()
 * uses. The key is generated once on first boot via esp_fill_random()
 * and persisted to NVS under the existing "securacv" namespace
 * (mirroring opera_secret's hygiene — flash-encryption gate is the
 * deployment-layer responsibility).
 *
 * Design choices:
 *   • The key is module-internal — callers never see raw key bytes;
 *     they call ble_scout_key_hash_beacon() which wraps
 *     ble_scan::hash_beacon_id() with the loaded key.
 *   • Host build (CSI_TEST_HOST_BUILD) returns a deterministic test
 *     key; we don't link Preferences host-side and we don't want
 *     real-key dependence in unit tests anyway.
 *   • Key never appears in events or logs. Wiped from working RAM
 *     when ble_scout_key_deinit() is called.
 */

#ifndef SECURACV_BLE_SCOUT_KEY_H
#define SECURACV_BLE_SCOUT_KEY_H

#include "ble_scan.h"   /* PER_DEVICE_KEY_LEN, MAC_LEN, HASHED_ID_LEN */

#include <stdint.h>
#include <stdbool.h>

namespace ble_scout {

/* Load the per-device key from NVS. If absent (first boot), generate
 * a fresh one via esp_fill_random() and persist it. Returns true on
 * success; false only if NVS read+create both fail (catastrophic).
 * Idempotent — calling twice is a no-op after the first success. */
bool ble_scout_key_init();

/* True if a key has been loaded into module RAM. */
bool ble_scout_key_loaded();

/* Hash a 6-byte MAC into a 16-byte beacon id using the loaded key.
 * Returns false if the key isn't loaded yet. The caller never sees
 * the key. */
bool ble_scout_key_hash_beacon(const uint8_t mac[ble_scan::MAC_LEN],
                               uint8_t       out[ble_scan::HASHED_ID_LEN]);

/* Wipe the in-RAM key and forget the loaded state. Persisted NVS
 * copy is NOT erased (use ble_scout_key_factory_reset() for that). */
void ble_scout_key_deinit();

/* Erase the persisted key. Next ble_scout_key_init() will generate
 * a new one. Use for "unpair all Scouts" / factory reset. Returns
 * false if NVS write fails. Host build is a no-op + returns true. */
bool ble_scout_key_factory_reset();

}  /* namespace ble_scout */

#endif /* SECURACV_BLE_SCOUT_KEY_H */
