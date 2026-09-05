/*
 * SecuraCV Canary — Beacon Channel implementation (skeleton)
 *
 * Status: scaffolding (v0.1). Implements the public API surface defined in
 * beacon_channel.h with the cryptographic origination, verification, and
 * NFPA-72 state-surface paths in place. Networking glue (REST endpoints,
 * MQTT discovery, audio pattern playback) is wired through but not enabled
 * by default — `FEATURE_BEACON_CHANNEL` is OFF in `build_config.h` and must
 * be explicitly turned on per build target.
 *
 * Critical security properties (per spec/beacon_channel_v0.md):
 *  - Every BEACON_MSG_ALERT requires two distinct Ed25519 signatures over
 *    the canonical alert body, from two distinct device pubkeys, both of
 *    which must be present in the local beacon set with trust_level != REVOKED.
 *  - Originator never counts itself; the cosigner must explicitly sign.
 *  - Self-test heartbeat (BEACON_MSG_SELFTEST_OK) cadenced daily; receivers
 *    surface Trouble if a known set member's selftest is absent for >36h.
 *  - All persistent state is NVS-stored behind the same flash-encryption gate
 *    used by the Opera mesh (audit O2 path).
 *  - The audit log is append-only with per-entry chain-hashing: the log of
 *    record is /beacon/audit.jsonl on SD (pure append — never truncated or
 *    rotated, per AGENTS.md Beacon invariant 9), with a 64-entry NVS ring
 *    serving as the bounded recent-view cache for the API/UI.
 *
 * Known limitations (tracked for v0.3):
 *  - The CAP gateway path is specified in spec/beacon_cap_gateway_v0.md but
 *    not implemented; gateway pubkeys with trust_level == BCN_TRUST_GATEWAY
 *    are accepted in the beacon set but the upstream-signature path is not
 *    wired.
 *  - Pairing's encrypted COSIGN_REQ channel currently uses an unencrypted
 *    broadcast for the COSIGN_REQ message; a follow-up will wrap it in a
 *    ChaCha20-Poly1305 envelope keyed by X25519 ECDH between the device
 *    pubkeys.
 */

#include "beacon_channel.h"

#if FEATURE_BEACON_CHANNEL

#include "mesh_network.h"
#include "airtime_governor.h"
#include "health_log.h"
#include "beacon_audit_recover.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_flash_encrypt.h>
#include <WiFi.h>
#include <Preferences.h>
#include <SD.h>
#include <mbedtls/sha256.h>

// While the background mount worker (hardware_state.h) is inside SD.begin(),
// the SD object's card struct is mid-initialization: SD.cardType() can read
// a garbage non-CARD_NONE value and an SD.open() here would race f_mount on
// the worker. External-linkage declaration; the definition lives in the
// sketch TU and resolves at link time.
bool sd_mount_in_flight();
#include <Ed25519.h>
#include <Curve25519.h>
#include <ChaChaPoly.h>
#include <time.h>
#include <string.h>

namespace beacon_channel {

// ════════════════════════════════════════════════════════════════════════════
// INTERNAL STATE
// ════════════════════════════════════════════════════════════════════════════

static bool g_initialized = false;
static bool g_enabled = false;
static BeaconState g_state = BEACON_STATE_DISABLED;

static uint8_t g_device_privkey[DEVICE_PRIVKEY_SIZE];
static uint8_t g_device_pubkey[DEVICE_PUBKEY_SIZE];
static uint8_t g_device_fp[DEVICE_FP_SIZE];
static char    g_device_name[BEACON_NAME_LEN + 1];
// v0.3: device's X25519 keypair for ECDH-encrypted cosign exchanges. Generated
// once at first init() and kept in RAM only (regenerable on reboot — since
// COSIGN_REQ/RESP are ephemeral within a 60 s window, persistence isn't
// required). The pubkey is published to beacon-set members at pair time.
static uint8_t g_x25519_privkey[32];
static uint8_t g_x25519_pubkey[32];
static bool    g_x25519_ready = false;

static BeaconSetEntry g_beacon_set[MAX_BEACON_SET];
static uint8_t g_beacon_set_count = 0;

static BeaconAlertCanonical g_active_alarm;
static bool g_active_alarm_valid = false;
static uint64_t g_active_alarm_expires = 0;

// Audit log: ring buffer indexed by g_audit_head (next write slot).
// Total entries valid = min(g_audit_log_count, AUDIT_LOG_MAX).
// When full, new entries overwrite the oldest (head wraps), and on disk
// we update only that single NVS slot — no full-array shuffle, so
// persistence stays in sync with RAM (gemini P1 + codex P2 closure).
static const size_t AUDIT_LOG_MAX = 64;
static BeaconAuditEntry g_audit_log[AUDIT_LOG_MAX];
static size_t g_audit_log_count = 0;     // total entries observed (caps at MAX)
static size_t g_audit_head = 0;          // next slot to write
static uint8_t g_audit_chain_head[32];

// Pending origination (waiting on cosigner).
struct PendingOrigination {
  bool valid;
  BeaconAlertCanonical canonical;
  uint8_t sig_originator[BEACON_SIGNATURE_SIZE];
  uint32_t requested_ms;
};
static PendingOrigination g_pending_origination;

// Pending inbound cosign request (waiting on local user confirmation).
struct PendingCosignRequest {
  bool valid;
  BeaconAlertCanonical canonical;
  uint8_t originator_fp[DEVICE_FP_SIZE];
  uint8_t sig_originator[BEACON_SIGNATURE_SIZE];
  uint32_t requested_ms;
};
static PendingCosignRequest g_pending_cosign_in;

// Per-pubkey origination rate-limit (audit C14 analog for Beacon).
// Drills carry a second, independent counter: AGENTS.md Beacon invariant 10
// forbids merging exercise and real-alert buckets, so a morning drill can
// never rate-limit away an afternoon fire alert from the same pubkey.
struct OriginationRate {
  uint8_t  fingerprint[DEVICE_FP_SIZE];
  uint32_t window_start_ms;
  uint8_t  count_in_window;
  uint32_t exercise_window_start_ms;
  uint8_t  exercise_count_in_window;
  bool     valid;
};
static OriginationRate g_origination_rate[MAX_BEACON_SET];

// Per-pair co-sign budget (spec §8, MAX_ORIGINATIONS_PER_PAIR_24H). Keyed on
// the two fingerprints in byte order, so (A,B) and (B,A) are one pair. The
// per-pubkey bucket above charges the originator only, so two devices that
// trade roles would otherwise hold twice that budget between them. Solo
// frames (originator == cosigner) are not a pair and stay on the per-pubkey
// bucket; drills are banked apart (invariant 10) and are not counted here.
struct PairRate {
  uint8_t  fp_lo[DEVICE_FP_SIZE];
  uint8_t  fp_hi[DEVICE_FP_SIZE];
  uint32_t window_start_ms;
  uint8_t  count_in_window;
  bool     valid;
};
static PairRate g_pair_rate[MAX_BEACON_SET];

// Seen-frame ring for receive-path replay dedup (spec §7.1 step 3; §8 sets
// the horizon at the last 5 minutes). A frame's identity is taken from its
// two signatures, never from the header nonce: the nonce sits outside
// BeaconAlertCanonical, so neither signature covers it, and a replay that
// rewrote it in flight would pass a nonce-keyed ring, verify, charge the
// originator's rate bucket and re-raise the alarm. Ed25519 signing is
// deterministic (RFC 8032), so the same canonical under the same keys
// always yields the same bytes, and nobody without those keys can mint a
// fresh identity that verifies. Checked before signature verification so a
// replay costs no Ed25519 work, and recorded only after a frame has fully
// validated, so unverifiable traffic cannot evict real entries. Origination
// is capped at a handful of frames per pubkey per day, so this cannot wrap
// within the horizon under real load.
static const size_t SEEN_FRAME_MAX = 32;
static const size_t FRAME_ID_HALF = 16;               // bytes taken from each signature
static const size_t FRAME_ID_SIZE = 2 * FRAME_ID_HALF;
struct SeenFrame {
  uint8_t  id[FRAME_ID_SIZE];
  uint32_t seen_ms;
  bool     valid;
};
static SeenFrame g_seen_frames[SEEN_FRAME_MAX];
static size_t g_seen_frame_head = 0;

// Header nonce of the frame that raised g_active_alarm. CANCEL and UPDATE
// must name it (spec §5.4, §7.2). An UPDATE amends the alarm without
// becoming its new identity, so a later CANCEL still resolves against the
// originating ALERT.
static uint8_t g_active_alarm_nonce[BEACON_NONCE_SIZE];

// Signature identity of the most recent accepted ALERT. The ring forgets
// after the freshness horizon and a CANCEL clears the alarm, but neither
// clears this, so a re-sent ALERT stays a duplicate for as long as it is the
// latest one. The case this covers that the ring cannot is a receiver whose
// clock never synced: there the freshness window (§7.1 step 4) does not run,
// and a copy held past the horizon would otherwise re-raise the alarm.
static uint8_t g_last_alert_id[FRAME_ID_SIZE];
static bool g_last_alert_id_valid = false;

// Newest signed selftest timestamp accepted per beacon-set slot. RAM only
// (spec §11). Monotonicity here is what refuses a replayed SELFTEST_OK.
static uint64_t g_last_selftest_ts[MAX_BEACON_SET];

static uint32_t g_last_selftest_ms = 0;
static uint16_t g_trouble_reasons = BCN_TROUBLE_BEACON_SET_EMPTY;

// Callbacks
static BeaconAlarmCallback g_alarm_callback = nullptr;
static BeaconStateCallback g_state_callback = nullptr;
static BeaconCosignRequestCallback g_cosign_request_callback = nullptr;

// NVS namespace
static Preferences g_prefs;
static const char* NVS_NS = "beacon";
static const char* NVS_SET_COUNT = "set_count";
static const char* NVS_SET_PREFIX = "set_";
// v0.3: audit log persistence (FE-gated, same as beacon_set).
static const char* NVS_AUDIT_COUNT = "audit_cnt";
static const char* NVS_AUDIT_PREFIX = "aud_";
static const char* NVS_AUDIT_HEAD = "aud_head";
// v0.3 codex P1 closure: persist the device's X25519 keypair so paired
// peers' stored x25519_pubkey remains valid across reboots. Without this,
// every reboot regenerated a fresh pair and broke cosign decrypt for every
// neighbor until the next pairing flow.
static const char* NVS_X25519_PRIV = "x25519_priv";
static const char* NVS_X25519_PUB  = "x25519_pub";

// ════════════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════════════════

static void set_state(BeaconState new_state);
static void compute_fingerprint(const uint8_t* pubkey, uint8_t* fp_out);
static size_t build_alert_canonical(const BeaconAlertCanonical* c,
                                    uint8_t* out, size_t out_max);
static size_t build_selftest_canonical(const BeaconSelfTestPayload* p,
                                       uint8_t* out, size_t out_max);
static const BeaconSetEntry* find_set_entry_by_fp(const uint8_t* fp);
static bool flash_encryption_enabled();
static bool persist_beacon_set();
static bool load_beacon_set();
static void recompute_trouble_reasons();
static bool rate_check_and_record(const uint8_t* fp, bool is_exercise);
static void emit_alert_frame();
static void chain_audit_entry(BeaconAuditEntry* entry);
static void on_espnow_recv(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi);
static void broadcast_message(const uint8_t* data, size_t len);

// ════════════════════════════════════════════════════════════════════════════
// FLASH ENCRYPTION GATE (shared rationale with Opera audit O2)
// ════════════════════════════════════════════════════════════════════════════

static bool flash_encryption_enabled() {
  return esp_flash_encryption_enabled();
}

// ════════════════════════════════════════════════════════════════════════════
// X25519 ECDH + ChaCha20-Poly1305 — v0.3 COSIGN_REQ/RESP envelope encryption
// ════════════════════════════════════════════════════════════════════════════

static void ensure_x25519_keypair() {
  if (g_x25519_ready) return;

  // v0.3 codex P1 closure: persist the keypair so paired peers' stored
  // x25519_pubkey stays valid across reboots. FE-gated identically to
  // beacon_set + audit log.
  if (flash_encryption_enabled()) {
    g_prefs.begin(NVS_NS, true);
    size_t got_priv = g_prefs.getBytes(NVS_X25519_PRIV, g_x25519_privkey, 32);
    size_t got_pub  = g_prefs.getBytes(NVS_X25519_PUB,  g_x25519_pubkey,  32);
    g_prefs.end();
    if (got_priv == 32 && got_pub == 32) {
      g_x25519_ready = true;
      return;
    }
  }

  // No keypair on disk (first boot, FE-off, or corruption): generate one
  // and (when FE is enabled) persist it.
  esp_fill_random(g_x25519_privkey, 32);
  // RFC 7748 scalar clamping (so eval() produces a canonical X25519 result).
  g_x25519_privkey[0]  &= 248;
  g_x25519_privkey[31] &= 127;
  g_x25519_privkey[31] |= 64;
  // X25519 basepoint = 9. eval(out, scalar, basepoint) produces the pubkey.
  static const uint8_t BASEPOINT[32] = { 9 };
  if (!Curve25519::eval(g_x25519_pubkey, g_x25519_privkey, BASEPOINT)) {
    return;
  }
  g_x25519_ready = true;

  if (flash_encryption_enabled()) {
    g_prefs.begin(NVS_NS, false);
    g_prefs.putBytes(NVS_X25519_PRIV, g_x25519_privkey, 32);
    g_prefs.putBytes(NVS_X25519_PUB,  g_x25519_pubkey,  32);
    g_prefs.end();
  } else {
    health_log(SCV_LOG_ALERT, SCV_CAT_CRYPTO,
               "beacon: X25519 keypair generated but not persisted — "
               "FE disabled. Paired peers will lose cosign capability "
               "on next reboot. (codex P1)");
  }
}

static bool ecdh_session_key(const uint8_t* their_x25519_pubkey,
                             uint8_t out_key[32]) {
  // Compute the shared secret via X25519, then HKDF-SHA256 it down to a
  // 32-byte session key with a domain-separated label.
  uint8_t shared[32];
  if (!Curve25519::eval(shared, g_x25519_privkey, their_x25519_pubkey)) {
    return false;
  }
  // Domain-separate so the same shared secret can't be cross-purposed.
  static const char LABEL[] = "securacv:beacon:cosign:v0";
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const uint8_t*)LABEL, sizeof(LABEL) - 1);
  mbedtls_sha256_update(&ctx, shared, 32);
  mbedtls_sha256_finish(&ctx, out_key);
  mbedtls_sha256_free(&ctx);
  return true;
}

// Encrypt `plaintext_len` bytes of `plaintext` to the recipient identified by
// `their_x25519_pubkey`. `nonce` (12 B) is written to the output; `tag` (16 B)
// is also written. `out_ciphertext` is at least `plaintext_len` bytes.
//
// Returns true on success, false if ECDH failed.
static bool cosign_encrypt(const uint8_t* their_x25519_pubkey,
                           const uint8_t* plaintext, size_t plaintext_len,
                           uint8_t nonce[12], uint8_t tag[16],
                           uint8_t* out_ciphertext) {
  uint8_t key[32];
  if (!ecdh_session_key(their_x25519_pubkey, key)) return false;
  esp_fill_random(nonce, 12);
  ChaChaPoly aead;
  aead.setKey(key, 32);
  aead.setIV(nonce, 12);
  aead.encrypt(out_ciphertext, plaintext, plaintext_len);
  aead.computeTag(tag, 16);
  return true;
}

static bool cosign_decrypt(const uint8_t* their_x25519_pubkey,
                           const uint8_t* ciphertext, size_t ciphertext_len,
                           const uint8_t nonce[12], const uint8_t tag[16],
                           uint8_t* out_plaintext) {
  uint8_t key[32];
  if (!ecdh_session_key(their_x25519_pubkey, key)) return false;
  ChaChaPoly aead;
  aead.setKey(key, 32);
  aead.setIV(nonce, 12);
  aead.decrypt(out_plaintext, ciphertext, ciphertext_len);
  return aead.checkTag(tag, 16);
}

// ════════════════════════════════════════════════════════════════════════════
// HELPERS
// ════════════════════════════════════════════════════════════════════════════

static void set_state(BeaconState new_state) {
  if (g_state == new_state) return;
  BeaconState old = g_state;
  g_state = new_state;
  if (g_state_callback) g_state_callback(old, new_state);
}

static void compute_fingerprint(const uint8_t* pubkey, uint8_t* fp_out) {
  mbedtls_sha256_context ctx;
  uint8_t hash[32];
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, pubkey, DEVICE_PUBKEY_SIZE);
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  memcpy(fp_out, hash, DEVICE_FP_SIZE);
}

static size_t build_alert_canonical(const BeaconAlertCanonical* c,
                                    uint8_t* out, size_t out_max) {
  static const char DOMAIN[] = "securacv:beacon:canonical:v0";
  const size_t domain_len = sizeof(DOMAIN) - 1;
  if (out_max < domain_len + sizeof(BeaconAlertCanonical)) return 0;
  memcpy(out, DOMAIN, domain_len);
  memcpy(out + domain_len, c, sizeof(BeaconAlertCanonical));
  return domain_len + sizeof(BeaconAlertCanonical);
}

static size_t build_selftest_canonical(const BeaconSelfTestPayload* p,
                                       uint8_t* out, size_t out_max) {
  static const char DOMAIN[] = "securacv:beacon:selftest:v0";
  const size_t domain_len = sizeof(DOMAIN) - 1;
  size_t body_len = 8 + 4 + 2 + 1 + DEVICE_FP_SIZE;
  if (out_max < domain_len + body_len) return 0;
  size_t i = 0;
  memcpy(out + i, DOMAIN, domain_len); i += domain_len;
  memcpy(out + i, &p->timestamp, 8); i += 8;
  memcpy(out + i, &p->uptime_sec, 4); i += 4;
  memcpy(out + i, &p->free_heap_kb, 2); i += 2;
  out[i++] = p->key_self_test_ok;
  memcpy(out + i, p->device_fp, DEVICE_FP_SIZE); i += DEVICE_FP_SIZE;
  return i;
}

static const BeaconSetEntry* find_set_entry_by_fp(const uint8_t* fp) {
  for (uint8_t i = 0; i < g_beacon_set_count; i++) {
    if (g_beacon_set[i].valid &&
        memcmp(g_beacon_set[i].fingerprint, fp, DEVICE_FP_SIZE) == 0) {
      return &g_beacon_set[i];
    }
  }
  return nullptr;
}

static bool rate_check_and_record(const uint8_t* fp, bool is_exercise) {
  uint32_t now = millis();
  const uint32_t WINDOW_MS = 86400000;  // 24 h
  OriginationRate* entry = nullptr;
  OriginationRate* free_slot = nullptr;
  for (size_t i = 0; i < MAX_BEACON_SET; i++) {
    if (g_origination_rate[i].valid &&
        memcmp(g_origination_rate[i].fingerprint, fp, DEVICE_FP_SIZE) == 0) {
      entry = &g_origination_rate[i]; break;
    }
    if (!g_origination_rate[i].valid && !free_slot) free_slot = &g_origination_rate[i];
  }
  if (!entry) {
    if (!free_slot) return true;
    memcpy(free_slot->fingerprint, fp, DEVICE_FP_SIZE);
    free_slot->valid = true;
    entry = free_slot;
  }
  // Separate buckets by frame class (AGENTS.md Beacon invariant 10).
  uint32_t* window_start = is_exercise ? &entry->exercise_window_start_ms
                                       : &entry->window_start_ms;
  uint8_t* count = is_exercise ? &entry->exercise_count_in_window
                               : &entry->count_in_window;
  if (*count == 0 || now - *window_start > WINDOW_MS) {
    *window_start = now;
    *count = 1;
    return true;
  }
  if (*count >= MAX_ORIGINATIONS_PER_PUBKEY_24H) return false;
  (*count)++;
  return true;
}

static bool pair_rate_check_and_record(const uint8_t* fp_a, const uint8_t* fp_b) {
  const uint32_t now = millis();
  const uint32_t WINDOW_MS = 86400000;  // 24 h
  const uint8_t* lo = fp_a;
  const uint8_t* hi = fp_b;
  if (memcmp(fp_a, fp_b, DEVICE_FP_SIZE) > 0) { lo = fp_b; hi = fp_a; }

  PairRate* entry = nullptr;
  for (size_t i = 0; i < MAX_BEACON_SET; i++) {
    PairRate* p = &g_pair_rate[i];
    if (p->valid && memcmp(p->fp_lo, lo, DEVICE_FP_SIZE) == 0 &&
        memcmp(p->fp_hi, hi, DEVICE_FP_SIZE) == 0) {
      entry = p;
      break;
    }
  }
  if (!entry) {
    for (size_t i = 0; i < MAX_BEACON_SET && !entry; i++) {
      if (!g_pair_rate[i].valid) entry = &g_pair_rate[i];
    }
  }
  if (!entry) {
    // Table full: take over the pair whose window is oldest. A still-active
    // pair that loses its slot restarts at 1, which errs toward accepting —
    // each signer's own per-pubkey bucket still holds.
    entry = &g_pair_rate[0];
    for (size_t i = 1; i < MAX_BEACON_SET; i++) {
      if (now - g_pair_rate[i].window_start_ms > now - entry->window_start_ms) {
        entry = &g_pair_rate[i];
      }
    }
    entry->valid = false;
  }
  if (!entry->valid) {
    memcpy(entry->fp_lo, lo, DEVICE_FP_SIZE);
    memcpy(entry->fp_hi, hi, DEVICE_FP_SIZE);
    entry->valid = true;
    entry->count_in_window = 0;
  }
  if (entry->count_in_window == 0 || now - entry->window_start_ms > WINDOW_MS) {
    entry->window_start_ms = now;
    entry->count_in_window = 1;
    return true;
  }
  if (entry->count_in_window >= MAX_ORIGINATIONS_PER_PAIR_24H) return false;
  entry->count_in_window++;
  return true;
}

// spec §4: the life-safety template set is the entire Beacon vocabulary.
// The byte space overlaps Chirp's (the magic byte discriminates the
// channel), so an unlisted id is not merely unknown — it can be a Chirp
// authority/aid category Beacon excludes by design.
static bool is_valid_beacon_template(uint8_t id) {
  switch (id) {
    case BCN_INFRA_POWER_OUT:
    case BCN_INFRA_GAS_SMELL:
    case BCN_EMERG_FIRE_VISIBLE:
    case BCN_EMERG_MEDICAL_SCENE:
    case BCN_EMERG_MULTIPLE_AMBULANCE:
    case BCN_EMERG_EVACUATION:
    case BCN_EMERG_SHELTER_IN_PLACE:
    case BCN_WX_SEVERE_WARNING:
    case BCN_WX_TORNADO:
    case BCN_WX_FLOOD:
    case BCN_CLR_RESOLVED:
    case BCN_CLR_SAFE:
    case BCN_CLR_FALSE_ALARM:
      return true;
    default:
      return false;
  }
}

// The replay identity of an ALERT-class frame: half of each signature. Both
// halves are authenticated bytes (a forgery fails verification and is never
// remembered), and taking from both means a solo frame — which carries the
// same signature in both slots — is keyed the same way as a dual one.
static void frame_identity(const uint8_t* sig_a, const uint8_t* sig_b, uint8_t* id) {
  memcpy(id, sig_a, FRAME_ID_HALF);
  memcpy(id + FRAME_ID_HALF, sig_b, FRAME_ID_HALF);
}

static bool frame_seen(const uint8_t* id) {
  if (g_last_alert_id_valid && memcmp(g_last_alert_id, id, FRAME_ID_SIZE) == 0) return true;
  const uint32_t now = millis();
  const uint32_t horizon_ms = BEACON_FRESHNESS_S * 1000UL;
  for (size_t i = 0; i < SEEN_FRAME_MAX; i++) {
    if (!g_seen_frames[i].valid) continue;
    if (now - g_seen_frames[i].seen_ms > horizon_ms) {
      g_seen_frames[i].valid = false;
      continue;
    }
    if (memcmp(g_seen_frames[i].id, id, FRAME_ID_SIZE) == 0) return true;
  }
  return false;
}

static void remember_frame(const uint8_t* id) {
  SeenFrame* slot = &g_seen_frames[g_seen_frame_head];
  memcpy(slot->id, id, FRAME_ID_SIZE);
  slot->seen_ms = millis();
  slot->valid = true;
  g_seen_frame_head = (g_seen_frame_head + 1) % SEEN_FRAME_MAX;
}

// spec §5.4 requires a non-zero ref_canceled_nonce on CANCEL and UPDATE;
// §7.2 scopes their effect to the alarm actually in force. Both were
// previously unenforceable — the old check compared zero bytes, and the
// accepted alarm's frame nonce was never stored anywhere.
static bool references_active_alarm(const BeaconAlertCanonical* c) {
  if (!g_active_alarm_valid) return false;
  bool nonzero = false;
  for (size_t i = 0; i < BEACON_NONCE_SIZE; i++) {
    if (c->ref_canceled_nonce[i] != 0) { nonzero = true; break; }
  }
  if (!nonzero) return false;
  return memcmp(c->ref_canceled_nonce, g_active_alarm_nonce,
                BEACON_NONCE_SIZE) == 0;
}

// spec §7.1 step 9: a signer whose supervised health has lapsed does not
// authorize alarms. `last_selftest == 0` means "not observed since boot"
// (the map is RAM-only per §11), not "stale" — treating it as stale would
// deafen a just-rebooted receiver for a full selftest cadence, which is a
// worse failure than accepting one frame from an unproven-but-paired
// signer. Same reading as recompute_trouble_reasons.
static bool signer_selftest_stale(const BeaconSetEntry* e) {
  if (e->last_selftest == 0) return false;
  const uint32_t now = millis();
  const uint64_t age_ms = (now > e->last_selftest) ? (now - e->last_selftest) : 0;
  return age_ms > SELFTEST_MISSING_MS;
}

// ════════════════════════════════════════════════════════════════════════════
// PERSISTENCE
// ════════════════════════════════════════════════════════════════════════════

static bool persist_beacon_set() {
  if (!flash_encryption_enabled()) {
    health_log(SCV_LOG_ALERT, SCV_CAT_CRYPTO,
               "beacon: refused to persist set — flash encryption disabled");
    return false;
  }
  g_prefs.begin(NVS_NS, false);
  g_prefs.putUChar(NVS_SET_COUNT, g_beacon_set_count);
  for (uint8_t i = 0; i < g_beacon_set_count; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%s%u", NVS_SET_PREFIX, (unsigned)i);
    g_prefs.putBytes(key, &g_beacon_set[i], sizeof(BeaconSetEntry));
  }
  g_prefs.end();
  return true;
}

static bool load_beacon_set() {
  if (!flash_encryption_enabled()) {
    g_beacon_set_count = 0;
    memset(g_beacon_set, 0, sizeof(g_beacon_set));
    return false;
  }
  g_prefs.begin(NVS_NS, true);
  g_beacon_set_count = g_prefs.getUChar(NVS_SET_COUNT, 0);
  if (g_beacon_set_count > MAX_BEACON_SET) g_beacon_set_count = MAX_BEACON_SET;
  for (uint8_t i = 0; i < g_beacon_set_count; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%s%u", NVS_SET_PREFIX, (unsigned)i);
    g_prefs.getBytes(key, &g_beacon_set[i], sizeof(BeaconSetEntry));
  }
  g_prefs.end();
  return true;
}

// v0.3 (audit follow-up): persist the audit log to NVS so that Beacon
// alarms survive reboots. FE-gated identically to beacon_set. The log is
// rotated in-place: at most AUDIT_LOG_MAX entries are kept, indexed by
// `aud_head`. Each individual entry is written to its own NVS key so a
// full read isn't required to append.
// Persist one slot + head + count atomically (single NVS commit). When the
// ring rotates, only the one overwritten slot has changed; head advances
// so on reload we know where the newest entry lives.
static bool persist_audit_entry(size_t index) {
  if (!flash_encryption_enabled()) return false;
  g_prefs.begin(NVS_NS, false);
  char key[16];
  snprintf(key, sizeof(key), "%s%u", NVS_AUDIT_PREFIX, (unsigned)index);
  g_prefs.putBytes(key, &g_audit_log[index], sizeof(BeaconAuditEntry));
  g_prefs.putULong(NVS_AUDIT_COUNT, g_audit_log_count);
  g_prefs.putULong(NVS_AUDIT_HEAD, (uint32_t)g_audit_head);
  g_prefs.end();
  return true;
}

static bool load_audit_log() {
  if (!flash_encryption_enabled()) {
    g_audit_log_count = 0;
    g_audit_head = 0;
    return false;
  }
  g_prefs.begin(NVS_NS, true);
  uint32_t cnt = g_prefs.getULong(NVS_AUDIT_COUNT, 0);
  uint32_t head = g_prefs.getULong(NVS_AUDIT_HEAD, 0);
  if (cnt > AUDIT_LOG_MAX) cnt = AUDIT_LOG_MAX;
  if (head >= AUDIT_LOG_MAX) head = 0;
  g_audit_log_count = cnt;
  g_audit_head = head;
  // Load every persisted slot. Slots beyond `cnt` (i.e. never written)
  // remain zeroed from the init() memset.
  for (size_t i = 0; i < cnt; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%s%u", NVS_AUDIT_PREFIX, (unsigned)i);
    g_prefs.getBytes(key, &g_audit_log[i], sizeof(BeaconAuditEntry));
  }
  // Re-derive the chain head from the most recently written entry
  // (the slot at (head - 1) mod MAX when count == MAX, else slot
  // count-1 in the pre-rotation regime).
  if (cnt > 0) {
    const size_t newest_idx = (head == 0) ? (cnt - 1) : (head - 1);
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, g_audit_log[newest_idx].prev_audit_hash, 32);
    mbedtls_sha256_update(&ctx, (const uint8_t*)&g_audit_log[newest_idx],
                          sizeof(BeaconAuditEntry));
    mbedtls_sha256_finish(&ctx, g_audit_chain_head);
    mbedtls_sha256_free(&ctx);
  }
  g_prefs.end();
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// TROUBLE / STATE
// ════════════════════════════════════════════════════════════════════════════

static void recompute_trouble_reasons() {
  uint16_t reasons = BCN_TROUBLE_NONE;
  if (time(nullptr) < (time_t)MIN_UNIX_TIME) reasons |= BCN_TROUBLE_TIME_UNSYNCED;
  // Airtime saturation surface:
  uint16_t airtime_x100 = airtime_governor::airtime_pct_x100(millis());
  if (airtime_x100 > 160) reasons |= BCN_TROUBLE_AIRTIME_SATURATED;  // 1.6% > 80% of 2% cap
  if (g_beacon_set_count == 0) reasons |= BCN_TROUBLE_BEACON_SET_EMPTY;

  // Neighbor selftest gap:
  uint32_t now = millis();
  for (uint8_t i = 0; i < g_beacon_set_count; i++) {
    if (!g_beacon_set[i].valid) continue;
    if (g_beacon_set[i].trust_level == BCN_TRUST_REVOKED) continue;
    if (g_beacon_set[i].last_selftest == 0) continue;
    uint64_t age_ms = (now > g_beacon_set[i].last_selftest)
                      ? (now - g_beacon_set[i].last_selftest) : 0;
    if (age_ms > SELFTEST_MISSING_MS) {
      reasons |= BCN_TROUBLE_NEIGHBOR_SELFTEST_GAP;
      break;
    }
  }

  g_trouble_reasons = reasons;

  // State transitions: ALARM dominates TROUBLE.
  if (g_active_alarm_valid) {
    set_state(BEACON_STATE_ALARM);
  } else if (reasons != BCN_TROUBLE_NONE) {
    set_state(BEACON_STATE_TROUBLE);
  } else {
    set_state(BEACON_STATE_NORMAL);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// AUDIT LOG
// ════════════════════════════════════════════════════════════════════════════
//
// Two storage tiers (AGENTS.md "Beacon channel invariants" item 9 — the
// audit log is append-only and chain-hashed; no rotate/delete; export-only):
//
//   1. SD: /beacon/audit.jsonl — the canonical append-only audit log. One
//      self-describing JSON line per entry, never truncated or rotated
//      (beacon traffic is rate-limited to a handful of frames per day, so
//      unbounded append is ~100 KB/year worst-case). Best-effort: a device
//      without an SD card keeps chaining (tier 2) and logs one health
//      warning rather than dropping the alarm path.
//   2. NVS: a 64-entry ring holding the most recent entries — a bounded
//      recent-view cache for the REST/UI surface and for alarm-survival
//      across reboots, NOT the audit log of record. The chain head spans
//      every entry ever appended, so continuity stays provable even for
//      entries that have aged out of the ring.

static bool g_audit_sd_warned = false;

// Latch one STORAGE health warning per failure streak — covering every
// failure mode (no card, mkdir, open, short write), not just CARD_NONE,
// since the SD file is the audit log of record (codex P2 on #748). A
// successful append re-arms the latch so a recovered card warns again on
// the next failure.
static bool audit_sd_fail(const char* why) {
  if (!g_audit_sd_warned) {
    g_audit_sd_warned = true;
    char msg[128];
    snprintf(msg, sizeof(msg),
             "beacon audit: SD log of record unavailable (%s) — "
             "NVS ring cache only", why);
    health_log(SCV_LOG_WARNING, SCV_CAT_STORAGE, msg);
  }
  return false;
}

static void audit_hex(char* out, const uint8_t* in, size_t len) {
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i]     = H[in[i] >> 4];
    out[2 * i + 1] = H[in[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

// Append one entry to the SD audit file. Pure append: no truncation, no
// rotation (AGENTS item 9). Returns false (after a latched health warning)
// when no SD card is present or the write fails.
static bool sd_append_audit_entry(const BeaconAuditEntry* entry,
                                  const uint8_t* new_chain_head) {
  if (sd_mount_in_flight()) return audit_sd_fail("mount in flight");
  if (SD.cardType() == CARD_NONE) return audit_sd_fail("no card");
  if (!SD.exists("/beacon") && !SD.mkdir("/beacon"))
    return audit_sd_fail("mkdir /beacon failed");

  char ref_hex[2 * BEACON_NONCE_SIZE + 1];
  char ofp_hex[2 * DEVICE_FP_SIZE + 1];
  char cfp_hex[2 * DEVICE_FP_SIZE + 1];
  char sigo_hex[2 * BEACON_SIGNATURE_SIZE + 1];
  char sigc_hex[2 * BEACON_SIGNATURE_SIZE + 1];
  char prev_hex[65];
  char head_hex[65];
  audit_hex(ref_hex, entry->canonical.ref_canceled_nonce, BEACON_NONCE_SIZE);
  audit_hex(ofp_hex, entry->canonical.originator_fp, DEVICE_FP_SIZE);
  audit_hex(cfp_hex, entry->canonical.cosigner_fp, DEVICE_FP_SIZE);
  audit_hex(sigo_hex, entry->sig_originator, BEACON_SIGNATURE_SIZE);
  audit_hex(sigc_hex, entry->sig_cosigner, BEACON_SIGNATURE_SIZE);
  audit_hex(prev_hex, entry->prev_audit_hash, 32);
  audit_hex(head_hex, new_chain_head, 32);

  char line[768];
  const int n = snprintf(
      line, sizeof(line),
      "{\"v\":1,\"at\":%llu,\"eff\":%llu,\"exp\":%llu,\"tpl\":%u,"
      "\"msg\":%u,\"urg\":%u,\"sev\":%u,\"cert\":%u,\"scope\":%u,"
      "\"detail\":%u,\"hop\":%u,\"ref\":\"%s\",\"ofp\":\"%s\","
      "\"cfp\":\"%s\",\"sigo\":\"%s\",\"sigc\":\"%s\",\"prev\":\"%s\","
      "\"head\":\"%s\"}\n",
      (unsigned long long)entry->received_at,
      (unsigned long long)entry->canonical.effective,
      (unsigned long long)entry->canonical.expires,
      (unsigned)entry->canonical.template_id,
      (unsigned)entry->canonical.msg_type,
      (unsigned)entry->canonical.urgency,
      (unsigned)entry->canonical.severity,
      (unsigned)entry->canonical.certainty,
      (unsigned)entry->canonical.scope,
      (unsigned)entry->canonical.detail_slot,
      (unsigned)entry->hop_count,
      ref_hex, ofp_hex, cfp_hex, sigo_hex, sigc_hex, prev_hex, head_hex);
  if (n <= 0 || (size_t)n >= sizeof(line)) return false;

  // FILE_APPEND opens for write at end-of-file; close after every write so
  // a power cut at most loses the in-flight line, never the file structure
  // (same crash model as csi_event_log.cpp).
  File f = SD.open("/beacon/audit.jsonl", FILE_APPEND);
  if (!f) return audit_sd_fail("open failed");
  const size_t wrote = f.write((const uint8_t*)line, (size_t)n);
  f.close();
  if (wrote != (size_t)n) return audit_sd_fail("short write (card full?)");
  g_audit_sd_warned = false;  // healthy again — re-arm the warning latch
  return true;
}

// Recover the chain head from the SD log of record at boot: read the tail
// of /beacon/audit.jsonl and adopt the newest complete line's "head" ONLY
// when it chain-links to the file (its "prev" == the previous line's "head",
// or genesis for a first record). The NVS ring is only a cache — if its
// persistence failed or it was wiped while SD retained later entries,
// deriving the head from NVS alone would fork the supposedly append-only
// chain (codex P2 on #748), so SD wins on disagreement. But we no longer
// trust the last "head":"…" substring verbatim: a torn/corrupt/spliced tail
// could otherwise silently redirect the chain. The linkage guard (and the
// tail parsing) is the pure, host-tested beacon_audit_recover::recover_head
// (test_beacon_audit_recover.cpp).
static bool sd_recover_chain_head(uint8_t out[32]) {
  if (sd_mount_in_flight()) return false;
  if (SD.cardType() == CARD_NONE) return false;
  File f = SD.open("/beacon/audit.jsonl", FILE_READ);
  if (!f) return false;
  const size_t size = f.size();
  if (size == 0) { f.close(); return false; }

  // The writer caps a line at `char line[768]`, so the linkage guard needs, in
  // the worst case, a torn final partial (≤767) + the newest COMPLETE line
  // (≤767) + its predecessor (≤767) + the predecessor's starting delimiter all
  // in view — otherwise the window could start inside the predecessor, the
  // guard would find no verifiable predecessor, refuse the (non-genesis) newest
  // line, and keep a stale NVS head, forking the log in exactly the stale-cache
  // case this guard exists to fix (codex on #865). 4 KiB clears 3×768 with
  // margin. Static (init is single-threaded, non-reentrant) to stay off the
  // loop-task stack.
  static char tail[4096];
  const size_t want = (size < sizeof(tail)) ? size : sizeof(tail);
  if (!f.seek(size - want)) { f.close(); return false; }
  const size_t got = f.read((uint8_t*)tail, want);
  f.close();
  if (got == 0) return false;

  const bool from_start = (want == size);
  return beacon_audit_recover::recover_head(tail, got, from_start, out);
}

static void chain_audit_entry(BeaconAuditEntry* entry) {
  memcpy(entry->prev_audit_hash, g_audit_chain_head, 32);
  // New chain head = SHA-256(prev_head || entry_serialized).
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, g_audit_chain_head, 32);
  mbedtls_sha256_update(&ctx, (const uint8_t*)entry, sizeof(BeaconAuditEntry));
  mbedtls_sha256_finish(&ctx, g_audit_chain_head);
  mbedtls_sha256_free(&ctx);

  // Tier 1: append-only SD audit log (the log of record — AGENTS item 9).
  // Best-effort: failure never blocks the alarm path.
  sd_append_audit_entry(entry, g_audit_chain_head);

  // Tier 2: NVS recent-view ring cache — write at g_audit_head, advance
  // head, persist the single touched slot + head pointer + count. No
  // memmove → on-disk contents stay consistent with RAM after rotation
  // (gemini P1 + codex P2 closure).
  const size_t written_idx = g_audit_head;
  g_audit_log[written_idx] = *entry;
  g_audit_head = (g_audit_head + 1) % AUDIT_LOG_MAX;
  if (g_audit_log_count < AUDIT_LOG_MAX) g_audit_log_count++;

  // Persist the single slot + the head + the count atomically per-slot.
  // FE-gated; refuses cleanly if flash encryption is off.
  persist_audit_entry(written_idx);
}

// ════════════════════════════════════════════════════════════════════════════
// BROADCAST
// ════════════════════════════════════════════════════════════════════════════

static void broadcast_message(const uint8_t* data, size_t len) {
  static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(BROADCAST_ADDR)) esp_now_add_peer(&peer);
  esp_now_send(BROADCAST_ADDR, data, len);
}

// ════════════════════════════════════════════════════════════════════════════
// ALERT EMISSION (after cosigner signature received)
// ════════════════════════════════════════════════════════════════════════════

static void emit_alert_frame() {
  if (!g_pending_origination.valid) return;
  // Frame layout: BeaconHeader || BeaconAlertCanonical || sig_originator || sig_cosigner.
  uint8_t buf[sizeof(BeaconHeader) + sizeof(BeaconAlertCanonical) + 2 * BEACON_SIGNATURE_SIZE];
  memset(buf, 0, sizeof(buf));
  BeaconHeader* hdr = (BeaconHeader*)buf;
  hdr->magic = BEACON_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = BEACON_MSG_ALERT;
  hdr->hop_count = 0;
  hdr->flags = 0;
  hdr->payload_len = sizeof(BeaconAlertCanonical) + 2 * BEACON_SIGNATURE_SIZE;
  memcpy(hdr->nonce, g_pending_origination.canonical.ref_canceled_nonce, BEACON_NONCE_SIZE);
  // Actually use a fresh nonce for the frame itself if canonical doesn't carry it:
  esp_fill_random(hdr->nonce, BEACON_NONCE_SIZE);

  uint8_t* p = buf + sizeof(BeaconHeader);
  memcpy(p, &g_pending_origination.canonical, sizeof(BeaconAlertCanonical));
  p += sizeof(BeaconAlertCanonical);
  memcpy(p, g_pending_origination.sig_originator, BEACON_SIGNATURE_SIZE);
  // sig_cosigner is filled in once the response arrives — caller of
  // emit_alert_frame() is the COSIGN_RESP handler; it places the signature in
  // the second slot before invoking this. We expect g_pending_origination has
  // been augmented; for simplicity we assume the cosigner sig has been copied
  // into the trailing 64 bytes of g_pending_origination.canonical's owner
  // structure — see on_cosign_resp handler.

  // Beacon frames go through the distinct beacon-slot airtime accounting
  // so HA MQTT can surface beacon.airtime_pct separately from Opera tamper
  // alerts. force_reserve_beacon never blocks (Beacon is always urgent).
  airtime_governor::force_reserve_beacon(millis(), sizeof(buf));
  broadcast_message(buf, sizeof(buf));
  g_pending_origination.valid = false;

  health_log(SCV_LOG_ALERT, SCV_CAT_NETWORK,
             "beacon: dual-signed ALERT broadcast (hop 0)");
}

// ════════════════════════════════════════════════════════════════════════════
// ESP-NOW DISPATCH (verification + state transitions)
// ════════════════════════════════════════════════════════════════════════════

static void handle_alert_frame(const uint8_t* data, size_t len);
static void handle_selftest_frame(const uint8_t* data, size_t len);

static void handle_cosign_req_frame(const uint8_t* data, size_t len);
static void handle_cosign_resp_frame(const uint8_t* data, size_t len);

static void on_espnow_recv(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi) {
  (void)mac; (void)rssi;
  if (!g_enabled) return;
  if (len < (int)sizeof(BeaconHeader)) return;
  const BeaconHeader* hdr = (const BeaconHeader*)data;
  if (hdr->magic != BEACON_MAGIC) return;
  if (hdr->version != PROTOCOL_VERSION) return;

  switch (hdr->msg_type) {
    case BEACON_MSG_ALERT:
    case BEACON_MSG_UPDATE:
    case BEACON_MSG_CANCEL:
    case BEACON_MSG_EXERCISE:
      handle_alert_frame(data, (size_t)len);
      break;
    case BEACON_MSG_SELFTEST_OK:
      handle_selftest_frame(data, (size_t)len);
      break;
    case BEACON_MSG_COSIGN_REQ:
      handle_cosign_req_frame(data, (size_t)len);
      break;
    case BEACON_MSG_COSIGN_RESP:
      handle_cosign_resp_frame(data, (size_t)len);
      break;
    default:
      // PAIR_OFFER / REVOKE paths are deferred to v0.3 pairing implementation.
      break;
  }
}

static void handle_cosign_req_frame(const uint8_t* data, size_t len) {
  if (len < sizeof(BeaconHeader) + sizeof(BeaconCosignRequestPayload)) return;
  const BeaconCosignRequestPayload* req =
      (const BeaconCosignRequestPayload*)(data + sizeof(BeaconHeader));

  // Only act if we're the addressed cosigner candidate.
  if (memcmp(req->candidate_cosigner_fp, g_device_fp, DEVICE_FP_SIZE) != 0) return;

  // Originator must be in our beacon set, not revoked, with x25519 pubkey known.
  const BeaconSetEntry* orig = find_set_entry_by_fp(req->originator_fp);
  if (!orig) return;
  if (orig->trust_level == BCN_TRUST_REVOKED) return;
  if (!orig->has_x25519_pubkey) return;

  ensure_x25519_keypair();

  uint8_t plaintext[sizeof(BeaconAlertCanonical)];
  if (req->ciphertext_len != sizeof(BeaconAlertCanonical)) return;
  if (!cosign_decrypt(orig->x25519_pubkey,
                      req->ciphertext, req->ciphertext_len,
                      req->nonce, req->tag, plaintext)) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "beacon: COSIGN_REQ decrypt/auth failed (dropped)");
    return;
  }

  BeaconAlertCanonical candidate;
  memcpy(&candidate, plaintext, sizeof(candidate));

  // Sanity: originator_fp inside canonical must match the wire field.
  if (memcmp(candidate.originator_fp, req->originator_fp, DEVICE_FP_SIZE) != 0) return;
  if (memcmp(candidate.cosigner_fp, g_device_fp, DEVICE_FP_SIZE) != 0) return;
  if (candidate.scope != BCN_SCOPE_PRIVATE) return;
  // spec §6.1 step 3: the cosigner checks the template before the user is
  // ever asked to confirm — nothing outside the life-safety set is signable.
  if (!is_valid_beacon_template(candidate.template_id)) return;

  // Verify originator's signature.
  uint8_t buf[64 + sizeof(BeaconAlertCanonical)];
  size_t cl = build_alert_canonical(&candidate, buf, sizeof(buf));
  if (cl == 0) return;
  if (!Ed25519::verify(req->originator_signature, orig->device_pubkey, buf, cl)) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "beacon: COSIGN_REQ originator signature invalid");
    return;
  }

  // Stash for the local UI to display and the user to confirm via
  // cosign_pending_request().
  g_pending_cosign_in.valid = true;
  g_pending_cosign_in.canonical = candidate;
  memcpy(g_pending_cosign_in.originator_fp, req->originator_fp, DEVICE_FP_SIZE);
  memcpy(g_pending_cosign_in.sig_originator, req->originator_signature,
         BEACON_SIGNATURE_SIZE);
  g_pending_cosign_in.requested_ms = millis();

  if (g_cosign_request_callback) {
    g_cosign_request_callback(&candidate, req->originator_fp);
  }
  health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
             "beacon: COSIGN_REQ received and verified — awaiting user confirm");
}

static void handle_cosign_resp_frame(const uint8_t* data, size_t len) {
  if (len < sizeof(BeaconHeader) + sizeof(BeaconCosignResponsePayload)) return;
  const BeaconCosignResponsePayload* resp =
      (const BeaconCosignResponsePayload*)(data + sizeof(BeaconHeader));

  // Only the originator listens.
  if (memcmp(resp->originator_fp, g_device_fp, DEVICE_FP_SIZE) != 0) return;
  if (!g_pending_origination.valid) return;
  // spec §6.1: an origination the cosigner did not answer inside
  // COSIGN_WINDOW_MS expires silently. A late response must not resurrect
  // it — the canonical's `effective` is already stale by then.
  if (millis() - g_pending_origination.requested_ms > COSIGN_WINDOW_MS) {
    g_pending_origination.valid = false;
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "beacon: COSIGN_RESP arrived after the cosign window — discarded");
    return;
  }
  if (memcmp(g_pending_origination.canonical.cosigner_fp, resp->cosigner_fp,
             DEVICE_FP_SIZE) != 0) return;

  const BeaconSetEntry* cosigner = find_set_entry_by_fp(resp->cosigner_fp);
  if (!cosigner || cosigner->trust_level == BCN_TRUST_REVOKED) return;
  if (!cosigner->has_x25519_pubkey) return;

  uint8_t plaintext[64];
  if (!cosign_decrypt(cosigner->x25519_pubkey,
                      resp->ciphertext, sizeof(plaintext),
                      resp->nonce, resp->tag, plaintext)) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "beacon: COSIGN_RESP decrypt/auth failed");
    return;
  }

  if (!resp->accept) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "beacon: cosigner declined; origination discarded");
    g_pending_origination.valid = false;
    return;
  }

  // Verify the cosigner's signature over the same canonical.
  uint8_t buf[64 + sizeof(BeaconAlertCanonical)];
  size_t cl = build_alert_canonical(&g_pending_origination.canonical,
                                    buf, sizeof(buf));
  if (cl == 0) return;
  if (!Ed25519::verify(plaintext, cosigner->device_pubkey, buf, cl)) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "beacon: COSIGN_RESP signature invalid");
    return;
  }

  // Emit the dual-signed ALERT now.
  uint8_t out[sizeof(BeaconHeader) + sizeof(BeaconAlertCanonical) +
              2 * BEACON_SIGNATURE_SIZE];
  memset(out, 0, sizeof(out));
  BeaconHeader* hdr = (BeaconHeader*)out;
  hdr->magic = BEACON_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = BEACON_MSG_ALERT;
  hdr->hop_count = 0;
  hdr->payload_len = sizeof(BeaconAlertCanonical) + 2 * BEACON_SIGNATURE_SIZE;
  esp_fill_random(hdr->nonce, BEACON_NONCE_SIZE);

  uint8_t* p = out + sizeof(BeaconHeader);
  memcpy(p, &g_pending_origination.canonical, sizeof(BeaconAlertCanonical));
  p += sizeof(BeaconAlertCanonical);
  memcpy(p, g_pending_origination.sig_originator, BEACON_SIGNATURE_SIZE);
  p += BEACON_SIGNATURE_SIZE;
  memcpy(p, plaintext, BEACON_SIGNATURE_SIZE);  // cosigner's sig

  airtime_governor::force_reserve_beacon(millis(), sizeof(out));
  broadcast_message(out, sizeof(out));
  g_pending_origination.valid = false;

  health_log(SCV_LOG_ALERT, SCV_CAT_NETWORK,
             "beacon: dual-signed ALERT broadcast at hop 0");
}

static void handle_alert_frame(const uint8_t* data, size_t len) {
  if (len < sizeof(BeaconHeader) + sizeof(BeaconAlertCanonical) + 2 * BEACON_SIGNATURE_SIZE) return;
  const BeaconHeader* hdr = (const BeaconHeader*)data;
  const BeaconAlertCanonical* canonical =
      (const BeaconAlertCanonical*)(data + sizeof(BeaconHeader));
  const uint8_t* sig_a = data + sizeof(BeaconHeader) + sizeof(BeaconAlertCanonical);
  const uint8_t* sig_b = sig_a + BEACON_SIGNATURE_SIZE;

  // Replay dedup first (spec §7.1 step 3), so a rebroadcast costs no
  // signature verification and never reaches the originator's rate bucket.
  // Keyed on the signatures, not hdr->nonce — see SeenFrame.
  uint8_t frame_id[FRAME_ID_SIZE];
  frame_identity(sig_a, sig_b, frame_id);
  if (frame_seen(frame_id)) return;

  // Scope must be Private (lint and spec invariant).
  if (canonical->scope != BCN_SCOPE_PRIVATE) return;

  // The two signatures cover the canonical only — the header msg_type is
  // unauthenticated. Without this cross-check, a captured drill or
  // all-clear can be rebroadcast with the header byte rewritten to ALERT
  // and both signatures still verify.
  if (canonical->msg_type != hdr->msg_type) return;

  // spec §5.4: EXERCISE frames carry BCN_FLAG_IS_EXERCISE and nothing else
  // does. Requiring the biconditional is what keeps a drill from presenting
  // as a real alert, and a real alert from presenting as a drill.
  const bool exercise_flag = (hdr->flags & BCN_FLAG_IS_EXERCISE) != 0;
  if ((canonical->msg_type == BEACON_MSG_EXERCISE) != exercise_flag) return;

  if (!is_valid_beacon_template(canonical->template_id)) return;

  // v0.4 (spec §6.2): solo-origination frames bypass the
  // "originator != cosigner" check because the cosigner IS the originator
  // (the BOOT button is the cosigner). They MUST be certainty=Observed
  // so receivers can downweight them in the UI.
  const bool is_solo = (hdr->flags & BCN_FLAG_SOLO_ORIGIN) != 0;
  if (is_solo) {
    if (canonical->certainty != BCN_CERT_OBSERVED) {
      health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
                 "beacon: rejected solo frame — certainty != Observed");
      return;
    }
    if (memcmp(canonical->originator_fp, canonical->cosigner_fp,
               DEVICE_FP_SIZE) != 0) {
      health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
                 "beacon: rejected solo frame — originator_fp != cosigner_fp");
      return;
    }
  }

  // Originator must be in the local beacon set (for solo, same lookup
  // covers the "cosigner" since they're the same pubkey).
  const BeaconSetEntry* a = find_set_entry_by_fp(canonical->originator_fp);
  const BeaconSetEntry* b = is_solo ? a
                                    : find_set_entry_by_fp(canonical->cosigner_fp);
  if (!a || !b) return;
  if (a->trust_level == BCN_TRUST_REVOKED) return;
  if (b->trust_level == BCN_TRUST_REVOKED) return;
  if (!is_solo && memcmp(canonical->originator_fp, canonical->cosigner_fp,
                         DEVICE_FP_SIZE) == 0) {
    return;  // Standard dual-pubkey frame with collapsed signers is malformed.
  }

  if (signer_selftest_stale(a) || signer_selftest_stale(b)) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "beacon: rejected frame — a signer's selftest is older than 36 h");
    return;
  }

  // Verify both signatures over the canonical. Solo frames carry the
  // same signature in both slots; we still verify both to keep the
  // accept-path uniform (any tampering with either slot is caught).
  uint8_t buf[64 + sizeof(BeaconAlertCanonical)];
  size_t cl = build_alert_canonical(canonical, buf, sizeof(buf));
  if (cl == 0) return;
  if (!Ed25519::verify(sig_a, a->device_pubkey, buf, cl)) return;
  if (!Ed25519::verify(sig_b, b->device_pubkey, buf, cl)) return;

  // Wall-clock freshness (spec §7.1 step 4).
  time_t now = time(nullptr);
  if (now < (time_t)MIN_UNIX_TIME) {
    // Time unsynced — accept but flag.
  } else {
    if ((uint64_t)now > canonical->expires) return;
    const uint64_t n = (uint64_t)now;
    const uint64_t skew = (canonical->effective > n) ? (canonical->effective - n)
                                                     : (n - canonical->effective);
    if (skew > BEACON_FRESHNESS_S) return;
  }

  // The frame is authentic and fresh: remember it before any bucket is
  // charged, so the rate limiter sees each origination exactly once.
  remember_frame(frame_id);

  // Rate limit per originator fingerprint, in the bucket for this frame's
  // class (AGENTS.md Beacon invariant 10 — drills never share with alerts).
  const bool is_exercise = (canonical->msg_type == BEACON_MSG_EXERCISE);
  if (!rate_check_and_record(canonical->originator_fp, is_exercise)) return;

  // spec §8: the pair that co-signed has a budget of its own. The per-pubkey
  // bucket charges the originator only, so two devices trading roles would
  // otherwise hold twice that budget between them. Solo frames are not a
  // pair; drills are banked apart (invariant 10) and are not counted here.
  if (!is_solo && !is_exercise &&
      !pair_rate_check_and_record(canonical->originator_fp, canonical->cosigner_fp)) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "beacon: rejected frame — this pair's 24 h co-sign budget is spent");
    return;
  }

  // Record audit entry.
  BeaconAuditEntry entry;
  memset(&entry, 0, sizeof(entry));
  entry.received_at = (uint64_t)now;
  entry.canonical = *canonical;
  memcpy(entry.sig_originator, sig_a, BEACON_SIGNATURE_SIZE);
  memcpy(entry.sig_cosigner, sig_b, BEACON_SIGNATURE_SIZE);
  entry.hop_count = hdr->hop_count;
  chain_audit_entry(&entry);

  // Action by the SIGNED msg_type. A frame whose reference doesn't resolve
  // is still audited above; only its state effect is dropped.
  if (canonical->msg_type == BEACON_MSG_ALERT) {
    g_active_alarm = *canonical;
    g_active_alarm_valid = true;
    g_active_alarm_expires = canonical->expires;
    memcpy(g_active_alarm_nonce, hdr->nonce, BEACON_NONCE_SIZE);
    memcpy(g_last_alert_id, frame_id, FRAME_ID_SIZE);
    g_last_alert_id_valid = true;
    if (g_alarm_callback) g_alarm_callback(&g_active_alarm);
    recompute_trouble_reasons();
  } else if (canonical->msg_type == BEACON_MSG_UPDATE) {
    // spec §5.3: an UPDATE amends an alert already in force, so it names
    // that alert rather than installing itself as a new one.
    if (references_active_alarm(canonical)) {
      g_active_alarm = *canonical;
      g_active_alarm_expires = canonical->expires;
      if (g_alarm_callback) g_alarm_callback(&g_active_alarm);
      recompute_trouble_reasons();
    }
  } else if (canonical->msg_type == BEACON_MSG_CANCEL) {
    if (references_active_alarm(canonical)) {
      g_active_alarm_valid = false;
      set_state(BEACON_STATE_SUPERVISORY);
    }
  } else if (canonical->msg_type == BEACON_MSG_EXERCISE) {
    // Exercises don't trigger ALARM; logged only.
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "beacon: exercise received");
  }
}

static void handle_selftest_frame(const uint8_t* data, size_t len) {
  if (len < sizeof(BeaconHeader) + sizeof(BeaconSelfTestPayload)) return;
  const BeaconSelfTestPayload* p =
      (const BeaconSelfTestPayload*)(data + sizeof(BeaconHeader));

  const BeaconSetEntry* entry = find_set_entry_by_fp(p->device_fp);
  if (!entry) return;
  if (entry->trust_level == BCN_TRUST_REVOKED) return;

  uint8_t canon[64 + 8 + 4 + 2 + 1 + DEVICE_FP_SIZE];
  size_t cl = build_selftest_canonical(p, canon, sizeof(canon));
  if (cl == 0) return;
  if (!Ed25519::verify(p->signature, entry->device_pubkey, canon, cl)) return;

  // The signed timestamp must be inside the freshness window and must
  // advance (spec §5.3). A replayed frame carries identical signed bytes,
  // so monotonicity is the only thing that distinguishes it from a live
  // heartbeat — without it a dead neighbor stays supervised-healthy forever.
  const time_t now_wall = time(nullptr);
  if (now_wall >= (time_t)MIN_UNIX_TIME) {
    const uint64_t n = (uint64_t)now_wall;
    const uint64_t skew = (p->timestamp > n) ? (p->timestamp - n) : (n - p->timestamp);
    if (skew > BEACON_FRESHNESS_S) return;
  }

  // Update last_selftest timestamp (cast away const for in-place update).
  for (uint8_t i = 0; i < g_beacon_set_count; i++) {
    if (memcmp(g_beacon_set[i].fingerprint, p->device_fp, DEVICE_FP_SIZE) == 0) {
      if (p->timestamp <= g_last_selftest_ts[i]) return;
      g_last_selftest_ts[i] = p->timestamp;
      g_beacon_set[i].last_selftest = (uint64_t)millis();
      break;
    }
  }
  recompute_trouble_reasons();
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

bool init(const uint8_t* device_privkey, const uint8_t* device_pubkey,
          const char* device_name) {
  if (g_initialized) return true;
  if (!device_privkey || !device_pubkey) return false;
  memcpy(g_device_privkey, device_privkey, DEVICE_PRIVKEY_SIZE);
  memcpy(g_device_pubkey, device_pubkey, DEVICE_PUBKEY_SIZE);
  compute_fingerprint(g_device_pubkey, g_device_fp);
  strncpy(g_device_name, device_name ? device_name : "", BEACON_NAME_LEN);
  g_device_name[BEACON_NAME_LEN] = '\0';

  memset(g_beacon_set, 0, sizeof(g_beacon_set));
  memset(g_origination_rate, 0, sizeof(g_origination_rate));
  memset(g_pair_rate, 0, sizeof(g_pair_rate));
  memset(g_seen_frames, 0, sizeof(g_seen_frames));
  memset(g_active_alarm_nonce, 0, sizeof(g_active_alarm_nonce));
  memset(g_last_alert_id, 0, sizeof(g_last_alert_id));
  g_last_alert_id_valid = false;
  memset(g_last_selftest_ts, 0, sizeof(g_last_selftest_ts));
  g_seen_frame_head = 0;
  memset(g_audit_log, 0, sizeof(g_audit_log));
  memset(g_audit_chain_head, 0, sizeof(g_audit_chain_head));
  g_audit_log_count = 0;
  g_audit_head = 0;
  g_active_alarm_valid = false;
  g_pending_origination.valid = false;
  g_pending_cosign_in.valid = false;
  g_beacon_set_count = 0;

  load_beacon_set();
  load_audit_log();

  // The SD file is the audit log of record; the NVS-derived head is only
  // as good as the cache's last successful write. If SD has a (different)
  // head, adopt it so the next append chains from the true tail instead
  // of forking the chain (codex P2 on #748).
  {
    uint8_t sd_head[32];
    if (sd_recover_chain_head(sd_head) &&
        memcmp(sd_head, g_audit_chain_head, 32) != 0) {
      const bool nvs_had_state = (g_audit_log_count > 0);
      memcpy(g_audit_chain_head, sd_head, 32);
      if (nvs_had_state) {
        health_log(SCV_LOG_WARNING, SCV_CAT_STORAGE,
                   "beacon audit: chain head recovered from SD log of record "
                   "(NVS ring cache was stale)");
      }
    }
  }

  g_initialized = true;
  set_state(BEACON_STATE_DISABLED);
  health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "beacon channel initialized");
  return true;
}

void deinit() {
  if (!g_initialized) return;
  g_enabled = false;
  set_state(BEACON_STATE_DISABLED);
  g_initialized = false;
}

void set_enabled(bool enabled) {
  g_enabled = enabled;
  if (enabled) {
    recompute_trouble_reasons();
  } else {
    set_state(BEACON_STATE_DISABLED);
  }
}

bool is_enabled() { return g_enabled; }

void update() {
  if (!g_enabled) return;
  uint32_t now = millis();

  // Self-test cadence.
  if (now - g_last_selftest_ms > SELFTEST_INTERVAL_MS) {
    emit_selftest();
    g_last_selftest_ms = now;
  }

  // Expire active alarm.
  if (g_active_alarm_valid && time(nullptr) > (time_t)g_active_alarm_expires) {
    g_active_alarm_valid = false;
    set_state(BEACON_STATE_SUPERVISORY);
  }

  // Periodically reassess trouble.
  static uint32_t last_trouble_check_ms = 0;
  if (now - last_trouble_check_ms > 30000) {
    recompute_trouble_reasons();
    last_trouble_check_ms = now;
  }
}

BeaconStatus get_status() {
  BeaconStatus s;
  memset(&s, 0, sizeof(s));
  s.state = g_state;
  s.trouble_reasons = g_trouble_reasons;
  s.beacon_set_size = g_beacon_set_count;
  s.active_alarm = g_active_alarm_valid;
  s.active_alarm_expires = g_active_alarm_expires;
  s.active_template_id = g_active_alarm_valid ? g_active_alarm.template_id : 0xFF;
  return s;
}

const char* state_name(BeaconState state) {
  switch (state) {
    case BEACON_STATE_NORMAL:      return "Normal";
    case BEACON_STATE_TROUBLE:     return "Trouble";
    case BEACON_STATE_ALARM:       return "Alarm";
    case BEACON_STATE_SUPERVISORY: return "Supervisory";
    case BEACON_STATE_DISABLED:    return "Disabled";
    case BEACON_STATE_PAIR_INIT:   return "PairInit";
    case BEACON_STATE_PAIR_JOIN:   return "PairJoin";
    default: return "unknown";
  }
}

uint8_t get_beacon_set_size() { return g_beacon_set_count; }

const BeaconSetEntry* get_beacon_set_entry(uint8_t index) {
  if (index >= g_beacon_set_count) return nullptr;
  return &g_beacon_set[index];
}

bool revoke_beacon_set_entry(const uint8_t* fingerprint) {
  for (uint8_t i = 0; i < g_beacon_set_count; i++) {
    if (memcmp(g_beacon_set[i].fingerprint, fingerprint, DEVICE_FP_SIZE) == 0) {
      if (g_beacon_set[i].trust_level == BCN_TRUST_REVOKED) {
        return true;  // already revoked; idempotent
      }
      g_beacon_set[i].trust_level = BCN_TRUST_REVOKED;
      persist_beacon_set();
      health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "beacon: set entry revoked");
      return true;
    }
  }
  return false;
}

// v0.5: auto-revoke on neighbor tamper. Wired from mesh_network's
// handle_tamper_alert path so a compromised paired neighbor is dropped
// from our Beacon trust surface automatically — no manual
// /api/beacon/revoke call required, no human-in-the-loop wait while a
// tampered device might be emitting fraudulent Beacon co-signatures.
//
// The device pubkey is the same identity used by both Opera and Beacon
// (spec/beacon_channel_v0.md §3.1). We compute the Beacon 16-byte
// fingerprint from the full pubkey and revoke the entry.
bool on_peer_tampered(const uint8_t* device_pubkey) {
  uint8_t fp[DEVICE_FP_SIZE];
  compute_fingerprint(device_pubkey, fp);
  bool revoked = revoke_beacon_set_entry(fp);
  if (revoked) {
    health_log(SCV_LOG_ALERT, SCV_CAT_NETWORK,
               "beacon: paired neighbor revoked on tamper alert (v0.5 auto-revoke)");
    // Recompute trouble state since beacon_set membership changed.
    recompute_trouble_reasons();
  }
  return revoked;
}

bool start_pair_init() {
  // Pairing flow implementation deferred to v0.3.
  set_state(BEACON_STATE_PAIR_INIT);
  return true;
}

bool start_pair_join() {
  set_state(BEACON_STATE_PAIR_JOIN);
  return true;
}

bool confirm_pair() {
  // Stub.
  set_state(BEACON_STATE_NORMAL);
  return true;
}

void cancel_pair() {
  set_state(BEACON_STATE_NORMAL);
}

bool is_pairing() {
  return g_state == BEACON_STATE_PAIR_INIT || g_state == BEACON_STATE_PAIR_JOIN;
}

// Pick a candidate cosigner from the beacon set: first non-revoked entry with
// a known X25519 pubkey (needed for the encrypted COSIGN_REQ) and a recent
// selftest. Returns nullptr if none qualify.
static const BeaconSetEntry* pick_cosign_candidate() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < g_beacon_set_count; i++) {
    const BeaconSetEntry& e = g_beacon_set[i];
    if (!e.valid) continue;
    if (e.trust_level == BCN_TRUST_REVOKED) continue;
    if (!e.has_x25519_pubkey) continue;
    // Freshness: selftest seen within COSIGN_FRESHNESS_MS.
    if (e.last_selftest != 0 &&
        now > (uint32_t)e.last_selftest &&
        (now - (uint32_t)e.last_selftest) > COSIGN_FRESHNESS_MS) {
      continue;
    }
    return &e;
  }
  return nullptr;
}

bool paired_cosigner_available() { return pick_cosign_candidate() != nullptr; }

bool originate_alert(BeaconTemplate template_id, BeaconUrgency urgency,
                     BeaconSeverity severity, BeaconCertainty certainty,
                     BeaconDetailSlot detail, uint32_t ttl_minutes) {
  if (!g_enabled) return false;
  if (time(nullptr) < (time_t)MIN_UNIX_TIME) return false;
  if (g_beacon_set_count == 0) return false;  // no cosigner available
  if (!is_valid_beacon_template((uint8_t)template_id)) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "beacon: origination refused — template outside the life-safety set");
    return false;
  }
  if (!rate_check_and_record(g_device_fp, /*is_exercise=*/false)) return false;

  ensure_x25519_keypair();

  const BeaconSetEntry* candidate = pick_cosign_candidate();
  if (!candidate) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "beacon: no eligible cosigner (rate/presence/x25519)");
    return false;
  }

  BeaconAlertCanonical canonical;
  memset(&canonical, 0, sizeof(canonical));
  canonical.effective = (uint64_t)time(nullptr);
  canonical.expires = canonical.effective + (uint64_t)ttl_minutes * 60ULL;
  canonical.template_id = (uint8_t)template_id;
  canonical.msg_type = BEACON_MSG_ALERT;
  canonical.urgency = (uint8_t)urgency;
  canonical.severity = (uint8_t)severity;
  canonical.certainty = (uint8_t)certainty;
  canonical.scope = BCN_SCOPE_PRIVATE;
  canonical.detail_slot = (uint8_t)detail;
  memcpy(canonical.originator_fp, g_device_fp, DEVICE_FP_SIZE);
  memcpy(canonical.cosigner_fp, candidate->fingerprint, DEVICE_FP_SIZE);

  uint8_t buf[64 + sizeof(BeaconAlertCanonical)];
  size_t cl = build_alert_canonical(&canonical, buf, sizeof(buf));
  if (cl == 0) return false;

  uint8_t sig_a[BEACON_SIGNATURE_SIZE];
  Ed25519::sign(sig_a, g_device_privkey, g_device_pubkey, buf, cl);

  g_pending_origination.valid = true;
  g_pending_origination.canonical = canonical;
  memcpy(g_pending_origination.sig_originator, sig_a, BEACON_SIGNATURE_SIZE);
  g_pending_origination.requested_ms = millis();

  // v0.3 (audit follow-up): encrypt the canonical to the candidate cosigner's
  // X25519 pubkey + broadcast the encrypted COSIGN_REQ. Per spec §6.3 the
  // body is ChaCha20-Poly1305 over the canonical, keyed by HKDF-SHA256 of
  // the X25519 shared secret.
  uint8_t req_buf[sizeof(BeaconHeader) + sizeof(BeaconCosignRequestPayload)];
  memset(req_buf, 0, sizeof(req_buf));
  BeaconHeader* hdr = (BeaconHeader*)req_buf;
  BeaconCosignRequestPayload* req =
      (BeaconCosignRequestPayload*)(req_buf + sizeof(BeaconHeader));
  hdr->magic = BEACON_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = BEACON_MSG_COSIGN_REQ;
  hdr->hop_count = 0;
  hdr->payload_len = sizeof(BeaconCosignRequestPayload);
  esp_fill_random(hdr->nonce, BEACON_NONCE_SIZE);

  memcpy(req->originator_fp, g_device_fp, DEVICE_FP_SIZE);
  memcpy(req->candidate_cosigner_fp, candidate->fingerprint, DEVICE_FP_SIZE);
  req->ciphertext_len = sizeof(BeaconAlertCanonical);
  if (!cosign_encrypt(candidate->x25519_pubkey,
                      (const uint8_t*)&canonical, sizeof(BeaconAlertCanonical),
                      req->nonce, req->tag, req->ciphertext)) {
    g_pending_origination.valid = false;
    return false;
  }
  memcpy(req->originator_signature, sig_a, BEACON_SIGNATURE_SIZE);

  airtime_governor::force_reserve_beacon(millis(), sizeof(req_buf));
  broadcast_message(req_buf, sizeof(req_buf));

  health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
             "beacon: encrypted COSIGN_REQ broadcast to candidate cosigner");
  return true;
}

bool cosign_pending_request(bool confirm) {
  if (!g_pending_cosign_in.valid) return false;

  // Look up the originator to address the encrypted response.
  const BeaconSetEntry* orig = find_set_entry_by_fp(g_pending_cosign_in.originator_fp);
  if (!orig || orig->trust_level == BCN_TRUST_REVOKED || !orig->has_x25519_pubkey) {
    g_pending_cosign_in.valid = false;
    return false;
  }

  ensure_x25519_keypair();

  uint8_t resp_buf[sizeof(BeaconHeader) + sizeof(BeaconCosignResponsePayload)];
  memset(resp_buf, 0, sizeof(resp_buf));
  BeaconHeader* hdr = (BeaconHeader*)resp_buf;
  BeaconCosignResponsePayload* resp =
      (BeaconCosignResponsePayload*)(resp_buf + sizeof(BeaconHeader));
  hdr->magic = BEACON_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = BEACON_MSG_COSIGN_RESP;
  hdr->hop_count = 0;
  hdr->payload_len = sizeof(BeaconCosignResponsePayload);
  esp_fill_random(hdr->nonce, BEACON_NONCE_SIZE);

  memcpy(resp->originator_fp, g_pending_cosign_in.originator_fp, DEVICE_FP_SIZE);
  memcpy(resp->cosigner_fp, g_device_fp, DEVICE_FP_SIZE);
  resp->accept = confirm ? 1 : 0;

  // Encrypt the cosigner's signature (or zeroes on decline) back to the
  // originator. The encrypted payload has fixed size sizeof(plaintext)=64.
  uint8_t plaintext[64];
  memset(plaintext, 0, sizeof(plaintext));
  if (confirm) {
    uint8_t buf[64 + sizeof(BeaconAlertCanonical)];
    size_t cl = build_alert_canonical(&g_pending_cosign_in.canonical, buf, sizeof(buf));
    if (cl == 0) { g_pending_cosign_in.valid = false; return false; }
    Ed25519::sign(plaintext, g_device_privkey, g_device_pubkey, buf, cl);
  }
  if (!cosign_encrypt(orig->x25519_pubkey, plaintext, sizeof(plaintext),
                      resp->nonce, resp->tag, resp->ciphertext)) {
    g_pending_cosign_in.valid = false;
    return false;
  }

  airtime_governor::force_reserve_beacon(millis(), sizeof(resp_buf));
  broadcast_message(resp_buf, sizeof(resp_buf));
  g_pending_cosign_in.valid = false;
  health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
             confirm ? "beacon: COSIGN_RESP (accepted) sent"
                     : "beacon: COSIGN_RESP (declined) sent");
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// BOOT-BUTTON SOLO ORIGINATION — v0.4 spec §6.2 closure
//
// Single-Canary households cannot pair a neighbor cosigner, so they would
// otherwise be locked out of Beacon entirely. The compromise: a user who
// physically holds the BOOT button while holding-to-send originates a
// frame marked SOLO_ORIGIN with certainty=Observed, so receivers can
// visibly downweight it (one notch lower in the urgency UI; "solo
// origination" badge in the audit log).
//
// The physical BOOT button check is the real protection — a software-only
// attacker who exfiltrates the device key still cannot make a remote
// device's BOOT pin transition from idle to held without physical access.
// Receivers don't enforce this; we rely on every device playing by the
// protocol when it's in our local beacon_set. Compromised devices get
// REVOKED via `revoke_beacon_set_entry()` per the standard recovery path.
// ════════════════════════════════════════════════════════════════════════════

static uint8_t g_boot_gpio = 0;  // ESP32-S3 BOOT button default
static bool g_boot_gpio_configured = false;

void set_boot_button_gpio(uint8_t gpio) {
  g_boot_gpio = gpio;
  g_boot_gpio_configured = false;  // re-config on next read
}

bool boot_button_held() {
  // Configure as INPUT_PULLUP once; BOOT button pulls the pin LOW when held.
  if (!g_boot_gpio_configured) {
    pinMode(g_boot_gpio, INPUT_PULLUP);
    g_boot_gpio_configured = true;
  }
  return digitalRead(g_boot_gpio) == LOW;
}

bool originate_alert_solo(BeaconTemplate template_id, BeaconUrgency urgency,
                          BeaconSeverity severity, BeaconDetailSlot detail,
                          uint32_t ttl_minutes) {
  if (!g_enabled) return false;
  if (time(nullptr) < (time_t)MIN_UNIX_TIME) return false;
  if (!is_valid_beacon_template((uint8_t)template_id)) {
    health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "beacon: solo origination refused — template outside the life-safety set");
    return false;
  }

  // spec §6.2 scopes this path to a device with no paired neighbor able to
  // cosign right now. If the two-device path is open, it is the path: the
  // BOOT-button attestation stands in for a second key that is missing, not
  // for one an operator would rather not ask. Checked before the rate bucket
  // so being sent to the right path costs nothing.
  if (paired_cosigner_available()) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "beacon: solo origination refused — a fresh paired cosigner is available; use the two-device path");
    return false;
  }
  if (!rate_check_and_record(g_device_fp, /*is_exercise=*/false)) return false;

  // ── Physical attestation: BOOT button MUST be held right now ──
  // This is the load-bearing security check for the solo path. The user
  // is physically present at the device pressing BOOT; a software-only
  // attacker cannot fake that.
  if (!boot_button_held()) {
    health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "beacon: solo origination refused — BOOT button not held");
    return false;
  }

  BeaconAlertCanonical canonical;
  memset(&canonical, 0, sizeof(canonical));
  canonical.effective = (uint64_t)time(nullptr);
  canonical.expires = canonical.effective + (uint64_t)ttl_minutes * 60ULL;
  canonical.template_id = (uint8_t)template_id;
  canonical.msg_type = BEACON_MSG_ALERT;
  canonical.urgency = (uint8_t)urgency;
  canonical.severity = (uint8_t)severity;
  // Spec invariant: solo frames MUST be certainty=Observed. We force it
  // regardless of the caller's wish so receivers can rely on this property.
  canonical.certainty = (uint8_t)BCN_CERT_OBSERVED;
  canonical.scope = BCN_SCOPE_PRIVATE;
  canonical.detail_slot = (uint8_t)detail;
  // originator and cosigner are the same device.
  memcpy(canonical.originator_fp, g_device_fp, DEVICE_FP_SIZE);
  memcpy(canonical.cosigner_fp,   g_device_fp, DEVICE_FP_SIZE);

  uint8_t buf[64 + sizeof(BeaconAlertCanonical)];
  size_t cl = build_alert_canonical(&canonical, buf, sizeof(buf));
  if (cl == 0) return false;

  uint8_t sig[BEACON_SIGNATURE_SIZE];
  Ed25519::sign(sig, g_device_privkey, g_device_pubkey, buf, cl);

  // Wire format: same struct as the dual-pubkey ALERT, but both signature
  // slots carry the same Ed25519 signature, and the BCN_FLAG_SOLO_ORIGIN
  // flag in the header tells receivers to skip the "originator != cosigner"
  // check.
  uint8_t out[sizeof(BeaconHeader) + sizeof(BeaconAlertCanonical) +
              2 * BEACON_SIGNATURE_SIZE];
  memset(out, 0, sizeof(out));
  BeaconHeader* hdr = (BeaconHeader*)out;
  hdr->magic = BEACON_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = BEACON_MSG_ALERT;
  hdr->hop_count = 0;
  hdr->flags = BCN_FLAG_SOLO_ORIGIN;
  hdr->payload_len = sizeof(BeaconAlertCanonical) + 2 * BEACON_SIGNATURE_SIZE;
  esp_fill_random(hdr->nonce, BEACON_NONCE_SIZE);

  uint8_t* p = out + sizeof(BeaconHeader);
  memcpy(p, &canonical, sizeof(BeaconAlertCanonical));
  p += sizeof(BeaconAlertCanonical);
  memcpy(p, sig, BEACON_SIGNATURE_SIZE);                // sig_originator
  p += BEACON_SIGNATURE_SIZE;
  memcpy(p, sig, BEACON_SIGNATURE_SIZE);                // sig_cosigner (same)

  airtime_governor::force_reserve_beacon(millis(), sizeof(out));
  broadcast_message(out, sizeof(out));

  health_log(SCV_LOG_ALERT, SCV_CAT_NETWORK,
             "beacon: solo ALERT broadcast (certainty=Observed)");
  return true;
}

// Silences the active alarm on THIS device only. Spec §10 has the endpoint
// behind this originate a BEACON_MSG_CANCEL so every receiver stands down
// together; that needs the dual-signed cosign flow with msg_type=CANCEL and
// ref_canceled_nonce set, which is not built yet. Until it is, the caller
// has to say so (beacon_api.h's handle_cancel does) rather than report a
// network cancel that never went out — neighbors stay in ALARM until the
// alarm's own `expires`.
bool cancel_active_alarm() {
  if (!g_active_alarm_valid) return false;
  g_active_alarm_valid = false;
  set_state(BEACON_STATE_SUPERVISORY);
  health_log(SCV_LOG_WARNING, SCV_CAT_NETWORK,
             "beacon: alarm silenced on this device only — no CANCEL originated; "
             "paired devices stay in alarm until it expires");
  return true;
}

bool has_active_alarm() { return g_active_alarm_valid; }
const BeaconAlertCanonical* get_active_alarm() {
  return g_active_alarm_valid ? &g_active_alarm : nullptr;
}

size_t get_audit_log_count() { return g_audit_log_count; }
const BeaconAuditEntry* get_audit_log_entry(size_t index) {
  // Oldest-first iteration over the ring buffer. When the buffer hasn't
  // rotated (count < MAX), head == count and slot 0 is oldest; when it
  // has rotated (count == MAX), slot `head` is oldest. Both collapse to:
  //   slot = (head + MAX - count + index) % MAX
  if (index >= g_audit_log_count) return nullptr;
  const size_t slot = (g_audit_head + AUDIT_LOG_MAX - g_audit_log_count + index)
                      % AUDIT_LOG_MAX;
  return &g_audit_log[slot];
}

bool emit_selftest() {
  if (!g_enabled) return false;
  uint8_t buf[sizeof(BeaconHeader) + sizeof(BeaconSelfTestPayload)];
  memset(buf, 0, sizeof(buf));
  BeaconHeader* hdr = (BeaconHeader*)buf;
  BeaconSelfTestPayload* p = (BeaconSelfTestPayload*)(buf + sizeof(BeaconHeader));

  hdr->magic = BEACON_MAGIC;
  hdr->version = PROTOCOL_VERSION;
  hdr->msg_type = BEACON_MSG_SELFTEST_OK;
  hdr->hop_count = 0;
  hdr->flags = 0;
  hdr->payload_len = sizeof(BeaconSelfTestPayload);
  esp_fill_random(hdr->nonce, BEACON_NONCE_SIZE);

  p->timestamp = (uint64_t)time(nullptr);
  p->uptime_sec = (uint32_t)(millis() / 1000);
  p->free_heap_kb = (uint16_t)(ESP.getFreeHeap() / 1024);
  // Round-trip a fresh signature to prove key works.
  uint8_t test_payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t test_sig[BEACON_SIGNATURE_SIZE];
  Ed25519::sign(test_sig, g_device_privkey, g_device_pubkey, test_payload, sizeof(test_payload));
  p->key_self_test_ok = Ed25519::verify(test_sig, g_device_pubkey, test_payload, sizeof(test_payload)) ? 1 : 0;
  memcpy(p->device_fp, g_device_fp, DEVICE_FP_SIZE);

  uint8_t canon[128];
  size_t cl = build_selftest_canonical(p, canon, sizeof(canon));
  if (cl == 0) return false;
  Ed25519::sign(p->signature, g_device_privkey, g_device_pubkey, canon, cl);

  if (!airtime_governor::try_reserve_routine(millis(), sizeof(buf))) return false;
  broadcast_message(buf, sizeof(buf));
  return true;
}

void set_alarm_callback(BeaconAlarmCallback callback) { g_alarm_callback = callback; }
void set_state_callback(BeaconStateCallback callback) { g_state_callback = callback; }
void set_cosign_request_callback(BeaconCosignRequestCallback callback) {
  g_cosign_request_callback = callback;
}

void dispatch_espnow_message(const uint8_t* mac, const uint8_t* data,
                             int len, int8_t rssi_dbm) {
  on_espnow_recv(mac, data, len, rssi_dbm);
}

} // namespace beacon_channel

#endif // FEATURE_BEACON_CHANNEL
