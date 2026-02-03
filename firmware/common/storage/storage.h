/**
 * @file storage.h
 * @brief Unified storage abstraction
 *
 * Provides a unified interface for persistent storage including:
 * - NVS (Non-Volatile Storage) for key-value data
 * - SD card for bulk data and logs
 * - Witness record storage with append-only guarantees
 */

#pragma once

#include "../core/types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// NVS STORAGE
// ============================================================================

/**
 * @brief NVS namespace identifiers
 */
#define NVS_NS_MAIN         "securacv"
#define NVS_NS_WIFI         "wifi"
#define NVS_NS_MESH         "mesh"
#define NVS_NS_IDENTITY     "identity"

/**
 * @brief Standard NVS keys
 */
#define NVS_KEY_PRIVKEY     "privkey"
#define NVS_KEY_PUBKEY      "pubkey"
#define NVS_KEY_SEQ         "seq"
#define NVS_KEY_BOOTS       "boots"
#define NVS_KEY_CHAIN       "chain"
#define NVS_KEY_TAMPER      "tamper"
#define NVS_KEY_LOGSEQ      "logseq"
#define NVS_KEY_WIFI_SSID   "wifi_ssid"
#define NVS_KEY_WIFI_PASS   "wifi_pass"
#define NVS_KEY_WIFI_EN     "wifi_en"
#define NVS_KEY_OPERA_ID    "opera_id"
#define NVS_KEY_OPERA_KEY   "opera_key"

/**
 * @brief Initialize NVS storage
 * @return RESULT_OK on success
 */
result_t nvs_storage_init(void);

/**
 * @brief Deinitialize NVS storage
 */
void nvs_storage_deinit(void);

/**
 * @brief Read uint32 from NVS
 * @param ns Namespace
 * @param key Key name
 * @param value Output value
 * @param default_val Default if not found
 * @return RESULT_OK on success
 */
result_t nvs_read_u32(const char* ns, const char* key, uint32_t* value, uint32_t default_val);

/**
 * @brief Write uint32 to NVS
 * @param ns Namespace
 * @param key Key name
 * @param value Value to write
 * @return RESULT_OK on success
 */
result_t nvs_write_u32(const char* ns, const char* key, uint32_t value);

/**
 * @brief Read blob from NVS
 * @param ns Namespace
 * @param key Key name
 * @param data Output buffer
 * @param len Input: buffer size, Output: bytes read
 * @return RESULT_OK on success
 */
result_t nvs_read_blob(const char* ns, const char* key, void* data, size_t* len);

/**
 * @brief Write blob to NVS
 * @param ns Namespace
 * @param key Key name
 * @param data Data to write
 * @param len Data length
 * @return RESULT_OK on success
 */
result_t nvs_write_blob(const char* ns, const char* key, const void* data, size_t len);

/**
 * @brief Read string from NVS
 * @param ns Namespace
 * @param key Key name
 * @param str Output buffer
 * @param max_len Buffer size
 * @return RESULT_OK on success
 */
result_t nvs_read_str(const char* ns, const char* key, char* str, size_t max_len);

/**
 * @brief Write string to NVS
 * @param ns Namespace
 * @param key Key name
 * @param str String to write
 * @return RESULT_OK on success
 */
result_t nvs_write_str(const char* ns, const char* key, const char* str);

/**
 * @brief Erase key from NVS
 * @param ns Namespace
 * @param key Key name
 * @return RESULT_OK on success
 */
result_t nvs_erase_key(const char* ns, const char* key);

/**
 * @brief Erase entire namespace
 * @param ns Namespace
 * @return RESULT_OK on success
 */
result_t nvs_erase_namespace(const char* ns);

// ============================================================================
// SD CARD STORAGE
// ============================================================================

/**
 * @brief SD card configuration
 */
typedef struct {
    int cs_pin;
    int sck_pin;
    int miso_pin;
    int mosi_pin;
    uint32_t freq_hz;
} sd_storage_config_t;

/**
 * @brief SD card status
 */
typedef struct {
    bool mounted;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
    uint32_t files_count;
    char fs_type[16];
} sd_status_t;

/**
 * @brief Initialize SD card storage
 * @param config SD configuration
 * @return RESULT_OK on success
 */
result_t sd_storage_init(const sd_storage_config_t* config);

/**
 * @brief Deinitialize SD card storage
 */
void sd_storage_deinit(void);

/**
 * @brief Check if SD card is mounted
 * @return true if mounted
 */
bool sd_storage_is_mounted(void);

/**
 * @brief Get SD card status
 * @param status Output status
 * @return RESULT_OK on success
 */
result_t sd_storage_status(sd_status_t* status);

/**
 * @brief Remount SD card
 * @return RESULT_OK on success
 */
result_t sd_storage_remount(void);

// ============================================================================
// WITNESS RECORD STORAGE
// ============================================================================

/**
 * @brief Witness storage paths
 */
#define WITNESS_DIR             "/witness"
#define WITNESS_RECORDS_FILE    "/witness/records.bin"
#define WITNESS_INDEX_FILE      "/witness/index.bin"

/**
 * @brief Initialize witness storage
 * @return RESULT_OK on success
 */
result_t witness_storage_init(void);

/**
 * @brief Append witness record to storage
 *
 * Records are stored in append-only fashion for integrity.
 *
 * @param record Record to store
 * @param payload Record payload
 * @param payload_len Payload length
 * @return RESULT_OK on success
 */
result_t witness_storage_append(
    const witness_record_t* record,
    const uint8_t* payload,
    size_t payload_len
);

/**
 * @brief Get witness record count
 * @return Number of records stored
 */
uint32_t witness_storage_count(void);

/**
 * @brief Read witness record by sequence number
 * @param sequence Sequence number
 * @param record Output record
 * @return RESULT_OK on success
 */
result_t witness_storage_read(uint32_t sequence, witness_record_t* record);

/**
 * @brief Export witness records to file
 * @param start_seq Starting sequence number
 * @param end_seq Ending sequence number (0 = to end)
 * @param output_path Output file path
 * @return Number of records exported, or negative on error
 */
int witness_storage_export(uint32_t start_seq, uint32_t end_seq, const char* output_path);

// ============================================================================
// LOG STORAGE
// ============================================================================

/**
 * @brief Log storage paths
 */
#define LOG_DIR                 "/logs"
#define LOG_HEALTH_FILE         "/logs/health.log"
#define LOG_EVENTS_FILE         "/logs/events.log"
#define LOG_SYSTEM_FILE         "/logs/system.log"

/**
 * @brief Log entry structure
 */
typedef struct {
    uint32_t sequence;
    uint32_t timestamp_ms;
    uint8_t level;
    char tag[16];
    char message[128];
    bool acknowledged;
} log_entry_t;

/**
 * @brief Initialize log storage
 * @return RESULT_OK on success
 */
result_t log_storage_init(void);

/**
 * @brief Append log entry
 * @param level Log level
 * @param tag Module tag
 * @param message Log message
 * @return RESULT_OK on success
 */
result_t log_storage_append(uint8_t level, const char* tag, const char* message);

/**
 * @brief Get unacknowledged log count
 * @return Number of unacknowledged logs
 */
uint32_t log_storage_unacked_count(void);

/**
 * @brief Acknowledge logs up to sequence
 * @param up_to_seq Sequence number to acknowledge up to
 * @return RESULT_OK on success
 */
result_t log_storage_acknowledge(uint32_t up_to_seq);

/**
 * @brief Export logs to file
 * @param output_path Output file path
 * @param include_acked Include acknowledged logs
 * @return Number of logs exported
 */
int log_storage_export(const char* output_path, bool include_acked);

#ifdef __cplusplus
}
#endif
