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

}  /* namespace mesh_pairing */

#endif  /* SECURACV_MESH_PAIRING_H */
