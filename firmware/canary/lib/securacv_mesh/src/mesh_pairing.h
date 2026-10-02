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
#include "mesh_wire.h"
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
 *   The owners confirm in either order (spec §5.2; F97). Every COMPLETE
 *   goes out with the initiator's own CONFIRM immediately in front of it
 *   (Action::leading_confirm). Initiator's owner first:
 *
 *   confirm_code() [after user OK]
 *     → SEND_CONFIRM (hash) ─────────────►   handle (CONFIRM) — hash checked
 *                                              (a pre-F97 joiner drops it)
 *                                            confirm_code() [after user OK]
 *   handle (CONFIRM) ◄──────────────────────  → SEND_CONFIRM (hash)
 *     → verify hash matches
 *     → SEND_COMPLETE: CONFIRM (hash) ────►   handle (CONFIRM) — hash checked
 *                      then AEAD(secret) ─►   handle (COMPLETE) — taken once
 *     → tick() returns NOTIFY_PAIRED            this owner confirmed
 *                                            → decrypt → NOTIFY_PAIRED
 *
 *   Joiner's owner first:
 *
 *                                            confirm_code() [after user OK]
 *   handle (CONFIRM) ◄──────────────────────  → SEND_CONFIRM (hash)
 *     → verify hash, keep it (peer_confirmed)
 *   confirm_code() [after user OK]
 *     → SEND_COMPLETE: CONFIRM (hash) ────►   handle (CONFIRM) — hash checked
 *                      then AEAD(secret) ─►   handle (COMPLETE)
 *     → tick() returns NOTIFY_PAIRED          → decrypt → NOTIFY_PAIRED
 *
 *   A CONFIRM counts only from the partner's address and only once the
 *   code is shown (AWAITING_CONFIRM or later). Until F97 a CONFIRM was
 *   taken only after this side's own owner confirmed and was sent once,
 *   so in either order one was dropped: the joiner's owner first left both
 *   sides waiting for the 5-minute timeout, and the initiator's owner first
 *   left the initiator PAIRED and the joiner dropping the COMPLETE.
 *
 *   An updated joiner takes the COMPLETE without the CONFIRM in front of
 *   it. That CONFIRM is for a joiner on firmware before F97, which reads a
 *   CONFIRM only after its own owner confirmed and takes a COMPLETE only
 *   after such a CONFIRM: with it, an updated initiator pairs such a joiner
 *   in either order; without it, the initiator would report PAIRED and the
 *   joiner drop the COMPLETE. canary-wap (F75) sends the COMPLETE alone,
 *   because its receive buffer holds one frame; this tree's transport ring
 *   holds eight.
 *
 *   The other mix (F117): an initiator on firmware before F97 drops a
 *   CONFIRM that arrives before its own owner confirms, and answers only a
 *   CONFIRM read after it, with a COMPLETE alone. So when the joiner's
 *   owner confirms first, the joiner's one CONFIRM was dropped and both
 *   sides timed out. An updated joiner now re-sends its CONFIRM once it has
 *   read the initiator's CONFIRM after its own owner confirmed (moving to
 *   AWAITING_COMPLETE): that CONFIRM is the sign the initiator's owner has
 *   confirmed too, so a pre-F97 initiator is now waiting for exactly this.
 *   tick() sends it CONFIRM_RESEND_FIRST_MS after that, then every
 *   CONFIRM_RESEND_INTERVAL_MS, at most CONFIRM_RESEND_MAX times, only to
 *   the partner and only while no COMPLETE has come. An updated initiator
 *   sends its COMPLETE right behind its CONFIRM, so between two updated
 *   devices the COMPLETE lands first and nothing is re-sent; one that came
 *   late would reach an initiator already PAIRED, which drops it.
 *
 *   Joiner's owner first, initiator on firmware before F97:
 *
 *                                            confirm_code() [after user OK]
 *   (dropped: owner not confirmed) ◄────────  → SEND_CONFIRM (hash)
 *   confirm_code() [after user OK]
 *     → SEND_CONFIRM (hash) ─────────────►   handle (CONFIRM) — hash checked
 *                                              → AWAITING_COMPLETE
 *                                            tick() [+CONFIRM_RESEND_FIRST_MS]
 *   handle (CONFIRM) ◄──────────────────────  → SEND_CONFIRM (hash) again
 *     → SEND_COMPLETE (alone) ───────────►   handle (COMPLETE) → NOTIFY_PAIRED
 *
 *   A lost COMPLETE (F134, canary-wap's F100 shape): nothing on the wire
 *   acknowledges a COMPLETE, and the initiator reports PAIRED once it sent
 *   one. So it keeps the frames it sent — its leading CONFIRM and the
 *   sealed COMPLETE, the bytes already on the air; the session key stays
 *   wiped — and tick() sends them again every COMPLETE_RESEND_INTERVAL_MS,
 *   to the partner only, for at most COMPLETE_RESEND_WINDOW_MS after the
 *   first send (the joiner's own wait ends before that), or until the
 *   integration layer calls stop_complete_resend() (it heard the joiner, or
 *   the joiner is no longer a member, or the opera changed). A joiner that
 *   took the COMPLETE drops the copies (PAIRED); one still waiting, in
 *   AWAITING_CONFIRM_PEER or AWAITING_COMPLETE, takes the next one. A
 *   joiner's CONFIRM reaching a PAIRED initiator is still dropped and
 *   prompts no copy: the next timed copy is at most 2 s away, and the
 *   CONFIRM authenticates nothing a radio in range could not replay or
 *   reflect (F94). Until F134 the COMPLETE went once; one lost on the air
 *   left the initiator holding a member that never joined, while the
 *   joiner re-sent its CONFIRM (F117) to an initiator that dropped it and
 *   timed out.
 *
 * On any failure or 5-minute timeout, both sides transition to FAILED
 * and the integration layer is told via NOTIFY_FAILED, with the reason
 * (Action::fail_reason).
 *
 *   A device that cannot hold its partner fails the pairing (spec §5.2,
 *   F118): the PartnerGate the integration layer passed to start_* is
 *   asked at this side's owner's confirm, before its CONFIRM goes out; on
 *   the initiator again before the opera_secret is sealed into the
 *   COMPLETE; on the joiner again before a COMPLETE is opened. A refusal
 *   is NOTIFY_FAILED (PARTNER_REFUSED) and nothing is sent. So a refusing
 *   joiner sends no CONFIRM and its initiator seals nothing to it and
 *   times out, and a refusing initiator seals and sends nothing. Until
 *   F118 nothing was asked: the initiator sealed the secret first, both
 *   sides reported PAIRED, and the session's bind of the partner's address
 *   (an address another member holds, a full table) failed after that, so
 *   the partner became a member heard from nowhere and sent nothing.
 *
 * Key lifecycle:
 *   • Ephemeral keypair: generated at start_*, wiped on terminate.
 *   • Session key: derived from x25519 once both ephemeral pubs are
 *     known, used to AEAD-encrypt the opera_secret on the COMPLETE
 *     message, wiped on terminate AND on PAIRED transition.
 *   • The sent COMPLETE (initiator, F134): the sealed frame and the
 *     leading CONFIRM as they went on the air, kept for the copies and
 *     wiped when they stop (stop_complete_resend, the window's end,
 *     context_init). It opens only under the session key, which is wiped.
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
  AWAITING_CONFIRM_PEER,    /* both:      owner confirmed, sent our CONFIRM hash; the initiator
                               awaits the joiner's CONFIRM, the joiner takes a COMPLETE here too */
  AWAITING_COMPLETE,        /* joiner:    sent CONFIRM and checked the initiator's, awaiting the
                               encrypted opera_secret */
  PAIRED,                   /* terminal:  success — opera_secret available on joiner */
  FAILED,                   /* terminal:  any error path or 5-min timeout */
};

/* ──────────────────────────────────────────────────────────────────────────
 * UI STATE MAPPING  (PR-8 — Mesh REST API)
 *
 * The web UI's opera panel + the HA pairing wizard both read a single
 * `state` string from GET /api/mesh and branch on it. The exact strings
 * the UI checks (securacv_webui.cpp refreshOpera / startPairingPoll) are:
 *
 *   "DISABLED"        — mesh switched off (POST /api/mesh/enable; F10)
 *   "NO_OPERA"        — mesh enabled, but no opera joined yet
 *   "CONNECTING"      — opera joined, no peers online yet
 *   "ACTIVE"          — opera joined, ≥1 peer online
 *   "PAIRING_INIT"    — pairing in progress, initiator side (pre-confirm)
 *   "PAIRING_JOIN"    — pairing in progress, joiner side (pre-confirm)
 *   "PAIRING_CONFIRM" — both sides have the 6-digit code, awaiting user OK
 *
 * mesh_state_name() resolves these from the inputs the status handler has
 * on hand. It is a pure function (no I/O, no globals) so host tests can
 * pin the mapping without standing up the whole network stack. The
 * pairing-state precedence is: an in-progress pairing flow always wins
 * over the steady-state DISABLED/NO_OPERA/CONNECTING/ACTIVE classification,
 * because the UI swaps to the pairing screen whenever state starts with
 * "PAIRING".
 * ────────────────────────────────────────────────────────────────────────── */

const char* mesh_state_name(bool        enabled,
                            bool        has_opera,
                            State       pairing_state,
                            size_t      peers_online);

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

/* Why a pairing ended in FAILED (F118). Every NOTIFY_FAILED carries one
 * (Action::fail_reason), and the context keeps it (PairingContext::
 * fail_reason) until the next start_*. */
enum class FailReason : uint8_t {
  NONE = 0,          /* not failed */
  CANCELED,          /* cancel(): the owner, a disable, a leave */
  TIMEOUT,           /* PAIRING_TIMEOUT_MS passed */
  BAD_CONFIRM,       /* the partner's CONFIRM hash did not match */
  BAD_COMPLETE,      /* the COMPLETE did not open under the session key */
  CRYPTO,            /* key generation, derivation or sealing failed */
  PARTNER_REFUSED,   /* this device cannot hold the partner (PartnerGate, F118) */
};

/* A short lowercase name for logs ("partner_refused"). */
const char* fail_reason_name(FailReason r);

/* May this device hold the partner — `peer_pubkey` at `peer_mac` — as a
 * member (F118; spec §5.2: a device that cannot hold its partner fails the
 * pairing)? The integration layer answers from its own tables:
 * mesh_session's answers false for a deny-listed key, a new member for a
 * full opera, an address another member holds, or an address the transport
 * has no room for — exactly the cases in which it could not register the
 * partner and bind it to that address once the pairing completed. Asked at
 * this device's owner's confirm, before any CONFIRM goes out (both roles);
 * on the initiator again before the opera_secret is sealed into the
 * COMPLETE; on the joiner again before a COMPLETE is opened. A false ends
 * the pairing (FailReason::PARTNER_REFUSED). nullptr admits everyone (the
 * pure state-machine tests). */
using PartnerGate = bool (*)(const uint8_t peer_pubkey[mesh_crypto::PUBKEY_LEN],
                             const uint8_t peer_mac[6]);

constexpr size_t MAX_ACTION_PAYLOAD =
    sizeof(PairCompletePayload) > sizeof(PairOfferPayload) ?
    sizeof(PairCompletePayload) : sizeof(PairOfferPayload);

struct Action {
  ActionType type;
  uint8_t    peer_mac[6];                          /* destination MAC, all-FF for broadcast */
  uint8_t    payload[MAX_ACTION_PAYLOAD];          /* raw payload bytes to send */
  size_t     payload_len;                          /* 0 if no payload */
  uint32_t   confirmation_code;                    /* non-zero for NOTIFY_CODE_READY,
                                                      and for the joiner's SEND_ACCEPT
                                                      (its code-derivation beat, F49) */
  /* SEND_COMPLETE only (F97): the initiator's own PAIR_CONFIRM, to go to
   * peer_mac immediately before the COMPLETE, as its own frame. Every
   * COMPLETE carries one. An updated joiner does not need it; a joiner on
   * firmware before F97 takes a COMPLETE only after reading the
   * initiator's CONFIRM once its own owner has confirmed, so without this
   * CONFIRM it drops the COMPLETE while the initiator reports PAIRED.
   * Built before the session key is wiped. */
  bool               leading_confirm_present;
  PairConfirmPayload leading_confirm;
  /* NOTIFY_FAILED only: why (F118). NONE on every other action. */
  FailReason         fail_reason;
};

/* Pairing timeout. Matches canary-wap (5 min). Crossing this fires
 * a FAILED transition + NOTIFY_FAILED action. */
constexpr uint32_t PAIRING_TIMEOUT_MS = 5 * 60 * 1000;

/* The joiner's CONFIRM re-send (F117, above): the first one this long after
 * it read the initiator's CONFIRM in AWAITING_CONFIRM_PEER, then one every
 * interval, at most CONFIRM_RESEND_MAX in all, while it waits in
 * AWAITING_COMPLETE. The delay lets an updated initiator's COMPLETE, sent
 * right behind its CONFIRM, land first; the bound keeps a joiner whose
 * initiator never answers from repeating itself to the timeout. */
constexpr uint32_t CONFIRM_RESEND_FIRST_MS    = 1000;
constexpr uint32_t CONFIRM_RESEND_INTERVAL_MS = 2000;
constexpr uint8_t  CONFIRM_RESEND_MAX         = 3;

/* The initiator's COMPLETE copies (F134, above): one every interval after
 * the first send, for at most the window after it — the pairing timeout,
 * which the joiner's own wait (from its start_joiner, before any COMPLETE)
 * cannot outlast. At most 150 copies, each the leading CONFIRM (33 bytes on
 * the wire) and the COMPLETE (61), unless stop_complete_resend() ends them
 * first. canary-wap's F100 uses the same 2 s over its own (2-minute)
 * pairing timeout. */
constexpr uint32_t COMPLETE_RESEND_INTERVAL_MS = 2000;
constexpr uint32_t COMPLETE_RESEND_WINDOW_MS   = PAIRING_TIMEOUT_MS;

/* Per-context state. Treat as opaque from the integration side — only
 * the API below should touch fields. About 440 B on the x86-64 host build
 * since F134's kept COMPLETE (336 before it); the session keeps its one
 * context in static storage, and the host tests hold two on the stack. */
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

  /* Initiator only (F97): the joiner's CONFIRM arrived, from the partner's
   * address and with the right hash, before this device's own owner
   * confirmed. confirm_code() then returns the SEND_COMPLETE (its own
   * CONFIRM rides in front of it, as with every COMPLETE). Cleared by
   * fail() and at PAIRED. */
  bool     peer_confirmed;

  /* One-shot flag: set when the initiator transitions to PAIRED after
   * SEND_COMPLETE. The next tick() reads it, clears it, and returns
   * NOTIFY_PAIRED so the integration layer gets a definitive success
   * signal on the initiator side (the joiner gets it inline when
   * COMPLETE decrypts). */
  bool     pending_notify_paired;

  /* Joiner only (F117): the CONFIRM re-send to a pre-F97 initiator. Armed
   * when the initiator's CONFIRM is read in AWAITING_CONFIRM_PEER;
   * confirm_resend_ms is when it was armed or last re-sent, and
   * confirm_resends how many went. tick() sends them. */
  bool     confirm_resend_armed;
  uint8_t  confirm_resends;
  uint32_t confirm_resend_ms;

  /* F118: the integration layer's admission check, set at start_* (see
   * PartnerGate). nullptr admits. */
  PartnerGate partner_gate;

  /* Why the pairing is FAILED; NONE until then (F118). */
  FailReason  fail_reason;

  /* Initiator only (F134): the COMPLETE it sent and the CONFIRM in front
   * of it, byte for byte, kept for the copies tick() sends while
   * complete_resend_armed. complete_first_ms is the first send,
   * complete_last_ms the last one, complete_copies how many copies went
   * (not counting the first). Wiped when the copies stop. */
  bool                complete_resend_armed;
  PairCompletePayload kept_complete;
  PairConfirmPayload  kept_confirm;
  uint32_t            complete_first_ms;
  uint32_t            complete_last_ms;
  uint16_t            complete_copies;
};

/* ──────────────────────────────────────────────────────────────────────────
 * STATE-MACHINE API
 *
 * All entry points are pure: take ctx by reference, mutate state, and
 * return at most one Action describing what the integration layer
 * should do next. ActionType::NONE means "no I/O this call".
 * ────────────────────────────────────────────────────────────────────────── */

/* Initialize a context. State becomes IDLE. */
void context_init(PairingContext& ctx);

/* Begin pairing as the initiator (an existing opera member who has a
 * valid opera_secret). Generates an ephemeral keypair and returns an
 * Action with type=BROADCAST_DISCOVER. `gate` is asked before this side
 * confirms and before it seals the opera_secret (PartnerGate, F118). */
Action start_initiator(PairingContext& ctx,
                       const uint8_t  device_pub[mesh_crypto::PUBKEY_LEN],
                       const uint8_t  device_priv[mesh_crypto::PRIVKEY_LEN],
                       const uint8_t  opera_secret[mesh_crypto::OPERA_SECRET_LEN],
                       const char*    opera_name,
                       uint32_t       now_ms,
                       PartnerGate    gate = nullptr);

/* Begin pairing as the joiner (a new device with no opera_secret yet).
 * State becomes DISCOVERING_JOINER and a DISCOVER(role=JOINER) goes out.
 * `gate` is asked before this side confirms and before it opens a
 * COMPLETE (PartnerGate, F118). */
Action start_joiner(PairingContext& ctx,
                    const uint8_t  device_pub[mesh_crypto::PUBKEY_LEN],
                    const uint8_t  device_priv[mesh_crypto::PRIVKEY_LEN],
                    uint32_t       now_ms,
                    PartnerGate    gate = nullptr);

/* Feed an incoming message into the state machine. msg_type identifies
 * the PAIR_* phase (see message types below). Returns the next action;
 * if the message is unexpected for the current state, returns NONE
 * (silently dropped) — pairing isn't aborted on every stray frame
 * because the same MAC may also be running heartbeat/etc traffic. */
/* The wire type byte of each pairing frame — the registry's values
 * (mesh_wire.h, spec §4.5: 8..12, canary-wap's numbering, chosen because a
 * pairing type must never equal a version byte). v0.4 moved this tree from
 * 0..4; the frame is [type][raw payload struct] as before. */
enum class MsgType : uint8_t {
  DISCOVER = mesh_wire::PAIR_DISCOVER,
  OFFER    = mesh_wire::PAIR_OFFER,
  ACCEPT   = mesh_wire::PAIR_ACCEPT,
  CONFIRM  = mesh_wire::PAIR_CONFIRM,
  COMPLETE = mesh_wire::PAIR_COMPLETE,
};

Action receive(PairingContext& ctx,
               const uint8_t  from_mac[6],
               MsgType        msg_type,
               const uint8_t* payload, size_t payload_len,
               uint32_t       now_ms);

/* Periodic tick from the main loop. now_ms is the current monotonic
 * time. Returns NOTIFY_FAILED if the 5-minute timeout has elapsed
 * since start_*; on an initiator the deferred NOTIFY_PAIRED, then each due
 * copy of its COMPLETE (SEND_COMPLETE with its leading CONFIRM, the bytes
 * first sent, to the partner; F134); on a joiner waiting for its COMPLETE
 * a due CONFIRM re-send (SEND_CONFIRM, F117); NONE otherwise. receive()'s
 * and confirm_code()'s now_ms must come from the same clock. */
Action tick(PairingContext& ctx, uint32_t now_ms);

/* F134: end the initiator's COMPLETE copies and wipe the kept frames. The
 * integration layer calls it once the copies are no longer wanted: it heard
 * the joiner (a verified frame of its own: it holds the opera_secret), the
 * joiner is no longer a member at the address it paired from, or the opera
 * the COMPLETE carried was left or rotated. Returns whether copies were
 * running. The pairing stays PAIRED. */
bool stop_complete_resend(PairingContext& ctx);

/* F134: is the initiator still sending copies of its COMPLETE? */
bool complete_resend_running(const PairingContext& ctx);

/* User-driven confirmation that the 6-digit code matches on both
 * screens. Valid only in AWAITING_CONFIRM (a second call returns NONE).
 * Returns SEND_CONFIRM, or — on an initiator that already holds the
 * joiner's verified CONFIRM (F97) — SEND_COMPLETE (with its leading
 * CONFIRM), after which the next tick() returns NOTIFY_PAIRED and the
 * copies begin (F134; now_ms is their first send). Returns
 * NOTIFY_FAILED (PARTNER_REFUSED) instead, sending nothing, when the
 * context's PartnerGate refuses the partner (F118). */
Action confirm_code(PairingContext& ctx, uint32_t now_ms);

/* Abort a running pairing: FAILED (CANCELED), the ephemeral key, session
 * key and opera_secret wiped, and NOTIFY_FAILED returned so the
 * integration layer tears down its UI. A pairing that has already ended,
 * PAIRED or FAILED, or never started (IDLE), is left as it is and NONE is
 * returned (F135): a cancel that lands after the initiator's COMPLETE went
 * out no longer turns a PAIRED context FAILED, so its NOTIFY_PAIRED still
 * fires, and a FAILED one does not report its failure twice. */
Action cancel(PairingContext& ctx);

/* Read out and zero the joiner's received opera_secret. Returns true
 * if there was a secret to consume; the caller (integration layer)
 * persists it to NVS (subject to its own flash-encryption gate per
 * canary-wap audit O2). Idempotent — calling again returns false. */
bool consume_opera_secret(PairingContext& ctx,
                          uint8_t out[mesh_crypto::OPERA_SECRET_LEN]);

}  /* namespace mesh_pairing */

#endif  /* SECURACV_MESH_PAIRING_H */
