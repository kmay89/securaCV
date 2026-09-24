/*
 * SecuraCV Canary — Beacon Channel wire format (Arduino-free)
 *
 * The constants, enums and packed-by-layout structs every Beacon frame is
 * built from, split out of beacon_channel.h so a host g++ run can include the
 * REAL definitions instead of re-declaring them (tests_host/
 * test_beacon_cancel_origination.cpp pins their sizes, and a moved or edited
 * field fails there before it ever reaches a radio). stdint/stddef only — no
 * Arduino.h, no ESP-IDF, no crypto.
 *
 * beacon_channel.h includes this file in the same namespace, so every call
 * site keeps spelling these beacon_channel::BEACON_MSG_CANCEL and so on. The
 * wire format itself is spec/beacon_channel_v0.md §5; changing anything here
 * is a wire-format change.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_BEACON_WIRE_H
#define SECURACV_BEACON_WIRE_H

#include <stddef.h>
#include <stdint.h>

namespace beacon_channel {

// ════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

static const uint8_t  PROTOCOL_VERSION              = 1;
static const uint8_t  BEACON_MAGIC                  = 0xB1;
static const uint8_t  MAX_BEACON_SET                = 32;
static const uint8_t  MAX_HOP_COUNT                 = 3;
static const uint32_t COSIGN_WINDOW_MS              = 60000;
static const uint32_t COSIGN_FRESHNESS_MS           = 600000;
static const uint32_t BEACON_FRESHNESS_S            = 300;
static const uint32_t SELFTEST_INTERVAL_MS          = 86400000;   // 24 h
static const uint32_t SELFTEST_MISSING_MS           = 129600000;  // 36 h
static const uint8_t  MAX_ORIGINATIONS_PER_PUBKEY_24H = 5;
static const uint8_t  MAX_ORIGINATIONS_PER_PAIR_24H = 8;
static const uint8_t  MAX_RELAYS_PER_MINUTE         = 5;
static const uint32_t POST_ALARM_REFLECT_MS         = 3600000;    // 1 h
static const uint32_t MIN_UNIX_TIME                 = 1700000000;
static const size_t   DEVICE_PUBKEY_SIZE            = 32;
static const size_t   DEVICE_PRIVKEY_SIZE           = 32;
static const size_t   DEVICE_FP_SIZE                = 16;
static const size_t   BEACON_NONCE_SIZE             = 16;
static const size_t   BEACON_SIGNATURE_SIZE         = 64;
static const size_t   BEACON_NAME_LEN               = 24;

// ════════════════════════════════════════════════════════════════════════════
// ENUMS
// ════════════════════════════════════════════════════════════════════════════

// Flag bits packed into BeaconHeader.flags. Treat as bit-OR-able values.
//   bit 0 (BCN_FLAG_IS_EXERCISE): drill, not a real alert
//   bit 1 (BCN_FLAG_IS_TEST): system test (vs CAP status=Test)
//   bit 2 (BCN_FLAG_SOLO_ORIGIN): single-device origination — see §6.2
//     of spec/beacon_channel_v0.md. The cosigner is the BOOT button on
//     the same device; cosigner_fp equals originator_fp; certainty is
//     forced to BCN_CERT_OBSERVED so receivers can visibly downweight.
//     Solo frames are accepted by receivers in lieu of the standard
//     "originator != cosigner" rule, but only when the certainty
//     constraint holds.
static const uint8_t BCN_FLAG_IS_EXERCISE  = 0x01;
static const uint8_t BCN_FLAG_IS_TEST      = 0x02;
static const uint8_t BCN_FLAG_SOLO_ORIGIN  = 0x04;

enum BeaconMsgType : uint8_t {
  BEACON_MSG_ALERT        = 0,
  BEACON_MSG_UPDATE       = 1,
  BEACON_MSG_CANCEL       = 2,
  BEACON_MSG_EXERCISE     = 3,
  BEACON_MSG_SELFTEST_OK  = 4,
  BEACON_MSG_PAIR_OFFER   = 5,
  BEACON_MSG_REVOKE       = 6,
  // v0.3: cosign request / response. Encrypted to the candidate cosigner
  // with ChaCha20-Poly1305 keyed by X25519 ECDH between device pubkeys
  // (spec/beacon_channel_v0.md §6.3). The plaintext is the full
  // BeaconAlertCanonical the originator wants the cosigner to sign;
  // ciphertext + 12-byte nonce + 16-byte poly1305 tag travel inside the
  // request body.
  BEACON_MSG_COSIGN_REQ   = 7,
  BEACON_MSG_COSIGN_RESP  = 8,
};

// Encrypted COSIGN_REQ wire body. The full canonical (≤128 B) is
// ChaCha20-Poly1305 encrypted to the candidate cosigner's pubkey.
struct BeaconCosignRequestPayload {
  uint8_t originator_fp[16];                  // who's asking
  uint8_t candidate_cosigner_fp[16];          // who's being asked
  uint8_t nonce[12];                          // ChaCha20 nonce (random)
  uint8_t tag[16];                            // Poly1305 auth tag
  uint16_t ciphertext_len;                    // length of encrypted canonical
  uint8_t ciphertext[160];                    // encrypted BeaconAlertCanonical
  uint8_t originator_signature[64];           // Ed25519(orig_priv, canonical)
};

// COSIGN_RESP wire body. The cosigner returns its signature over the same
// canonical. Encrypted symmetrically so a third party can't substitute.
struct BeaconCosignResponsePayload {
  uint8_t originator_fp[16];
  uint8_t cosigner_fp[16];
  uint8_t nonce[12];
  uint8_t tag[16];
  uint8_t ciphertext[80];                     // 64-byte signature + 16-byte mac slot
  uint8_t accept;                              // 1 = accepted + signed, 0 = declined
};

// CAP-aligned enums.
enum BeaconUrgency : uint8_t {
  BCN_URG_IMMEDIATE = 0,
  BCN_URG_EXPECTED  = 1,
  BCN_URG_FUTURE    = 2,
  BCN_URG_PAST      = 3,
  BCN_URG_UNKNOWN   = 4,
};

enum BeaconSeverity : uint8_t {
  BCN_SEV_EXTREME  = 0,
  BCN_SEV_SEVERE   = 1,
  BCN_SEV_MODERATE = 2,
  BCN_SEV_MINOR    = 3,
  BCN_SEV_UNKNOWN  = 4,
};

enum BeaconCertainty : uint8_t {
  BCN_CERT_OBSERVED = 0,
  BCN_CERT_LIKELY   = 1,
  BCN_CERT_POSSIBLE = 2,
  BCN_CERT_UNLIKELY = 3,
  BCN_CERT_UNKNOWN  = 4,
};

enum BeaconScope : uint8_t {
  BCN_SCOPE_PRIVATE = 2,  // Always 2 for Beacon. Per spec; lint-enforced.
};

enum BeaconTrustLevel : uint8_t {
  BCN_TRUST_COSIGNER = 0,
  BCN_TRUST_GATEWAY  = 1,
  BCN_TRUST_REVOKED  = 2,
};

// Beacon templates (life-safety only).
// IDs deliberately overlap with Chirp template IDs where the semantic event is
// the same; the magic byte (0xB1 vs 0xC4) discriminates the channel.
enum BeaconTemplate : uint8_t {
  BCN_INFRA_POWER_OUT          = 0x10,
  BCN_INFRA_GAS_SMELL          = 0x12,
  BCN_EMERG_FIRE_VISIBLE       = 0x20,
  BCN_EMERG_MEDICAL_SCENE      = 0x21,
  BCN_EMERG_MULTIPLE_AMBULANCE = 0x22,
  BCN_EMERG_EVACUATION         = 0x23,
  BCN_EMERG_SHELTER_IN_PLACE   = 0x24,
  BCN_WX_SEVERE_WARNING        = 0x30,
  BCN_WX_TORNADO               = 0x31,
  BCN_WX_FLOOD                 = 0x32,
  BCN_CLR_RESOLVED             = 0x80,
  BCN_CLR_SAFE                 = 0x81,
  BCN_CLR_FALSE_ALARM          = 0x82,
  BCN_TPL_INVALID              = 0xFF,
};

// NFPA-72 publicly visible state.
enum BeaconState : uint8_t {
  BEACON_STATE_NORMAL       = 0,
  BEACON_STATE_TROUBLE      = 1,
  BEACON_STATE_ALARM        = 2,
  BEACON_STATE_SUPERVISORY  = 3,
  BEACON_STATE_DISABLED     = 4,
  BEACON_STATE_PAIR_INIT    = 5,
  BEACON_STATE_PAIR_JOIN    = 6,
};

// Detail slot (constrained, no PII).
enum BeaconDetailSlot : uint8_t {
  BCN_DETAIL_NONE             = 0,
  BCN_DETAIL_STATUS_ONGOING   = 10,
  BCN_DETAIL_STATUS_CONTAINED = 11,
  BCN_DETAIL_STATUS_SPREADING = 12,
};

// ════════════════════════════════════════════════════════════════════════════
// WIRE FORMAT
// ════════════════════════════════════════════════════════════════════════════

struct BeaconHeader {
  uint8_t  magic;          // 0xB1
  uint8_t  version;
  uint8_t  msg_type;
  uint8_t  hop_count;
  uint8_t  flags;          // bit 0: is_exercise, bit 1: is_test
  uint8_t  reserved;
  uint16_t payload_len;    // network byte order
  uint8_t  nonce[BEACON_NONCE_SIZE];
};

// Canonical alert body (signed twice).
struct BeaconAlertCanonical {
  uint64_t effective;
  uint64_t expires;
  uint8_t  template_id;
  uint8_t  msg_type;
  uint8_t  urgency;
  uint8_t  severity;
  uint8_t  certainty;
  uint8_t  scope;
  uint8_t  detail_slot;
  uint8_t  reserved;
  uint8_t  ref_canceled_nonce[BEACON_NONCE_SIZE];
  uint8_t  originator_fp[DEVICE_FP_SIZE];
  uint8_t  cosigner_fp[DEVICE_FP_SIZE];
};

struct BeaconAlertFrame {
  BeaconAlertCanonical canonical;
  uint8_t  sig_originator[BEACON_SIGNATURE_SIZE];
  uint8_t  sig_cosigner[BEACON_SIGNATURE_SIZE];
};

struct BeaconSelfTestPayload {
  // Wall clock at emission (spec §5.3). Load-bearing: without it every
  // emission signs identical bytes, so one captured frame replays forever
  // and keeps a dead neighbor inside the 36 h supervised-health window.
  uint64_t timestamp;
  uint32_t uptime_sec;
  uint16_t free_heap_kb;
  uint8_t  key_self_test_ok;
  uint8_t  reserved;
  uint8_t  device_fp[DEVICE_FP_SIZE];
  uint8_t  signature[BEACON_SIGNATURE_SIZE];
};

// Local beacon-set entry (NVS-persisted; FE-gated).
// v0.3: added `x25519_pubkey` for ECDH-encrypted COSIGN_REQ/RESP messages.
// Exchanged during pairing alongside the Ed25519 device pubkey.
struct BeaconSetEntry {
  uint8_t  device_pubkey[DEVICE_PUBKEY_SIZE];      // Ed25519 identity / signing
  uint8_t  x25519_pubkey[DEVICE_PUBKEY_SIZE];      // X25519 for ECDH (v0.3)
  uint8_t  fingerprint[DEVICE_FP_SIZE];
  char     name[BEACON_NAME_LEN + 1];
  uint64_t paired_at;
  uint64_t last_selftest;
  uint8_t  trust_level;                            // BeaconTrustLevel
  bool     valid;
  bool     has_x25519_pubkey;                      // false for legacy v0.1 entries
};

// Local audit log entry (chain-hashed, signed).
struct BeaconAuditEntry {
  uint64_t received_at;
  BeaconAlertCanonical canonical;
  uint8_t  sig_originator[BEACON_SIGNATURE_SIZE];
  uint8_t  sig_cosigner[BEACON_SIGNATURE_SIZE];
  uint8_t  hop_count;
  uint8_t  prev_audit_hash[32];
};

// Trouble reasons (bit field).
enum BeaconTroubleReason : uint16_t {
  BCN_TROUBLE_NONE                  = 0,
  BCN_TROUBLE_TIME_UNSYNCED         = 1 << 0,
  BCN_TROUBLE_AIRTIME_SATURATED     = 1 << 1,
  BCN_TROUBLE_NEIGHBOR_SELFTEST_GAP = 1 << 2,
  BCN_TROUBLE_KEY_SELFTEST_FAILED   = 1 << 3,
  BCN_TROUBLE_BEACON_SET_EMPTY      = 1 << 4,
};

struct BeaconStatus {
  BeaconState state;
  uint16_t    trouble_reasons;   // bitmask of BeaconTroubleReason
  uint8_t     beacon_set_size;
  bool        active_alarm;
  uint64_t    active_alarm_expires;
  uint8_t     active_template_id;
};

} // namespace beacon_channel

#endif // SECURACV_BEACON_WIRE_H
