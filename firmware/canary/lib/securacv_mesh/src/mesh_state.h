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
 *   • trusted_peers — trusted-peer pubkeys (save_/load_trusted_peers,
 *                     below), so a reboot does not forget who is household.
 *
 *   • replay_ctrs   — per-peer last_counter, the replay defense across
 *                     reboots (audit O1: the counter is the freshness
 *                     mechanism).
 *
 *   • elected_hub   — the last hub election result, so the fleet resumes
 *                     its role split without a fresh election.
 *
 * NVS layout (mirrors the existing canary "securacv" namespace used by
 * ble_scout_key, device_id, mic_muted, etc.):
 *
 *     namespace = "securacv"
 *     keys:
 *       opera_secret   — 32-byte blob
 *       trusted_peers  — blob (peer table)
 *       replay_ctrs    — blob (per-peer counters)
 *       elected_hub    — blob (election result)
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
 * mesh-layer key — it exposes OTHER devices, not just this one. This
 * module therefore ENFORCES the flash-encryption gate (audit O2): every
 * save_*/load_* below returns false when esp_flash_encryption_enabled()
 * is false, so nothing household-shared is persisted on silicon without
 * flash encryption, and firmware/scripts/regression_check.sh ("Mesh
 * secret persistence is FE-gated") asserts the check stays in this file
 * and mesh_network.cpp.
 *
 * That is ALL the gate buys. Flash encryption does not cover NVS: ESP-IDF
 * encrypts only the app, otadata and nvs_keys partitions, and this tree's
 * tables (partitions_ota.csv, the core's default_8MB.csv) leave nvs
 * unflagged, so on a board WITH flash encryption these four entries still
 * sit in plaintext on the flash chip. They would be ciphertext only under
 * NVS encryption, which is not available under framework = arduino (the
 * Arduino core's sdkconfig does not compile it in; roadmap item 9 is the
 * route). So today the gate keeps the secret off un-fused boards; it does
 * not make it confidential at rest on fused ones.
 *
 * The device's OWN identity key is deliberately NOT gated this way at the
 * default tier: Tier 0 of docs/design/hardware_root_of_trust.md (§5.1,
 * decisions §8 #1/#3/#4) keeps it in NVS, reports the posture as
 * `key_at_rest` (plaintext-nvs on every board in this tree, for the reason
 * above), and refuses only in images built with
 * SECURACV_REQUIRE_FLASH_ENCRYPTION=1 — the decision is written down once
 * in common/identity/key_at_rest.h. (ble_scout_key's per-device scout key
 * and the API token are Tier-0 by the same reasoning and carry no gate.)
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
 *   • flash encryption disabled on this device (audit O2 — refuse to
 *     persist household secrets on FE-off hardware; on FE-on hardware the
 *     NVS entry is still plaintext, see the file comment above)
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

/* ──────────────────────────────────────────────────────────────────────────
 * TRUSTED-PEER PUBKEY PERSISTENCE
 *
 * The receive side of mesh_session needs to know the Ed25519 pubkey of
 * every paired peer to verify inbound BEACON_EVENT envelopes. Without
 * persistence, every boot starts with an empty peer table and RX is
 * effectively dead until the user manually re-pairs every device.
 *
 * On-disk layout: a single 256-byte NVS blob "trusted_peers" holding
 * up to MAX_TRUSTED_PEERS × 32 = 256 concatenated pubkey bytes. The
 * stored length tells us how many peers are populated.
 *
 * Same flash-encryption gate as opera_secret: the pubkeys themselves
 * aren't secret, but the bond between this device and its set of
 * trusted peers IS sensitive metadata (an attacker who reads the
 * unencrypted partition learns the household graph). canary-wap's
 * opera_config persistence applies the same posture.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t MAX_TRUSTED_PEERS = 8;   /* mirrors mesh_session value */

/* Add `pubkey` to the persisted trusted-peer list. Dedups: if an
 * identical pubkey is already stored, returns true without
 * re-writing (idempotent). Returns false on:
 *   • null pointer
 *   • flash encryption disabled
 *   • table already full (count == MAX_TRUSTED_PEERS, pubkey new)
 *   • NVS read/write failure
 *
 * On the host build, always returns true (no-op success). */
bool save_trusted_peer(const uint8_t pubkey[mesh_crypto::PUBKEY_LEN]);

/* Load up to MAX_TRUSTED_PEERS pubkeys into `out_pubkeys`. The buffer
 * must be at least `MAX_TRUSTED_PEERS * PUBKEY_LEN` (= 256) bytes.
 * On success, `*out_count` is set to the actual number of peers
 * (0..MAX_TRUSTED_PEERS) and the corresponding contiguous bytes in
 * out_pubkeys are populated.
 *
 * Returns false on null pointer, FE disabled, NVS read failure, or
 * buffer too small. On "no peers persisted" (first boot / factory
 * reset), returns true with *out_count = 0.
 *
 * On the host build, always returns true with *out_count = 0
 * (deterministic empty list for tests). */
bool load_trusted_peers(uint8_t* out_pubkeys,
                        size_t   out_buf_cap,
                        size_t*  out_count);

/* Erase the persisted trusted-peer list. Used on factory reset and
 * "un-pair all" UI. Idempotent — succeeds if the list was empty.
 *
 * On the host build, always returns true (no-op success). */
bool clear_trusted_peers();

/* ──────────────────────────────────────────────────────────────────────────
 * REPLAY COUNTERS — per-peer last_counter persistence
 *
 * Stores the per-peer (fingerprint[8], last_counter[8]) table to NVS
 * so replay defense survives reboots. Without persistence, a reboot
 * resets all counters to 0, opening a narrow replay window for any
 * frames recorded before the reboot whose counter > 0.
 *
 * Flash-encryption gated (same as opera_secret + trusted_peers).
 * ────────────────────────────────────────────────────────────────────────── */

struct ReplayEntry {
  uint8_t  fingerprint[mesh_crypto::FINGERPRINT_LEN];
  uint64_t last_counter;
};

constexpr size_t MAX_REPLAY_ENTRIES = 8;

bool save_replay_counters(const ReplayEntry* entries, size_t count);

bool load_replay_counters(ReplayEntry* out_entries,
                          size_t       out_cap,
                          size_t*      out_count);

bool clear_replay_counters();

/* ──────────────────────────────────────────────────────────────────────────
 * ELECTED-HUB FINGERPRINT — failover continuity across reboots
 *
 * Hub failover (mesh_hub_election) deterministically elects the live
 * peer with the lowest 8-byte fingerprint when the Hub's heartbeat is
 * absent. The election itself is stateless — every node recomputes the
 * same winner — but the *result* is worth persisting: after a reboot a
 * node that re-joins the mesh already knows who the current coordinator
 * is, so it neither re-broadcasts a redundant election for a Hub that
 * is still up nor sits through a cold 60 s absence window before it has
 * any notion of the coordinator. The stored value is the elected Hub's
 * 8-byte fingerprint (mesh_crypto::FINGERPRINT_LEN).
 *
 * The fingerprint is not itself a secret — it is derivable from the
 * trusted-peer set — but it is persisted under the same flash-encryption
 * gate as the rest of this namespace for posture consistency: the
 * coordinator identity is household-graph metadata of the same class as
 * the trusted-peer list it is drawn from.
 * ────────────────────────────────────────────────────────────────────────── */

/* Persist the elected Hub's fingerprint. Overwrites any prior value.
 * Returns false on null pointer, flash encryption disabled, or NVS
 * write failure. On the host build, always returns true (no-op). */
bool save_elected_hub(const uint8_t fingerprint[mesh_crypto::FINGERPRINT_LEN]);

/* Load the persisted elected-Hub fingerprint into `out`. Returns:
 *   true  — a fingerprint was present and copied into out[].
 *   false — null pointer, FE disabled, nothing persisted (first boot /
 *           factory reset), or NVS read failure.
 * On failure out[] is left untouched. On the host build, always
 * returns false (no persistence stub). */
bool load_elected_hub(uint8_t out[mesh_crypto::FINGERPRINT_LEN]);

/* Erase the persisted elected-Hub fingerprint. Idempotent — succeeds
 * if already absent. On the host build, always returns true. */
bool clear_elected_hub();

}  /* namespace mesh_state */

#endif  /* SECURACV_MESH_STATE_H */
