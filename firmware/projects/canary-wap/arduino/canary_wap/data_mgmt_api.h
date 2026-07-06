/*
 * SecuraCV Canary — Data Management API (header-only)
 *
 * Ported from PIO's securacv_data_mgmt library for the Arduino sketch build.
 *
 * Provides:
 *   - SD log rotation (health directory only; /WITNESS and /CHAIN are
 *     never rotated — Invariant IV)
 *   - Chain backup/restore (NVS <-> /CHAIN/backup.bin on SD)
 *   - (Chain verification lives off-device: the /WITNESS/records.jsonl
 *     lines carry everything a verifier needs — see witness_store.h.)
 *   - Auto-processing (rate-limited rotation + hourly backup)
 *   - Stats (file counts, rotation totals, last rotation/backup times)
 *
 * Header-only on purpose, matching the WAP *_api.h pattern.
 * Include from canary_wap.ino AFTER sd_storage.h, hardware_state.h,
 * nvs_store.h, and the device identity struct are available.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_DATA_MGMT_API_H
#define SECURACV_DATA_MGMT_API_H

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <string.h>

#include "build_config.h"
#include "log_level.h"
#include "health_log.h"
#include "hardware_state.h"
#include <mbedtls/md.h>

// Forward declaration — defined in canary_wap.ino
static void hmac_sha256(const uint8_t* key, size_t key_len,
                        const uint8_t* data, size_t data_len,
                        uint8_t out[32]);

// ════════════════════════════════════════════════════════════════════════════
// FEATURE GATE
// ════════════════════════════════════════════════════════════════════════════

#ifndef FEATURE_DATA_MGMT
#define FEATURE_DATA_MGMT 0
#endif

#if FEATURE_DATA_MGMT && FEATURE_SD_STORAGE

namespace datamgmt {

// ════════════════════════════════════════════════════════════════════════════
// ROTATION POLICY
//
// /WITNESS and /CHAIN are NEVER rotated — the sealed log and its backup
// are the tamper-evident record (Invariant IV); only regenerable
// artifacts are bounded (/HEALTH here, /EXPORT via the operator route).
// ════════════════════════════════════════════════════════════════════════════

static const uint32_t MAX_HEALTH_FILES    = 200;
static const uint8_t  SD_USAGE_TARGET_PCT = 85;

// ════════════════════════════════════════════════════════════════════════════
// STATS
// ════════════════════════════════════════════════════════════════════════════

struct datamgmt_stats_t {
  uint32_t witness_files;
  uint32_t health_files;
  uint32_t files_rotated_total;
  uint32_t last_rotation_ms;
  uint32_t last_backup_ms;
  bool     backup_exists;
};

// ════════════════════════════════════════════════════════════════════════════
// CHAIN BACKUP BINARY FORMAT
//
// Offset  Size  Description
// 0       4     magic (0x53434256 "SCVB")
// 4       4     version (2)
// 8       4     seq
// 12      32    chain_head hash
// 44      32    HMAC-SHA256 of bytes 0..43 (keyed by device privkey)
// ════════════════════════════════════════════════════════════════════════════

static const uint32_t BACKUP_MAGIC   = 0x53434256;
static const uint32_t BACKUP_VERSION = 2;
static const size_t   BACKUP_PAYLOAD = 44;
static const size_t   BACKUP_SIZE    = 76;  // 44 payload + 32 HMAC

// ════════════════════════════════════════════════════════════════════════════
// RATE LIMITS
// ════════════════════════════════════════════════════════════════════════════

static const uint32_t PROCESS_INTERVAL_MS = 5UL * 60UL * 1000UL;   // 5 min
static const uint32_t BACKUP_INTERVAL_MS  = 60UL * 60UL * 1000UL;  // 1 hour

// ════════════════════════════════════════════════════════════════════════════
// ROTATION BATCH CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

static const size_t BATCH_SIZE   = 32;
static const size_t MAX_NAME_LEN = 48;

// ════════════════════════════════════════════════════════════════════════════
// INTERNAL STATE
// ════════════════════════════════════════════════════════════════════════════

static bool     s_initialized       = false;
static uint32_t s_last_process_ms   = 0;
static uint32_t s_last_backup_ms    = 0;

static uint32_t s_witness_files     = 0;
static uint32_t s_health_files      = 0;
static uint32_t s_files_rotated     = 0;
static uint32_t s_last_rotation_ms  = 0;
static bool     s_backup_exists     = false;

// ════════════════════════════════════════════════════════════════════════════
// SD USAGE HELPER
// ════════════════════════════════════════════════════════════════════════════

inline uint8_t sd_usage_pct() {
  uint64_t total = SD.totalBytes();
  if (total == 0) return 0;
  uint64_t used = SD.usedBytes();
  return (uint8_t)(used * 100 / total);
}

// ════════════════════════════════════════════════════════════════════════════
// FILE COUNT HELPER
// ════════════════════════════════════════════════════════════════════════════

inline uint32_t count_files(const char* dir_path) {
  File dir = SD.open(dir_path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  uint32_t count = 0;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (!entry.isDirectory()) count++;
    entry.close();
  }
  dir.close();
  return count;
}

// ════════════════════════════════════════════════════════════════════════════
// INSERTION SORT — stack-local buffer of short filenames
// ════════════════════════════════════════════════════════════════════════════

inline void sort_names(char names[][MAX_NAME_LEN], size_t n) {
  for (size_t i = 1; i < n; i++) {
    char tmp[MAX_NAME_LEN];
    memcpy(tmp, names[i], MAX_NAME_LEN);
    size_t j = i;
    while (j > 0 && strcmp(names[j - 1], tmp) > 0) {
      memcpy(names[j], names[j - 1], MAX_NAME_LEN);
      --j;
    }
    memcpy(names[j], tmp, MAX_NAME_LEN);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// ROTATION — batch-sort approach
//
// Opens the directory and collects up to BATCH_SIZE filenames into a
// stack-local buffer, sorts them lexicographically, then deletes from
// the front (oldest) until the file count drops to max_files. Repeats
// if more than BATCH_SIZE need deleting.
// ════════════════════════════════════════════════════════════════════════════

inline uint32_t rotate_impl(const char* dir_path, uint32_t max_files) {
  if (!sd_is_available()) return 0;

  uint32_t total_deleted = 0;

  for (;;) {
    uint32_t file_count = count_files(dir_path);
    if (file_count <= max_files) break;

    uint32_t to_delete = file_count - max_files;

    // Collect up to BATCH_SIZE names on the stack.
    char names[BATCH_SIZE][MAX_NAME_LEN];
    size_t collected = 0;

    File dir = SD.open(dir_path);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      break;
    }

    for (File entry = dir.openNextFile(); entry && collected < BATCH_SIZE;
         entry = dir.openNextFile()) {
      if (!entry.isDirectory()) {
        const char* n = entry.name();
        // entry.name() on ESP32 returns the full path; extract basename.
        const char* slash = strrchr(n, '/');
        const char* base = slash ? (slash + 1) : n;
        strncpy(names[collected], base, MAX_NAME_LEN - 1);
        names[collected][MAX_NAME_LEN - 1] = '\0';
        collected++;
      }
      entry.close();
    }
    dir.close();

    if (collected == 0) break;

    sort_names(names, collected);

    // Delete the oldest (first in sorted order) up to `to_delete`.
    size_t deleting = (to_delete < collected) ? (size_t)to_delete : collected;
    for (size_t i = 0; i < deleting; i++) {
      char full_path[MAX_NAME_LEN + 16];
      snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, names[i]);
      if (SD.remove(full_path)) {
        total_deleted++;
      } else {
        log_health(SCV_LOG_WARNING, SCV_CAT_STORAGE,
                   "Rotation: failed to delete file", names[i]);
      }
    }
  }

  return total_deleted;
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API — rotate_dir
// ════════════════════════════════════════════════════════════════════════════

inline uint32_t rotate_dir(const char* dir_path, uint32_t max_files) {
  uint32_t deleted = rotate_impl(dir_path, max_files);
  if (deleted > 0) s_files_rotated += deleted;
  return deleted;
}

// ════════════════════════════════════════════════════════════════════════════
// CHAIN BACKUP — write NVS chain state to /CHAIN/backup.bin on SD
//
// Needs the live chain_head and seq from the caller (the .ino owns these).
// ════════════════════════════════════════════════════════════════════════════

inline bool backup_chain(const uint8_t* chain_head, uint32_t seq,
                         const uint8_t* privkey) {
  if (!privkey || !sd_is_available()) return false;

  uint8_t buf[BACKUP_SIZE];
  memset(buf, 0, sizeof(buf));

  // Magic + version
  memcpy(buf + 0, &BACKUP_MAGIC,   4);
  memcpy(buf + 4, &BACKUP_VERSION, 4);

  // Sequence number
  memcpy(buf + 8, &seq, 4);

  // Chain head hash (32 bytes)
  memcpy(buf + 12, chain_head, 32);

  // HMAC-SHA256 of payload bytes 0..43, keyed by device private key
  uint8_t hmac[32];
  hmac_sha256(privkey, 32, buf, BACKUP_PAYLOAD, hmac);
  memcpy(buf + BACKUP_PAYLOAD, hmac, 32);

  // Ensure /CHAIN directory exists.
  if (!SD.exists("/CHAIN")) SD.mkdir("/CHAIN");

  File f = SD.open("/CHAIN/backup.bin", FILE_WRITE);
  bool ok = false;
  if (f) {
    ok = (f.write(buf, BACKUP_SIZE) == BACKUP_SIZE);
    f.close();
  }

  if (ok) {
    s_backup_exists  = true;
    s_last_backup_ms = millis();
    log_health(SCV_LOG_INFO, SCV_CAT_STORAGE,
               "Chain backup written", "/CHAIN/backup.bin");
  } else {
    log_health(SCV_LOG_ERROR, SCV_CAT_STORAGE,
               "Chain backup write failed", nullptr);
  }

  return ok;
}

// ════════════════════════════════════════════════════════════════════════════
// CHAIN RESTORE — read /CHAIN/backup.bin, validate CRC, return fields
//
// Returns the restored seq and chain_head to the caller. The caller is
// responsible for writing them back to NVS.
// ════════════════════════════════════════════════════════════════════════════

inline bool restore_chain(uint8_t* chain_head_out, uint32_t* seq_out,
                          const uint8_t* privkey) {
  if (!sd_is_available()) return false;

  File f = SD.open("/CHAIN/backup.bin", FILE_READ);
  if (!f) {
    log_health(SCV_LOG_ERROR, SCV_CAT_STORAGE,
               "Chain restore: backup not found", nullptr);
    return false;
  }

  uint8_t buf[BACKUP_SIZE];
  size_t rd = f.read(buf, BACKUP_SIZE);
  f.close();

  if (rd != BACKUP_SIZE) {
    log_health(SCV_LOG_ERROR, SCV_CAT_STORAGE,
               "Chain restore: truncated backup", nullptr);
    return false;
  }

  // Validate magic
  uint32_t magic;
  memcpy(&magic, buf + 0, 4);
  if (magic != BACKUP_MAGIC) {
    log_health(SCV_LOG_ERROR, SCV_CAT_STORAGE,
               "Chain restore: bad magic", nullptr);
    return false;
  }

  // Validate HMAC-SHA256
  if (!privkey) {
    log_health(SCV_LOG_ERROR, SCV_CAT_STORAGE,
               "Chain restore: no privkey for HMAC", nullptr);
    return false;
  }
  {
    uint8_t calc_hmac[32];
    hmac_sha256(privkey, 32, buf, BACKUP_PAYLOAD, calc_hmac);
    if (memcmp(buf + BACKUP_PAYLOAD, calc_hmac, 32) != 0) {
      log_health(SCV_LOG_ERROR, SCV_CAT_STORAGE,
                 "Chain restore: HMAC mismatch (tampered?)", nullptr);
      return false;
    }
  }

  // Extract fields
  memcpy(seq_out, buf + 8, 4);
  memcpy(chain_head_out, buf + 12, 32);

  char detail[32];
  snprintf(detail, sizeof(detail), "seq=%u", (unsigned)*seq_out);
  log_health(SCV_LOG_INFO, SCV_CAT_STORAGE,
             "Chain restored from backup", detail);
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// HAS BACKUP
// ════════════════════════════════════════════════════════════════════════════

inline bool has_backup() {
  if (!sd_is_available()) return false;
  return SD.exists("/CHAIN/backup.bin");
}

// ════════════════════════════════════════════════════════════════════════════
// INIT
// ════════════════════════════════════════════════════════════════════════════

inline void init() {
  if (s_initialized) return;

  if (sd_is_available()) {
    s_witness_files = count_files("/WITNESS");
    s_health_files  = count_files("/HEALTH");
    s_backup_exists = SD.exists("/CHAIN/backup.bin");
  }

  s_files_rotated    = 0;
  s_last_rotation_ms = 0;
  s_last_backup_ms   = 0;
  s_last_process_ms  = millis();
  s_initialized      = true;

  log_health(SCV_LOG_INFO, SCV_CAT_STORAGE,
             "Data management initialized", nullptr);
}

// ════════════════════════════════════════════════════════════════════════════
// PROCESS — call from loop(), rate-limited internally
//
// chain_head and seq must be supplied by the caller (the .ino owns them).
// ════════════════════════════════════════════════════════════════════════════

inline bool process(const uint8_t* chain_head, uint32_t seq,
                    const uint8_t* privkey) {
  if (!s_initialized || !privkey) return false;

  uint32_t now = millis();
  if ((int32_t)(now - s_last_process_ms) < (int32_t)PROCESS_INTERVAL_MS) {
    return false;
  }
  s_last_process_ms = now;

  if (!sd_is_available()) return false;

  // Refresh file counts
  s_witness_files = count_files("/WITNESS");
  s_health_files  = count_files("/HEALTH");

  // Rotation bounds regenerable artifacts only. /WITNESS is the sealed
  // log of record and is NEVER rotated (Invariant IV) — it is a single
  // append-only jsonl whose growth is bounded by the record rate, not by
  // this sweep.
  bool needs_rotation = (s_health_files > MAX_HEALTH_FILES) ||
                        (sd_usage_pct() > SD_USAGE_TARGET_PCT);

  if (needs_rotation) {
    uint32_t deleted = rotate_impl("/HEALTH", MAX_HEALTH_FILES);

    if (deleted > 0) {
      s_files_rotated   += deleted;
      s_last_rotation_ms = millis();
      s_health_files     = count_files("/HEALTH");

      char detail[32];
      snprintf(detail, sizeof(detail), "%u deleted", (unsigned)deleted);
      log_health(SCV_LOG_INFO, SCV_CAT_STORAGE,
                 "Log rotation completed", detail);
    }
  }

  // Periodic chain backup (every hour)
  if ((int32_t)(now - s_last_backup_ms) >= (int32_t)BACKUP_INTERVAL_MS ||
      s_last_backup_ms == 0) {
    backup_chain(chain_head, seq, privkey);
  }

  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// STATS
// ════════════════════════════════════════════════════════════════════════════

inline bool get_stats(datamgmt_stats_t* out) {
  if (!out) return false;

  out->witness_files       = s_witness_files;
  out->health_files        = s_health_files;
  out->files_rotated_total = s_files_rotated;
  out->last_rotation_ms    = s_last_rotation_ms;
  out->last_backup_ms      = s_last_backup_ms;
  out->backup_exists       = s_backup_exists;

  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// SERIAL STATUS PRINT
// ════════════════════════════════════════════════════════════════════════════

inline void print_status() {
  datamgmt_stats_t stats;
  get_stats(&stats);

  Serial.println();
  Serial.println("┌─────────────────────────────────────┐");
  Serial.println("│       DATA MANAGEMENT STATUS        │");
  Serial.println("├─────────────────────────────────────┤");
  Serial.printf("│ Witness files   : %5u (kept)      │\n",
                (unsigned)stats.witness_files);
  Serial.printf("│ Health files    : %5u / %5u      │\n",
                (unsigned)stats.health_files,
                (unsigned)MAX_HEALTH_FILES);
  Serial.printf("│ Files rotated   : %5u             │\n",
                (unsigned)stats.files_rotated_total);
  Serial.printf("│ Chain backup    : %s              │\n",
                stats.backup_exists ? "YES" : " NO");

  if (stats.last_rotation_ms > 0) {
    uint32_t ago = (millis() - stats.last_rotation_ms) / 1000;
    Serial.printf("│ Last rotation   : %5us ago        │\n", (unsigned)ago);
  } else {
    Serial.println("│ Last rotation   : never             │");
  }

  if (stats.last_backup_ms > 0) {
    uint32_t ago = (millis() - stats.last_backup_ms) / 1000;
    Serial.printf("│ Last backup     : %5us ago        │\n", (unsigned)ago);
  } else {
    Serial.println("│ Last backup     : never             │");
  }

  if (sd_is_available()) {
    Serial.printf("│ SD usage        : %3u%%              │\n",
                  (unsigned)sd_usage_pct());
  }

  Serial.println("└─────────────────────────────────────┘");
  Serial.println();
}

// ════════════════════════════════════════════════════════════════════════════
// JSON OUTPUT
// ════════════════════════════════════════════════════════════════════════════

inline size_t get_json(char* buf, size_t buf_size) {
  datamgmt_stats_t stats;
  get_stats(&stats);

  int len = snprintf(buf, buf_size,
    "{"
    "\"ok\":true,"
    "\"witness_files\":%u,"
    "\"health_files\":%u,"
    "\"max_health_files\":%u,"
    "\"files_rotated_total\":%u,"
    "\"last_rotation_ms\":%u,"
    "\"last_backup_ms\":%u,"
    "\"backup_exists\":%s,"
    "\"sd_usage_pct\":%u"
    "}",
    (unsigned)stats.witness_files,
    (unsigned)stats.health_files,
    (unsigned)MAX_HEALTH_FILES,
    (unsigned)stats.files_rotated_total,
    (unsigned)stats.last_rotation_ms,
    (unsigned)stats.last_backup_ms,
    stats.backup_exists ? "true" : "false",
    (unsigned)(sd_is_available() ? sd_usage_pct() : 0));

  return (len > 0 && len < (int)buf_size) ? (size_t)len : 0;
}

}  // namespace datamgmt

#endif  // FEATURE_DATA_MGMT && FEATURE_SD_STORAGE

#endif  // SECURACV_DATA_MGMT_API_H
