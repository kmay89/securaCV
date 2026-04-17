/*
 * SecuraCV Canary — SD Storage Manager Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_storage.h"
#include "securacv_witness.h"

#if FEATURE_SD_STORAGE

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE
// ════════════════════════════════════════════════════════════════════════════

static StorageManager s_storage;
static SPIClass s_sd_spi(FSPI);

StorageManager& storage_get_instance() {
  return s_storage;
}

// ════════════════════════════════════════════════════════════════════════════
// STORAGE MANAGER IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

StorageManager::StorageManager()
  : m_spi(nullptr), m_mounted(false), m_write_errors(0),
    m_read_errors(0), m_last_write_ms(0) {}

bool StorageManager::begin(SPIClass* spi) {
  if (spi) {
    m_spi = spi;
  } else {
    m_spi = &s_sd_spi;
    m_spi->begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  }

  if (!SD.begin(SD_CS_PIN, *m_spi, SD_SPI_FAST)) {
    // Try slower speed
    if (!SD.begin(SD_CS_PIN, *m_spi, SD_SPI_SLOW)) {
      m_mounted = false;
      return false;
    }
  }

  m_mounted = true;

  // Create directories
  ensureDirectories();

  return true;
}

void StorageManager::end() {
  SD.end();
  m_mounted = false;
}

bool StorageManager::ensureDirectories() {
  if (!m_mounted) return false;

  if (!SD.exists("/WITNESS")) SD.mkdir("/WITNESS");
  if (!SD.exists("/HEALTH")) SD.mkdir("/HEALTH");
  if (!SD.exists("/CHAIN")) SD.mkdir("/CHAIN");
  if (!SD.exists("/EXPORT")) SD.mkdir("/EXPORT");

  return true;
}

SDStatus StorageManager::getStatus() {
  SDStatus status;
  memset(&status, 0, sizeof(status));

  status.mounted = m_mounted;
  status.healthy = m_mounted;
  status.write_errors = m_write_errors;
  status.read_errors = m_read_errors;
  status.last_write_ms = m_last_write_ms;

  if (m_mounted) {
    status.total_bytes = SD.totalBytes();
    status.used_bytes = SD.usedBytes();
    status.free_bytes = status.total_bytes - status.used_bytes;
    status.witness_count = countFilesInDir("/WITNESS");
    status.health_count  = countFilesInDir("/HEALTH");
  }

  // Unacked count is owned by the witness/health-log subsystem; mirror it
  // here so the storage status API is a single source of truth for dashboards.
  status.unacked_count = witness_get_health().logs_unacked;

  return status;
}

uint32_t StorageManager::countFilesInDir(const char* dir_path) {
  if (!m_mounted || !dir_path) return 0;

  File dir = SD.open(dir_path);
  if (!dir) return 0;
  if (!dir.isDirectory()) {
    dir.close();
    return 0;
  }

  uint32_t count = 0;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (!entry.isDirectory()) {
      count++;
    }
    entry.close();
  }
  dir.close();
  return count;
}

bool StorageManager::fileExists(const char* path) {
  if (!m_mounted) return false;
  return SD.exists(path);
}

size_t StorageManager::fileSize(const char* path) {
  if (!m_mounted) return 0;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  size_t sz = f.size();
  f.close();
  return sz;
}

// ════════════════════════════════════════════════════════════════════════════
// CONVENIENCE FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

bool storage_init(SPIClass* spi) {
  return storage_get_instance().begin(spi);
}

bool storage_is_mounted() {
  return storage_get_instance().isMounted();
}

#endif // FEATURE_SD_STORAGE
