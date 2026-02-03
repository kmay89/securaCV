/**
 * @file health_log.h
 * @brief Health and diagnostic logging with categories
 *
 * Provides structured logging for system health and diagnostic events.
 * Extends the core log.h with category-based logging and acknowledgment
 * tracking for security/tamper events.
 *
 * Severity Levels (compatible with PWK event_contract.md):
 * - DEBUG: Verbose debugging (not stored by default)
 * - INFO: Normal operational events
 * - NOTICE: Notable but expected events
 * - WARNING: Potential issues requiring attention
 * - ERROR: Errors requiring review
 * - CRITICAL: Critical failures affecting operation
 * - ALERT: Immediate action required
 * - TAMPER: Security/integrity events (highest priority)
 */

#pragma once

#include "../core/types.h"
#include "../core/log.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// LOG LEVELS (Extended)
// ============================================================================

typedef enum {
    HEALTH_LOG_DEBUG = 0,       // Verbose debugging (not stored)
    HEALTH_LOG_INFO = 1,        // Normal operational events
    HEALTH_LOG_NOTICE = 2,      // Notable but expected events
    HEALTH_LOG_WARNING = 3,     // Potential issues requiring attention
    HEALTH_LOG_ERROR = 4,       // Errors requiring review
    HEALTH_LOG_CRITICAL = 5,    // Critical failures affecting operation
    HEALTH_LOG_ALERT = 6,       // Immediate action required
    HEALTH_LOG_TAMPER = 7,      // Security/integrity events
} health_log_level_t;

// ============================================================================
// LOG CATEGORIES
// ============================================================================

typedef enum {
    HEALTH_CAT_SYSTEM = 0,      // Boot, shutdown, watchdog
    HEALTH_CAT_CRYPTO = 1,      // Key generation, signing, verification
    HEALTH_CAT_CHAIN = 2,       // Hash chain operations
    HEALTH_CAT_GPS = 3,         // GNSS fix, satellites, time sync
    HEALTH_CAT_STORAGE = 4,     // SD card, NVS operations
    HEALTH_CAT_NETWORK = 5,     // WiFi, HTTP server
    HEALTH_CAT_SENSOR = 6,      // PIR, tamper, environmental
    HEALTH_CAT_USER = 7,        // User actions (config changes, acknowledgments)
    HEALTH_CAT_WITNESS = 8,     // Witness record creation
    HEALTH_CAT_MESH = 9,        // Mesh network (opera) operations
    HEALTH_CAT_BLUETOOTH = 10,  // Bluetooth Low Energy operations
    HEALTH_CAT_CHIRP = 11,      // Chirp channel operations
} health_log_category_t;

// ============================================================================
// ACKNOWLEDGMENT STATUS
// ============================================================================

typedef enum {
    HEALTH_ACK_UNREAD = 0,      // Not yet reviewed
    HEALTH_ACK_REVIEWED = 1,    // Reviewed but not resolved
    HEALTH_ACK_ACKNOWLEDGED = 2, // Acknowledged by user
    HEALTH_ACK_ARCHIVED = 3,    // Archived (retained for audit trail)
} health_ack_status_t;

// ============================================================================
// HEALTH LOG ENTRY
// ============================================================================

typedef struct {
    uint32_t sequence;
    uint32_t timestamp_ms;
    health_log_level_t level;
    health_log_category_t category;
    char message[128];
    char detail[64];
    health_ack_status_t ack_status;
} health_log_entry_t;

// ============================================================================
// CONFIGURATION
// ============================================================================

typedef struct {
    health_log_level_t min_store_level;  // Minimum level to store
    health_log_level_t min_serial_level; // Minimum level to print
    uint16_t max_entries;                // Maximum entries to keep
    bool persist_to_sd;                  // Save to SD card
} health_log_config_t;

#define HEALTH_LOG_CONFIG_DEFAULT { \
    .min_store_level = HEALTH_LOG_INFO, \
    .min_serial_level = HEALTH_LOG_DEBUG, \
    .max_entries = 256, \
    .persist_to_sd = true, \
}

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize health logging
 * @param config Configuration (NULL for defaults)
 * @return RESULT_OK on success
 */
result_t health_log_init(const health_log_config_t* config);

/**
 * @brief Deinitialize health logging
 */
void health_log_deinit(void);

// ============================================================================
// LOGGING FUNCTIONS
// ============================================================================

/**
 * @brief Log a health/diagnostic event
 * @param level Severity level
 * @param category Event category
 * @param message Human-readable message
 */
void health_log(
    health_log_level_t level,
    health_log_category_t category,
    const char* message
);

/**
 * @brief Log a health/diagnostic event with detail
 * @param level Severity level
 * @param category Event category
 * @param message Human-readable message
 * @param detail Optional additional detail (can be NULL)
 */
void health_log_detail(
    health_log_level_t level,
    health_log_category_t category,
    const char* message,
    const char* detail
);

/**
 * @brief Log a formatted health event
 * @param level Severity level
 * @param category Event category
 * @param fmt Printf-style format string
 */
void health_logf(
    health_log_level_t level,
    health_log_category_t category,
    const char* fmt,
    ...
);

// ============================================================================
// RETRIEVAL
// ============================================================================

/**
 * @brief Get log entry count
 * @return Number of stored entries
 */
uint32_t health_log_count(void);

/**
 * @brief Get unacknowledged log count
 * @return Number of unacknowledged entries
 */
uint32_t health_log_unacked_count(void);

/**
 * @brief Get log entries
 * @param entries Output array
 * @param max_entries Maximum to return
 * @param offset Starting offset
 * @return Number of entries returned
 */
int health_log_get(
    health_log_entry_t* entries,
    size_t max_entries,
    uint32_t offset
);

/**
 * @brief Get entries by category
 * @param category Category to filter
 * @param entries Output array
 * @param max_entries Maximum to return
 * @return Number of entries returned
 */
int health_log_get_by_category(
    health_log_category_t category,
    health_log_entry_t* entries,
    size_t max_entries
);

/**
 * @brief Get entries by minimum level
 * @param min_level Minimum level
 * @param entries Output array
 * @param max_entries Maximum to return
 * @return Number of entries returned
 */
int health_log_get_by_level(
    health_log_level_t min_level,
    health_log_entry_t* entries,
    size_t max_entries
);

// ============================================================================
// ACKNOWLEDGMENT
// ============================================================================

/**
 * @brief Acknowledge a log entry
 * @param sequence Entry sequence number
 * @return RESULT_OK on success
 */
result_t health_log_acknowledge(uint32_t sequence);

/**
 * @brief Acknowledge all entries up to sequence
 * @param up_to_sequence Maximum sequence to acknowledge
 * @return RESULT_OK on success
 */
result_t health_log_acknowledge_all(uint32_t up_to_sequence);

/**
 * @brief Acknowledge all entries at or below level
 * @param max_level Maximum level to acknowledge
 * @return RESULT_OK on success
 */
result_t health_log_acknowledge_level(health_log_level_t max_level);

// ============================================================================
// MAINTENANCE
// ============================================================================

/**
 * @brief Clear all log entries
 */
void health_log_clear(void);

/**
 * @brief Export logs to file
 * @param path Output file path
 * @param include_acked Include acknowledged entries
 * @return Number of entries exported, negative on error
 */
int health_log_export(const char* path, bool include_acked);

// ============================================================================
// HELPERS
// ============================================================================

/**
 * @brief Get level name
 * @param level Log level
 * @return Level name string
 */
const char* health_log_level_name(health_log_level_t level);

/**
 * @brief Get short level name (3 chars)
 * @param level Log level
 * @return Short level name
 */
const char* health_log_level_short(health_log_level_t level);

/**
 * @brief Get category name
 * @param category Log category
 * @return Category name string
 */
const char* health_log_category_name(health_log_category_t category);

/**
 * @brief Get ack status name
 * @param status Ack status
 * @return Status name string
 */
const char* health_log_ack_name(health_ack_status_t status);

/**
 * @brief Check if level requires attention
 * @param level Log level
 * @return true if WARNING or higher
 */
bool health_log_requires_attention(health_log_level_t level);

/**
 * @brief Check if level is security-related
 * @param level Log level
 * @return true if ALERT or TAMPER
 */
bool health_log_is_security(health_log_level_t level);

#ifdef __cplusplus
}
#endif
