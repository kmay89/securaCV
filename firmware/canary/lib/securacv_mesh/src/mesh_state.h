/*
 * SecuraCV Canary — Mesh-layer NVS persistence
 * Version 0.1.0
 *
 * Owns the durable bits the mesh layer needs across reboots:
 *
 *   • opera_secret  — the 32-byte household-shared secret distributed by
 *                     pairing (mesh_pairing). Without it the device can't
 *                     derive opera_id, sign outbound BEACON_EVENT frames,
 *                     or filter inbound frames by opera. Persisting it
 *                     means subsequent boots resume the role without
 *                     re-pairing every time.
 *
 *   • (PR follow-up) trusted-peer pubkeys + per-peer last_counter —
 *                     replay defense across reboots. Not in this PR.
 *
 * NVS layout (mirrors the existing canary "securacv" namespace used by
 * ble_scout_key, device_id, mic_muted, etc.):
 *
 *     namespace = "securacv"
 *     keys:
 *       opera_secret   — 32-byte blob
 *
 * Host build (CSI_TEST_HOST_BUILD): all functions compile as
 * deterministic stubs. load_opera_secret() always returns false (no
 * persisted secret), save_/clear_opera_secret() are no-op success.
 * Tests that need a populated secret can set it via the in-memory
 * mesh_session::set_opera_secret() API instead.
 *
 * Threading: every function opens its own short-lived Preferences
 * handle inside the call. Safe to call from main-loop context only —
 * NOT from interrupt or BLE/WiFi task callbacks. The integration
 * layer is the only legitimate caller.
 *
 * Privacy: the opera_secret is the household's single most sensitive
 * mesh-layer key. Production deployments should be paired with the
 * flash-encryption fuse blown so the NVS partition is encrypted on
 * disk (audit-O2 deferred work, same gate ble_scout_key sits behind).
 * This module does NOT enforce flash-encryption — it just trusts
 * NVS to be backed by encrypted storage when the gate is set.
 */

#ifndef SECURACV_MESH_STATE_H
#define SECURACV_MESH_STATE_H

#include "mesh_crypto.h"   /* OPERA_SECRET_LEN */

#include <stdint.h>
#include <stdbool.h>

namespace mesh_state {

/* Persist the 32-byte opera_secret to NVS. Overwrites any existing
 * value (re-pairing replaces the household secret cleanly).
 *
 * Returns false on:
 *   • null pointer
 *   • flash encryption disabled on this device (AGENTS.md project
 *     invariant — refuse to persist secrets on FE-off hardware)
 *   • NVS write failure (corrupt partition / hardware fault)
 *
 * On the host build, always returns true (no-op success — tests use
 * mesh_session's in-memory set_opera_secret() API instead).
 *
 * The caller is responsible for wiping its OWN copy of `secret`
 * after this call — mesh_state does not retain a copy in module RAM. */
bool save_opera_secret(const uint8_t secret[mesh_crypto::OPERA_SECRET_LEN]);

/* Load the persisted opera_secret into `out`. Returns:
 *   true   — secret was present and copied into out[].
 *   false  — any of:
 *              * out is null
 *              * flash encryption disabled (refuse to load too —
 *                matches canary-wap's symmetric load+save FE gate)
 *              * no secret persisted (first boot, factory-reset state)
 *              * NVS read failure
 *
 * On failure the contents of out[] are NOT modified — callers can
 * poison-fill their buffer beforehand to detect spurious writes.
 *
 * On the host build, ALWAYS returns false (no persistence stub).
 * Production code should treat false as "this device hasn't paired
 * yet" and skip mesh_session::set_opera_secret(). */
bool load_opera_secret(uint8_t out[mesh_crypto::OPERA_SECRET_LEN]);

/* Erase the persisted opera_secret. Use on factory reset or
 * "un-pair from this opera" UI.
 *
 * Returns true when:
 *   • the key was removed successfully, OR
 *   • the key was already absent (idempotent — factory-reset semantics)
 *
 * Returns false when the NVS partition can't be opened OR a real
 * remove failed (key still present after remove returned false).
 *
 * On the host build, always returns true (no-op success). */
bool clear_opera_secret();

}  /* namespace mesh_state */

#endif  /* SECURACV_MESH_STATE_H */
