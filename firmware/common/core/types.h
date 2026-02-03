/**
 * @file types.h
 * @brief Core type definitions for SecuraCV firmware
 *
 * Defines common data structures used across all firmware modules.
 * This file should have no dependencies on board-specific code.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// VERSION INFO
// ============================================================================

#define SECURACV_CORE_VERSION_MAJOR  1
#define SECURACV_CORE_VERSION_MINOR  0
#define SECURACV_CORE_VERSION_PATCH  0
#define SECURACV_CORE_VERSION_STRING "1.0.0"

// ============================================================================
// RESULT TYPE
// ============================================================================

/**
 * @brief Standard result type for operations
 */
typedef enum {
    RESULT_OK = 0,
    RESULT_ERROR = -1,
    RESULT_INVALID_PARAM = -2,
    RESULT_NOT_INITIALIZED = -3,
    RESULT_TIMEOUT = -4,
    RESULT_BUSY = -5,
    RESULT_NO_MEMORY = -6,
    RESULT_NOT_FOUND = -7,
    RESULT_FULL = -8,
    RESULT_EMPTY = -9,
    RESULT_IO_ERROR = -10,
    RESULT_CRYPTO_ERROR = -11,
    RESULT_VERIFY_FAILED = -12,
} result_t;

/**
 * @brief Check if result is success
 */
#define RESULT_IS_OK(r) ((r) == RESULT_OK)

/**
 * @brief Check if result is error
 */
#define RESULT_IS_ERROR(r) ((r) != RESULT_OK)

// ============================================================================
// GPS/GNSS TYPES
// ============================================================================

/**
 * @brief GPS fix quality enumeration
 */
typedef enum {
    GPS_FIX_INVALID = 0,
    GPS_FIX_GPS = 1,
    GPS_FIX_DGPS = 2,
    GPS_FIX_PPS = 3,
    GPS_FIX_RTK = 4,
    GPS_FIX_FLOAT_RTK = 5,
    GPS_FIX_ESTIMATED = 6,
    GPS_FIX_MANUAL = 7,
    GPS_FIX_SIMULATION = 8,
} gps_fix_quality_t;

/**
 * @brief GPS fix mode (2D/3D)
 */
typedef enum {
    GPS_MODE_NONE = 1,
    GPS_MODE_2D = 2,
    GPS_MODE_3D = 3,
} gps_fix_mode_t;

/**
 * @brief GPS/GNSS fix data structure
 */
typedef struct {
    bool valid;                 // Fix is valid
    double latitude;            // Decimal degrees
    double longitude;           // Decimal degrees
    double altitude_m;          // Meters above MSL
    double geoid_sep_m;         // Geoid separation
    double speed_knots;         // Speed over ground (knots)
    double speed_kmh;           // Speed over ground (km/h)
    double course_deg;          // Course over ground (degrees)
    double hdop;                // Horizontal dilution of precision
    double pdop;                // Position dilution of precision
    double vdop;                // Vertical dilution of precision
    gps_fix_quality_t quality;  // Fix quality indicator
    gps_fix_mode_t mode;        // Fix mode (2D/3D)
    uint8_t satellites;         // Satellites used in fix
    uint8_t sats_in_view;       // Satellites in view
    uint32_t last_update_ms;    // Timestamp of last update
} gnss_fix_t;

/**
 * @brief GPS UTC time structure
 */
typedef struct {
    bool valid;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t centisecond;
    uint32_t last_update_ms;
} gnss_time_t;

// ============================================================================
// MOTION / STATE TYPES
// ============================================================================

/**
 * @brief Device motion/fix state
 */
typedef enum {
    STATE_NO_FIX = 0,
    STATE_FIX_ACQUIRED,
    STATE_STATIONARY,
    STATE_MOVING,
    STATE_FIX_LOST,
} motion_state_t;

/**
 * @brief State change reason
 */
typedef enum {
    REASON_INIT = 0,
    REASON_GPS_LOCK,
    REASON_GPS_LOST,
    REASON_SPEED_CHANGE,
    REASON_TIMEOUT,
    REASON_USER_REQUEST,
    REASON_ERROR,
} state_change_reason_t;

// ============================================================================
// WITNESS RECORD TYPES
// ============================================================================

/**
 * @brief Record type enumeration
 */
typedef enum {
    RECORD_TYPE_BOOT_ATTESTATION = 0,
    RECORD_TYPE_WITNESS_EVENT = 1,
    RECORD_TYPE_TAMPER_ALERT = 2,
    RECORD_TYPE_STATE_CHANGE = 3,
    RECORD_TYPE_PRESENCE = 4,
    RECORD_TYPE_MESH_EVENT = 5,
    RECORD_TYPE_CHIRP = 6,
} record_type_t;

/**
 * @brief Witness record structure
 */
typedef struct {
    uint32_t sequence;              // Monotonic sequence number
    uint32_t time_bucket;           // Coarsened timestamp (privacy)
    record_type_t type;             // Record type
    uint8_t payload_hash[32];       // SHA-256 of payload
    uint8_t prev_hash[32];          // Previous record hash
    uint8_t chain_hash[32];         // Current chain hash
    uint8_t signature[64];          // Ed25519 signature
    size_t payload_len;             // Payload length
    bool verified;                  // Self-verification passed
} witness_record_t;

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

/**
 * @brief Device identity and cryptographic state
 */
typedef struct {
    uint8_t private_key[32];        // Ed25519 private key
    uint8_t public_key[32];         // Ed25519 public key
    uint8_t pubkey_fingerprint[8];  // Short fingerprint for display
    uint8_t chain_head[32];         // Current chain head hash
    uint32_t sequence;              // Current sequence number
    uint32_t seq_persisted;         // Last persisted sequence
    uint32_t boot_count;            // Number of boots
    uint32_t tamper_count;          // Tamper events
    uint32_t log_seq;               // Log sequence number
    uint32_t boot_ms;               // Boot timestamp (millis)
    bool initialized;               // Identity initialized
    bool tamper_active;             // Tamper currently active
    char device_id[32];             // Device identifier string
    char ap_ssid[32];               // Access point SSID
} device_identity_t;

// ============================================================================
// SYSTEM HEALTH
// ============================================================================

/**
 * @brief System health metrics
 */
typedef struct {
    uint32_t records_created;
    uint32_t records_verified;
    uint32_t verify_failures;
    uint32_t gps_sentences;
    uint32_t chain_persists;
    uint32_t state_changes;
    uint32_t tamper_events;
    uint32_t uptime_sec;
    uint32_t free_heap;
    uint32_t min_heap;
    uint32_t http_requests;
    uint32_t http_errors;
    uint32_t sd_writes;
    uint32_t sd_errors;
    uint32_t mesh_messages_sent;
    uint32_t mesh_messages_recv;
    uint32_t ble_connections;
    uint32_t chirp_sent;
    uint32_t chirp_recv;
    uint32_t logs_stored;
    bool gps_healthy;
    bool crypto_healthy;
    bool sd_healthy;
    bool wifi_active;
    bool ble_active;
    bool mesh_active;
    bool chirp_active;
} system_health_t;

// ============================================================================
// WIFI STATUS
// ============================================================================

/**
 * @brief WiFi provisioning state
 */
typedef enum {
    WIFI_PROV_IDLE = 0,
    WIFI_PROV_SCANNING,
    WIFI_PROV_CONNECTING,
    WIFI_PROV_CONNECTED,
    WIFI_PROV_FAILED,
    WIFI_PROV_AP_ONLY,
} wifi_prov_state_t;

/**
 * @brief WiFi credentials
 */
typedef struct {
    char ssid[33];
    char password[65];
    bool enabled;
    bool configured;
} wifi_credentials_t;

/**
 * @brief WiFi status
 */
typedef struct {
    wifi_prov_state_t state;
    bool ap_active;
    bool sta_connected;
    int8_t rssi;
    char sta_ip[16];
    char ap_ip[16];
    uint8_t ap_clients;
    uint32_t connect_attempts;
    uint32_t last_connect_ms;
    uint32_t connected_since_ms;
} wifi_status_info_t;

// ============================================================================
// PRESENCE DETECTION
// ============================================================================

/**
 * @brief Bounding box for detection
 */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint8_t score;      // 0-100 confidence
} bbox_t;

/**
 * @brief Voxel grid position
 */
typedef struct {
    uint8_t row;
    uint8_t col;
    uint8_t rows;
    uint8_t cols;
} voxel_t;

/**
 * @brief Vision sample data
 */
typedef struct {
    bool person_detected;
    bbox_t bbox;
    voxel_t voxel;
    uint32_t timestamp_ms;
} vision_sample_t;

/**
 * @brief Presence state snapshot
 */
typedef struct {
    bool presence;
    bool dwelling;
    uint32_t presence_ms;
    uint32_t dwell_ms;
    uint8_t confidence;
    voxel_t voxel;
    bbox_t bbox;
    const char* last_event;
    uint32_t uptime_sec;
    uint32_t timestamp_ms;
} presence_state_t;

// ============================================================================
// RF PRESENCE (Privacy-Preserving)
// ============================================================================

/**
 * @brief RF presence mode
 */
typedef enum {
    RF_MODE_DISABLED = 0,
    RF_MODE_WIFI_ONLY,
    RF_MODE_BLE_ONLY,
    RF_MODE_BOTH,
} rf_mode_t;

/**
 * @brief RF presence data (privacy-preserving - no MAC addresses)
 */
typedef struct {
    uint8_t wifi_device_count;      // Current WiFi devices detected
    uint8_t ble_device_count;       // Current BLE devices detected
    uint8_t total_device_count;     // Combined count
    int8_t strongest_rssi;          // Strongest signal seen
    int8_t average_rssi;            // Average signal strength
    uint32_t sample_time_ms;        // When sample was taken
    bool active;                    // RF scanning active
} rf_presence_t;

// ============================================================================
// UTILITY MACROS
// ============================================================================

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define CLAMP(x, lo, hi) (MIN(MAX(x, lo), hi))
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#ifdef __cplusplus
}
#endif
