/**
 * @file hal_wifi.h
 * @brief HAL WiFi Interface
 *
 * Provides hardware-independent WiFi functionality.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// WIFI TYPES
// ============================================================================

typedef enum {
    WIFI_MODE_OFF = 0,
    WIFI_MODE_STA,          // Station (client)
    WIFI_MODE_AP,           // Access point
    WIFI_MODE_APSTA,        // Both AP and STA
} wifi_mode_t;

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA3_PSK,
} wifi_auth_t;

typedef enum {
    WIFI_EVENT_DISCONNECTED = 0,
    WIFI_EVENT_CONNECTING,
    WIFI_EVENT_CONNECTED,
    WIFI_EVENT_GOT_IP,
    WIFI_EVENT_LOST_IP,
    WIFI_EVENT_AP_START,
    WIFI_EVENT_AP_STOP,
    WIFI_EVENT_AP_CLIENT_CONNECTED,
    WIFI_EVENT_AP_CLIENT_DISCONNECTED,
} wifi_event_t;

typedef struct {
    char ssid[33];          // Max 32 chars + null
    char password[65];      // Max 64 chars + null
    wifi_auth_t auth;
    uint8_t channel;        // 0 for auto
    bool hidden;            // Hide SSID in AP mode
    uint8_t max_connections; // AP mode max clients
} wifi_config_t;

typedef struct {
    wifi_mode_t mode;
    bool sta_connected;
    bool ap_active;
    uint8_t sta_ip[4];
    uint8_t ap_ip[4];
    uint8_t sta_mac[6];
    uint8_t ap_mac[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t ap_clients;
} wifi_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_t auth;
    uint8_t channel;
    uint8_t bssid[6];
} wifi_scan_result_t;

/**
 * @brief WiFi event callback
 * @param event Event type
 * @param user_data User context
 */
typedef void (*wifi_event_cb_t)(wifi_event_t event, void* user_data);

// ============================================================================
// WIFI FUNCTIONS
// ============================================================================

/**
 * @brief Initialize WiFi subsystem
 * @param mode Initial mode
 * @return 0 on success, negative on error
 */
int hal_wifi_init(wifi_mode_t mode);

/**
 * @brief Deinitialize WiFi
 * @return 0 on success, negative on error
 */
int hal_wifi_deinit(void);

/**
 * @brief Set WiFi mode
 * @param mode New mode
 * @return 0 on success, negative on error
 */
int hal_wifi_set_mode(wifi_mode_t mode);

/**
 * @brief Connect to WiFi network (station mode)
 * @param config Network configuration
 * @return 0 on success (connection started), negative on error
 */
int hal_wifi_connect(const wifi_config_t* config);

/**
 * @brief Disconnect from WiFi network
 * @return 0 on success, negative on error
 */
int hal_wifi_disconnect(void);

/**
 * @brief Start access point
 * @param config AP configuration
 * @return 0 on success, negative on error
 */
int hal_wifi_start_ap(const wifi_config_t* config);

/**
 * @brief Stop access point
 * @return 0 on success, negative on error
 */
int hal_wifi_stop_ap(void);

/**
 * @brief Get WiFi status
 * @param status Output status structure
 * @return 0 on success, negative on error
 */
int hal_wifi_get_status(wifi_status_t* status);

/**
 * @brief Scan for WiFi networks
 * @param results Output array for results
 * @param max_results Maximum results to return
 * @param timeout_ms Scan timeout
 * @return Number of networks found, negative on error
 */
int hal_wifi_scan(wifi_scan_result_t* results, size_t max_results, uint32_t timeout_ms);

/**
 * @brief Register event callback
 * @param callback Callback function
 * @param user_data User context
 * @return 0 on success, negative on error
 */
int hal_wifi_on_event(wifi_event_cb_t callback, void* user_data);

/**
 * @brief Get WiFi signal strength (RSSI)
 * @return RSSI in dBm, or 0 if not connected
 */
int8_t hal_wifi_rssi(void);

/**
 * @brief Format IP address to string
 * @param ip IP bytes (4 bytes)
 * @param str Output string (at least 16 chars)
 */
void hal_wifi_ip_to_str(const uint8_t ip[4], char* str);

#ifdef __cplusplus
}
#endif
