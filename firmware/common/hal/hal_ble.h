/**
 * @file hal_ble.h
 * @brief HAL Bluetooth Low Energy Interface
 *
 * Provides hardware-independent BLE functionality.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// BLE CONSTANTS
// ============================================================================

#define HAL_BLE_ADDR_LEN        6
#define HAL_BLE_NAME_MAX        32
#define HAL_BLE_UUID_128_LEN    16

// ============================================================================
// BLE TYPES
// ============================================================================

typedef enum {
    BLE_ROLE_PERIPHERAL = 0,    // Server (advertiser)
    BLE_ROLE_CENTRAL,           // Client (scanner)
    BLE_ROLE_BOTH,
} ble_role_t;

typedef enum {
    BLE_ADDR_PUBLIC = 0,
    BLE_ADDR_RANDOM_STATIC,
    BLE_ADDR_RANDOM_PRIVATE_RESOLVABLE,
    BLE_ADDR_RANDOM_PRIVATE_NON_RESOLVABLE,
} ble_addr_type_t;

typedef enum {
    BLE_EVENT_CONNECTED = 0,
    BLE_EVENT_DISCONNECTED,
    BLE_EVENT_ADV_START,
    BLE_EVENT_ADV_STOP,
    BLE_EVENT_SCAN_RESULT,
    BLE_EVENT_PAIRING_REQUEST,
    BLE_EVENT_PAIRING_COMPLETE,
    BLE_EVENT_PAIRING_FAILED,
    BLE_EVENT_MTU_UPDATED,
    BLE_EVENT_NOTIFY_ENABLED,
    BLE_EVENT_NOTIFY_DISABLED,
    BLE_EVENT_WRITE_RECEIVED,
    BLE_EVENT_READ_REQUEST,
} ble_event_t;

typedef struct {
    uint8_t addr[HAL_BLE_ADDR_LEN];
    ble_addr_type_t type;
} ble_addr_t;

typedef struct {
    char name[HAL_BLE_NAME_MAX + 1];
    uint16_t appearance;
    uint16_t mtu;
    ble_role_t role;
} ble_config_t;

typedef struct {
    uint16_t interval_min_ms;   // Min advertising interval
    uint16_t interval_max_ms;   // Max advertising interval
    bool connectable;
    bool scannable;
    const uint8_t* mfg_data;    // Manufacturer data (optional)
    size_t mfg_data_len;
} ble_adv_config_t;

typedef struct {
    ble_addr_t addr;
    char name[HAL_BLE_NAME_MAX + 1];
    int8_t rssi;
    bool connectable;
    const uint8_t* adv_data;
    size_t adv_data_len;
} ble_scan_result_t;

typedef struct {
    bool connected;
    bool advertising;
    bool scanning;
    uint8_t num_connections;
    uint16_t mtu;
    ble_addr_t local_addr;
} ble_status_t;

/**
 * @brief BLE event callback
 * @param event Event type
 * @param data Event-specific data
 * @param user_data User context
 */
typedef void (*ble_event_cb_t)(ble_event_t event, void* data, void* user_data);

// ============================================================================
// BLE CORE FUNCTIONS
// ============================================================================

/**
 * @brief Initialize BLE subsystem
 * @param config BLE configuration
 * @return 0 on success, negative on error
 */
int hal_ble_init(const ble_config_t* config);

/**
 * @brief Deinitialize BLE
 * @return 0 on success, negative on error
 */
int hal_ble_deinit(void);

/**
 * @brief Get BLE status
 * @param status Output status structure
 * @return 0 on success, negative on error
 */
int hal_ble_get_status(ble_status_t* status);

/**
 * @brief Register event callback
 * @param callback Callback function
 * @param user_data User context
 * @return 0 on success, negative on error
 */
int hal_ble_on_event(ble_event_cb_t callback, void* user_data);

// ============================================================================
// ADVERTISING (PERIPHERAL)
// ============================================================================

/**
 * @brief Start advertising
 * @param config Advertising configuration
 * @return 0 on success, negative on error
 */
int hal_ble_adv_start(const ble_adv_config_t* config);

/**
 * @brief Stop advertising
 * @return 0 on success, negative on error
 */
int hal_ble_adv_stop(void);

/**
 * @brief Update advertising data
 * @param mfg_data New manufacturer data
 * @param len Data length
 * @return 0 on success, negative on error
 */
int hal_ble_adv_update_data(const uint8_t* mfg_data, size_t len);

// ============================================================================
// SCANNING (CENTRAL)
// ============================================================================

/**
 * @brief Start scanning for devices
 * @param duration_ms Scan duration (0 = continuous)
 * @param active True for active scan (requests scan response)
 * @return 0 on success, negative on error
 */
int hal_ble_scan_start(uint32_t duration_ms, bool active);

/**
 * @brief Stop scanning
 * @return 0 on success, negative on error
 */
int hal_ble_scan_stop(void);

// ============================================================================
// CONNECTION MANAGEMENT
// ============================================================================

/**
 * @brief Connect to a device (central role)
 * @param addr Device address
 * @param timeout_ms Connection timeout
 * @return Connection handle (>= 0) or negative error
 */
int hal_ble_connect(const ble_addr_t* addr, uint32_t timeout_ms);

/**
 * @brief Disconnect from a device
 * @param conn_handle Connection handle
 * @return 0 on success, negative on error
 */
int hal_ble_disconnect(int conn_handle);

/**
 * @brief Get connection RSSI
 * @param conn_handle Connection handle
 * @return RSSI in dBm, or 0 on error
 */
int8_t hal_ble_conn_rssi(int conn_handle);

// ============================================================================
// SECURITY / PAIRING
// ============================================================================

typedef struct {
    bool bonding;           // Store pairing info
    bool mitm_protection;   // Require MITM protection
    bool secure_connections; // Use LE Secure Connections
    uint32_t passkey;       // Fixed passkey (0 = random)
} ble_security_config_t;

/**
 * @brief Configure security parameters
 * @param config Security configuration
 * @return 0 on success, negative on error
 */
int hal_ble_set_security(const ble_security_config_t* config);

/**
 * @brief Start pairing with connected device
 * @param conn_handle Connection handle
 * @return 0 on success, negative on error
 */
int hal_ble_pair(int conn_handle);

/**
 * @brief Accept/reject pairing request
 * @param accept True to accept
 * @return 0 on success, negative on error
 */
int hal_ble_pair_response(bool accept);

/**
 * @brief Delete bonding information
 * @param addr Device address (NULL = delete all)
 * @return 0 on success, negative on error
 */
int hal_ble_delete_bond(const ble_addr_t* addr);

#ifdef __cplusplus
}
#endif
