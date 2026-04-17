/*
 * SecuraCV Canary — SD Storage Manager Implementation
 *
 * Stub implementation for Arduino sketch linking.
 * SD operations are currently handled inline by the main .ino
 * via hardware_state.h. These stubs satisfy the linker for
 * sd_storage.h namespace declarations.
 */

#include "sd_storage.h"

namespace sd_storage {

// ════════════════════════════════════════════════════════════════════════════
// INITIALIZATION
// ════════════════════════════════════════════════════════════════════════════

bool init(SPIClass* spi) {
  // SD init is handled by hardware_state.h sd_mount_safe()
  return false;
}

void deinit() {
}

bool is_mounted() {
  return false;
}

SDStatus get_status() {
  SDStatus s = {};
  return s;
}

// ════════════════════════════════════════════════════════════════════════════
// DIRECTORY MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

bool ensure_directories() {
  return false;
}

bool format_date_path(char* out, size_t cap, const char* base_dir, const char* ext) {
  if (out && cap > 0) out[0] = '\0';
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// WITNESS RECORD STORAGE
// ════════════════════════════════════════════════════════════════════════════

bool append_witness_record(const uint8_t* cbor_payload, size_t payload_len,
                           const uint8_t* chain_hash, const uint8_t* signature,
                           uint32_t seq, uint32_t time_bucket, uint8_t record_type) {
  return false;
}

bool read_witness_records(const char* date,
                          void (*callback)(const WitnessLogEntry&, const uint8_t* payload, void* ctx),
                          void* ctx, uint32_t start_seq, uint32_t limit) {
  return false;
}

uint32_t count_witness_records(const char* date) {
  return 0;
}

// ════════════════════════════════════════════════════════════════════════════
// HEALTH LOG STORAGE
// ════════════════════════════════════════════════════════════════════════════

bool append_health_log(LogLevel level, LogCategory category,
                       const char* message, const char* detail) {
  return false;
}

bool read_health_logs(const char* date,
                      void (*callback)(const HealthLogEntry&, void* ctx),
                      void* ctx, uint32_t start_seq, uint32_t limit) {
  return false;
}

bool acknowledge_log(uint32_t log_seq, AckStatus new_status, const char* reason) {
  return false;
}

uint32_t count_health_logs(const char* date, LogLevel min_level) {
  return 0;
}

uint32_t count_unacknowledged(LogLevel min_level) {
  return 0;
}

// ════════════════════════════════════════════════════════════════════════════
// CHAIN STATE PERSISTENCE
// ════════════════════════════════════════════════════════════════════════════

bool save_chain_state(const uint8_t* chain_head, uint32_t seq, uint32_t boot_count) {
  return false;
}

bool load_chain_state(uint8_t* chain_head_out, uint32_t* seq_out, uint32_t* boot_count_out) {
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// EXPORT FUNCTIONALITY
// ════════════════════════════════════════════════════════════════════════════

bool create_export_bundle(const char* output_path, const char* start_date, const char* end_date) {
  return false;
}

bool list_available_dates(void (*callback)(const char* date, uint32_t witness_count,
                                           uint32_t health_count, void* ctx), void* ctx) {
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// MAINTENANCE
// ════════════════════════════════════════════════════════════════════════════

bool rotate_old_logs(uint32_t max_age_days) {
  return false;
}

uint64_t get_storage_used() {
  return 0;
}

uint64_t get_storage_free() {
  return 0;
}

// ════════════════════════════════════════════════════════════════════════════
// PATH UTILITIES
// ════════════════════════════════════════════════════════════════════════════

bool is_safe_path(const char* path) {
  if (!path || path[0] == '\0') return false;
  // Reject path traversal
  if (strstr(path, "..") != nullptr) return false;
  return true;
}

bool file_exists(const char* path) {
  return false;
}

size_t file_size(const char* path) {
  return 0;
}

} // namespace sd_storage
