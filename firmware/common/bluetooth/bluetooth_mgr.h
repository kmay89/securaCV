/**
 * @file bluetooth_mgr.h
 * @brief Bluetooth Low Energy management
 *
 * Manages BLE connections, pairing, and services for witness devices.
 * Uses NimBLE 2.x API for ESP32 platforms.
 *
 * SERVICES:
 * - Device Information Service (DIS)
 * - Witness Service (custom)
 * - Pairing/Configuration Service (custom)
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
// CONSTANTS
// ============================================================================

// Maximum connections
#define BLE_MAX_CONNECTIONS     3

// Service UUIDs (128-bit)
#define BLE_UUID_WITNESS_SVC    "8e3d4f1a-2c6b-4a8e-9f1d-3e5c7b9a0d2f"
#define BLE_UUID_CONFIG_SVC     "9f4e5a2b-3d7c-5b9f-a02e-4f6d8c0b1e3a"

// Characteristic UUIDs
#define BLE_UUID_DEVICE_ID      "8e3d4f1a-2c6b-4a8e-0001-3e5c7b9a0d2f"
#define BLE_UUID_STATUS         "8e3d4f1a-2c6b-4a8e-0002-3e5c7b9a0d2f"
#define BLE_UUID_WITNESS_DATA   "8e3d4f1a-2c6b-4a8e-0003-3e5c7b9a0d2f"
#define BLE_UUID_PAIRING_CODE   "9f4e5a2b-3d7c-5b9f-0001-4f6d8c0b1e3a"
#define BLE_UUID_CONFIG_WRITE   "9f4e5a2b-3d7c-5b9f-0002-4f6d8c0b1e3a"

// ============================================================================
// TYPES
// ============================================================================

/**
 * @brief BLE manager state
 */
typedef enum {
    BLE_STATE_DISABLED = 0,
    BLE_STATE_INITIALIZING,
    BLE_STATE_READY,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_PAIRING,
    BLE_STATE_ERROR,
} ble_state_t;

/**
 * @brief BLE connection info
 */
typedef struct {
    uint16_t conn_handle;
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    uint16_t mtu;
    bool authenticated;
    bool bonded;
    uint32_t connected_at_ms;
} ble_connection_t;

/**
 * @brief BLE manager status
 */
typedef struct {
    ble_state_t state;
    bool advertising;
    bool connected;
    uint8_t connection_count;
    uint16_t mtu;
    int8_t tx_power;
    uint8_t local_addr[6];
    char device_name[32];
} ble_status_t;

/**
 * @brief BLE event type
 */
typedef enum {
    BLE_EVENT_READY = 0,
    BLE_EVENT_ADV_STARTED,
    BLE_EVENT_ADV_STOPPED,
    BLE_EVENT_CONNECTED,
    BLE_EVENT_DISCONNECTED,
    BLE_EVENT_MTU_UPDATED,
    BLE_EVENT_PAIRING_REQUEST,
    BLE_EVENT_PAIRED,
    BLE_EVENT_PAIRING_FAILED,
    BLE_EVENT_DATA_RECEIVED,
    BLE_EVENT_SUBSCRIBED,
    BLE_EVENT_UNSUBSCRIBED,
} ble_event_type_t;

/**
 * @brief BLE event data
 */
typedef struct {
    ble_event_type_t type;
    uint16_t conn_handle;
    union {
        struct {
            uint8_t* data;
            size_t len;
        } data_received;
        struct {
            uint16_t mtu;
        } mtu_updated;
        struct {
            uint32_t passkey;
        } pairing_request;
    };
} ble_event_t;

/**
 * @brief BLE event callback
 */
typedef void (*ble_event_callback_t)(const ble_event_t* event, void* user_data);

// ============================================================================
// CONFIGURATION
// ============================================================================

/**
 * @brief BLE manager configuration
 */
typedef struct {
    const char* device_name;            // Advertised device name
    const char* device_id;              // Device identifier
    const uint8_t* public_key;          // Ed25519 public key (for pairing)
    int8_t tx_power;                    // TX power in dBm
    bool pairable;                      // Allow new pairings
    bool require_bonding;               // Require bonded connection
    uint32_t adv_interval_ms;           // Advertising interval
    ble_event_callback_t event_callback;
    void* user_data;
} ble_config_t;

// Default configuration
#define BLE_CONFIG_DEFAULT { \
    .device_name = "SecuraCV", \
    .device_id = NULL, \
    .public_key = NULL, \
    .tx_power = 0, \
    .pairable = true, \
    .require_bonding = false, \
    .adv_interval_ms = 100, \
    .event_callback = NULL, \
    .user_data = NULL, \
}

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize BLE manager
 * @param config Configuration
 * @return RESULT_OK on success
 */
result_t ble_mgr_init(const ble_config_t* config);

/**
 * @brief Deinitialize BLE manager
 * @return RESULT_OK on success
 */
result_t ble_mgr_deinit(void);

/**
 * @brief Start BLE advertising
 * @return RESULT_OK on success
 */
result_t ble_mgr_start_advertising(void);

/**
 * @brief Stop BLE advertising
 * @return RESULT_OK on success
 */
result_t ble_mgr_stop_advertising(void);

/**
 * @brief Get BLE status
 * @param status Output status
 * @return RESULT_OK on success
 */
result_t ble_mgr_get_status(ble_status_t* status);

// ============================================================================
// CONNECTION MANAGEMENT
// ============================================================================

/**
 * @brief Get connected devices
 * @param connections Output array
 * @param max_connections Maximum to return
 * @return Number of connections
 */
int ble_mgr_get_connections(ble_connection_t* connections, size_t max_connections);

/**
 * @brief Disconnect a device
 * @param conn_handle Connection handle
 * @return RESULT_OK on success
 */
result_t ble_mgr_disconnect(uint16_t conn_handle);

/**
 * @brief Disconnect all devices
 * @return RESULT_OK on success
 */
result_t ble_mgr_disconnect_all(void);

// ============================================================================
// PAIRING
// ============================================================================

/**
 * @brief Enable/disable pairing mode
 * @param enable true to enable
 * @return RESULT_OK on success
 */
result_t ble_mgr_set_pairable(bool enable);

/**
 * @brief Get current pairing code
 * @param code Output buffer (7 chars: 6 digits + null)
 * @return RESULT_OK on success
 */
result_t ble_mgr_get_pairing_code(char code[7]);

/**
 * @brief Confirm pairing with passkey
 * @param conn_handle Connection handle
 * @param accept true to accept
 * @return RESULT_OK on success
 */
result_t ble_mgr_pairing_response(uint16_t conn_handle, bool accept);

/**
 * @brief Delete bonding information
 * @param addr Device address (NULL = delete all)
 * @return RESULT_OK on success
 */
result_t ble_mgr_delete_bond(const uint8_t* addr);

// ============================================================================
// DATA TRANSFER
// ============================================================================

/**
 * @brief Send notification to subscribed clients
 * @param data Data to send
 * @param len Data length
 * @return Number of clients notified
 */
int ble_mgr_notify(const uint8_t* data, size_t len);

/**
 * @brief Send indication (with acknowledgment)
 * @param conn_handle Connection handle
 * @param data Data to send
 * @param len Data length
 * @return RESULT_OK on success
 */
result_t ble_mgr_indicate(uint16_t conn_handle, const uint8_t* data, size_t len);

/**
 * @brief Update status characteristic
 * @param status Status JSON string
 * @return RESULT_OK on success
 */
result_t ble_mgr_update_status(const char* status);

// ============================================================================
// PROCESSING
// ============================================================================

/**
 * @brief Process BLE events (call from main loop)
 */
void ble_mgr_process(void);

#ifdef __cplusplus
}
#endif
