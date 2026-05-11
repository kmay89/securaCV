/*
 * SecuraCV Canary — Mesh pairing (wire format + confirmation code)
 * Version 0.1.0
 *
 * PR 2d of 7. First slice of the pairing layer. Lands ONLY the
 * deterministic, side-effect-free pieces:
 *
 *   • compute_confirmation_code() — derives the 6-digit code that both
 *     pairing peers display so the user can visually verify they're
 *     speaking to each other and not to a man-in-the-middle.
 *
 *   • Wire-format payload structs for the 5 PAIR_* message types,
 *     byte-identical to canary-wap's mesh_network.h so once the state
 *     machine (PR 2e) lands, paired canary + canary-wap nodes can
 *     complete a pairing handshake.
 *
 * The pairing state machine itself (PR 2e) sits above this module and
 * uses these primitives + mesh_crypto (x25519_derive, ed25519_sign,
 * aead_encrypt) + mesh_transport (send_to_peer, broadcast).
 *
 * Wire compatibility:
 *   The 6-digit confirmation code follows exactly the canary-wap
 *   derivation (mesh_network.cpp:799-800):
 *
 *     sha256_domain(DOMAIN_PAIR_CONFIRM, session_key, 32, hash);
 *     code = ((hash[0] << 16) | (hash[1] << 8) | hash[2]) % 1000000;
 *
 *   A pinned test vector asserts that a 32-byte zero session key
 *   produces code 884555 (computed independently via openssl).
 */

#ifndef SECURACV_MESH_PAIRING_H
#define SECURACV_MESH_PAIRING_H

#include "mesh_crypto.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_pairing {

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

/* Domain-separation string for confirmation-code hashing. Wire-compat
 * with canary-wap (mesh_network.cpp:46). */
constexpr const char* DOMAIN_PAIR_CONFIRM = "securacv:pair:confirm:v0";

/* Session key length. Equal to the X25519 shared-secret width because
 * the canary-wap pairing flow uses the DH output directly as the
 * session key (mesh_network.cpp:833). */
constexpr size_t SESSION_KEY_LEN = mesh_crypto::X25519_SHARED_LEN;

/* Max lengths for human-readable name fields (canary-wap mesh_network.h
 * MAX_PEER_NAME_LEN / MAX_OPERA_NAME_LEN). */
constexpr size_t MAX_PEER_NAME_LEN  = 24;
constexpr size_t MAX_OPERA_NAME_LEN = 32;

/* Confirmation code is a decimal value in [0, 999999]; the UI pads it
 * to 6 digits. CONFIRMATION_CODE_MODULUS exposes the bound so callers
 * don't re-magic-number it. */
constexpr uint32_t CONFIRMATION_CODE_MODULUS = 1000000;

/* ──────────────────────────────────────────────────────────────────────────
 * CONFIRMATION CODE
 *
 * Both pairing peers compute the same session_key via X25519 ECDH on
 * their ephemeral keys, then call compute_confirmation_code() with that
 * key. Identical session_key → identical code, so the user simply
 * checks that the 6 digits on both screens match.
 *
 * Security: SHA-256 of the session key with a fixed domain string. The
 * code's purpose is human-verifiable MITM detection, not secrecy — an
 * attacker cannot fake the code without breaking the underlying X25519
 * exchange. Truncation to ~20 bits gives a 1-in-10^6 collision chance,
 * which is the standard 6-digit-OOB-confirmation level.
 * ────────────────────────────────────────────────────────────────────────── */

uint32_t compute_confirmation_code(const uint8_t session_key[SESSION_KEY_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * CONFIRMATION HASH
 *
 * SHA-256(DOMAIN_PAIR_CONFIRM || session_key || code_u32_le), where
 * code_u32_le is the little-endian byte order of compute_confirmation_code
 * for the same session_key. Wire-compat with canary-wap mesh_network.cpp
 * lines 856-860 (verify path) and 1448-1450 (send path).
 *
 * Purpose: the joiner sends this 32-byte hash inside PairConfirmPayload
 * after the user confirms the 6-digit code matches on both screens. The
 * initiator recomputes and compares; if any byte differs, a MITM has
 * mutated the exchange and pairing aborts.
 *
 * `code` is included in the hash (rather than just the session_key) so
 * an attacker who knows the session_key alone — e.g. an honest middlebox
 * that proxies the X25519 exchange — still cannot forge a matching hash
 * without also seeing both peers' randomly-chosen ephemeral keys (which
 * is what determines the 6-digit code).
 * ────────────────────────────────────────────────────────────────────────── */

void compute_confirmation_hash(const uint8_t session_key[SESSION_KEY_LEN],
                               uint32_t      code,
                               uint8_t       out[mesh_crypto::SHA256_OUT_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * WIRE-FORMAT PAYLOADS
 *
 * Byte-identical to canary-wap mesh_network.h:281-304. The state
 * machine in PR 2e will produce/consume these via mesh_transport's
 * raw send/recv path. Packed so the on-the-wire layout is identical
 * across endianness; the receiver casts the raw bytes back to the
 * struct.
 *
 * Roles encoded as uint8_t (matches canary-wap PairingRole enum).
 * ────────────────────────────────────────────────────────────────────────── */

enum Role : uint8_t {
  ROLE_NONE      = 0,
  ROLE_INITIATOR = 1,   /* existing opera member */
  ROLE_JOINER    = 2,   /* new device joining */
};

struct __attribute__((packed)) PairDiscoverPayload {
  uint8_t pubkey[mesh_crypto::PUBKEY_LEN];     /* device long-term Ed25519 pub */
  char    device_name[MAX_PEER_NAME_LEN + 1];  /* null-terminated, may be empty */
  uint8_t role;                                 /* Role enum */
};

struct __attribute__((packed)) PairOfferPayload {
  uint8_t ephemeral_pubkey[mesh_crypto::PUBKEY_LEN];  /* X25519 ephemeral pub */
  uint8_t device_pubkey[mesh_crypto::PUBKEY_LEN];     /* long-term Ed25519 pub */
  char    opera_name[MAX_OPERA_NAME_LEN + 1];
  uint8_t opera_member_count;
};

/* PairAcceptPayload reuses PairOfferPayload's shape per canary-wap
 * (mesh_network.cpp:803 — `PairOfferPayload accept;  // Reuse structure`).
 * We expose a typedef so callers can read the intent. */
using PairAcceptPayload = PairOfferPayload;

struct __attribute__((packed)) PairConfirmPayload {
  /* SHA-256(DOMAIN_PAIR_CONFIRM || session_key || code_u32_le).
   * See compute_confirmation_hash() above. Sent by the joiner once the
   * user has visually verified the 6-digit code matches on both screens;
   * receiver recomputes and rejects on any byte difference. */
  uint8_t confirmation_hash[mesh_crypto::SHA256_OUT_LEN];
};

struct __attribute__((packed)) PairCompletePayload {
  /* Initiator's opera_secret encrypted to the joiner under the derived
   * session key. Layout: ciphertext (OPERA_SECRET_LEN) || tag (16). */
  uint8_t encrypted_secret[mesh_crypto::OPERA_SECRET_LEN + mesh_crypto::AEAD_TAG_LEN];
  uint8_t nonce[mesh_crypto::AEAD_NONCE_LEN];
};

/* Sanity-pin payload sizes so a future header reorganization can't
 * silently change the wire format. */
static_assert(sizeof(PairDiscoverPayload) == mesh_crypto::PUBKEY_LEN + (MAX_PEER_NAME_LEN + 1) + 1,
              "PairDiscoverPayload wire size drifted from canary-wap");
static_assert(sizeof(PairOfferPayload) == mesh_crypto::PUBKEY_LEN * 2 + (MAX_OPERA_NAME_LEN + 1) + 1,
              "PairOfferPayload wire size drifted from canary-wap");
static_assert(sizeof(PairConfirmPayload) == mesh_crypto::SHA256_OUT_LEN,
              "PairConfirmPayload wire size drifted from canary-wap");
static_assert(sizeof(PairCompletePayload) ==
              (mesh_crypto::OPERA_SECRET_LEN + mesh_crypto::AEAD_TAG_LEN) + mesh_crypto::AEAD_NONCE_LEN,
              "PairCompletePayload wire size drifted from canary-wap");

/* ──────────────────────────────────────────────────────────────────────────
 * STATE MACHINE
 *
 * Pure state-machine driver. Holds no globals; the integration layer
 * (PR 2f) keeps one PairingContext per device and shuttles bytes to/
 * from mesh_transport. Tests can run two contexts in parallel to
 * simulate a complete handshake host-side without I/O.
 *
 * The 5-message exchange (matches canary-wap mesh_network.cpp:
 *   handle_pair_discover @748, handle_pair_offer @782,
 *   handle_pair_accept @825, handle_pair_confirm @845):
 *
 *   Initiator (existing opera member)        Joiner (new device)
 *   ─────────────────────────────────        ───────────────────
 *   start_initiator()
 *     → BROADCAST_DISCOVER (role=INIT) ────► (ignored: not for joiner)
 *                                            start_joiner()
 *   handle (DISCOVER role=JOIN) ◄────────────  → BROADCAST_DISCOVER (role=JOIN)
 *     → SEND_OFFER (initiator ephem + pub)
 *                            ─────────────►  handle (OFFER)
 *                                            → derive session_key + code
 *                                            → SEND_ACCEPT (joiner ephem + pub)
 *                                            → NOTIFY_CODE_READY (UI prompt)
 *   handle (ACCEPT) ◄───────────────────────
 *     → derive session_key + code
 *     → NOTIFY_CODE_READY (UI prompt)
 *
 *   confirm_code() [after user OK]           confirm_code() [after user OK]
 *     → SEND_CONFIRM (hash) ─────────────►   handle (CONFIRM) — joiner accepts
 *                                              the initiator's confirm hash
 *   handle (CONFIRM) ◄──────────────────────  (joiner already moved to
 *     → verify hash matches                   AWAITING_COMPLETE after sending
 *     → SEND_COMPLETE (AEAD(opera_secret))►   its own confirm, so a peer-side
 *     → tick() returns NOTIFY_PAIRED          confirm here is a no-op drop)
 *                                            handle (COMPLETE)
 *                                            → decrypt → NOTIFY_PAIRED
 *
 * On any failure or 5-minute timeout, both sides transition to FAILED
 * and the integration layer is told via NOTIFY_FAILED.
 *
 * Key lifecycle:
 *   • Ephemeral keypair: generated at start_*, wiped on terminate.
 *   • Session key: derived from x25519 once both ephemeral pubs are
 *     known, used to AEAD-encrypt the opera_secret on the COMPLETE
 *     message, wiped on terminate AND on PAIRED transition.
 *   • Opera secret: held in RAM by the initiator (loaded from NVS) and
 *     by the joiner only between COMPLETE-receive and the integration
 *     layer reading it out via consume_opera_secret(). The integration
 *     layer is responsible for the NVS write + the flash-encryption
 *     gate (canary-wap audit O2).
 * ────────────────────────────────────────────────────────────────────────── */

enum class State : uint8_t {
  IDLE = 0,
  DISCOVERING_INITIATOR,    /* initiator: broadcasting DISCOVER(INIT), waiting for joiner DISCOVER(JOIN) */
  DISCOVERING_JOINER,       /* joiner:    broadcasting DISCOVER(JOIN), waiting for initiator OFFER */
  AWAITING_ACCEPT,          /* initiator: sent OFFER, awaiting joiner ACCEPT */
  AWAITING_CONFIRM,         /* both:      have session_key + code, awaiting user_confirm() */
  AWAITING_CONFIRM_PEER,    /* both:      sent our CONFIRM hash, awaiting peer's CONFIRM */
  AWAITING_COMPLETE,        /* joiner:    sent CONFIRM, awaiting encrypted opera_secret */
  PAIRED,                   /* terminal:  success — opera_secret available on joiner */
  FAILED,                   /* terminal:  any error path or 5-min timeout */
};

enum class ActionType : uint8_t {
  NONE = 0,
  BROADCAST_DISCOVER,    /* send to FF:FF:FF:FF:FF:FF (or mesh_transport::broadcast) */
  SEND_OFFER,            /* unicast to action.peer_mac */
  SEND_ACCEPT,
  SEND_CONFIRM,
  SEND_COMPLETE,
  NOTIFY_CODE_READY,     /* UI: show 6-digit confirmation_code */
  NOTIFY_PAIRED,         /* integration: persist opera_secret, transition to ACTIVE */
  NOTIFY_FAILED,         /* integration: clear pairing UI; log */
};

constexpr size_t MAX_ACTION_PAYLOAD =
    sizeof(PairCompletePayload) > sizeof(PairOfferPayload) ?
    sizeof(PairCompletePayload) : sizeof(PairOfferPayload);

struct Action {
  ActionType type;
  uint8_t    peer_mac[6];                          /* destination MAC, all-FF for broadcast */
  uint8_t    payload[MAX_ACTION_PAYLOAD];          /* raw payload bytes to send */
  size_t     payload_len;                          /* 0 if no payload */
  uint32_t   confirmation_code;                    /* non-zero for NOTIFY_CODE_READY */
};

/* Pairing timeout. Matches canary-wap (5 min). Crossing this fires
 * a FAILED transition + NOTIFY_FAILED action. */
constexpr uint32_t PAIRING_TIMEOUT_MS = 5 * 60 * 1000;

/* Per-context state. Treat as opaque from the integration side — only
 * the API below should touch fields. Sized so multiple contexts can sit
 * on the stack without pressure (~250 B). */
struct PairingContext {
  State    state;
  Role     role;

  /* Long-term keys, supplied at start_* time (the integration layer
   * loads/derives these from device identity). */
  uint8_t  device_pubkey[mesh_crypto::PUBKEY_LEN];
  uint8_t  device_privkey[mesh_crypto::PRIVKEY_LEN];

  /* Ephemeral X25519 keypair generated at start_* time. Wiped on
   * terminate() / cancel(). */
  uint8_t  ephem_pubkey[mesh_crypto::PUBKEY_LEN];
  uint8_t  ephem_privkey[mesh_crypto::PRIVKEY_LEN];

  /* Peer info captured during the exchange. */
  uint8_t  peer_mac[6];
  uint8_t  peer_pubkey[mesh_crypto::PUBKEY_LEN];
  uint8_t  peer_ephem_pubkey[mesh_crypto::PUBKEY_LEN];

  /* Derived once both ephemeral pubs known. */
  uint8_t  session_key[SESSION_KEY_LEN];
  uint32_t confirmation_code;

  /* Initiator-only: opera_secret to distribute on COMPLETE.
   * Joiner-only: opera_secret received via COMPLETE (consumed exactly
   * once via consume_opera_secret() then wiped). */
  uint8_t  opera_secret[mesh_crypto::OPERA_SECRET_LEN];
  bool     opera_secret_present;

  /* Opera display name (initiator: from config; joiner: from OFFER). */
  char     opera_name[MAX_OPERA_NAME_LEN + 1];

  /* Timeout / liveness. */
  uint32_t started_ms;

  /* Whether the user has confirmed the 6-digit code matches on both
   * screens. Set by confirm_code(); used to gate SEND_CONFIRM. */
  bool     user_confirmed;

  /* One-shot flag: set when the initiator transitions to PAIRED after
   * SEND_COMPLETE. The next tick() reads it, clears it, and returns
   * NOTIFY_PAIRED so the integration layer gets a definitive success
   * signal on the initiator side (the joiner gets it inline when
   * COMPLETE decrypts). */
  bool     pending_notify_paired;
};

/* ──────────────────────────────────────────────────────────────────────────
 * STATE-MACHINE API
 *
 * All entry points are pure: take ctx by reference, mutate state, and
 * return at most one Action describing what the integration layer
 * should do next. ActionType::NONE means "no I/O this call".
 * ────────────────────────────────────────────────────────────────────────── */

/* Initialise a context. State becomes IDLE. */
void context_init(PairingContext& ctx);

/* Begin pairing as the initiator (an existing opera member who has a
 * valid opera_secret). Generates an ephemeral keypair and returns an
 * Action with type=BROADCAST_DISCOVER. */
Action start_initiator(PairingContext& ctx,
                       const uint8_t  device_pub[mesh_crypto::PUBKEY_LEN],
                       const uint8_t  device_priv[mesh_crypto::PRIVKEY_LEN],
                       const uint8_t  opera_secret[mesh_crypto::OPERA_SECRET_LEN],
                       const char*    opera_name,
                       uint32_t       now_ms);

/* Begin pairing as the joiner (a new device with no opera_secret yet).
 * State becomes AWAITING_OFFER; no message goes out (the initiator's
 * DISCOVER is the trigger). */
Action start_joiner(PairingContext& ctx,
                    const uint8_t  device_pub[mesh_crypto::PUBKEY_LEN],
                    const uint8_t  device_priv[mesh_crypto::PRIVKEY_LEN],
                    uint32_t       now_ms);

/* Feed an incoming message into the state machine. msg_type identifies
 * the PAIR_* phase (see message types below). Returns the next action;
 * if the message is unexpected for the current state, returns NONE
 * (silently dropped) — pairing isn't aborted on every stray frame
 * because the same MAC may also be running heartbeat/etc traffic. */
enum class MsgType : uint8_t {
  DISCOVER = 0,
  OFFER    = 1,
  ACCEPT   = 2,
  CONFIRM  = 3,
  COMPLETE = 4,
};

Action receive(PairingContext& ctx,
               const uint8_t  from_mac[6],
               MsgType        msg_type,
               const uint8_t* payload, size_t payload_len,
               uint32_t       now_ms);

/* Periodic tick from the main loop. now_ms is the current monotonic
 * time. Returns NOTIFY_FAILED if the 5-minute timeout has elapsed
 * since start_*, NONE otherwise. */
Action tick(PairingContext& ctx, uint32_t now_ms);

/* User-driven confirmation that the 6-digit code matches on both
 * screens. Valid only in AWAITING_CONFIRM / AWAITING_CONFIRM_PEER.
 * Returns SEND_CONFIRM. */
Action confirm_code(PairingContext& ctx, uint32_t now_ms);

/* Abort pairing from any state. Wipes the ephemeral key + session
 * key + opera_secret. Returns NOTIFY_FAILED so the integration layer
 * tears down UI. */
Action cancel(PairingContext& ctx);

/* Read out and zero the joiner's received opera_secret. Returns true
 * if there was a secret to consume; the caller (integration layer)
 * persists it to NVS (subject to its own flash-encryption gate per
 * canary-wap audit O2). Idempotent — calling again returns false. */
bool consume_opera_secret(PairingContext& ctx,
                          uint8_t out[mesh_crypto::OPERA_SECRET_LEN]);

}  /* namespace mesh_pairing */

#endif  /* SECURACV_MESH_PAIRING_H */
