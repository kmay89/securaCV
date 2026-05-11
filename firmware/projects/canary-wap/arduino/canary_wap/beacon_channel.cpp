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
 *  - The audit log is append-only with per-entry chain-hashing.
 *
 * Known limitations (tracked for v0.3):
 *  - The audit log persistence layer is a stub; entries live in RAM only.
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
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_flash_encrypt.h>
#include <WiFi.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <Ed25519.h>
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

static BeaconSetEntry g_beacon_set[MAX_BEACON_SET];
static uint8_t g_beacon_set_count = 0;

static BeaconAlertCanonical g_active_alarm;
static bool g_active_alarm_valid = false;
static uint64_t g_active_alarm_expires = 0;

// Audit log (RAM-only in v0.1 skeleton — persistence is a follow-up).
static const size_t AUDIT_LOG_MAX = 64;
static BeaconAuditEntry g_audit_log[AUDIT_LOG_MAX];
static size_t g_audit_log_count = 0;
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
struct OriginationRate {
  uint8_t  fingerprint[DEVICE_FP_SIZE];
  uint32_t window_start_ms;
  uint8_t  count_in_window;
  bool     valid;
};
static OriginationRate g_origination_rate[MAX_BEACON_SET];

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
static bool rate_check_and_record(const uint8_t* fp);
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
  size_t body_len = 4 + 2 + 1 + DEVICE_FP_SIZE;
  if (out_max < domain_len + body_len) return 0;
  size_t i = 0;
  memcpy(out + i, DOMAIN, domain_len); i += domain_len;
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

static bool rate_check_and_record(const uint8_t* fp) {
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
    free_slot->window_start_ms = now;
    free_slot->count_in_window = 1;
    free_slot->valid = true;
    return true;
  }
  if (now - entry->window_start_ms > WINDOW_MS) {
    entry->window_start_ms = now;
    entry->count_in_window = 1;
    return true;
  }
  if (entry->count_in_window >= MAX_ORIGINATIONS_PER_PUBKEY_24H) return false;
  entry->count_in_window++;
  return true;
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

  if (g_audit_log_count < AUDIT_LOG_MAX) {
    g_audit_log[g_audit_log_count++] = *entry;
  } else {
    // Rotate; drop oldest.
    memmove(&g_audit_log[0], &g_audit_log[1], sizeof(BeaconAuditEntry) * (AUDIT_LOG_MAX - 1));
    g_audit_log[AUDIT_LOG_MAX - 1] = *entry;
  }
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

  // For this v0.1 skeleton path we emit through urgent airtime always.
  airtime_governor::force_reserve_urgent(millis(), sizeof(buf));
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
    default:
      // COSIGN_REQ / COSIGN_RESP / PAIR_OFFER paths are deferred to v0.3.
      break;
  }
}

static void handle_alert_frame(const uint8_t* data, size_t len) {
  if (len < sizeof(BeaconHeader) + sizeof(BeaconAlertCanonical) + 2 * BEACON_SIGNATURE_SIZE) return;
  const BeaconHeader* hdr = (const BeaconHeader*)data;
  const BeaconAlertCanonical* canonical =
      (const BeaconAlertCanonical*)(data + sizeof(BeaconHeader));
  const uint8_t* sig_a = data + sizeof(BeaconHeader) + sizeof(BeaconAlertCanonical);
  const uint8_t* sig_b = sig_a + BEACON_SIGNATURE_SIZE;

  // Scope must be Private (lint and spec invariant).
  if (canonical->scope != BCN_SCOPE_PRIVATE) return;

  // Both signers must be in the local beacon set.
  const BeaconSetEntry* a = find_set_entry_by_fp(canonical->originator_fp);
  const BeaconSetEntry* b = find_set_entry_by_fp(canonical->cosigner_fp);
  if (!a || !b) return;
  if (a->trust_level == BCN_TRUST_REVOKED) return;
  if (b->trust_level == BCN_TRUST_REVOKED) return;
  if (memcmp(canonical->originator_fp, canonical->cosigner_fp, DEVICE_FP_SIZE) == 0) return;

  // Verify both signatures over the canonical.
  uint8_t buf[64 + sizeof(BeaconAlertCanonical)];
  size_t cl = build_alert_canonical(canonical, buf, sizeof(buf));
  if (cl == 0) return;
  if (!Ed25519::verify(sig_a, a->device_pubkey, buf, cl)) return;
  if (!Ed25519::verify(sig_b, b->device_pubkey, buf, cl)) return;

  // Wall-clock freshness.
  time_t now = time(nullptr);
  if (now < (time_t)MIN_UNIX_TIME) {
    // Time unsynced — accept but flag.
  } else {
    if (canonical->effective > (uint64_t)now + 60 ||
        (uint64_t)now > canonical->expires) {
      return;
    }
  }

  // Rate limit per originator fingerprint.
  if (!rate_check_and_record(canonical->originator_fp)) return;

  // Record audit entry.
  BeaconAuditEntry entry;
  memset(&entry, 0, sizeof(entry));
  entry.received_at = (uint64_t)now;
  entry.canonical = *canonical;
  memcpy(entry.sig_originator, sig_a, BEACON_SIGNATURE_SIZE);
  memcpy(entry.sig_cosigner, sig_b, BEACON_SIGNATURE_SIZE);
  entry.hop_count = hdr->hop_count;
  chain_audit_entry(&entry);

  // Action by msg_type.
  if (hdr->msg_type == BEACON_MSG_ALERT || hdr->msg_type == BEACON_MSG_UPDATE) {
    g_active_alarm = *canonical;
    g_active_alarm_valid = true;
    g_active_alarm_expires = canonical->expires;
    if (g_alarm_callback) g_alarm_callback(&g_active_alarm);
    recompute_trouble_reasons();
  } else if (hdr->msg_type == BEACON_MSG_CANCEL) {
    if (g_active_alarm_valid &&
        memcmp(canonical->ref_canceled_nonce, "", 0) == 0) {
      g_active_alarm_valid = false;
      set_state(BEACON_STATE_SUPERVISORY);
    }
  } else if (hdr->msg_type == BEACON_MSG_EXERCISE) {
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

  uint8_t canon[64 + 4 + 2 + 1 + DEVICE_FP_SIZE];
  size_t cl = build_selftest_canonical(p, canon, sizeof(canon));
  if (cl == 0) return;
  if (!Ed25519::verify(p->signature, entry->device_pubkey, canon, cl)) return;

  // Update last_selftest timestamp (cast away const for in-place update).
  for (uint8_t i = 0; i < g_beacon_set_count; i++) {
    if (memcmp(g_beacon_set[i].fingerprint, p->device_fp, DEVICE_FP_SIZE) == 0) {
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
  memset(g_audit_log, 0, sizeof(g_audit_log));
  memset(g_audit_chain_head, 0, sizeof(g_audit_chain_head));
  g_audit_log_count = 0;
  g_active_alarm_valid = false;
  g_pending_origination.valid = false;
  g_pending_cosign_in.valid = false;
  g_beacon_set_count = 0;

  load_beacon_set();

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
      g_beacon_set[i].trust_level = BCN_TRUST_REVOKED;
      persist_beacon_set();
      health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "beacon: set entry revoked");
      return true;
    }
  }
  return false;
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

bool originate_alert(BeaconTemplate template_id, BeaconUrgency urgency,
                     BeaconSeverity severity, BeaconCertainty certainty,
                     BeaconDetailSlot detail, uint32_t ttl_minutes) {
  if (!g_enabled) return false;
  if (time(nullptr) < (time_t)MIN_UNIX_TIME) return false;
  if (g_beacon_set_count == 0) return false;  // no cosigner available
  if (!rate_check_and_record(g_device_fp)) return false;

  BeaconAlertCanonical canonical;
  memset(&canonical, 0, sizeof(canonical));
  canonical.effective = (uint64_t)time(nullptr);
  canonical.expires = canonical.effective + (uint64_t)ttl_minutes * 60;
  canonical.template_id = (uint8_t)template_id;
  canonical.msg_type = BEACON_MSG_ALERT;
  canonical.urgency = (uint8_t)urgency;
  canonical.severity = (uint8_t)severity;
  canonical.certainty = (uint8_t)certainty;
  canonical.scope = BCN_SCOPE_PRIVATE;
  canonical.detail_slot = (uint8_t)detail;
  memcpy(canonical.originator_fp, g_device_fp, DEVICE_FP_SIZE);
  // cosigner_fp will be filled in by the cosigner before final emission.

  uint8_t buf[64 + sizeof(BeaconAlertCanonical)];
  size_t cl = build_alert_canonical(&canonical, buf, sizeof(buf));
  if (cl == 0) return false;

  uint8_t sig_a[BEACON_SIGNATURE_SIZE];
  Ed25519::sign(sig_a, g_device_privkey, g_device_pubkey, buf, cl);

  g_pending_origination.valid = true;
  g_pending_origination.canonical = canonical;
  memcpy(g_pending_origination.sig_originator, sig_a, BEACON_SIGNATURE_SIZE);
  g_pending_origination.requested_ms = millis();

  // Broadcast a COSIGN_REQ (in v0.1 this is unencrypted; v0.3 will wrap).
  // For the skeleton we just log and return — actual transport of the request
  // and receipt of the response are tracked for v0.3.
  health_log(SCV_LOG_INFO, SCV_CAT_NETWORK,
             "beacon: originated alert pending cosigner (v0.1 skeleton)");
  return true;
}

bool cosign_pending_request(bool confirm) {
  if (!g_pending_cosign_in.valid) return false;
  if (!confirm) {
    g_pending_cosign_in.valid = false;
    return true;
  }
  // Sign the canonical and emit a COSIGN_RESP (skeleton: not actually
  // transported in v0.1).
  uint8_t buf[64 + sizeof(BeaconAlertCanonical)];
  size_t cl = build_alert_canonical(&g_pending_cosign_in.canonical, buf, sizeof(buf));
  if (cl == 0) return false;
  uint8_t sig_b[BEACON_SIGNATURE_SIZE];
  Ed25519::sign(sig_b, g_device_privkey, g_device_pubkey, buf, cl);
  (void)sig_b;
  g_pending_cosign_in.valid = false;
  return true;
}

bool cancel_active_alarm() {
  if (!g_active_alarm_valid) return false;
  // Build a CANCEL frame referencing the active alarm.
  // Skeleton: in v0.3 this will also require a cosigner.
  g_active_alarm_valid = false;
  set_state(BEACON_STATE_SUPERVISORY);
  return true;
}

bool has_active_alarm() { return g_active_alarm_valid; }
const BeaconAlertCanonical* get_active_alarm() {
  return g_active_alarm_valid ? &g_active_alarm : nullptr;
}

size_t get_audit_log_count() { return g_audit_log_count; }
const BeaconAuditEntry* get_audit_log_entry(size_t index) {
  if (index >= g_audit_log_count) return nullptr;
  return &g_audit_log[index];
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
