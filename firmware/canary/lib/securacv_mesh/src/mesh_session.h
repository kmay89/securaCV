/*
 * SecuraCV Canary — Mesh session bridge
 * Version 0.1.0
 *
 * Singleton glue layer that wires mesh_transport (raw ESP-NOW recv/send)
 * to mesh_pairing (pure state machine) and to the opera-authenticated
 * traffic on top of it: beacon events, channel lock, hub election,
 * tamper alerts, leave and opera_secret rotation on removal (this file),
 * each with its payload codec or state machine in its own pure module
 * (mesh_beacon, mesh_channel_hop, mesh_hub_election, mesh_alert,
 * mesh_rekey).
 *
 * Wire envelope:
 *   • Every mesh-session frame is prefixed with a 1-byte MsgType.
 *   • Pair frames (MsgType 0..4) carry only the raw PairXxxPayload after
 *     the prefix — pairing is a pre-membership flow and does not need
 *     the opera_id/sender_fp/counter/signature outer header.
 *   • MsgType values 16+ are opera-authenticated traffic: the prefix byte
 *     is followed by a full signed mesh_envelope (header + payload +
 *     Ed25519 signature), verified in on_opera_frame against the
 *     sender's TrustedPeer pubkey, opera_id and replay counter.
 *
 * Wire-compat note:
 *   • canary-wap used to send its pair frames raw (no prefix, no
 *     envelope) while its receive path dropped anything shorter than the
 *     102-byte header+signature minimum, so its pair frames never reached
 *     handle_pair_*. F14 fixed that in canary-wap the same way this bridge
 *     always worked: a 1-byte type prefix, classified before the gate
 *     (canary_wap/mesh_pair_frame.h, host-tested). The trees still do not
 *     pair with EACH OTHER — canary-wap's pair types are 8..12, ours 0..4,
 *     and the signed-envelope version and type numbering differ too (see
 *     mesh_envelope.h "Layout parity").
 *
 * Layering:
 *   integration_layer (canary main.cpp, host tests)
 *       │
 *       ▼   set_paired_callback / set_failed_callback / set_code_ready_callback
 *   mesh_session  (this module)
 *       │
 *       ▼   pair_receive / pair_send / pair_tick
 *   mesh_pairing  (PR 2d/2e)
 *       │
 *       ▼   set_recv_callback / send_to_peer / broadcast / process
 *   mesh_transport (PR 2a)
 *       │
 *       ▼   esp_now_send / register_recv_cb
 *   ESP-NOW driver
 *
 * Threading:
 *   • mesh_transport's recv callback runs from process() on the main
 *     loop (it drains its SPSC ring there); the bridge's recv hook
 *     therefore also runs on the main loop and is free to call into
 *     pairing state machine + mesh_transport send paths directly.
 *   • mesh_session::process() is meant to be called from the main loop
 *     at any reasonable cadence (>= 10 Hz).
 *   • Every mutator belongs to that same task. Another task (the REST
 *     handlers on the httpd task) reaches the F10 mutators only through
 *     the request slot at the end of this header, which process() drains.
 */

#ifndef SECURACV_MESH_SESSION_H
#define SECURACV_MESH_SESSION_H

#include "mesh_pairing.h"
#include "mesh_transport.h"
#include "mesh_beacon.h"
#include "mesh_channel_hop.h"
#include "mesh_hub_election.h"
#include "mesh_alert.h"
#include "mesh_rekey.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_session {

/* MsgType values on the wire. The byte at offset 0 of every mesh_session
 * frame is one of these. Values 0..4 align with mesh_pairing::MsgType
 * exactly so a forwarding switch is trivial. Values 16+ are
 * opera-authenticated traffic — mesh_envelope::MsgType. */
enum class MsgType : uint8_t {
  PAIR_DISCOVER = 0,
  PAIR_OFFER    = 1,
  PAIR_ACCEPT   = 2,
  PAIR_CONFIRM  = 3,
  PAIR_COMPLETE = 4,
  /* 5..15 reserved for additional pairing extensions. */
  /* 16+ is opera-authenticated traffic: see mesh_envelope::MsgType. */
};

constexpr size_t MSGTYPE_HEADER_LEN = 1;
constexpr size_t MAX_SESSION_FRAME =
    MSGTYPE_HEADER_LEN + mesh_pairing::MAX_ACTION_PAYLOAD;

/* ──────────────────────────────────────────────────────────────────────────
 * CALLBACKS (integration-layer hooks)
 * ────────────────────────────────────────────────────────────────────────── */

/* Fires when pairing has SUCCEEDED. On the joiner side, `opera_secret`
 * is the freshly-decrypted secret (32 bytes) — the integration layer
 * MUST persist it to NVS within this callback, through
 * mesh_state::save_opera_secret(), which enforces the audit-O2
 * flash-encryption gate. On the initiator side, `opera_secret`
 * is nullptr (the initiator already had the secret; pairing only
 * distributed it to the joiner).
 *
 * confirmation_code is the matched 6-digit code (useful for telemetry). */
using PairedCallback = void (*)(const uint8_t* opera_secret_or_null,
                                uint32_t       confirmation_code);

/* Fires on any error path (tamper, timeout, AEAD-fail). The integration
 * layer should tear down its pairing UI. */
using FailedCallback = void (*)();

/* Fires when both ephemeral keys have been exchanged and the 6-digit
 * code is ready for the user to confirm on this device's screen. */
using CodeReadyCallback = void (*)(uint32_t confirmation_code);

void set_paired_callback(PairedCallback cb);
void set_failed_callback(FailedCallback cb);
void set_code_ready_callback(CodeReadyCallback cb);

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 *
 * mesh_session::init() expects mesh_transport::init() to have already
 * succeeded; the bridge installs its own recv callback on the transport
 * inside init().
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const uint8_t device_pubkey [mesh_crypto::PUBKEY_LEN],
          const uint8_t device_privkey[mesh_crypto::PRIVKEY_LEN]);
void deinit();
/* start() refuses (returns false) while the mesh is disabled — see
 * set_enabled() below — so "disabled" always implies "not running". */
bool start();
void stop();
bool is_running();

/* ──────────────────────────────────────────────────────────────────────────
 * ENABLE / DISABLE  (F10 — POST /api/mesh/enable)
 *
 * The user-facing on/off switch. set_enabled(false) cancels any pairing
 * in flight and stops the session: no send, no receive dispatch, no
 * pairing tick (the opera membership itself is kept — disabling is not
 * leaving). set_enabled(true) restarts the session when init() has run.
 * is_enabled() feeds GET /api/mesh's `enabled` and mesh_state_name()'s
 * "DISABLED".
 *
 * The flag is RAM state here; its persistence is the integration layer's
 * job (mesh_state::save_mesh_enabled / load_mesh_enabled — NVS key
 * "mesh_enabled", not flash-encryption gated because it is a preference,
 * not a secret). deinit() resets it to enabled.
 * ────────────────────────────────────────────────────────────────────────── */

void set_enabled(bool enabled);
bool is_enabled();

/* ──────────────────────────────────────────────────────────────────────────
 * PAIRING ENTRY POINTS  (wrappers over mesh_pairing)
 * ────────────────────────────────────────────────────────────────────────── */

bool start_pairing_initiator(const uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN],
                             const char*   opera_name,
                             uint32_t      now_ms);
bool start_pairing_joiner   (uint32_t now_ms);
bool confirm_pairing_code   (uint32_t now_ms);
void cancel_pairing         ();

mesh_pairing::State pairing_state();
uint32_t            pairing_confirmation_code();

/* Copy the peer's Ed25519 pubkey into `out` if the pairing state
 * machine has captured one (i.e. at least the OFFER/ACCEPT exchange
 * has completed). Returns true on copy, false if no peer pubkey is
 * available yet (state < AWAITING_CONFIRM) or out is null.
 *
 * The integration layer calls this from its PairedCallback to:
 *   • Persist the peer to NVS via mesh_state::save_trusted_peer().
 *   • Immediately register it via register_trusted_peer() so this
 *     boot's receive path accepts frames from the just-paired peer
 *     without waiting for the next reboot.
 *
 * Threading: must be called from the same task as process() (main
 * loop) — same task the PairedCallback fires from. */
bool get_paired_peer_pubkey(uint8_t out[mesh_crypto::PUBKEY_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN LOOP
 *
 * Call process() at any reasonable cadence; it drives the pairing
 * tick() (5-min timeout + initiator NOTIFY_PAIRED) and dispatches any
 * pending Actions out via mesh_transport.
 * ────────────────────────────────────────────────────────────────────────── */

void process(uint32_t now_ms);

/* ──────────────────────────────────────────────────────────────────────────
 * OPERA-AUTHENTICATED BROADCAST (PR 5c-3)
 *
 * After pairing has succeeded, the integration layer calls
 * set_opera_secret() with the 32-byte secret that pairing distributed
 * (or the persisted one loaded from NVS on a subsequent boot).
 * mesh_session derives + caches the 16-byte opera_id and the 8-byte
 * sender fingerprint; the 32-byte secret itself is NOT retained in
 * module state. The caller may zero its own copy of the secret as
 * soon as set_opera_secret() returns. Neither derived value is
 * persisted, by design: both are re-derived at boot — opera_id from the
 * FE-gated opera_secret mesh_state loads, sender_fp from the device key —
 * so there is nothing extra to store or to keep in sync.
 *
 *   set_opera_secret() — call ONCE per process (idempotent — calling
 *   again with the same secret is harmless; calling with a different
 *   secret rebinds, which the integration layer SHOULD NOT do mid-
 *   session). Returns false if called before init() or on null pointer.
 *   The secret is CONSUMED: only opera_id + sender_fp survive past
 *   the call.
 *
 *   send_beacon_event() — build a signed envelope carrying a BLE
 *   Scout beacon-event payload (state + label, see mesh_beacon.h)
 *   and broadcast it to every paired peer.
 *
 *     Threading: MUST be invoked from the same task as process()
 *     (the main loop). The outbound counter is incremented without
 *     synchronization. Integrations whose source of beacon events
 *     might run cross-task (e.g. ble_scout's broadcast callback when
 *     ble_scout_on_advert is reached from the NimBLE host task) MUST
 *     marshal the call back to the main loop themselves — typically
 *     by enqueuing a (state, label) tuple and draining it from the
 *     main loop tick. Do NOT add a mutex/atomic here; per the
 *     project's threading rule, that would mask violations rather
 *     than surface them.
 *
 *     Returns false if set_opera_secret() has not been called, if
 *     the underlying envelope serialization fails, or if the
 *     broadcast had no peers to send to. sender_fp is derived from
 *     the device pubkey passed to init().
 * ────────────────────────────────────────────────────────────────────────── */

bool set_opera_secret(const uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN]);

/* True iff set_opera_secret() has been called successfully. Integrations
 * check this before wiring the broadcast callback. */
bool has_opera_secret();

/* ──────────────────────────────────────────────────────────────────────────
 * STATUS ACCESSORS  (PR-8 — Mesh REST API)
 *
 * Thin, read-only getters the GET /api/mesh + GET /api/mesh/peers handlers
 * need. They expose only what those endpoints surface; nothing here
 * mutates network state.
 * ────────────────────────────────────────────────────────────────────────── */

/* Copy the cached 16-byte opera_id into `out`. Returns true iff an opera
 * secret has been set (i.e. has_opera_secret()); on false `out` is left
 * untouched. The opera_id is derived + cached in set_opera_secret() — the
 * 32-byte secret itself is never retained in module RAM. */
bool get_opera_id(uint8_t out[mesh_crypto::OPERA_ID_LEN]);

/* Convenience alias for has_opera_secret() — reads naturally in the
 * status handler ("does this device belong to an opera?"). */
bool has_opera();

/* Opera display name. set_opera_name() caches into a module-static
 * buffer; this module does not touch NVS. The integration layer persists
 * it through mesh_state::save_opera_name() (NVS key "opera_name",
 * flash-encryption gated like the rest of the household metadata) and
 * restores it at boot with load_opera_name() → set_opera_name(). The name
 * is local-only: POST /api/mesh/name renames this device's label for the
 * opera and does not propagate to peers. On an FE-off board the save is
 * refused, so the name lasts until reboot. get_opera_name() always
 * null-terminates `out` (writes "" when cap>0 and no name is cached). */
void set_opera_name(const char* name);
void get_opera_name(char* out, size_t cap);

bool send_beacon_event(mesh_beacon::BeaconState state,
                       const char*              label,
                       uint32_t                 now_ms);

/* Live-link view of the trusted peers: the MAC each peer last spoke
 * from. The binding is learned ONLY from a fully verified
 * opera-authenticated frame — signature, opera_id and replay checks all
 * passed — so at that instant the MAC provably spoke for the
 * fingerprint (forging it needs the peer's private key; replaying an
 * old frame from a new MAC fails the counter check). It goes stale the
 * moment the peer reboots onto a new address and refreshes on its next
 * verified frame, which is exactly the best-effort quality the
 * /api/mesh/peers join wants. mac_known is false until the first
 * verified frame this boot.
 *
 * Returns the number of in-use entries written (≤ cap). Threading: the
 * table is mutated on the main loop; the REST handlers read it from the
 * httpd task, same as trusted_peer_count() — a torn 6-byte MAC read can
 * at worst garble one row of a status view for one poll. */
struct PeerLink {
  uint8_t  fp [mesh_crypto::FINGERPRINT_LEN];
  uint8_t  mac[mesh_transport::MESH_TRANSPORT_MAC_LEN];
  bool     mac_known;
  /* Verified TAMPER_ALERT frames from this peer since it was registered
   * (per boot — not persisted). Counted only after signature, opera_id
   * and replay checks pass, so a forgery or a replay cannot inflate it. */
  uint32_t alerts_received;
};
size_t get_peer_links(PeerLink* out, size_t cap);

/* ──────────────────────────────────────────────────────────────────────────
 * RECEIVE-SIDE DISPATCH (PR 5c-4)
 *
 * Opera-authenticated frames (mesh_session MsgType >= 16) carry a full
 * mesh_envelope::Header + payload + Ed25519 signature. Verifying a
 * frame requires the SENDER'S pubkey; the integration layer registers
 * the pubkeys of peer Canaries it has paired with via
 * register_trusted_peer(). Each entry tracks a per-peer monotonic
 * last_counter for replay defense.
 *
 * On a verified BEACON_EVENT frame, mesh_session decodes the payload
 * (state, label) and invokes the handler set via
 * set_beacon_event_handler(). The handler runs on the same task as
 * mesh_transport::process() (the main loop) — safe to call into
 * csi_event, log_health, etc.
 *
 * Frames are silently dropped (no error feedback) when:
 *   • sender_fp doesn't match any trusted peer (unknown sender);
 *   • Ed25519 signature verification fails (forged or corrupted);
 *   • counter <= last_counter for that peer (replay);
 *   • opera_id in the header doesn't match our own (cross-opera leak —
 *     impossible if both sides paired together but defensive anyway);
 *   • payload length doesn't match the BEACON_EVENT wire format.
 *
 * register_trusted_peer() — copies the 32-byte pubkey into the local
 * table, computes its fingerprint, and starts the per-peer last_counter
 * at 0 — or at that fingerprint's tombstone, when the device was trusted
 * before (below). Returns false if the table is full
 * (MAX_TRUSTED_PEERS) OR the same pubkey is already registered (the
 * call would otherwise reset last_counter and allow replay). Idempotent
 * across boots: the integration layer persists the pubkey list and the
 * per-peer last_counter (mesh_state trusted_peers / replay_ctrs) and
 * restores both at boot (main.cpp: register every stored pubkey, then
 * restore_replay_counter; counters are re-saved every 5 minutes).
 *
 * clear_trusted_peers() — empties the table (leave_opera() uses it);
 * every dropped peer's counter survives as a tombstone (below).
 *
 * Replay tombstones: dropping a trusted peer — a verified LEAVE,
 * unregister_trusted_peer(), a removal or a rotation that forgets it,
 * clear_trusted_peers() — keeps its last_counter as a tombstone, and
 * register_trusted_peer() re-applies it to the same fingerprint. So a
 * device re-paired into the same, un-rotated opera cannot have anything
 * it signed before it was dropped replayed as fresh (its old LEAVE
 * included). At most MAX_COUNTER_TOMBSTONES are kept (oldest evicted);
 * only deinit() wipes them. They ride the same NVS replay_ctrs blob as
 * the live counters (get_replay_counters / restore_replay_counter).
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t MAX_TRUSTED_PEERS      = 8;
constexpr size_t MAX_COUNTER_TOMBSTONES = 8;
/* Live counters + tombstones: what get_replay_counters() can return. */
constexpr size_t MAX_REPLAY_COUNTERS    = MAX_TRUSTED_PEERS + MAX_COUNTER_TOMBSTONES;

bool   register_trusted_peer(const uint8_t pubkey[mesh_crypto::PUBKEY_LEN]);
void   clear_trusted_peers();
size_t trusted_peer_count();

/* Drop ONE trusted peer by fingerprint (empties its slot and MAC binding;
 * its replay counter becomes a tombstone). Returns true iff an entry was
 * removed. Used by the verified-LEAVE_OPERA receive path and by peer
 * removal. The NVS copy is the integration layer's
 * (mesh_state::remove_trusted_peer). */
bool   unregister_trusted_peer(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN]);

/* Snapshot the replay counters for NVS persistence: every tombstone,
 * oldest first, then every trusted peer's live counter — the order in
 * which restore_replay_counter() calls rebuild the same tombstone ages.
 * out must hold MAX_REPLAY_COUNTERS entries to get them all. Returns the
 * number of entries written. */
size_t get_replay_counters(uint8_t (*out_fps)[mesh_crypto::FINGERPRINT_LEN],
                           uint64_t* out_counters,
                           size_t    out_cap);

/* Restore a counter from NVS, after the trusted peers are registered. A
 * trusted fingerprint's last_counter is raised to it; any other
 * fingerprint becomes (or raises) a tombstone. Returns true if a counter
 * or tombstone was raised or created. */
bool   restore_replay_counter(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN],
                              uint64_t counter);

/* Beacon-event receiver callback. `sender_fp` lets the handler
 * correlate events with the Scout that sent them (each paired Scout
 * has a unique fingerprint). `label` is sanitized printable ASCII —
 * the chokepoint contract at the sender side guarantees this. */
typedef void (*beacon_event_received_fn)(
    const uint8_t              sender_fp[mesh_crypto::FINGERPRINT_LEN],
    mesh_beacon::BeaconState   state,
    const char*                label);

void set_beacon_event_handler(beacon_event_received_fn fn);

/* ──────────────────────────────────────────────────────────────────────────
 * CHANNEL LOCK — coordinated channel-hop (PR 4b)
 *
 * The Hub broadcasts a CHANNEL_LOCK frame to all peers when channel
 * utilization exceeds the threshold. Receivers should call
 * csi_hal::set_channel_lock() with the proposed channel.
 *
 * send_channel_lock() works identically to send_beacon_event(): builds
 * a signed envelope, broadcasts to all trusted peers.
 *
 * Threading: send MUST be called from the main loop; the receive handler
 * runs on the same task as mesh_transport::process().
 * ────────────────────────────────────────────────────────────────────────── */

bool send_channel_lock(uint8_t channel,
                       mesh_channel_hop::Reason reason,
                       uint32_t now_ms);

typedef void (*channel_lock_received_fn)(
    const uint8_t              sender_fp[mesh_crypto::FINGERPRINT_LEN],
    uint8_t                    channel,
    mesh_channel_hop::Reason   reason);

void set_channel_lock_handler(channel_lock_received_fn fn);

/* ──────────────────────────────────────────────────────────────────────────
 * HUB ELECTION — failover broadcast (PR 4c)
 *
 * When a sensor detects Hub absence, it broadcasts HUB_ELECTION with
 * the elected fingerprint. Peers verify the election is deterministic
 * (lowest fingerprint wins) and adopt the new Hub.
 * ────────────────────────────────────────────────────────────────────────── */

bool send_hub_election(mesh_hub_election::Event event,
                       const uint8_t fingerprint[mesh_crypto::FINGERPRINT_LEN],
                       uint32_t now_ms);

typedef void (*hub_election_received_fn)(
    const uint8_t              sender_fp[mesh_crypto::FINGERPRINT_LEN],
    mesh_hub_election::Event   event,
    const uint8_t              elected_fp[mesh_crypto::FINGERPRINT_LEN]);

void set_hub_election_handler(hub_election_received_fn fn);

/* ──────────────────────────────────────────────────────────────────────────
 * LEAVE  (F10 — POST /api/mesh/leave)
 *
 * leave_opera() makes this device forget its opera:
 *   1. if it holds an opera, it signs a LEAVE_OPERA frame (empty payload)
 *      under the current opera_id and broadcasts it — best effort, the
 *      return value says whether any peer took the frame;
 *   2. then cancels any pairing and wipes every opera-scoped RAM item:
 *      opera_id / sender_fp binding, trusted-peer table, opera name,
 *      alert history and counter. Two things are KEPT on purpose: the
 *      dropped peers' replay counters (as tombstones — see the receive
 *      side above) and this device's outbound counter, which peers keep
 *      as a tombstone for it, so after a re-pair into the same opera its
 *      frames keep counting up rather than restart and be dropped.
 * NVS is the integration layer's (mesh_state::clear_*). No rekey is
 * needed: the LEAVER discards its own copy of the secret; the survivors'
 * opera is unchanged. (Removing SOMEONE ELSE is the rekey path, spec
 * §5.6 — a different operation.)
 *
 * Receive side: a LEAVE_OPERA frame that passes signature + opera_id +
 * replay checks, with a zero-length payload, makes this session
 * unregister the SIGNER (and only the signer) and then fire the
 * peer-left handler with the signer's fingerprint and pubkey so the
 * integration layer can drop it from NVS. A peer can therefore only ever
 * remove itself.
 *
 * Threading: leave_opera() sends and mutates the counter — same task
 * contract as send_beacon_event(). The handler runs on the process()
 * task.
 * ────────────────────────────────────────────────────────────────────────── */

bool leave_opera(uint32_t now_ms);

typedef void (*peer_left_fn)(
    const uint8_t sender_fp[mesh_crypto::FINGERPRINT_LEN],
    const uint8_t sender_pubkey[mesh_crypto::PUBKEY_LEN]);

void set_peer_left_handler(peer_left_fn fn);

/* ──────────────────────────────────────────────────────────────────────────
 * TAMPER ALERTS  (F10 — the alerts channel; GET/DELETE /api/mesh/alerts)
 *
 * send_tamper_alert() builds a signed TAMPER_ALERT envelope carrying the
 * 6-byte mesh_alert payload (kind, severity, witness_seq — no free text)
 * and broadcasts it to every paired peer. Same return contract and task
 * contract as send_beacon_event(): false before set_opera_secret(), while
 * disabled, on an invalid payload (severity > 7), or when no peer took it.
 *
 * Receive side: a verified TAMPER_ALERT (signature + opera_id + replay
 * checks passed, payload decodes) increments the sender's
 * PeerLink.alerts_received and the opera-wide alerts_received(), pushes a
 * mesh_alert::Record into a RAM ring of MAX_ALERT_HISTORY entries
 * (oldest overwritten), then fires the tamper-alert handler. A malformed
 * payload is dropped silently and counts nothing.
 *
 * get_alerts() copies the ring newest-first. clear_alerts() empties the
 * history only; the per-peer and opera-wide counters keep counting for
 * the boot (canary-wap parity: its DELETE clears history, not
 * g_alerts_received). Nothing here is persisted — per boot, by design.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t MAX_ALERT_HISTORY = 16;

bool send_tamper_alert(mesh_alert::Kind kind,
                       uint8_t          severity,
                       uint32_t         witness_seq,
                       uint32_t         now_ms);

typedef void (*tamper_alert_received_fn)(
    const uint8_t    sender_fp[mesh_crypto::FINGERPRINT_LEN],
    mesh_alert::Kind kind,
    uint8_t          severity,
    uint32_t         witness_seq);

void     set_tamper_alert_handler(tamper_alert_received_fn fn);
uint32_t alerts_received();
size_t   get_alerts(mesh_alert::Record* out, size_t cap);
void     clear_alerts();

/* ──────────────────────────────────────────────────────────────────────────
 * PEER REMOVAL WITH opera_secret ROTATION  (F10-rekey — POST /api/mesh/remove)
 *
 * CRYPTO: maintainer review required before merge; bench-gated (U1 Track C3).
 *
 * remove_peer() drops a trusted peer AND rotates the household secret
 * (spec §5.6; option B of the plan — an ephemeral X25519 exchange per
 * rotation over signed envelopes, the pure state machine in mesh_rekey.h).
 * The exclusion itself is every survivor unregistering the removed pubkey;
 * opera_id is cleartext, so the new secret alone does not lock the removed
 * device out, and a survivor that misses the window keeps trusting it with
 * no signal. The rotation kills every pre-removal frame for the survivors
 * that switch (mesh_rekey.h spells this out):
 *   • the rotation is started first; only if it starts is the peer
 *     forgotten (trust entry + its transport MAC, so later broadcasts stop
 *     reaching it) — a refused start leaves the table untouched;
 *   • no survivors left → the new secret is installed at once
 *     (COMMITTED); otherwise an OFFER goes out (STARTED) and the session
 *     finishes the exchange from its receive path and process() timeout;
 *   • on this device's commit — all ACKs, or the 60 s timeout — it
 *     switches to the new secret, forgets every survivor that did not ACK,
 *     and fires the rekey-commit handler. A survivor does the same when it
 *     installs: it sends its ACK under the OLD opera_id first, then
 *     switches, forgets the removed device and fires the same handler.
 * The outbound counter is NOT reset on a switch: receivers key their
 * replay counters by fingerprint, not by opera_id.
 *
 * The handler receives the new secret (persist it — mesh_state::
 * persist_rotation, FE-gated) and the pubkeys of every peer this rotation
 * dropped, for NVS: on a survivor, the removed device; on the initiator,
 * the removed device (again — it left the table at start) followed by
 * every survivor that did not ACK. persist_rotation drops them all BEFORE
 * it saves the secret, so an interrupted commit fails closed. The removed
 * peer's pubkey also comes back at once through `removed_pubkey_out`, for
 * the caller to drop from NVS right away (idempotent with the commit).
 *
 * Refusals: DISABLED (mesh off / not initialized), NO_OPERA, NOT_FOUND
 * (fp is not a trusted peer), IN_FLIGHT (a rotation is already running
 * on this device, as initiator or survivor), FAILED (key generation),
 * PAIRING (a pairing exchange is in progress — it would hand the joiner
 * the secret this rotation is about to retire).
 * Threading: main-loop task, like every mutator here. POST
 * /api/mesh/remove reaches it through the request slot below, never
 * directly from the httpd task.
 * ────────────────────────────────────────────────────────────────────────── */

enum class RemoveResult : uint8_t {
  STARTED = 0,
  COMMITTED,
  DISABLED,
  NO_OPERA,
  NOT_FOUND,
  IN_FLIGHT,
  FAILED,
  PAIRING,
};

RemoveResult remove_peer(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN],
                         uint32_t      now_ms,
                         uint8_t       removed_pubkey_out[mesh_crypto::PUBKEY_LEN]);

bool rekey_in_progress();

typedef void (*rekey_commit_fn)(
    const uint8_t new_secret[mesh_crypto::OPERA_SECRET_LEN],
    const uint8_t (*forgotten_pubkeys)[mesh_crypto::PUBKEY_LEN],
    size_t        forgotten_count);

void set_rekey_commit_handler(rekey_commit_fn fn);

/* ──────────────────────────────────────────────────────────────────────────
 * REST REQUEST SLOT  (review fix — the F10 REST mutators run on the main loop)
 *
 * Every mutator in this module belongs to the task that runs process()
 * (the main loop): the receive path, the pairing and rotation ticks and
 * the senders all run there and share the trusted-peer table, the rekey
 * context, the outbound counter and the opera binding without locks, by
 * design. The esp_http_server handlers run on the httpd task, so POST
 * /api/mesh/leave, /name, /enable, /remove and DELETE /api/mesh/alerts do
 * NOT call leave_opera / set_opera_name / set_enabled / remove_peer /
 * clear_alerts: they submit ONE request into this one-deep slot and wait;
 * process() executes it on the main loop — first thing, even while the
 * session is stopped, so enable and leave work while disabled — and
 * publishes the result. (securacv_network.cpp poisons those five names
 * after its mesh includes, so a direct call there does not compile.)
 *
 *   submit_request()      httpd task. false while another request holds
 *                         the slot (the handler answers 409 mesh_busy).
 *   take_request_result() httpd task. true once the result is ready; copies
 *                         it out and frees the slot.
 *   withdraw_request()    httpd task, after a wait timed out: true iff the
 *                         request had not started — it is gone and will
 *                         never run. false once process() has taken it: the
 *                         result lands within that same process() call.
 *   abandon_request()     httpd task, last resort: a request that already
 *                         runs finishes, its result is discarded and the
 *                         slot frees itself.
 *
 * Execution refuses what would strand a rotation: LEAVE and SET_ENABLED
 * {false} while one runs (REKEY_IN_FLIGHT); REMOVE while a pairing runs
 * (RemoveResult::PAIRING). SET_NAME without an opera is NO_OPERA.
 *
 * The slot state moves by GCC __atomic builtins (acquire/release, a
 * compare-exchange at every hand-off) — the pattern securacv_audio's mute
 * request uses. The request and result bodies are only touched by the
 * side that owns the current state, and wiped when they change hands.
 * ────────────────────────────────────────────────────────────────────────── */

enum class RequestType : uint8_t {
  NONE = 0,
  LEAVE,          /* leave_opera() */
  SET_NAME,       /* set_opera_name(name) */
  SET_ENABLED,    /* set_enabled(enabled) */
  CLEAR_ALERTS,   /* clear_alerts() */
  REMOVE,         /* remove_peer(fp) */
};

enum class RequestStatus : uint8_t {
  OK = 0,
  REKEY_IN_FLIGHT,   /* LEAVE / SET_ENABLED {false} while a rotation runs */
  NO_OPERA,          /* SET_NAME with no opera */
  BAD_REQUEST,       /* NONE or an unknown type */
};

struct Request {
  RequestType type;
  bool        enabled;                                     /* SET_ENABLED */
  uint8_t     fp[mesh_crypto::FINGERPRINT_LEN];            /* REMOVE */
  char        name[mesh_pairing::MAX_OPERA_NAME_LEN + 1];  /* SET_NAME */
};

struct RequestResult {
  RequestType   type;
  RequestStatus status;
  bool          notified;   /* LEAVE: some peer took the signed LEAVE */
  bool          enabled;    /* SET_ENABLED: is_enabled() afterwards */
  RemoveResult  remove;     /* REMOVE */
  uint8_t       removed_pubkey[mesh_crypto::PUBKEY_LEN];  /* REMOVE, STARTED/COMMITTED */
};

bool submit_request(const Request& req);
bool take_request_result(RequestResult* out);
bool withdraw_request();
void abandon_request();

}  /* namespace mesh_session */

#endif  /* SECURACV_MESH_SESSION_H */
