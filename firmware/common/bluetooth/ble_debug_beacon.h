/**
 * @file ble_debug_beacon.h
 * @brief BLE debug beacon for external RF diagnostics
 *
 * Encodes device health into BLE manufacturer-specific advertising data
 * so that a Flipper Zero or any BLE scanner can read subsystem status
 * without connecting. Activated by boot-button hold or compile flag.
 *
 * Privacy: only aggregate counts and operational metrics — no MAC
 * addresses, no identity substrates, no raw sensor data.
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

#define BLE_DEBUG_MAGIC             0x5C    // "SC" for SecuraCV
#define BLE_DEBUG_VERSION           0x01
#define BLE_DEBUG_COMPANY_ID        0xFFFF  // BLE SIG testing ID
#define BLE_DEBUG_PAYLOAD_LEN       20      // Total manufacturer data bytes

// Subsystem flag bits (byte 4 of payload)
#define BLE_DEBUG_FLAG_WIFI         (1 << 0)
#define BLE_DEBUG_FLAG_BLE          (1 << 1)
#define BLE_DEBUG_FLAG_MESH         (1 << 2)
#define BLE_DEBUG_FLAG_CHIRP        (1 << 3)
#define BLE_DEBUG_FLAG_GPS          (1 << 4)
#define BLE_DEBUG_FLAG_SD           (1 << 5)
#define BLE_DEBUG_FLAG_CRYPTO       (1 << 6)
#define BLE_DEBUG_FLAG_TAMPER       (1 << 7)

// Error codes (byte 18)
typedef enum {
    BLE_DEBUG_ERR_NONE          = 0x00,
    BLE_DEBUG_ERR_CRYPTO_FAIL   = 0x01,
    BLE_DEBUG_ERR_SD_FAIL       = 0x02,
    BLE_DEBUG_ERR_MESH_FAIL     = 0x03,
    BLE_DEBUG_ERR_WIFI_FAIL     = 0x04,
    BLE_DEBUG_ERR_HEAP_LOW      = 0x05,
    BLE_DEBUG_ERR_CHAIN_VERIFY  = 0x06,
    BLE_DEBUG_ERR_WATCHDOG      = 0x07,
} ble_debug_error_t;

// ============================================================================
// TYPES
// ============================================================================

/**
 * @brief Debug beacon payload (20 bytes, matches wire format)
 *
 * Wire format:
 *   [0-1]   Company ID (0xFFFF, little-endian — added by BLE stack)
 *   [2]     Magic (0x5C)
 *   [3]     Version (0x01)
 *   [4]     Subsystem flags (bitfield)
 *   [5]     Mesh peer count (0-8)
 *   [6]     RF device count (aggregate, privacy-safe)
 *   [7-8]   Free heap KB (big-endian)
 *   [9-12]  Uptime seconds (big-endian)
 *   [13-14] Witness chain height (big-endian)
 *   [15]    Chain verify status (0=ok, 1=fail, 0xFF=pending)
 *   [16-17] Reserved (0x0000)
 *   [18]    Error code
 *   [19]    XOR checksum of bytes 2-18
 */
typedef struct __attribute__((packed)) {
    uint8_t  company_id[2];     // 0xFFFF little-endian
    uint8_t  magic;             // 0x5C
    uint8_t  version;           // 0x01
    uint8_t  subsystem_flags;
    uint8_t  mesh_peers;
    uint8_t  rf_device_count;
    uint8_t  free_heap_kb[2];   // big-endian
    uint8_t  uptime_sec[4];     // big-endian
    uint8_t  chain_height[2];   // big-endian
    uint8_t  chain_verify;      // 0=ok, 1=fail, 0xFF=pending
    uint8_t  reserved[2];
    uint8_t  error_code;
    uint8_t  checksum;          // XOR of bytes 2-18
} ble_debug_payload_t;

static_assert(sizeof(ble_debug_payload_t) == BLE_DEBUG_PAYLOAD_LEN,
              "debug beacon payload must be 20 bytes");

// ============================================================================
// API
// ============================================================================

/**
 * @brief Initialize debug beacon
 * @param fingerprint_hex Last 4 hex chars of pubkey fingerprint (for device name)
 * @return RESULT_OK on success
 */
result_t ble_debug_beacon_init(const char* fingerprint_hex);

/**
 * @brief Deinitialize and restore normal advertising
 * @return RESULT_OK on success
 */
result_t ble_debug_beacon_deinit(void);

/**
 * @brief Update debug beacon with current health data
 * @param health Current system health snapshot
 * @return RESULT_OK on success
 */
result_t ble_debug_beacon_update(const system_health_t* health);

/**
 * @brief Set the most recent error code
 * @param err Error code to advertise
 */
void ble_debug_beacon_set_error(ble_debug_error_t err);

/**
 * @brief Set mesh peer count (called from mesh module)
 * @param count Number of active mesh peers
 */
void ble_debug_beacon_set_mesh_peers(uint8_t count);

/**
 * @brief Set RF device count (called from rf_presence module)
 * @param count Aggregate nearby device count
 */
void ble_debug_beacon_set_rf_devices(uint8_t count);

/**
 * @brief Set chain height (called from witness chain)
 * @param height Current chain sequence number
 */
void ble_debug_beacon_set_chain_height(uint16_t height);

/**
 * @brief Set chain verification status
 * @param status 0=ok, 1=fail, 0xFF=pending
 */
void ble_debug_beacon_set_chain_verify(uint8_t status);

/**
 * @brief Check if debug beacon is currently active
 * @return true if broadcasting debug data
 */
bool ble_debug_beacon_is_active(void);

/**
 * @brief Build a payload struct from current state (for testing)
 * @param out Output payload
 * @param health System health data
 */
void ble_debug_beacon_build_payload(ble_debug_payload_t* out,
                                     const system_health_t* health);

#ifdef __cplusplus
}
#endif
