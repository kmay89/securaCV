/*
 * SecuraCV Canary — Witness Record Management Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_witness.h"
#include "securacv_crypto.h"
#include "canary_config.h"

#include <Arduino.h>
#include <Crypto.h>
#include <Ed25519.h>

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL STATE
// ════════════════════════════════════════════════════════════════════════════

static DeviceIdentity g_device;
static SystemHealth g_health;
static WitnessRecord g_last_record;
static FixState g_state = STATE_NO_FIX;
static FixState g_pending_state = STATE_NO_FIX;
static uint32_t g_state_entered_ms = 0;
static uint32_t g_pending_state_ms = 0;
static float g_speed_ema = 0.0f;

// Health log ring buffer
static const size_t HEALTH_LOG_RING_SIZE = 100;
static HealthLogRingEntry g_health_log_ring[HEALTH_LOG_RING_SIZE];
static size_t g_health_log_ring_head = 0;
static size_t g_health_log_ring_count = 0;

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL STATE ACCESSORS
// ════════════════════════════════════════════════════════════════════════════

DeviceIdentity& witness_get_device() { return g_device; }
SystemHealth& witness_get_health() { return g_health; }
WitnessRecord& witness_get_last_record() { return g_last_record; }
FixState witness_get_state() { return g_state; }
float witness_get_speed_ema() { return g_speed_ema; }

HealthLogRingEntry* witness_get_health_log_ring() { return g_health_log_ring; }
size_t witness_get_health_log_count() { return g_health_log_ring_count; }
size_t witness_get_health_log_head() { return g_health_log_ring_head; }

// ════════════════════════════════════════════════════════════════════════════
// UTILITIES
// ════════════════════════════════════════════════════════════════════════════

const char* state_name(FixState s) {
  switch (s) {
    case STATE_NO_FIX:       return "NO_FIX";
    case STATE_FIX_ACQUIRED: return "FIX_ACQ";
    case STATE_STATIONARY:   return "STATIC";
    case STATE_MOVING:       return "MOVING";
    case STATE_FIX_LOST:     return "LOST";
    default:                 return "???";
  }
}

const char* state_name_short(FixState s) {
  switch (s) {
    case STATE_NO_FIX:       return "NOFIX";
    case STATE_FIX_ACQUIRED: return "ACQRD";
    case STATE_STATIONARY:   return "STAT";
    case STATE_MOVING:       return "MOVE";
    case STATE_FIX_LOST:     return "LOST";
    default:                 return "???";
  }
}

const char* record_type_name(RecordType t) {
  switch (t) {
    case RECORD_BOOT_ATTESTATION: return "BOOT";
    case RECORD_WITNESS_EVENT:    return "EVNT";
    case RECORD_TAMPER_ALERT:     return "TAMP";
    case RECORD_STATE_CHANGE:     return "STCH";
    default:                      return "???";
  }
}

uint32_t time_bucket() {
  return millis() / TIME_BUCKET_MS;
}

uint32_t uptime_seconds() {
  return millis() / 1000;
}

void format_uptime(char* out, size_t cap, uint32_t secs) {
  uint32_t h = secs / 3600;
  uint32_t m = (secs % 3600) / 60;
  uint32_t s = secs % 60;
  snprintf(out, cap, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
}

// ════════════════════════════════════════════════════════════════════════════
// DEVICE PROVISIONING
// ════════════════════════════════════════════════════════════════════════════

bool witness_provision_device() {
  Serial.println("[..] Provisioning device identity...");

  // Generate device ID from MAC
  generate_device_id(g_device.device_id, sizeof(g_device.device_id), DEVICE_ID_PREFIX);
  generate_ap_ssid(g_device.ap_ssid, sizeof(g_device.ap_ssid));

  // Try to load existing key
  if (nvs_load_key(g_device.privkey)) {
    Serial.println("[OK] Loaded existing keypair from NVS");
  } else {
    Serial.println("[..] Generating new keypair...");
    if (!crypto_generate_keypair(g_device.privkey, g_device.pubkey)) {
      Serial.println("[!!] Keypair generation failed");
      return false;
    }
    if (!nvs_store_key(g_device.privkey)) {
      Serial.println("[!!] Failed to store keypair");
      return false;
    }
    Serial.println("[OK] New keypair generated and stored");
  }

  // Derive public key
  Ed25519::derivePublicKey(g_device.pubkey, g_device.privkey);
  crypto_fingerprint(g_device.pubkey, g_device.pubkey_fp);

  // Load chain state
  g_device.seq = nvs_load_u32(NVS_KEY_SEQ, 0);
  g_device.seq_persisted = g_device.seq;
  g_device.boot_count = nvs_load_u32(NVS_KEY_BOOTS, 0) + 1;
  nvs_store_u32(NVS_KEY_BOOTS, g_device.boot_count);
  g_device.log_seq = nvs_load_u32(NVS_KEY_LOGSEQ, 0);

  if (!nvs_load_bytes(NVS_KEY_CHAIN, g_device.chain_head, 32)) {
    // Initialize genesis chain hash
    sha256_domain("securacv:genesis:v1", (const uint8_t*)g_device.device_id,
                  strlen(g_device.device_id), g_device.chain_head);
    nvs_store_bytes(NVS_KEY_CHAIN, g_device.chain_head, 32);
  }

  g_device.boot_ms = millis();
  g_device.initialized = true;
  g_health.crypto_healthy = true;
  g_health.min_heap = ESP.getFreeHeap();
  g_state_entered_ms = millis();
  g_pending_state = STATE_NO_FIX;

  Serial.printf("[OK] Device ID: %s\n", g_device.device_id);
  Serial.printf("[OK] Boot count: %u\n", g_device.boot_count);
  Serial.printf("[OK] Chain seq: %u\n", g_device.seq);

  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// CHAIN OPERATIONS
// ════════════════════════════════════════════════════════════════════════════

static void update_chain(const uint8_t payload_hash[32], uint32_t tb, WitnessRecord* rec) {
  rec->seq = ++g_device.seq;
  rec->time_bucket = tb;
  memcpy(rec->prev_hash, g_device.chain_head, 32);
  memcpy(rec->payload_hash, payload_hash, 32);

  compute_chain_hash(rec->prev_hash, payload_hash, rec->seq, tb, rec->chain_hash);
  memcpy(g_device.chain_head, rec->chain_hash, 32);
}

void witness_persist_chain_state() {
  nvs_store_u32(NVS_KEY_SEQ, g_device.seq);
  nvs_store_bytes(NVS_KEY_CHAIN, g_device.chain_head, 32);
  g_device.seq_persisted = g_device.seq;
  g_health.chain_persists++;

  #if DEBUG_CHAIN
  Serial.print("[CHAIN] Persisted seq=");
  Serial.println(g_device.seq);
  #endif
}

// ════════════════════════════════════════════════════════════════════════════
// RECORD CREATION
// ════════════════════════════════════════════════════════════════════════════

bool witness_create_record(const uint8_t* payload, size_t len, RecordType type, WitnessRecord* out) {
  // Hash payload
  uint8_t payload_hash[32];
  sha256_domain("securacv:payload:v1", payload, len, payload_hash);

  // Update chain
  uint32_t tb = time_bucket();
  update_chain(payload_hash, tb, out);
  out->type = type;
  out->payload_len = len;

  // Sign chain hash
  crypto_sign(g_device.privkey, g_device.pubkey, out->chain_hash, 32, out->signature);

  // Verify immediately
  out->verified = crypto_verify(g_device.pubkey, out->chain_hash, 32, out->signature);

  if (!out->verified) {
    g_health.verify_failures++;
    return false;
  }

  g_health.records_created++;
  g_health.records_verified++;

  // Persist chain state periodically
  if ((g_device.seq - g_device.seq_persisted) >= SD_PERSIST_INTERVAL) {
    witness_persist_chain_state();
  }

  #if FEATURE_SD_STORAGE
  g_health.sd_writes++;
  #endif

  return true;
}

bool witness_verify_record(const WitnessRecord* rec) {
  return crypto_verify(g_device.pubkey, rec->chain_hash, 32, rec->signature);
}

// ════════════════════════════════════════════════════════════════════════════
// STATE MACHINE
// ════════════════════════════════════════════════════════════════════════════

void witness_log_state_transition(FixState from, FixState to, const char* reason) {
  #if FEATURE_STATE_LOG
  g_health.state_changes++;

  char msg[64];
  snprintf(msg, sizeof(msg), "%s -> %s", state_name(from), state_name(to));
  log_health(LOG_LEVEL_NOTICE, LOG_CAT_GPS, msg, reason);
  #endif
}

void witness_update_state(bool has_valid_fix, uint32_t last_fix_ms, float speed_mps) {
  uint32_t now = millis();
  g_speed_ema = g_speed_ema * (1.0f - SPEED_EMA_ALPHA) + speed_mps * SPEED_EMA_ALPHA;

  FixState cur = g_state;
  FixState desired = cur;
  const char* reason = nullptr;

  bool has_recent_fix = has_valid_fix && (now - last_fix_ms < FIX_LOST_TIMEOUT_MS);

  if (!has_recent_fix) {
    if (cur != STATE_NO_FIX && cur != STATE_FIX_LOST) {
      desired = STATE_FIX_LOST;
      reason = "timeout";
    } else if (cur == STATE_FIX_LOST && (now - g_state_entered_ms) > 10000) {
      desired = STATE_NO_FIX;
      reason = "prolonged_loss";
    }
  } else {
    if (cur == STATE_NO_FIX || cur == STATE_FIX_LOST) {
      desired = STATE_FIX_ACQUIRED;
      reason = "fix_obtained";
      g_health.gps_healthy = true;
    } else if (cur == STATE_FIX_ACQUIRED) {
      if (g_speed_ema >= MOVING_THRESHOLD_MPS) {
        desired = STATE_MOVING;
        reason = "speed_high";
      } else if (g_speed_ema <= STATIC_THRESHOLD_MPS) {
        desired = STATE_STATIONARY;
        reason = "speed_low";
      }
    } else if (cur == STATE_STATIONARY && g_speed_ema >= MOVING_THRESHOLD_MPS) {
      desired = STATE_MOVING;
      reason = "started_moving";
    } else if (cur == STATE_MOVING && g_speed_ema <= STATIC_THRESHOLD_MPS) {
      desired = STATE_STATIONARY;
      reason = "stopped";
    }
  }

  if (desired != cur) {
    bool needs_hysteresis =
      (cur == STATE_STATIONARY && desired == STATE_MOVING) ||
      (cur == STATE_MOVING && desired == STATE_STATIONARY);

    if (needs_hysteresis) {
      if (g_pending_state != desired) {
        g_pending_state = desired;
        g_pending_state_ms = now;
      }

      if ((now - g_pending_state_ms) >= STATE_HYSTERESIS_MS) {
        witness_log_state_transition(cur, desired, reason);
        g_state_entered_ms = now;
        g_state = desired;
        g_pending_state = desired;
      }
    } else {
      witness_log_state_transition(cur, desired, reason);
      g_state_entered_ms = now;
      g_state = desired;
      g_pending_state = desired;
    }
  } else {
    g_pending_state = cur;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// HEALTH LOGGING
// ════════════════════════════════════════════════════════════════════════════

void log_health(LogLevel level, LogCategory category, const char* message, const char* detail) {
  // Skip DEBUG by default
  if (level < LOG_LEVEL_INFO) return;

  HealthLogRingEntry& entry = g_health_log_ring[g_health_log_ring_head];
  entry.seq = ++g_device.log_seq;
  entry.timestamp_ms = millis();
  entry.level = level;
  entry.category = category;
  entry.ack_status = ACK_STATUS_UNREAD;

  strncpy(entry.message, message ? message : "", sizeof(entry.message) - 1);
  entry.message[sizeof(entry.message) - 1] = '\0';

  if (detail) {
    strncpy(entry.detail, detail, sizeof(entry.detail) - 1);
    entry.detail[sizeof(entry.detail) - 1] = '\0';
  } else {
    entry.detail[0] = '\0';
  }

  g_health_log_ring_head = (g_health_log_ring_head + 1) % HEALTH_LOG_RING_SIZE;
  if (g_health_log_ring_count < HEALTH_LOG_RING_SIZE) {
    g_health_log_ring_count++;
  }

  g_health.logs_stored++;
  if (log_level_requires_attention(level)) {
    g_health.logs_unacked++;
  }

  // Also print to Serial
  Serial.printf("[%s/%s] %s", log_level_name(level), log_category_name(category), message);
  if (detail && detail[0]) {
    Serial.printf(" | %s", detail);
  }
  Serial.println();
}

void health_log(LogLevel level, LogCategory category, const char* message) {
  log_health(level, category, message, nullptr);
}

bool acknowledge_log_entry(uint32_t log_seq, AckStatus new_status, const char* reason) {
  for (size_t i = 0; i < g_health_log_ring_count; i++) {
    HealthLogRingEntry& entry = g_health_log_ring[i];
    if (entry.seq == log_seq) {
      if (entry.ack_status == ACK_STATUS_UNREAD && log_level_requires_attention(entry.level)) {
        if (g_health.logs_unacked > 0) g_health.logs_unacked--;
      }
      entry.ack_status = new_status;
      return true;
    }
  }
  return false;
}
