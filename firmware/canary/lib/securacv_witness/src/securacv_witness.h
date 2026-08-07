/*
 * SecuraCV Canary — Witness Record Management
 *
 * PWK witness record creation, signing, and chain management.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_WITNESS_H
#define SECURACV_WITNESS_H

#include <Arduino.h>
#include <stdint.h>
#include "canary_config.h"
#include "log_level.h"

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

enum FixState : uint8_t {
  STATE_NO_FIX       = 0,
  STATE_FIX_ACQUIRED = 1,
  STATE_STATIONARY   = 2,
  STATE_MOVING       = 3,
  STATE_FIX_LOST     = 4
};

enum RecordType : uint8_t {
  RECORD_BOOT_ATTESTATION = 0,
  RECORD_WITNESS_EVENT    = 1,
  RECORD_TAMPER_ALERT     = 2,
  RECORD_STATE_CHANGE     = 3,
  RECORD_POWER_SHUTDOWN   = 4,
};

enum TimeSource : uint8_t {
  TIME_SRC_DEVICE   = 0,
  TIME_SRC_GPS_UTC  = 1,
  TIME_SRC_RTC_SYNC = 2,
};

struct GpsTimeAttestation {
  bool        available;
  uint16_t    year;
  uint8_t     month;
  uint8_t     day;
  uint8_t     hour;
  uint8_t     minute;
  uint8_t     second;
  uint8_t     fix_quality;
  uint8_t     satellites;
  uint32_t    fix_age_ms;
};

struct WitnessRecord {
  uint32_t    seq;
  uint32_t    time_bucket;
  RecordType  type;
  uint8_t     payload_hash[32];
  uint8_t     prev_hash[32];
  uint8_t     chain_hash[32];
  uint8_t     signature[64];
  size_t      payload_len;
  bool        verified;
  TimeSource  time_source;
  GpsTimeAttestation gps_time;
};

struct DeviceIdentity {
  uint8_t  privkey[32];
  uint8_t  pubkey[32];
  uint8_t  pubkey_fp[8];
  uint8_t  chain_head[32];
  uint32_t seq;
  uint32_t seq_persisted;
  uint32_t boot_count;
  uint32_t boot_ms;
  uint32_t tamper_count;
  uint32_t log_seq;
  // ── when this key was born ────────────────────────────────────────────────
  // The device id, the AP SSID and the app's birth certificate all derive from
  // the keypair, so "how old is this Canary" is really "how old is this key".
  // A key is always born before any clock exists (no battery-backed RTC — the
  // chip boots at the epoch and only learns the date from GPS/NTP later), so
  // these are filled in on the first believable clock and then never again.
  // See firmware/common/identity/birth_day.h for the three rules.
  uint32_t born_day;      ///< Days since the Unix epoch; 0 = never dated.
  bool     born_exact;    ///< False ⇒ first DATED, not born. Don't call it a birthday.
  uint32_t key_born_ms;   ///< millis() when this boot generated the key…
  bool     key_is_new;    ///< …meaningful only if this boot is the one that made it.
  bool     initialized;
  bool     tamper_active;
  char     device_id[32];
  char     ap_ssid[32];
};

struct SystemHealth {
  uint32_t records_created;
  uint32_t records_verified;
  uint32_t verify_failures;
  uint32_t gps_sentences;
  uint32_t gga_count;
  uint32_t rmc_count;
  uint32_t gsa_count;
  uint32_t gsv_count;
  uint32_t vtg_count;
  uint32_t chain_persists;
  uint32_t state_changes;
  uint32_t tamper_events;
  uint32_t uptime_sec;
  uint32_t free_heap;
  uint32_t min_heap;
  uint32_t gps_lock_ms;
  uint32_t http_requests;
  uint32_t http_errors;
  uint32_t sd_writes;
  uint32_t sd_errors;
  uint32_t logs_stored;
  uint32_t logs_unacked;
  bool     gps_healthy;
  bool     crypto_healthy;
  bool     sd_healthy;
  bool     wifi_active;
  uint16_t battery_mv;
  uint8_t  battery_soc;
  uint8_t  charge_state;
  uint8_t  power_source;
  int16_t  battery_trend;
};

// Health log ring buffer entry
struct HealthLogRingEntry {
  uint32_t seq;
  uint32_t timestamp_ms;
  LogLevel level;
  LogCategory category;
  AckStatus ack_status;
  char message[80];
  char detail[48];
};

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL STATE ACCESSORS
// ════════════════════════════════════════════════════════════════════════════

// Get device identity
DeviceIdentity& witness_get_device();

// Get system health
SystemHealth& witness_get_health();

// Get last witness record
WitnessRecord& witness_get_last_record();

// Get current fix state
FixState witness_get_state();

// Get speed EMA value
float witness_get_speed_ema();

// ════════════════════════════════════════════════════════════════════════════
// INITIALIZATION
// ════════════════════════════════════════════════════════════════════════════

// Provision device identity (keys, chain state)
bool witness_provision_device();

// Reconcile the chain head with the durable SD log (/WITNESS/records.jsonl).
// NVS is only a fast-boot cache persisted every SD_PERSIST_INTERVAL records;
// the SD tail is adopted when it is strictly ahead AND its signature
// verifies under this device's public key (a foreign or tampered card must
// never move the head). Call once, after the SD card mounts and before the
// first record of the boot is created. Returns true if the head advanced.
bool witness_recover_from_sd();

// ════════════════════════════════════════════════════════════════════════════
// RECORD CREATION
// ════════════════════════════════════════════════════════════════════════════

// Create a witness record with given payload
bool witness_create_record(const uint8_t* payload, size_t len, RecordType type, WitnessRecord* out);

// Create a witness record with GPS time attestation (pass NULL for no GPS)
bool witness_create_record_gps(const uint8_t* payload, size_t len, RecordType type,
                               const GpsTimeAttestation* gps, WitnessRecord* out);

// Verify record signature
bool witness_verify_record(const WitnessRecord* rec);

// Persist chain state to NVS
void witness_persist_chain_state();

// Offer the current wall clock to the birth-day recorder. Safe and cheap to
// call from the main loop at any cadence: it returns immediately once a day is
// recorded (which is once, for the life of the key) or while the clock is still
// the boot epoch. Returns true only on the one call that actually stamped —
// callers can log it, but nothing depends on catching that moment.
bool witness_note_wall_clock(uint32_t unix_s);

// ════════════════════════════════════════════════════════════════════════════
// STATE MACHINE
// ════════════════════════════════════════════════════════════════════════════

// Update state based on GPS fix and current state
void witness_update_state(bool has_valid_fix, uint32_t last_fix_ms, float speed_mps);

// Log state transition
void witness_log_state_transition(FixState from, FixState to, const char* reason);

// ════════════════════════════════════════════════════════════════════════════
// HEALTH LOGGING
// ════════════════════════════════════════════════════════════════════════════

// Log a health event
void log_health(LogLevel level, LogCategory category, const char* message, const char* detail = nullptr);

// Public wrapper for external modules
void health_log(LogLevel level, LogCategory category, const char* message);

// Acknowledge a log entry
bool acknowledge_log_entry(uint32_t log_seq, AckStatus new_status, const char* reason);

// Get health log ring buffer
HealthLogRingEntry* witness_get_health_log_ring();
size_t witness_get_health_log_count();
size_t witness_get_health_log_head();

// Recent witness-record ring (display-only; serves GET /api/witness?last=N).
// Bounded and volatile across reboot — integrity lives in the hash chain, not here.
// Reads go through witness_copy_record_at() so a concurrent write can't be observed torn.
size_t witness_get_record_ring_size();
size_t witness_get_record_count();
size_t witness_get_record_head();
bool   witness_copy_record_at(size_t ring_index, WitnessRecord* out);

// ════════════════════════════════════════════════════════════════════════════
// UTILITIES
// ════════════════════════════════════════════════════════════════════════════

// Get state name
const char* state_name(FixState s);
const char* state_name_short(FixState s);
const char* record_type_name(RecordType t);

// Time bucket calculation
uint32_t time_bucket();
uint32_t uptime_seconds();

// Format uptime as HH:MM:SS
void format_uptime(char* out, size_t cap, uint32_t secs);

#endif // SECURACV_WITNESS_H
