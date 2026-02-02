/*
 * SecuraCV Canary — SD Storage Manager
 * 
 * Manages append-only storage for:
 * - Witness records (cryptographically signed, immutable)
 * - Health/diagnostic logs (append-only with acknowledgment)
 * - Chain state persistence
 * 
 * Storage Layout:
 * /sd/
 * ├── WITNESS/           # Witness records (immutable)
 * │   ├── 2026-01-31.wit # Daily witness log (CBOR + signature)
 * │   └── INDEX.idx      # Quick lookup index
 * ├── HEALTH/            # Health/diagnostic logs
 * │   ├── 2026-01-31.log # Daily health log (JSON lines)
 * │   └── ACK.json       # Acknowledgment status
 * ├── CHAIN/             # Chain state backup
 * │   └── state.bin      # Chain head + sequence (redundant to NVS)
 * └── EXPORT/            # Export staging area
 *     └── bundle.json    # PWK-compatible export bundle
 */

#ifndef SECURACV_SD_STORAGE_H
#define SECURACV_SD_STORAGE_H

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "log_level.h"

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

namespace sd_storage {

// Pin configuration for XIAO ESP32S3 Sense
static const int SD_CS_PIN   = 21;
static const int SD_SCK_PIN  = 7;
static const int SD_MISO_PIN = 8;
static const int SD_MOSI_PIN = 9;

// SPI speeds (fallback on error)
static const uint32_t SD_SPI_FAST = 4000000;   // 4 MHz
static const uint32_t SD_SPI_SLOW = 1000000;   // 1 MHz fallback

// Directory structure
static const char* MOUNT_POINT   = "/sd";
static const char* WITNESS_DIR   = "/sd/WITNESS";
static const char* HEALTH_DIR    = "/sd/HEALTH";
static const char* CHAIN_DIR     = "/sd/CHAIN";
static const char* EXPORT_DIR    = "/sd/EXPORT";

// File limits
static const size_t MAX_LOG_FILE_SIZE = 1024 * 1024;  // 1 MB per file
static const size_t MAX_HEALTH_ENTRIES = 10000;       // Max entries per day
static const size_t MAX_WITNESS_ENTRIES = 86400;      // Max records per day (1/sec)

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

struct SDStatus {
  bool mounted;
  bool healthy;
  uint64_t total_bytes;
  uint64_t used_bytes;
  uint64_t free_bytes;
  uint32_t witness_count;
  uint32_t health_count;
  uint32_t unacked_count;
  uint32_t last_write_ms;
  uint32_t write_errors;
  uint32_t read_errors;
};

struct HealthLogEntry {
  uint32_t seq;
  uint32_t timestamp_ms;
  LogLevel level;
  LogCategory category;
  AckStatus ack_status;
  char message[128];
  char detail[64];
};

struct WitnessLogEntry {
  uint32_t seq;
  uint32_t time_bucket;
  uint8_t record_type;
  uint8_t chain_hash[32];
  uint8_t signature[64];
  size_t payload_len;
};

struct ChainState {
  uint8_t chain_head[32];
  uint32_t seq;
  uint32_t boot_count;
  uint32_t checksum;
};

struct AckRecord {
  uint32_t log_seq;
  uint32_t ack_timestamp_ms;
  AckStatus new_status;
  char ack_reason[64];
};

// ════════════════════════════════════════════════════════════════════════════
// FUNCTION DECLARATIONS
// ════════════════════════════════════════════════════════════════════════════

// Initialization
bool init(SPIClass* spi = nullptr);
void deinit();
bool is_mounted();
SDStatus get_status();

// Directory management
bool ensure_directories();
bool format_date_path(char* out, size_t cap, const char* base_dir, const char* ext);

// Witness record storage (immutable, append-only)
bool append_witness_record(const uint8_t* cbor_payload, size_t payload_len,
                           const uint8_t* chain_hash, const uint8_t* signature,
                           uint32_t seq, uint32_t time_bucket, uint8_t record_type);
bool read_witness_records(const char* date, 
                          void (*callback)(const WitnessLogEntry&, const uint8_t* payload, void* ctx),
                          void* ctx, uint32_t start_seq = 0, uint32_t limit = 100);
uint32_t count_witness_records(const char* date = nullptr);

// Health log storage (append-only with acknowledgment)
bool append_health_log(LogLevel level, LogCategory category, 
                       const char* message, const char* detail = nullptr);
bool read_health_logs(const char* date,
                      void (*callback)(const HealthLogEntry&, void* ctx),
                      void* ctx, uint32_t start_seq = 0, uint32_t limit = 100);
bool acknowledge_log(uint32_t log_seq, AckStatus new_status, const char* reason);
uint32_t count_health_logs(const char* date = nullptr, LogLevel min_level = LOG_LEVEL_DEBUG);
uint32_t count_unacknowledged(LogLevel min_level = LOG_LEVEL_WARNING);

// Chain state persistence (redundant backup to NVS)
bool save_chain_state(const uint8_t* chain_head, uint32_t seq, uint32_t boot_count);
bool load_chain_state(uint8_t* chain_head_out, uint32_t* seq_out, uint32_t* boot_count_out);

// Export functionality
bool create_export_bundle(const char* output_path, const char* start_date, const char* end_date);
bool list_available_dates(void (*callback)(const char* date, uint32_t witness_count, 
                                           uint32_t health_count, void* ctx), void* ctx);

// Maintenance
bool rotate_old_logs(uint32_t max_age_days);
uint64_t get_storage_used();
uint64_t get_storage_free();

// Path utilities
bool is_safe_path(const char* path);
bool file_exists(const char* path);
size_t file_size(const char* path);

} // namespace sd_storage

#endif // SECURACV_SD_STORAGE_H
