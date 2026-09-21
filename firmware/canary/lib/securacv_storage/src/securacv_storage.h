/*
 * SecuraCV Canary — SD Storage Manager
 *
 * Manages append-only storage for witness records and health logs, with
 * bounded mount recovery: the blocking SD.begin() runs on a dedicated
 * idle-priority worker task (never on the watchdog-subscribed loop task),
 * and a periodic loop-side check re-probes a lost or late-inserted card.
 * Every branchy decision around that machinery is the pure, host-tested
 * table in common/storage/sd_mount_policy.h.
 *
 * Threading contract: everything in this class except the worker's own
 * SD.begin() runs on the Arduino loopTask (the single writer of all SD
 * state — witness appends included). USB MSC raw-sector reads come from
 * the TinyUSB task, which is why teardown is policy-gated on MSC holding
 * the card.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_STORAGE_H
#define SECURACV_STORAGE_H

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "canary_config.h"
#include "log_level.h"

#if FEATURE_SD_STORAGE

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

struct SDStatus {
  bool mounted;
  bool healthy;
  uint64_t total_bytes;
  uint64_t used_bytes;
  uint64_t free_bytes;
  uint32_t witness_count;   // Files in /WITNESS (enumerated on getStatus)
  uint32_t health_count;    // Files in /HEALTH (enumerated on getStatus)
  uint32_t unacked_count;   // Unread health log entries (mirrors SystemHealth.logs_unacked)
  uint32_t last_write_ms;
  uint32_t write_errors;
  uint32_t read_errors;
};

// ════════════════════════════════════════════════════════════════════════════
// STORAGE MANAGER
// ════════════════════════════════════════════════════════════════════════════

class StorageManager {
public:
  StorageManager();

  // Initialize SD card (boot path). Starts the mount worker, requests a
  // mount, and waits a bounded budget for it — feeding the task watchdog in
  // slices — so a wedged or absent card can never crash-loop the boot. On
  // budget expiry it returns false and keeps booting; the eventual worker
  // result is adopted by a later periodicCheck() pass.
  bool begin(SPIClass* spi = nullptr);
  void end();

  // Status
  bool isMounted() const { return m_mounted; }
  bool mountInFlight() const;
  uint32_t mountGeneration() const { return m_mount_generation; }
  SDStatus getStatus();

  // Loop-task periodic mount health check (adopt-first, then the
  // sd_mount_policy decision: verify a mounted card, request a background
  // remount for an absent one). `msc_holds_card` gates every teardown and
  // remount: USB MSC reads raw sectors from the TinyUSB task, so driver
  // state must never be freed or rebuilt under it.
  void periodicCheck(bool msc_holds_card);

  // Write-outcome notes from the SD producers (witness appends — loop task
  // only). A run of consecutive failures past the policy threshold marks
  // the card lost so periodicCheck() can tear down and remount it.
  void noteWriteFailure();
  void noteWriteSuccess();

  // Directory management
  bool ensureDirectories();

  // File operations
  bool fileExists(const char* path);
  size_t fileSize(const char* path);

  // Count regular files directly under `dir_path` (non-recursive).
  // Returns 0 if the directory does not exist or the card is not mounted.
  uint32_t countFilesInDir(const char* dir_path);

private:
  bool requestMount();
  bool tryAdoptMount();

  SPIClass* m_spi;
  bool m_mounted;
  bool m_needs_teardown;        // card marked lost; SD.end() still owed
  uint32_t m_mount_generation;  // successful mounts this boot
  uint32_t m_consecutive_errors;
  uint32_t m_last_check_ms;
  uint32_t m_write_errors;
  uint32_t m_read_errors;
  uint32_t m_last_write_ms;
};

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE
// ════════════════════════════════════════════════════════════════════════════

StorageManager& storage_get_instance();

// Convenience functions
bool storage_init(SPIClass* spi = nullptr);
bool storage_is_mounted();
void storage_periodic_check(bool msc_holds_card);
void storage_note_write_failure();
void storage_note_write_success();

// Is a background mount attempt running right now? While true, the worker
// owns the global SD object (it may be inside a blocking SD.begin()), so no
// other code may touch SD.* — not even cheap probes like SD.cardType().
// Every direct SD consumer outside this manager gates on it.
bool storage_mount_in_flight();

// Monotonic count of successful mounts this boot (0 = never mounted).
// Consumers that must re-run a per-card check after any (re)mount — the
// witness fork guard — compare against their cached value.
uint32_t storage_mount_generation();

#endif // FEATURE_SD_STORAGE

#endif // SECURACV_STORAGE_H
