/*
 * SecuraCV Canary — Beacon Channel (Neighborhood Harm-Reduction Network)
 *
 * Higher-trust, narrowly-scoped, supervised-health broadcast layer.
 * Sits next to Chirp on the same ESP-NOW radio but enforces a fundamentally
 * different trust model:
 *
 *   - Persistent device Ed25519 identity (NOT ephemeral session keys).
 *   - Two-pubkey cryptographic co-signing required for every origination.
 *   - Narrow life-safety-only template set (~13 templates).
 *   - NFPA-72-style supervised health state surface
 *     (Normal / Trouble / Alarm / Supervisory).
 *   - CAP-aligned wire fields (severity, urgency, certainty, msgType).
 *   - Audit log of every received frame, chain-hashed and signed.
 *
 * Magic byte: 0xB1 (distinct from Chirp 0xC4).
 *
 * See spec/beacon_channel_v0.md for the full specification and
 * spec/beacon_cap_gateway_v0.md for the deferred CAP interop layer.
 *
 * Status: scaffolding (v0.1). Full integration with the application loop and
 * REST surface is gated behind a separate FEATURE_BEACON_CHANNEL build flag
 * (default OFF). When wired up, the REST endpoints MUST follow the
 * Bearer-token template trampoline pattern used by /api/mesh/* and
 * /api/bluetooth/*.
 */

#ifndef SECURACV_BEACON_CHANNEL_H
#define SECURACV_BEACON_CHANNEL_H

#include <Arduino.h>
#include "build_config.h"
#include "log_level.h"

#ifndef FEATURE_BEACON_CHANNEL
#define FEATURE_BEACON_CHANNEL 0
#endif

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

// ════════════════════════════════════════════════════════════════════════════
// CALLBACKS
// ════════════════════════════════════════════════════════════════════════════

typedef void (*BeaconAlarmCallback)(const BeaconAlertCanonical* alert);
typedef void (*BeaconStateCallback)(BeaconState old_state, BeaconState new_state);
typedef void (*BeaconCosignRequestCallback)(const BeaconAlertCanonical* requested,
                                            const uint8_t* originator_fp);

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_BEACON_CHANNEL

// Initialize the Beacon channel (call once at boot).
// device_privkey/pubkey are the device's persistent Ed25519 identity (same
// keys used by Opera and witness records). beacon_channel does NOT generate
// its own keys.
bool init(const uint8_t* device_privkey, const uint8_t* device_pubkey,
          const char* device_name);

// Shutdown.
void deinit();

// Enable / disable.
void set_enabled(bool enabled);
bool is_enabled();

// Main-loop tick (handles state transitions, self-test cadence, trouble
// detection, expired alarms).
void update();

// Status (state, trouble reasons, active alarm).
BeaconStatus get_status();
const char* state_name(BeaconState state);

// ── Beacon set management ──
uint8_t get_beacon_set_size();
const BeaconSetEntry* get_beacon_set_entry(uint8_t index);
bool revoke_beacon_set_entry(const uint8_t* fingerprint);

// v0.5: auto-revoke a Beacon-set member when their device sends a tamper
// alert (or a local tamper sensor fires on a paired neighbor's Opera
// channel). Called from mesh_network.cpp::handle_tamper_alert. The lookup
// computes the Beacon set's 16-byte fingerprint from the device's full
// Ed25519 pubkey and revokes the entry if present.
//
// Returns true if a matching beacon set entry was found and revoked
// (false if the device isn't paired into our Beacon set — no action
// needed). Idempotent; safe to call multiple times.
//
// Threading: same single-task invariant as the rest of beacon_channel.
// Called from the mesh_network message-dispatch path, which runs on the
// main loop task (see mesh_network.cpp::update() and the g_rx_pending
// queue model).
bool on_peer_tampered(const uint8_t* device_pubkey);

// Pairing flow.
bool start_pair_init();
bool start_pair_join();
bool confirm_pair();
void cancel_pair();
bool is_pairing();

// ── Origination flow (two-pubkey co-signing) ──
//
// Step 1: user A holds-to-send → originate_alert() builds the canonical,
// signs with the local device key, and broadcasts a COSIGN_REQ to nearby
// paired devices.
// Returns false if rate-limited, presence requirement not met, no paired
// cosigner candidate available, or time not synced.
bool originate_alert(BeaconTemplate template_id, BeaconUrgency urgency,
                     BeaconSeverity severity, BeaconCertainty certainty,
                     BeaconDetailSlot detail, uint32_t ttl_minutes);

// Step 2 (on the cosigner): respond to a pending COSIGN_REQ. The user has
// confirmed the alarm. The local device signs the canonical and emits a
// COSIGN_RESP back to the originator.
bool cosign_pending_request(bool confirm);

// Solo-degraded origination path (spec §6.2). For genuinely single-device
// households that have no paired Beacon-set neighbor.
//
// Caller MUST:
//   - Have the physical BOOT button held DOWN at the moment of this call
//     (firmware checks the GPIO state in real time).
//   - Have passed the hold-to-send UI interaction (the REST handler
//     enforces this).
//
// Frame produced:
//   - originator_fp == cosigner_fp == this device's fingerprint
//   - flags |= BCN_FLAG_SOLO_ORIGIN
//   - certainty = BCN_CERT_OBSERVED (regardless of the parameter; the
//     spec forbids elevation of solo frames)
//   - single Ed25519 signature, copied into both signature slots
//
// Returns false if:
//   - BOOT button is not currently held
//   - device key self-test fails
//   - rate limit exceeded
//   - time is not wall-clock-synced
bool originate_alert_solo(BeaconTemplate template_id, BeaconUrgency urgency,
                          BeaconSeverity severity, BeaconDetailSlot detail,
                          uint32_t ttl_minutes);

// Returns the GPIO state of the BOOT button as a real-time check (no
// debouncing — solo origination is a held-button-while-holding-to-send
// pattern that takes ~2 s, swamping any switch bounce). Implementation
// reads GPIO 0 on ESP32-S3 (the standard BOOT pin); other boards may
// remap via beacon_set_boot_button_gpio(uint8_t).
bool boot_button_held();
void set_boot_button_gpio(uint8_t gpio);

// Step 3 (back on originator, automatic): receive COSIGN_RESP, verify, emit
// the dual-signed BEACON_MSG_ALERT at hop 0.

// Cancel the currently active alarm originated locally.
bool cancel_active_alarm();

// ── Active alarm + audit log ──
bool has_active_alarm();
const BeaconAlertCanonical* get_active_alarm();
size_t get_audit_log_count();
const BeaconAuditEntry* get_audit_log_entry(size_t index);

// ── Self-test ──
// Force a self-test emission (mostly for tests / on-demand operator check).
bool emit_selftest();

// ── Callbacks ──
void set_alarm_callback(BeaconAlarmCallback callback);
void set_state_callback(BeaconStateCallback callback);
void set_cosign_request_callback(BeaconCosignRequestCallback callback);

// Dispatch an ESP-NOW frame received by the shared mesh callback.
void dispatch_espnow_message(const uint8_t* mac, const uint8_t* data,
                             int len, int8_t rssi_dbm);

#endif // FEATURE_BEACON_CHANNEL

} // namespace beacon_channel

#endif // SECURACV_BEACON_CHANNEL_H
