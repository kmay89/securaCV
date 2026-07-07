/*
 * SecuraCV Canary — Data Management implementation
 *
 * SD log rotation (health diagnostics only — the witness log is
 * append-only and never rotated) and chain backup/restore.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_data_mgmt.h"

#include <Arduino.h>
#include <string.h>

#include "canary_config.h"
#include "log_level.h"
#include "securacv_witness.h"
#include "securacv_crypto.h"

#if FEATURE_SD_STORAGE
#include "securacv_storage.h"
#include <SD.h>
#endif

#if FEATURE_DIAGNOSTICS
#include "securacv_diagnostics.h"
#endif

namespace datamgmt {

/* ──────────────────────────────────────────────────────────────────────────
 * INTERNAL STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool     s_initialized       = false;
static uint32_t s_last_process_ms   = 0;
static uint32_t s_last_backup_ms    = 0;

static uint32_t s_witness_files     = 0;
static uint32_t s_health_files      = 0;
static uint32_t s_files_rotated     = 0;
static uint32_t s_last_rotation_ms  = 0;
static bool     s_backup_exists     = false;

/* Rate limits (milliseconds) */
static constexpr uint32_t PROCESS_INTERVAL_MS = 5UL * 60UL * 1000UL;   /* 5 min  */
static constexpr uint32_t BACKUP_INTERVAL_MS  = 60UL * 60UL * 1000UL;  /* 1 hour */

/* Rotation helper: small fixed buffer to avoid heap pressure when sorting
 * filenames. We process BATCH_SIZE filenames at a time. */
static constexpr size_t BATCH_SIZE     = 32;
static constexpr size_t MAX_NAME_LEN   = 48;

/* ──────────────────────────────────────────────────────────────────────────
 * CHAIN BACKUP BINARY FORMAT
 *
 * Offset  Size  Description
 * 0       4     magic (0x53434256 "SCVB")
 * 4       4     version (1)
 * 8       4     seq
 * 12      32    chain_head hash
 * 44      32    HMAC-SHA256 of bytes 0..43 (keyed by device privkey)
 * ────────────────────────────────────────────────────────────────────────── */

static constexpr uint32_t BACKUP_MAGIC   = 0x53434256;
static constexpr uint32_t BACKUP_VERSION = 2;
static constexpr size_t   BACKUP_PAYLOAD = 44;
static constexpr size_t   BACKUP_SIZE    = 76;  /* 44 payload + 32 HMAC */

/* ──────────────────────────────────────────────────────────────────────────
 * SD USAGE CHECK
 * ────────────────────────────────────────────────────────────────────────── */

#if FEATURE_SD_STORAGE
static uint8_t sd_usage_pct() {
  uint64_t total = SD.totalBytes();
  if (total == 0) return 0;
  uint64_t used = SD.usedBytes();
  return (uint8_t)(used * 100 / total);
}
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * FILE COUNT HELPER
 * ────────────────────────────────────────────────────────────────────────── */

#if FEATURE_SD_STORAGE
static uint32_t count_files(const char* dir_path) {
  return storage_get_instance().countFilesInDir(dir_path);
}
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * ROTATION — batch-sort approach
 *
 * Opens the directory and collects up to BATCH_SIZE filenames into a
 * stack-local buffer, sorts them lexicographically, then deletes from
 * the front (oldest) until the file count drops to max_files. Repeats
 * if more than BATCH_SIZE need deleting.
 * ────────────────────────────────────────────────────────────────────────── */

#if FEATURE_SD_STORAGE
/* Simple insertion sort on a small fixed array of short strings. */
static void sort_names(char names[][MAX_NAME_LEN], size_t n) {
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

static uint32_t rotate_impl(const char* dir_path, uint32_t max_files) {
  if (!storage_is_mounted()) return 0;

  uint32_t total_deleted = 0;

  /* Outer loop: each pass collects one batch of the oldest filenames
   * and deletes from the front until the directory is within limits. */
  for (;;) {
    uint32_t file_count = count_files(dir_path);
    if (file_count <= max_files) break;

    uint32_t to_delete = file_count - max_files;

    /* Collect up to BATCH_SIZE names. */
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
        /* entry.name() on ESP32 returns the full path; extract basename. */
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

    /* Delete the oldest (first in sorted order) up to `to_delete`. */
    size_t deleting = (to_delete < collected) ? (size_t)to_delete : collected;
    for (size_t i = 0; i < deleting; i++) {
      char full_path[MAX_NAME_LEN + 16];
      snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, names[i]);
      bool ok = SD.remove(full_path);
#if FEATURE_DIAGNOSTICS
      diag_record_sd_write(ok);
#endif
      if (ok) {
        total_deleted++;
      } else {
        log_health(LOG_LEVEL_WARNING, LOG_CAT_STORAGE,
                   "Rotation: failed to delete file", names[i]);
      }
    }
  }

  return total_deleted;
}
#endif  /* FEATURE_SD_STORAGE */

}  /* namespace datamgmt */


/* ════════════════════════════════════════════════════════════════════════════
 * C API
 * ════════════════════════════════════════════════════════════════════════════ */

extern "C" {

bool datamgmt_init(void) {
  using namespace datamgmt;
  if (s_initialized) return true;

#if FEATURE_SD_STORAGE
  if (!storage_is_mounted()) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_STORAGE,
               "Data mgmt init: SD not mounted", nullptr);
    /* Non-fatal — we'll retry when process() runs. */
  }

  /* Count current files. */
  s_witness_files = count_files("/WITNESS");
  s_health_files  = count_files("/HEALTH");

  /* Check for existing backup. */
  s_backup_exists = SD.exists("/CHAIN/backup.bin");
#endif

  s_files_rotated    = 0;
  s_last_rotation_ms = 0;
  s_last_backup_ms   = 0;
  s_last_process_ms  = millis();
  s_initialized      = true;

  log_health(LOG_LEVEL_INFO, LOG_CAT_STORAGE,
             "Data management initialized", nullptr);
  return true;
}

bool datamgmt_process(void) {
  using namespace datamgmt;
  if (!s_initialized) return false;

  uint32_t now = millis();
  if ((int32_t)(now - s_last_process_ms) < (int32_t)PROCESS_INTERVAL_MS) {
    return false;
  }
  s_last_process_ms = now;

#if FEATURE_SD_STORAGE
  if (!storage_is_mounted()) return false;

  /* Refresh file counts. */
  s_witness_files = count_files("/WITNESS");
  s_health_files  = count_files("/HEALTH");

  /* Check if rotation is needed: file count exceeds limits OR SD usage
   * exceeds the target percentage.
   *
   * /WITNESS is deliberately NOT rotated (Invariant IV: the sealed log is
   * a locally owned, tamper-evident record — rotation must never touch
   * it). Its durable tier is the single append-only
   * /WITNESS/records.jsonl written by securacv_witness; deleting from it
   * would silently truncate the evidence the whole chain exists to
   * protect. Only /HEALTH (diagnostics) is bounded here. */
  bool needs_rotation = (s_health_files  > DATAMGMT_MAX_HEALTH_FILES)  ||
                        (sd_usage_pct()  > DATAMGMT_SD_USAGE_TARGET_PCT);

  if (needs_rotation) {
    uint32_t deleted = rotate_impl("/HEALTH", DATAMGMT_MAX_HEALTH_FILES);

    if (deleted > 0) {
      s_files_rotated   += deleted;
      s_last_rotation_ms = millis();
      s_health_files     = count_files("/HEALTH");

      char detail[32];
      snprintf(detail, sizeof(detail), "%u deleted", (unsigned)deleted);
      log_health(LOG_LEVEL_INFO, LOG_CAT_STORAGE,
                 "Log rotation completed", detail);
    }
  }

  /* Periodic chain backup (every hour). */
  if ((int32_t)(now - s_last_backup_ms) >= (int32_t)BACKUP_INTERVAL_MS ||
      s_last_backup_ms == 0) {
    datamgmt_backup_chain();
  }
#endif

  return true;
}

uint32_t datamgmt_rotate_dir(const char* dir_path, uint32_t max_files) {
#if FEATURE_SD_STORAGE
  using namespace datamgmt;
  uint32_t deleted = datamgmt::rotate_impl(dir_path, max_files);
  if (deleted > 0) s_files_rotated += deleted;
  return deleted;
#else
  (void)dir_path; (void)max_files;
  return 0;
#endif
}

bool datamgmt_backup_chain(void) {
  using namespace datamgmt;

#if FEATURE_SD_STORAGE
  if (!storage_is_mounted()) return false;

  DeviceIdentity& dev = witness_get_device();

  uint8_t buf[BACKUP_SIZE];
  memset(buf, 0, sizeof(buf));

  /* Magic + version */
  memcpy(buf + 0, &BACKUP_MAGIC,   4);
  memcpy(buf + 4, &BACKUP_VERSION, 4);

  /* Sequence number */
  memcpy(buf + 8, &dev.seq, 4);

  /* Chain head hash (32 bytes) */
  memcpy(buf + 12, dev.chain_head, 32);

  /* HMAC-SHA256 of payload bytes 0..43, keyed by device private key.
   * Prevents an attacker with SD access from forging a backup to
   * roll back the chain to a previous state. */
  uint8_t hmac[32];
  if (!hmac_sha256(dev.privkey, 32, buf, BACKUP_PAYLOAD, hmac)) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_STORAGE,
               "Chain backup: HMAC failed", nullptr);
    return false;
  }
  memcpy(buf + BACKUP_PAYLOAD, hmac, 32);

  /* Ensure /CHAIN directory exists. */
  if (!SD.exists("/CHAIN")) SD.mkdir("/CHAIN");

  File f = SD.open("/CHAIN/backup.bin", FILE_WRITE);
  bool ok = false;
  if (f) {
    ok = (f.write(buf, BACKUP_SIZE) == BACKUP_SIZE);
    f.close();
  }

#if FEATURE_DIAGNOSTICS
  diag_record_sd_write_bytes(BACKUP_SIZE, ok);
#endif

  if (ok) {
    s_backup_exists  = true;
    s_last_backup_ms = millis();
    log_health(LOG_LEVEL_INFO, LOG_CAT_STORAGE,
               "Chain backup written", "/CHAIN/backup.bin");
  } else {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_STORAGE,
               "Chain backup write failed", nullptr);
  }

  return ok;
#else
  return false;
#endif
}

bool datamgmt_restore_chain(void) {
#if FEATURE_SD_STORAGE
  using namespace datamgmt;

  if (!storage_is_mounted()) return false;

  File f = SD.open("/CHAIN/backup.bin", FILE_READ);
  if (!f) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_STORAGE,
               "Chain restore: backup not found", nullptr);
    return false;
  }

  uint8_t buf[BACKUP_SIZE];
  size_t read = f.read(buf, BACKUP_SIZE);
  f.close();

  if (read != BACKUP_SIZE) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_STORAGE,
               "Chain restore: truncated backup", nullptr);
    return false;
  }

  /* Validate magic */
  uint32_t magic;
  memcpy(&magic, buf + 0, 4);
  if (magic != BACKUP_MAGIC) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_STORAGE,
               "Chain restore: bad magic", nullptr);
    return false;
  }

  /* Validate HMAC-SHA256 using device private key. */
  DeviceIdentity& dev = witness_get_device();
  uint8_t calc_hmac[32];
  if (!hmac_sha256(dev.privkey, 32, buf, BACKUP_PAYLOAD, calc_hmac)) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_STORAGE,
               "Chain restore: HMAC compute failed", nullptr);
    return false;
  }
  if (memcmp(buf + BACKUP_PAYLOAD, calc_hmac, 32) != 0) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_STORAGE,
               "Chain restore: HMAC mismatch (tampered?)", nullptr);
    return false;
  }

  /* Extract fields */
  uint32_t seq;
  memcpy(&seq, buf + 8, 4);

  dev.seq = seq;
  memcpy(dev.chain_head, buf + 12, 32);

  /* Persist to NVS so the restored state survives a reboot. */
  witness_persist_chain_state();

  char detail[32];
  snprintf(detail, sizeof(detail), "seq=%u", (unsigned)seq);
  log_health(LOG_LEVEL_INFO, LOG_CAT_STORAGE,
             "Chain restored from backup", detail);
  return true;
#else
  return false;
#endif
}

bool datamgmt_has_backup(void) {
#if FEATURE_SD_STORAGE
  if (!storage_is_mounted()) return false;
  return SD.exists("/CHAIN/backup.bin");
#else
  return false;
#endif
}


bool datamgmt_get_stats(datamgmt_stats_t* out) {
  using namespace datamgmt;
  if (!out) return false;

  out->witness_files       = s_witness_files;
  out->health_files        = s_health_files;
  out->files_rotated_total = s_files_rotated;
  out->last_rotation_ms    = s_last_rotation_ms;
  out->last_backup_ms      = s_last_backup_ms;
  out->backup_exists       = s_backup_exists;

  return true;
}

}  /* extern "C" */
