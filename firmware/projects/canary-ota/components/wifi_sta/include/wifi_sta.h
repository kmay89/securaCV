/**
 * @file wifi_sta.h
 * @brief WiFi Station Mode Helper for SecuraCV Canary
 *
 * Provides a simple interface for connecting to WiFi in station mode.
 * Uses ESP-IDF's native WiFi APIs with event-driven connection handling.
 *
 * This is a minimal implementation for OTA testing. Production firmware
 * should use the full WiFi provisioning system with captive portal support.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi connection status
 */
typedef enum {
    WIFI_STA_DISCONNECTED,      /**< Not connected to any AP */
    WIFI_STA_CONNECTING,        /**< Connection in progress */
    WIFI_STA_CONNECTED,         /**< Connected to AP with IP address */
    WIFI_STA_FAILED,            /**< Connection failed (wrong password, AP not found, etc.) */
} wifi_sta_status_t;

/**
 * @brief WiFi configuration
 */
typedef struct {
    const char *ssid;           /**< WiFi network name (max 32 chars) */
    const char *password;       /**< WiFi password (max 64 chars, NULL for open networks) */
    uint32_t timeout_ms;        /**< Connection timeout in milliseconds (0 = no timeout) */
} wifi_sta_config_t;

/**
 * @brief Initialize WiFi in station mode
 *
 * Must be called once before any other wifi_sta functions.
 * Initializes the WiFi driver and event handlers.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t wifi_sta_init(void);

/**
 * @brief Deinitialize WiFi
 *
 * Disconnects from any connected network and cleans up resources.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_sta_deinit(void);

/**
 * @brief Connect to a WiFi network
 *
 * Initiates connection to the specified WiFi network. This function
 * blocks until connected, failed, or timeout expires.
 *
 * @param config Connection configuration
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT if timed out,
 *         ESP_ERR_WIFI_... on other failures
 */
esp_err_t wifi_sta_connect(const wifi_sta_config_t *config);

/**
 * @brief Connect using credentials from NVS
 *
 * Reads SSID and password from NVS storage and attempts connection.
 * Falls back to Kconfig defaults if NVS values not found.
 *
 * @param timeout_ms Connection timeout in milliseconds (0 = no timeout)
 * @return ESP_OK if connected, error code on failure
 */
esp_err_t wifi_sta_connect_from_nvs(uint32_t timeout_ms);

/**
 * @brief Disconnect from WiFi
 *
 * Disconnects from the current network without deinitializing WiFi.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_sta_disconnect(void);

/**
 * @brief Get current WiFi status
 *
 * @return Current connection status
 */
wifi_sta_status_t wifi_sta_get_status(void);

/**
 * @brief Check if connected to WiFi
 *
 * @return true if connected with valid IP, false otherwise
 */
bool wifi_sta_is_connected(void);

/**
 * @brief Get assigned IP address
 *
 * @param ip_str Buffer to store IP address string (at least 16 bytes)
 * @return ESP_OK if connected and IP retrieved, ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t wifi_sta_get_ip(char *ip_str);

/**
 * @brief Get WiFi signal strength (RSSI)
 *
 * @param rssi Pointer to store RSSI value (dBm)
 * @return ESP_OK if connected, ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t wifi_sta_get_rssi(int8_t *rssi);

/**
 * @brief Save WiFi credentials to NVS
 *
 * @param ssid WiFi network name
 * @param password WiFi password (can be NULL for open networks)
 * @return ESP_OK on success
 */
esp_err_t wifi_sta_save_credentials(const char *ssid, const char *password);

/**
 * @brief Clear WiFi credentials from NVS
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_sta_clear_credentials(void);

#ifdef __cplusplus
}
#endif
