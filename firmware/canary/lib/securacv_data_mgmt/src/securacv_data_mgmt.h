/*
 * SecuraCV Canary — Data Management
 *
 * SD log rotation, chain backup/restore, integrity verification,
 * and witness record export.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_DATA_MGMT_H
#define SECURACV_DATA_MGMT_H

#include <stdint.h>
#include <stdbool.h>

/* ────────────────────────────────────────────────────────────────────────── */
/*  ROTATION POLICY                                                          */
/* ────────────────────────────────────────────────────────────────────────── */

#define DATAMGMT_MAX_WITNESS_FILES   500
#define DATAMGMT_MAX_HEALTH_FILES    200
#define DATAMGMT_SD_USAGE_TARGET_PCT 85   // rotate when SD usage exceeds this

/* ────────────────────────────────────────────────────────────────────────── */
/*  CHAIN INTEGRITY VERIFICATION RESULT                                      */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint32_t records_checked;
  uint32_t records_valid;
  uint32_t chain_breaks;
  uint32_t signature_failures;
  bool     chain_intact;
  bool     partial;          /* true if capped before all records were checked */
} chain_verify_result_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  STATS                                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint32_t witness_files;
  uint32_t health_files;
  uint32_t files_rotated_total;
  uint32_t last_rotation_ms;
  uint32_t last_backup_ms;
  bool     backup_exists;
} datamgmt_stats_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  C API                                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/* SD Log Rotation */
bool datamgmt_init(void);
bool datamgmt_process(void);  /* call from loop, rate-limited internally */

/* Rotate old files in a directory, keeping at most max_files.
 * Deletes oldest files first (lexicographic sort = chronological for
 * timestamped names). Returns number of files deleted. */
uint32_t datamgmt_rotate_dir(const char* dir_path, uint32_t max_files);

/* Chain Backup & Restore (NVS <-> SD) */
bool datamgmt_backup_chain(void);   /* NVS chain state -> /CHAIN/backup.bin on SD */
bool datamgmt_restore_chain(void);  /* /CHAIN/backup.bin -> NVS (for recovery)    */
bool datamgmt_has_backup(void);     /* check if backup exists on SD               */

/* Chain Integrity Verification
 * Walk witness record files and verify hash chain continuity.
 * Returns true on success (result filled in), false on I/O error. */
bool datamgmt_verify_chain(chain_verify_result_t* result);

/* Export
 * Copy a range of witness records to /EXPORT/ for USB retrieval.
 * Returns number of files copied. */
uint32_t datamgmt_export_records(uint32_t from_seq, uint32_t to_seq);

/* Stats */
bool datamgmt_get_stats(datamgmt_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_DATA_MGMT_H */
