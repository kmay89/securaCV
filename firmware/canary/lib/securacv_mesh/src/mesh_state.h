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
 *   • peer_macs     — each trusted peer's radio MAC (F33 part 1), so a
 *                     reboot can put the peers back into mesh_transport's
 *                     table; without it the transport drops their frames.
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
 *       peer_macs      — blob (fingerprint → radio MAC; F33, FE-gated)
 *       opera_name     — string, <= 32 bytes (F10; flash-encryption gated
 *                        like the four above)
 *       mesh_enabled   — 1 byte (F10; NOT gated: a preference, not a
 *                        secret — see MESH ENABLED FLAG below)
 *       mesh_out_ctr   — u64 (F33; NOT gated: the outbound counter's
 *                        reserve-ahead high-water mark)
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
 * save_ and load_ function below returns false when
 * esp_flash_encryption_enabled()
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
 * "un-pair all" UI. Idempotent — succeeds if the list was empty. Also
 * erases "peer_macs" (below), best effort: an address with no peer is
 * meaningless, and the return value reports the pubkey list only.
 *
 * On the host build, always returns true (no-op success). */
bool clear_trusted_peers();

/* ──────────────────────────────────────────────────────────────────────────
 * TRUSTED-PEER RADIO ADDRESSES  (F33 part 1)
 *
 * mesh_transport accepts frames only from MACs in its peer table, and a
 * pubkey says nothing about the address a peer transmits from, so the
 * integration layer stores each trusted peer's radio MAC — learned when the
 * pairing completes (mesh_session::get_paired_peer_mac) — and re-binds it
 * at boot (mesh_session::bind_peer_mac). NVS key "peer_macs": up to
 * MAX_TRUSTED_PEERS entries of fingerprint (8 B) || MAC (6 B), 14 B each,
 * one entry per fingerprint. FE-gated like "trusted_peers": which radios a
 * device trusts is household-graph metadata of the same class.
 *
 *   save_peer_mac()   — insert, or replace that fingerprint's address.
 *                       False on null, FE off, a full table with a new
 *                       fingerprint, or an NVS failure.
 *   load_peer_macs()  — every stored entry; true with count 0 when none.
 *                       False on null, cap < MAX_TRUSTED_PEERS, FE off, a
 *                       read failure or a malformed blob.
 *   remove_peer_mac() — drop one fingerprint's entry; true when it is gone
 *                       afterwards (idempotent). remove_trusted_peer() calls
 *                       it for the removed pubkey's fingerprint.
 *   clear_peer_macs() — erase the key; idempotent.
 * Host build: save/remove/clear → true, load → true with count 0.
 *
 * The blob edits are the pure peer_mac_blob:: helpers, host-tested
 * (test_mesh_state).
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t PEER_MAC_LEN        = 6;
constexpr size_t PEER_MAC_ENTRY_LEN  = mesh_crypto::FINGERPRINT_LEN + PEER_MAC_LEN;   /* 14 */
constexpr size_t PEER_MACS_BLOB_MAX  = MAX_TRUSTED_PEERS * PEER_MAC_ENTRY_LEN;        /* 112 */

struct PeerMac {
  uint8_t fingerprint[mesh_crypto::FINGERPRINT_LEN];
  uint8_t mac[PEER_MAC_LEN];
};

bool save_peer_mac(const uint8_t fingerprint[mesh_crypto::FINGERPRINT_LEN],
                   const uint8_t mac[PEER_MAC_LEN]);
bool load_peer_macs(PeerMac* out, size_t out_cap, size_t* out_count);
bool remove_peer_mac(const uint8_t fingerprint[mesh_crypto::FINGERPRINT_LEN]);
bool clear_peer_macs();

namespace peer_mac_blob {
/* A stored blob is well formed iff its length is a whole number of
 * entries, at most PEER_MACS_BLOB_MAX. */
bool valid_len(size_t len);
/* Insert fingerprint → mac, or replace that fingerprint's MAC, in
 * blob[0 .. *len) (capacity PEER_MACS_BLOB_MAX); *len is updated. False —
 * blob untouched — on a malformed *len or a full blob with a new
 * fingerprint. */
bool upsert(uint8_t* blob, size_t* len,
            const uint8_t fingerprint[mesh_crypto::FINGERPRINT_LEN],
            const uint8_t mac[PEER_MAC_LEN]);
/* Drop that fingerprint's entry, keeping the others in order. Returns true
 * when an entry was dropped; *len is updated. */
bool remove(uint8_t* blob, size_t* len,
            const uint8_t fingerprint[mesh_crypto::FINGERPRINT_LEN]);
/* Decode into out[] (cap entries); false on a malformed len. */
bool decode(const uint8_t* blob, size_t len, PeerMac* out, size_t cap, size_t* count);
}  /* namespace peer_mac_blob */

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

/* Live counters for up to 8 trusted peers + up to 8 replay tombstones
 * (mesh_session::MAX_REPLAY_COUNTERS, which main.cpp static_asserts
 * against): 16 × 16 B = 256 B blob. */
constexpr size_t MAX_REPLAY_ENTRIES = 16;

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

/* ──────────────────────────────────────────────────────────────────────────
 * SINGLE TRUSTED-PEER REMOVAL  (F10)
 *
 * Drop ONE pubkey from the "trusted_peers" blob (read-modify-write:
 * the remaining pubkeys are re-written in order, or the key is removed
 * when none remain). Used when a peer's verified LEAVE_OPERA arrives and
 * by peer removal. Same flash-encryption gate as save_trusted_peer():
 * the blob is household-graph metadata, never read or re-written on an
 * FE-off device.
 *
 * Returns true when the pubkey is gone afterwards — removed, or not
 * present in the first place (idempotent). Returns false on a null
 * pointer, FE disabled, or an NVS read/write failure (a malformed blob
 * is a read failure: refuse rather than clobber). On the host build,
 * always returns true (no-op success). Also drops the peer's "peer_macs"
 * entry (F33), best effort — the return value is the pubkey's.
 * ────────────────────────────────────────────────────────────────────────── */

bool remove_trusted_peer(const uint8_t pubkey[mesh_crypto::PUBKEY_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * MESH ENABLED FLAG  (F10 — POST /api/mesh/enable)
 *
 * NVS key "mesh_enabled", one byte (1 = on, 0 = off). NOT flash-
 * encryption gated: it is a user preference, not a secret and not
 * household-identifying, and gating it would make "off" silently revert
 * to "on" at every reboot of an FE-off dev board.
 *
 * load_mesh_enabled() returns true and writes *out only when the key is
 * present; false (with *out untouched) when absent, on a null pointer,
 * or on a read failure — callers default to enabled. On the host build
 * load always returns false and save always returns true.
 * ────────────────────────────────────────────────────────────────────────── */

bool save_mesh_enabled(bool enabled);
bool load_mesh_enabled(bool* out);

/* ──────────────────────────────────────────────────────────────────────────
 * OUTBOUND COUNTER HIGH-WATER MARK  (F33 part 3)
 *
 * NVS key "mesh_out_ctr", a u64: the highest outbound counter this device
 * may have signed (mesh_session's reserve-ahead mark, written once per
 * COUNTER_RESERVE_BLOCK frames by the session's reserve handler). NOT
 * flash-encryption gated, like mesh_enabled: it is a count, not a secret
 * and not household-identifying, and gating it would restart the counter
 * at every reboot of an FE-off board — whose frames the peers then drop as
 * replays of the counters they remember.
 *
 * save_outbound_counter() returns true only when the value is durably
 * written (read back equal). load_outbound_counter() returns true and
 * writes *out only when the key is present; false (out untouched) when
 * absent, on a null pointer or a read failure. Host build: save → true,
 * load → false.
 * ────────────────────────────────────────────────────────────────────────── */

bool save_outbound_counter(uint64_t high_water);
bool load_outbound_counter(uint64_t* out);

/* ──────────────────────────────────────────────────────────────────────────
 * OPERA DISPLAY NAME  (F10 — POST /api/mesh/name)
 *
 * NVS key "opera_name", up to MAX_OPERA_NAME_BYTES (32) bytes, stored
 * without the terminator. Flash-encryption gated like the rest of this
 * namespace — the name a household gives its devices is identifying
 * text, the same posture canary-wap's persist_opera_config applies. On
 * an FE-off board save refuses (the name lasts until reboot) and load
 * reports nothing stored.
 *
 * save_opera_name() rejects null / empty / over-long names (the REST
 * handler validates first; this is the belt to its braces).
 * load_opera_name() writes a NUL-terminated name into out (cap must be
 * at least MAX_OPERA_NAME_BYTES + 1) and returns true only when a
 * non-empty name was stored; on false out is untouched.
 * clear_opera_name() is idempotent. Host build: save/clear → true,
 * load → false.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t MAX_OPERA_NAME_BYTES = 32;   /* == mesh_pairing::MAX_OPERA_NAME_LEN */

bool save_opera_name(const char* name);
bool load_opera_name(char* out, size_t cap);
bool clear_opera_name();

/* ──────────────────────────────────────────────────────────────────────────
 * ROTATION PERSISTENCE  (F10-rekey — CRYPTO: maintainer review required)
 *
 * Called from mesh_session's rekey-commit handler once this device has
 * switched to a rotated opera_secret. Drops every forgotten peer's pubkey
 * from "trusted_peers", THEN re-persists the NEW secret through the same
 * flash-encryption gate as save_opera_secret().
 *
 * The order fails closed (review fix). opera_id travels in cleartext in
 * every frame header and opera_secret buys nothing else on this tree, so
 * a board that boots with the new secret while a forgotten device is
 * still in trusted_peers accepts that device again as soon as it copies
 * the new opera_id off the air. Removing first means a power cut between
 * the writes leaves the OLD secret with the peer gone — never the new
 * secret with the peer present. And if any removal fails, the new secret
 * is not saved at all.
 *
 * When the new secret is not saved (a removal failed, FE off, NVS
 * failure) the OLD one is cleared instead of left behind: a reboot must
 * come up with no opera (re-pair) rather than silently rejoin with a
 * secret the household just rotated away from. The live session keeps the
 * rotated secret in RAM either way (canary-wap's O2 branch). On an FE-off
 * board nothing was persisted to begin with, so this changes nothing
 * there.
 *
 * Returns true iff every forgotten pubkey was removed AND the new secret
 * was saved. Returns false on a null secret, on a null list with a
 * non-zero count, or on any NVS refusal/failure. Host build: the stubs
 * record the write order in test::journal() (below).
 * ────────────────────────────────────────────────────────────────────────── */

bool persist_rotation(const uint8_t new_secret[mesh_crypto::OPERA_SECRET_LEN],
                      const uint8_t (*forgotten_pubkeys)[mesh_crypto::PUBKEY_LEN],
                      size_t forgotten_count);

#ifdef CSI_TEST_HOST_BUILD
/* Host-test hooks. The host stubs write nothing, so an ORDER of writes is
 * invisible to a test; the stubs that stand in for the rotation's three
 * writes append one letter each to a journal instead — 'R'
 * remove_trusted_peer, 'S' save_opera_secret, 'C' clear_opera_secret — and
 * remove_trusted_peer can be told to fail. */
namespace test {
const char* journal();
void        reset_journal();
void        fail_remove_trusted_peer(bool fail);
}  /* namespace test */
#endif

}  /* namespace mesh_state */

#endif  /* SECURACV_MESH_STATE_H */
