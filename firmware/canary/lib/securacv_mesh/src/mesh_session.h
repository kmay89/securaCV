/*
 * SecuraCV Canary — Mesh session bridge
 * Version 0.1.0
 *
 * Singleton glue layer that wires mesh_transport (raw ESP-NOW recv/send)
 * to mesh_pairing (pure state machine) and to whatever else lives on top
 * of the mesh in future slices (PR 3 multilink fusion, etc.).
 *
 * Wire envelope:
 *   • Every mesh-session frame is prefixed with a 1-byte MsgType.
 *   • Pair frames (MsgType 0..4) carry only the raw PairXxxPayload after
 *     the prefix — pairing is a pre-membership flow and does not need
 *     the opera_id/sender_fp/counter/signature outer header that
 *     opera-authenticated traffic will use in a future slice.
 *   • Reserved MsgType values 16+ are for opera-authenticated traffic
 *     (heartbeat, alerts, witness records) which will land in PR 2g
 *     with the full MessageHeader format.
 *
 * Wire-compat note:
 *   • canary-wap sends pair frames raw (no envelope) — see
 *     mesh_network.cpp:1234, 778, 817 — but its recv path rejects
 *     anything shorter than the 102-byte header+signature minimum
 *     (mesh_network.cpp:432). That is an existing canary-wap bug:
 *     canary-wap pair frames never actually reach handle_pair_*.
 *     Rather than inherit a non-functional wire format, this bridge
 *     uses a 1-byte MsgType prefix. Documented divergence — see
 *     docs/audit/mesh_and_chirp_audit_v1.md (the existing audit
 *     covered the chirp v0.1→v0.2 break; this is an analogous
 *     pairing fix that will land separately in canary-wap when the
 *     two lanes are consolidated.
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
 */

#ifndef SECURACV_MESH_SESSION_H
#define SECURACV_MESH_SESSION_H

#include "mesh_pairing.h"
#include "mesh_transport.h"
#include "mesh_beacon.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_session {

/* MsgType values on the wire. The byte at offset 0 of every mesh_session
 * frame is one of these. Values 0..4 align with mesh_pairing::MsgType
 * exactly so a forwarding switch is trivial. Values 16+ are reserved
 * for opera-authenticated traffic in PR 2g. */
enum class MsgType : uint8_t {
  PAIR_DISCOVER = 0,
  PAIR_OFFER    = 1,
  PAIR_ACCEPT   = 2,
  PAIR_CONFIRM  = 3,
  PAIR_COMPLETE = 4,
  /* 5..15 reserved for additional pairing extensions. */
  /* 16+ reserved for opera-authenticated traffic (PR 2g). */
};

constexpr size_t MSGTYPE_HEADER_LEN = 1;
constexpr size_t MAX_SESSION_FRAME =
    MSGTYPE_HEADER_LEN + mesh_pairing::MAX_ACTION_PAYLOAD;

/* ──────────────────────────────────────────────────────────────────────────
 * CALLBACKS (integration-layer hooks)
 * ────────────────────────────────────────────────────────────────────────── */

/* Fires when pairing has SUCCEEDED. On the joiner side, `opera_secret`
 * is the freshly-decrypted secret (32 bytes) — the integration layer
 * MUST persist it to NVS within this callback (PR 2g will add the
 * audit-O2 flash-encryption gate). On the initiator side, `opera_secret`
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
bool start();
void stop();
bool is_running();

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
 * soon as set_opera_secret() returns. (PR 5c-4 will move the cached
 * opera_id/sender_fp into a flash-encryption-gated NVS slot to match
 * opera_secret's existing hygiene; PR 5c-3 keeps them in module RAM
 * for the life of the process.)
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

bool send_beacon_event(mesh_beacon::BeaconState state,
                       const char*              label,
                       uint32_t                 now_ms);

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
 * table, computes its fingerprint, and zeroes the per-peer
 * last_counter. Returns false if the table is full
 * (MAX_TRUSTED_PEERS) OR the same pubkey is already registered (the
 * call would otherwise reset last_counter and allow replay). Idempotent
 * across boots: persist the same pubkey list and last_counter to NVS
 * (PR 5c-5 / PR 4b will add the NVS persistence).
 *
 * clear_trusted_peers() — wipes the table. Used on opera-secret-
 * rotation and at deinit().
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t MAX_TRUSTED_PEERS = 8;

bool   register_trusted_peer(const uint8_t pubkey[mesh_crypto::PUBKEY_LEN]);
void   clear_trusted_peers();
size_t trusted_peer_count();

/* Beacon-event receiver callback. `sender_fp` lets the handler
 * correlate events with the Scout that sent them (each paired Scout
 * has a unique fingerprint). `label` is sanitized printable ASCII —
 * the chokepoint contract at the sender side guarantees this. */
typedef void (*beacon_event_received_fn)(
    const uint8_t              sender_fp[mesh_crypto::FINGERPRINT_LEN],
    mesh_beacon::BeaconState   state,
    const char*                label);

void set_beacon_event_handler(beacon_event_received_fn fn);

}  /* namespace mesh_session */

#endif  /* SECURACV_MESH_SESSION_H */
