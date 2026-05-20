/*
 * SecuraCV Canary — BLE Scout integration module
 * Version 0.1.0
 *
 * PR 5b. Wires the PR 5a primitives into a working Scout role:
 *
 *   • Per-device key (NVS-persisted) — see ble_scout_key.h
 *   • Paired-beacon registry — see ble_scan.h (Registry)
 *   • Per-beacon presence state machine — see ble_scout_state.h
 *   • Privacy-chokepoint event emission — via csi_event::emit()
 *   • NimBLE passive scan loop — see ble_scout_nimble.cpp (device only)
 *
 * The Scout NEVER advertises. It only listens. The raw beacon MAC is
 * hashed inside ble_scout_on_advert() and the raw bytes never leave
 * that function — the registry, the presence tracker, and every event
 * see only the 16-byte hashed_id.
 *
 * Public API (FEATURE_BLE_SCAN=1 device build):
 *   ble_scout_init()          — load key, init registry+tracker, start scan
 *   ble_scout_pair(mac, label)— hash MAC, add to registry
 *   ble_scout_unpair(...)     — remove from registry, forget presence
 *   ble_scout_count()         — number of paired beacons
 *   ble_scout_tick()          — drive LOST_MS detection; called from main loop
 *   ble_scout_module()        — csi_module_t* for the registry runtime
 *
 * Internal entry point (called by the NimBLE scan TU):
 *   ble_scout_on_advert(mac, rssi, now_ms)
 *     Drives the presence state machine + emits arrived/departed
 *     events through the privacy chokepoint.
 */

#ifndef SECURACV_BLE_SCOUT_H
#define SECURACV_BLE_SCOUT_H

#include "ble_scan.h"

#include <stdint.h>
#include <stdbool.h>

/* Forward-declare without dragging in csi_module.h's heavy include
 * tree for callers that just want the public Scout API. */
struct csi_module;

namespace ble_scout {

/* Initialize the Scout: load the per-device key, init the in-RAM
 * registry+tracker, and (on device) start the NimBLE passive scan
 * loop. Returns false if the key store fails. Safe to call twice;
 * second call is a no-op. */
bool ble_scout_init();

/* Pair a beacon. Hashes the raw MAC inside this function — the caller
 * never persists or logs the MAC. Returns true on success (added or
 * idempotent re-pair); false if the registry is full or the key
 * isn't loaded. */
bool ble_scout_pair(const uint8_t mac[ble_scan::MAC_LEN],
                    const char*   label);

/* Forget a paired beacon by hashed_id. Idempotent. */
bool ble_scout_unpair(const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]);

/* Number of currently paired beacons. */
size_t ble_scout_count();

/* Drive the presence-timer side of the state machine. Should be
 * called from the same task as the CSI module dispatcher (the
 * existing main-loop tick path is fine). Idempotent and safe to
 * call when no beacons are paired. */
void ble_scout_tick(uint32_t now_ms);

/* Entry point for the NimBLE scan callback. Hashes the MAC and
 * routes through the presence state machine. Emits arrived/
 * departed events when transitions fire. Idempotent for
 * unpaired-beacon adverts (cheap registry lookup, then drop). */
void ble_scout_on_advert(const uint8_t mac[ble_scan::MAC_LEN],
                         int8_t        rssi_dbm,
                         uint32_t      now_ms);

/* csi_module manifest. Registered by csi_modules_integration.cpp when
 * FEATURE_BLE_SCAN=1. The module's tick() bridges to ble_scout_tick()
 * using the CSI feature-window timestamp. */
const ::csi_module* ble_scout_module();

}  /* namespace ble_scout */

#endif /* SECURACV_BLE_SCOUT_H */
