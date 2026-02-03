/**
 * @file wifi_sta.c
 * @brief WiFi Station Mode Helper Implementation
 *
 * Implements WiFi station mode connectivity using ESP-IDF APIs.
 * Uses event groups for synchronous connection waiting.
 */

#include "wifi_sta.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

// ============================================================================
// LOGGING
// ============================================================================

static const char *TAG = "wifi_sta";

// ============================================================================
// CONSTANTS
// ============================================================================

#define NVS_NAMESPACE           "wifi"
#define NVS_KEY_SSID            "ssid"
#define NVS_KEY_PASSWORD        "password"

// Event group bits
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1

// Default retry count
#define WIFI_MAX_RETRY          5

// ============================================================================
// INTERNAL STATE
// ============================================================================

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static wifi_sta_status_t s_status = WIFI_STA_DISCONNECTED;
static int s_retry_count = 0;
static bool s_initialized = false;

// ============================================================================
// EVENT HANDLERS
// ============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                s_status = WIFI_STA_DISCONNECTED;
                if (s_retry_count < WIFI_MAX_RETRY) {
                    ESP_LOGI(TAG, "Retry connection (%d/%d)", s_retry_count + 1, WIFI_MAX_RETRY);
                    esp_wifi_connect();
                    s_retry_count++;
                } else {
                    ESP_LOGE(TAG, "Connection failed after %d retries", WIFI_MAX_RETRY);
                    s_status = WIFI_STA_FAILED;
                    if (s_wifi_event_group) {
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }
                }
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Connected to AP");
                s_status = WIFI_STA_CONNECTING;  // Still waiting for IP
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
                s_status = WIFI_STA_CONNECTED;
                s_retry_count = 0;
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                }
                break;
            }

            case IP_EVENT_STA_LOST_IP:
                ESP_LOGW(TAG, "Lost IP address");
                s_status = WIFI_STA_DISCONNECTED;
                break;

            default:
                break;
        }
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

esp_err_t wifi_sta_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi already initialized");
        return ESP_OK;
    }

    esp_err_t err;

    // Create event group
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Initialize TCP/IP stack
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create default event loop if not already created
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create default WiFi station interface
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA");
        return ESP_FAIL;
    }

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Register event handlers
    err = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        NULL
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler");
        return err;
    }

    err = esp_event_handler_instance_register(
        IP_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        NULL
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler");
        return err;
    }

    // Set WiFi mode to station
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    s_status = WIFI_STA_DISCONNECTED;

    ESP_LOGI(TAG, "WiFi station mode initialized");
    return ESP_OK;
}

esp_err_t wifi_sta_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    wifi_sta_disconnect();

    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_initialized = false;
    s_status = WIFI_STA_DISCONNECTED;

    ESP_LOGI(TAG, "WiFi deinitialized");
    return ESP_OK;
}

esp_err_t wifi_sta_connect(const wifi_sta_config_t *config)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL || config->ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", config->ssid);

    // Configure WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid) - 1);

    if (config->password != NULL) {
        strncpy((char *)wifi_config.sta.password, config->password,
                sizeof(wifi_config.sta.password) - 1);
    }

    // Set security threshold
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (config->password == NULL || strlen(config->password) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    // PMF configuration
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Clear event bits and retry counter
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;
    s_status = WIFI_STA_CONNECTING;

    // Start WiFi
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // Wait for connection or failure
    TickType_t timeout = portMAX_DELAY;
    if (config->timeout_ms > 0) {
        timeout = pdMS_TO_TICKS(config->timeout_ms);
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        timeout
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s", config->ssid);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to %s", config->ssid);
        s_status = WIFI_STA_FAILED;
        return ESP_ERR_WIFI_NOT_CONNECT;
    } else {
        ESP_LOGE(TAG, "Connection timeout");
        s_status = WIFI_STA_FAILED;
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t wifi_sta_connect_from_nvs(uint32_t timeout_ms)
{
    char ssid[33] = {0};
    char password[65] = {0};
    bool use_defaults = false;

    // Try to read from NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);

    if (err == ESP_OK) {
        size_t ssid_len = sizeof(ssid);
        size_t pass_len = sizeof(password);

        err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
        if (err == ESP_OK && ssid_len > 1) {
            nvs_get_str(nvs, NVS_KEY_PASSWORD, password, &pass_len);
            ESP_LOGI(TAG, "Using WiFi credentials from NVS");
        } else {
            use_defaults = true;
        }

        nvs_close(nvs);
    } else {
        use_defaults = true;
    }

    // Fall back to Kconfig defaults
    if (use_defaults) {
        ESP_LOGI(TAG, "Using WiFi credentials from Kconfig");
#ifdef CONFIG_ESP_WIFI_SSID
        strncpy(ssid, CONFIG_ESP_WIFI_SSID, sizeof(ssid) - 1);
#endif
#ifdef CONFIG_ESP_WIFI_PASSWORD
        strncpy(password, CONFIG_ESP_WIFI_PASSWORD, sizeof(password) - 1);
#endif
    }

    if (strlen(ssid) == 0) {
        ESP_LOGE(TAG, "No WiFi SSID configured");
        return ESP_ERR_NOT_FOUND;
    }

    wifi_sta_config_t config = {
        .ssid = ssid,
        .password = (strlen(password) > 0) ? password : NULL,
        .timeout_ms = timeout_ms,
    };

    return wifi_sta_connect(&config);
}

esp_err_t wifi_sta_disconnect(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
    }

    s_status = WIFI_STA_DISCONNECTED;
    return ESP_OK;
}

wifi_sta_status_t wifi_sta_get_status(void)
{
    return s_status;
}

bool wifi_sta_is_connected(void)
{
    return s_status == WIFI_STA_CONNECTED;
}

esp_err_t wifi_sta_get_ip(char *ip_str)
{
    if (ip_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_sta_is_connected() || s_sta_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }

    sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t wifi_sta_get_rssi(int8_t *rssi)
{
    if (rssi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_sta_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        return err;
    }

    *rssi = ap_info.rssi;
    return ESP_OK;
}

esp_err_t wifi_sta_save_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    if (password != NULL) {
        err = nvs_set_str(nvs, NVS_KEY_PASSWORD, password);
        if (err != ESP_OK) {
            nvs_close(nvs);
            return err;
        }
    } else {
        nvs_erase_key(nvs, NVS_KEY_PASSWORD);
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    return err;
}

esp_err_t wifi_sta_clear_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASSWORD);
    err = nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "WiFi credentials cleared from NVS");
    return err;
}
