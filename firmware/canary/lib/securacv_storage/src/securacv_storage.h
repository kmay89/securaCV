/*
 * SecuraCV Canary — SD Storage Manager
 *
 * Manages append-only storage for witness records and health logs.
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
  uint32_t witness_count;   // TODO: Implement file counting in getStatus()
  uint32_t health_count;    // TODO: Implement file counting in getStatus()
  uint32_t unacked_count;   // TODO: Implement acknowledgment tracking
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

  // Initialize SD card
  bool begin(SPIClass* spi = nullptr);
  void end();

  // Status
  bool isMounted() const { return m_mounted; }
  SDStatus getStatus();

  // Directory management
  bool ensureDirectories();

  // File operations
  bool fileExists(const char* path);
  size_t fileSize(const char* path);

private:
  SPIClass* m_spi;
  bool m_mounted;
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

#endif // FEATURE_SD_STORAGE

#endif // SECURACV_STORAGE_H
