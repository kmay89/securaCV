/**
 * @file ble_events_module.h
 * @brief ble.events — chokepoint-routed manifest for BLE Discovery
 *        semantic events (spec/event_contract.md §10).
 *
 * The spec's §10 declares eight semantic event types the BLE
 * subsystem (Opera / Chirp / Nearby) emits as new witness-chain
 * records. Without this module, those events would either bypass the
 * chokepoint entirely (privacy-contract hole) or simply not exist.
 *
 * Every emit goes through csi_event_emit("ble.events", "<type>", ...);
 * the chokepoint then strips fields outside the per-event allow-list,
 * coarsens timestamps to the 10-minute bucket, and persists through
 * the same witness-chain bridge the CSI sensing modules already use.
 *
 * Privacy constraints (spec §10):
 *   - NO MAC addresses or stable hardware identifiers.
 *   - Device identification uses the truncated Ed25519 pubkey hash
 *     (8 bytes hex == 16 ASCII chars) only, carried in `note`.
 *   - Non-Canary device counts are aggregate-only.
 *   - RSSI MAY be approximated for proximity context but MUST NOT be
 *     stored at tracking precision; this module's allow-list excludes
 *     RSSI entirely — proximity context lives on the live stream, not
 *     the witness chain.
 *
 * Helpers below accept the minimum information per event so callers
 * can't construct a values struct that smuggles disallowed fields.
 * If a caller needs a field this header doesn't expose, that's a
 * privacy-contract conversation, not a header tweak.
 */

#ifndef SECURACV_CSI_MODULE_BLE_EVENTS_H
#define SECURACV_CSI_MODULE_BLE_EVENTS_H

#include "csi_module.h"

#ifdef __cplusplus
extern "C" {
#endif

const csi_module_t* ble_events_module(void);

/* Emit ble_initialized — BLE subsystem started successfully. */
void ble_events_emit_initialized(void);

/* Emit ble_init_failed — BLE failed to start. `reason` is a short
 * stable tag (e.g. "no_antenna", "hw_fault"); free-text rationales
 * belong in the host log, not the witness chain. */
void ble_events_emit_init_failed(const char* reason);

/* Emit ble_client_connected — A BLE client connected to Opera server.
 * `peer_hash_hex` is the truncated Ed25519 pubkey hash hex (≤16 chars)
 * or nullptr if the peer's identity hasn't been authenticated yet. */
void ble_events_emit_client_connected(const char* peer_hash_hex);

/* Emit ble_client_disconnected — A BLE client disconnected. */
void ble_events_emit_client_disconnected(const char* peer_hash_hex);

/* Emit chirp_sent — A chirp broadcast was sent. `chirp_type` is a
 * short stable tag (e.g. "boot", "alarm", "presence", "ack"). */
void ble_events_emit_chirp_sent(const char* chirp_type);

/* Emit chirp_received — A chirp was received from another Canary.
 * `chirp_type` and `peer_hash_hex` follow the conventions above. */
void ble_events_emit_chirp_received(const char* chirp_type,
                                    const char* peer_hash_hex);

/* Emit canary_discovered — A new Canary appeared in scan. */
void ble_events_emit_canary_discovered(const char* peer_hash_hex);

/* Emit canary_lost — A previously visible Canary dropped off. */
void ble_events_emit_canary_lost(const char* peer_hash_hex);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_BLE_EVENTS_H */
