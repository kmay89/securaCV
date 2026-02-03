/**
 * @file rf_presence.h
 * @brief Privacy-preserving RF presence detection
 *
 * Detects nearby WiFi and BLE devices WITHOUT storing MAC addresses
 * or creating device fingerprints. Only aggregate presence counts
 * and signal strengths are reported.
 *
 * PRIVACY GUARANTEES:
 * - NO MAC address storage
 * - NO device fingerprinting
 * - NO persistent tracking
 * - Only aggregate statistics exported
 * - Session tokens rotated every 4 hours
 */

#pragma once

#include "../core/types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CONSTANTS
// ============================================================================

// Privacy: Maximum session duration before rotation
#define RF_SESSION_ROTATION_MS      (4 * 60 * 60 * 1000)  // 4 hours

// Detection windows
#define RF_WIFI_SCAN_DURATION_MS    2000
#define RF_BLE_SCAN_DURATION_MS     3000

// Device count thresholds
#define RF_PRESENCE_THRESHOLD       1       // Min devices for presence
#define RF_CROWD_THRESHOLD          10      // Devices for "crowd" detection

// ============================================================================
// TYPES
// ============================================================================

/**
 * @brief RF detection mode
 */
typedef enum {
    RF_MODE_DISABLED = 0,
    RF_MODE_WIFI_ONLY,
    RF_MODE_BLE_ONLY,
    RF_MODE_WIFI_AND_BLE,
} rf_detection_mode_t;

/**
 * @brief RF presence event type
 */
typedef enum {
    RF_EVENT_PRESENCE_START = 0,
    RF_EVENT_PRESENCE_END,
    RF_EVENT_COUNT_CHANGE,
    RF_EVENT_CROWD_DETECTED,
    RF_EVENT_CROWD_CLEARED,
} rf_event_t;

/**
 * @brief RF presence sample (privacy-preserving)
 *
 * Contains ONLY aggregate data, no identifiers.
 */
typedef struct {
    uint8_t wifi_device_count;      // Unique WiFi devices seen
    uint8_t ble_device_count;       // Unique BLE devices seen
    uint8_t total_device_count;     // Combined unique count
    int8_t wifi_strongest_rssi;     // Strongest WiFi signal
    int8_t ble_strongest_rssi;      // Strongest BLE signal
    int8_t average_rssi;            // Overall average RSSI
    uint32_t sample_time_ms;        // Sample timestamp
    bool presence_detected;         // Presence threshold met
    bool crowd_detected;            // Crowd threshold met
} rf_sample_t;

/**
 * @brief RF presence statistics
 */
typedef struct {
    uint32_t samples_taken;
    uint32_t presence_events;
    uint32_t crowd_events;
    uint32_t wifi_scans;
    uint32_t ble_scans;
    uint32_t session_rotations;
    uint32_t current_session_ms;
    uint32_t uptime_ms;
} rf_stats_t;

/**
 * @brief RF event callback
 */
typedef void (*rf_event_callback_t)(
    rf_event_t event,
    const rf_sample_t* sample,
    void* user_data
);

// ============================================================================
// CONFIGURATION
// ============================================================================

/**
 * @brief RF presence configuration
 */
typedef struct {
    rf_detection_mode_t mode;
    uint32_t sample_interval_ms;        // How often to sample
    uint32_t presence_threshold;        // Min devices for presence
    uint32_t crowd_threshold;           // Min devices for crowd
    uint32_t presence_timeout_ms;       // Time before presence clears
    rf_event_callback_t event_callback;
    void* user_data;
} rf_presence_config_t;

// Default configuration
#define RF_PRESENCE_CONFIG_DEFAULT { \
    .mode = RF_MODE_WIFI_AND_BLE, \
    .sample_interval_ms = 10000, \
    .presence_threshold = 1, \
    .crowd_threshold = 10, \
    .presence_timeout_ms = 30000, \
    .event_callback = NULL, \
    .user_data = NULL, \
}

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize RF presence detection
 * @param config Configuration
 * @return RESULT_OK on success
 */
result_t rf_presence_init(const rf_presence_config_t* config);

/**
 * @brief Deinitialize RF presence
 * @return RESULT_OK on success
 */
result_t rf_presence_deinit(void);

/**
 * @brief Start RF presence detection
 * @return RESULT_OK on success
 */
result_t rf_presence_start(void);

/**
 * @brief Stop RF presence detection
 * @return RESULT_OK on success
 */
result_t rf_presence_stop(void);

/**
 * @brief Check if RF presence is running
 * @return true if running
 */
bool rf_presence_is_running(void);

// ============================================================================
// DATA ACCESS
// ============================================================================

/**
 * @brief Get current RF sample
 * @param sample Output sample
 * @return RESULT_OK on success
 */
result_t rf_presence_get_sample(rf_sample_t* sample);

/**
 * @brief Get RF statistics
 * @param stats Output statistics
 * @return RESULT_OK on success
 */
result_t rf_presence_get_stats(rf_stats_t* stats);

/**
 * @brief Check if presence is currently detected
 * @return true if presence detected
 */
bool rf_presence_detected(void);

/**
 * @brief Check if crowd is currently detected
 * @return true if crowd detected
 */
bool rf_presence_crowd_detected(void);

/**
 * @brief Get current device count
 * @return Total device count
 */
uint8_t rf_presence_device_count(void);

// ============================================================================
// CONTROL
// ============================================================================

/**
 * @brief Trigger immediate scan
 *
 * Performs an immediate RF scan outside the normal interval.
 *
 * @return RESULT_OK on success
 */
result_t rf_presence_scan_now(void);

/**
 * @brief Set detection mode
 * @param mode New mode
 * @return RESULT_OK on success
 */
result_t rf_presence_set_mode(rf_detection_mode_t mode);

/**
 * @brief Force session rotation
 *
 * Immediately rotates internal session tokens.
 * Normally happens automatically every 4 hours.
 *
 * @return RESULT_OK on success
 */
result_t rf_presence_rotate_session(void);

// ============================================================================
// PROCESSING
// ============================================================================

/**
 * @brief Process RF presence (call from main loop)
 *
 * Handles scan scheduling, session rotation, and event generation.
 */
void rf_presence_process(void);

#ifdef __cplusplus
}
#endif
