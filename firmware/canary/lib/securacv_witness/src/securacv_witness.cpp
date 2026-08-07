/*
 * SecuraCV Canary — Witness Record Management Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_witness.h"
#include "securacv_crypto.h"
#include "securacv_auth.h"
#include "canary_config.h"
// The birth-day decision itself — board-agnostic, host-tested
// (firmware/tests_host/test_birth_day.cpp). This file owns only the NVS.
#include "identity/birth_day.h"

#if FEATURE_DIAGNOSTICS
#include "securacv_diagnostics.h"
#endif

#include <Arduino.h>
#include <Crypto.h>
#include <Ed25519.h>

#if FEATURE_SD_STORAGE
#include <SD.h>
#include "securacv_storage.h"
// Pure line-format + SD-wins reconciliation logic, shared byte-for-byte
// with the canary-wap tree (canonical: firmware/common/witness/,
// host-tested by test_witness_store_logic.cpp).
#include "witness/witness_store.h"
#endif

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

// Recent witness record ring — display-only, for serving GET /api/witness?last=N.
// The tamper-evident guarantee lives in the hash chain (chain_head/NVS + signatures),
// NOT this ring; it is bounded RAM and intentionally volatile across reboot. Full
// history remains in the signed SD chain. Mirrors the health-log ring pattern above.
static const size_t WITNESS_RECORD_RING_SIZE = 32;
static WitnessRecord g_record_ring[WITNESS_RECORD_RING_SIZE];
static size_t g_record_ring_head = 0;
static size_t g_record_ring_count = 0;
// The ring is written from the record-creation task and read from the HTTP server task on the
// other core; a WitnessRecord copy is not atomic, so guard both with a short critical section
// (same approach as the vision history ring). The chain's integrity does not depend on this ring.
static portMUX_TYPE g_record_ring_mux = portMUX_INITIALIZER_UNLOCKED;

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

size_t witness_get_record_ring_size() { return WITNESS_RECORD_RING_SIZE; }
size_t witness_get_record_count() { return g_record_ring_count; }
size_t witness_get_record_head() { return g_record_ring_head; }

// Copy one ring slot under the lock so a reader on the HTTP task never observes a torn record
// mid-write. Returns false for an out-of-range index.
bool witness_copy_record_at(size_t ring_index, WitnessRecord* out) {
  if (out == nullptr || ring_index >= WITNESS_RECORD_RING_SIZE) return false;
  portENTER_CRITICAL(&g_record_ring_mux);
  *out = g_record_ring[ring_index];
  portEXIT_CRITICAL(&g_record_ring_mux);
  return true;
}

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
    case RECORD_POWER_SHUTDOWN:   return "PWSD";
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

  // Establish the keypair FIRST: the device ID / AP SSID are derived from the
  // pubkey fingerprint (never the MAC — event_contract §10 / privacy Invariant
  // III), so the fingerprint must exist before we build those handles.
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
    // This boot made the key, so this boot is the only one that can honestly
    // say how old it is when a clock finally arrives.
    g_device.key_is_new = true;
    g_device.key_born_ms = millis();
  }

  // What we already know about when this key was born. Loaded before anything
  // can offer a clock, so the "already recorded" rule is in force from the
  // first opportunity to stamp.
  g_device.born_day = nvs_load_u32(NVS_KEY_BORN, 0);
  g_device.born_exact = nvs_load_u32(NVS_KEY_BORN_EX, 0) != 0;

  // Derive public key + fingerprint
  Ed25519::derivePublicKey(g_device.pubkey, g_device.privkey);
  crypto_fingerprint(g_device.pubkey, g_device.pubkey_fp);

  // Device ID / AP SSID from the pubkey fingerprint (no MAC read).
  generate_device_id(g_device.device_id, sizeof(g_device.device_id),
                     DEVICE_ID_PREFIX, g_device.pubkey_fp);
  generate_ap_ssid(g_device.ap_ssid, sizeof(g_device.ap_ssid),
                   g_device.pubkey_fp);

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

  // Provision the transport-layer bearer credential. Owned entirely by
  // securacv_auth — we just trigger derivation here so it happens during
  // device boot. The credential MUST NOT enter the witness chain.
  if (!auth_load_or_derive(g_device.privkey)) {
    Serial.println("[!!] Bearer credential provisioning failed");
    return false;
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

bool witness_note_wall_clock(uint32_t unix_s) {
  birth::Stamp stored;
  stored.day = g_device.born_day;
  stored.exact = g_device.born_exact;

  birth::Observation now;
  now.unix_s = unix_s;
  now.key_age_known = g_device.key_is_new;
  now.key_age_s = g_device.key_is_new
                      ? (millis() - g_device.key_born_ms) / 1000u
                      : 0u;

  birth::Stamp fresh;
  if (!birth::consider(stored, now, &fresh)) return false;

  // Order matters: the day is what `recorded()` tests, so writing it last
  // means a power cut between the two writes leaves no half-stamped birth —
  // the next boot simply tries again.
  nvs_store_u32(NVS_KEY_BORN_EX, fresh.exact ? 1 : 0);
  nvs_store_u32(NVS_KEY_BORN, fresh.day);
  g_device.born_day = fresh.day;
  g_device.born_exact = fresh.exact;

  Serial.printf("[BIRTH] key %s day %lu (UTC)\n",
                fresh.exact ? "born on" : "first dated",
                (unsigned long)fresh.day);
  return true;
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
// DURABLE SD LOG (/WITNESS/records.jsonl)
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_SD_STORAGE

// Latched warning so a missing/failing card logs once per outage, not once
// per record (records keep chaining in RAM/NVS; the verifier reports the
// resulting seq gap as a card-absent segment, honestly).
static bool g_witness_sd_warned = false;

static bool sd_append_fail(const char* why) {
  g_health.sd_errors++;
  if (!g_witness_sd_warned) {
    g_witness_sd_warned = true;
    log_health(LOG_LEVEL_WARNING, LOG_CAT_STORAGE,
               "Witness SD append failed", why);
  }
  return false;
}

// Append one signed record to the durable log. Loop-task only (every
// record producer — setup, loop, the *_process() event callbacks, and
// power_graceful_shutdown — runs on the Arduino loopTask; the HTTP task
// never touches SD). FILE_APPEND + close-per-write: a power cut at most
// loses the in-flight line, never the file structure. The torn tail is
// tolerated by both the boot recovery below and the offline verifier.
static bool sd_append_record(const WitnessRecord* rec) {
  if (!storage_is_mounted()) return sd_append_fail("no card");

  char line[witness_store::RECORD_LINE_MAX];
  const size_t n = witness_store::line_build(
      line, sizeof(line), rec->seq, rec->time_bucket, (uint8_t)rec->type,
      rec->payload_hash, rec->prev_hash, rec->chain_hash, rec->signature);
  if (n == 0) return sd_append_fail("line build failed");

  if (!SD.exists("/WITNESS") && !SD.mkdir("/WITNESS"))
    return sd_append_fail("mkdir /WITNESS failed");

  File f = SD.open("/WITNESS/records.jsonl", FILE_APPEND);
  if (!f) return sd_append_fail("open failed");
  const size_t wrote = f.write((const uint8_t*)line, n);
  f.close();
  if (wrote != n) return sd_append_fail("short write (card full?)");

  g_health.sd_writes++;
#if FEATURE_DIAGNOSTICS
  diag_record_sd_write_bytes(n, true);
#endif
  if (g_witness_sd_warned) {
    g_witness_sd_warned = false;  // healthy again — re-arm the warning latch
    log_health(LOG_LEVEL_NOTICE, LOG_CAT_STORAGE,
               "Witness SD append healthy again", nullptr);
  }
  return true;
}

#endif  // FEATURE_SD_STORAGE

bool witness_recover_from_sd() {
#if FEATURE_SD_STORAGE
  // The NVS cache persists only every SD_PERSIST_INTERVAL records, so
  // after a power cut (or an NVS wipe/reflash while the card kept its
  // history) the cached head can be BEHIND the last record actually
  // signed — resuming from it would fork the supposedly append-only
  // chain. SD wins when its tail is strictly ahead AND the tail record's
  // signature verifies under THIS device's public key: a foreign card
  // (another device's history) or a tampered tail must never move our
  // chain head. Call once, right after the SD card mounts and BEFORE the
  // first record of the boot is created.
  if (!storage_is_mounted()) return false;

  File f = SD.open("/WITNESS/records.jsonl", FILE_READ);
  if (!f) return false;
  const size_t size = f.size();
  if (size == 0) {
    f.close();
    return false;
  }

  char tail[witness_store::TAIL_READ + 1];
  const size_t want =
      (size < witness_store::TAIL_READ) ? size : witness_store::TAIL_READ;
  if (!f.seek(size - want)) {
    f.close();
    return false;
  }
  const size_t got = f.read((uint8_t*)tail, want);
  f.close();
  if (got == 0) return false;
  tail[got] = '\0';

  witness_store::TailRecord rec;
  if (!witness_store::tail_parse(tail, &rec)) return false;
  if (!witness_store::sd_wins(g_device.seq, rec.seq)) return false;

  // Bind seq/tb to the signature: the Ed25519 signature covers only the
  // chain hash, so recompute that hash from the line's own fields and
  // require a match — otherwise a tampered card could keep a genuine
  // ch/sig pair while editing seq to move the device sequence to an
  // arbitrary value (the same check verify_witness_log.py runs offline).
  uint8_t recomputed[32];
  compute_chain_hash(rec.prev, rec.ph, rec.seq, rec.tb, recomputed);
  if (memcmp(recomputed, rec.ch, 32) != 0) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_STORAGE,
               "Witness SD tail ignored: chain hash mismatch", nullptr);
    return false;
  }
  if (!crypto_verify(g_device.pubkey, rec.ch, 32, rec.sig)) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_STORAGE,
               "Witness SD tail ignored: signature not ours", nullptr);
    return false;
  }

  g_device.seq = rec.seq;
  memcpy(g_device.chain_head, rec.ch, 32);
  witness_persist_chain_state();
  char detail[32];
  snprintf(detail, sizeof(detail), "seq %u", (unsigned)rec.seq);
  log_health(LOG_LEVEL_NOTICE, LOG_CAT_STORAGE,
             "Witness chain head recovered from SD", detail);
  return true;
#else
  return false;
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// RECORD CREATION
// ════════════════════════════════════════════════════════════════════════════

bool witness_create_record_gps(const uint8_t* payload, size_t len, RecordType type,
                               const GpsTimeAttestation* gps, WitnessRecord* out) {
  if (out == nullptr) return false;

  // Hash payload
  uint8_t payload_hash[32];
  sha256_domain("securacv:fw:payload:v1", payload, len, payload_hash);

  // Update chain
  uint32_t tb = time_bucket();
  update_chain(payload_hash, tb, out);
  out->type = type;
  out->payload_len = len;

  // GPS time attestation
  if (gps && gps->available && gps->fix_age_ms < 30000) {
    out->time_source = TIME_SRC_GPS_UTC;
    out->gps_time = *gps;
  } else {
    out->time_source = TIME_SRC_DEVICE;
    memset(&out->gps_time, 0, sizeof(out->gps_time));
  }

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

  // Append to the display-only recent-record ring (newest overwrites oldest).
  // Shared by both create paths; the chain integrity itself lives in the hash
  // chain, so losing this ring on reboot is harmless.
  portENTER_CRITICAL(&g_record_ring_mux);
  g_record_ring[g_record_ring_head] = *out;
  g_record_ring_head = (g_record_ring_head + 1) % WITNESS_RECORD_RING_SIZE;
  if (g_record_ring_count < WITNESS_RECORD_RING_SIZE) g_record_ring_count++;
  portEXIT_CRITICAL(&g_record_ring_mux);

  // Durable tier FIRST, NVS second (ordering is load-bearing): the SD
  // append must precede the NVS persist so a crash between the two leaves
  // SD ahead — exactly the state the boot recovery (SD-wins) repairs. If
  // NVS advanced first and the append tore, reboot would resume from a
  // head the card never received and the next line would chain across an
  // unverifiable gap. A failed append (card absent/full) is tolerated:
  // the chain keeps advancing and the offline verifier reports the seq
  // gap as a card-absent segment.
  #if FEATURE_SD_STORAGE
  sd_append_record(out);
  #endif

  // Persist chain state periodically (fast-boot cache only — the durable
  // history lives in /WITNESS/records.jsonl).
  if ((g_device.seq - g_device.seq_persisted) >= SD_PERSIST_INTERVAL) {
    witness_persist_chain_state();
  }

  return true;
}

bool witness_create_record(const uint8_t* payload, size_t len, RecordType type, WitnessRecord* out) {
  return witness_create_record_gps(payload, len, type, NULL, out);
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
