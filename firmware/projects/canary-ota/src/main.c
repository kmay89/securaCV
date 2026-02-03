/**
 * @file main.c
 * @brief SecuraCV Canary OTA Demo Application
 *
 * This is the main entry point for the OTA update system demo.
 * It demonstrates:
 * - NVS initialization
 * - WiFi connection in station mode
 * - OTA self-test validation at boot
 * - OTA update check and install
 *
 * USAGE:
 * 1. Configure WiFi credentials in sdkconfig or NVS
 * 2. Build and flash: pio run -t upload
 * 3. Monitor: pio device monitor
 * 4. Run mock OTA server: python tools/mock_ota_server.py firmware.bin 1.1.0
 * 5. Device will check for updates on boot
 *
 * @author ERRERlabs
 * @copyright MIT License
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "securacv_ota.h"
#include "wifi_sta.h"

// ============================================================================
// LOGGING
// ============================================================================

static const char *TAG = "main";

// ============================================================================
// CONFIGURATION
// ============================================================================

// WiFi connection timeout (30 seconds)
#define WIFI_CONNECT_TIMEOUT_MS     30000

// ============================================================================
// TLS CERTIFICATE
// ============================================================================

/**
 * @brief Let's Encrypt Root CA Certificate (ISRG Root X1)
 *
 * This certificate is used to verify TLS connections to operacanary.com
 * and most HTTPS servers using Let's Encrypt certificates.
 *
 * For development with self-signed certificates, use SECURACV_OTA_SKIP_CERT_VERIFY.
 */
static const char server_root_ca_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

// ============================================================================
// SELF-TEST FUNCTIONS
// ============================================================================

/**
 * @brief Self-test: Check NVS is accessible
 */
static bool selftest_nvs(const char *name)
{
    ESP_LOGI(TAG, "Self-test: %s", name);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("test", NVS_READONLY, &nvs);

    // ESP_ERR_NVS_NOT_FOUND is OK - namespace doesn't exist yet
    if (err == ESP_OK) {
        nvs_close(nvs);
        return true;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;  // NVS works, just no data
    }

    ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
    return false;
}

/**
 * @brief Self-test: Check WiFi can be initialized
 */
static bool selftest_wifi(const char *name)
{
    ESP_LOGI(TAG, "Self-test: %s", name);
    // WiFi is already initialized if we got here
    return wifi_sta_is_connected();
}

/**
 * @brief Self-test: Check partition table is valid
 */
static bool selftest_partition(const char *name)
{
    ESP_LOGI(TAG, "Self-test: %s", name);

    // Check that we can find the OTA partitions
    const esp_partition_t *ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *ota1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);

    if (ota0 == NULL || ota1 == NULL) {
        ESP_LOGE(TAG, "OTA partitions not found");
        return false;
    }

    ESP_LOGI(TAG, "  ota_0: %s (0x%lx, %lu bytes)",
             ota0->label, (unsigned long)ota0->address, (unsigned long)ota0->size);
    ESP_LOGI(TAG, "  ota_1: %s (0x%lx, %lu bytes)",
             ota1->label, (unsigned long)ota1->address, (unsigned long)ota1->size);

    return true;
}

// ============================================================================
// OTA PROGRESS CALLBACK
// ============================================================================

/**
 * @brief OTA progress callback
 *
 * Called by the OTA engine to report progress.
 */
static void ota_progress_callback(securacv_ota_state_t state, uint8_t percent,
                                  securacv_ota_error_t error, void *user_data)
{
    (void)user_data;

    switch (state) {
        case SECURACV_OTA_CHECKING:
            ESP_LOGI(TAG, "OTA: Checking for updates...");
            break;

        case SECURACV_OTA_DOWNLOADING:
            // Only log every 10%
            if (percent % 10 == 0) {
                ESP_LOGI(TAG, "OTA: Downloading... %d%%", percent);
            }
            break;

        case SECURACV_OTA_VERIFYING:
            ESP_LOGI(TAG, "OTA: Verifying firmware...");
            break;

        case SECURACV_OTA_FLASHING:
            ESP_LOGI(TAG, "OTA: Writing to flash...");
            break;

        case SECURACV_OTA_REBOOTING:
            ESP_LOGI(TAG, "OTA: Complete! Rebooting...");
            break;

        case SECURACV_OTA_ERROR:
            ESP_LOGE(TAG, "OTA Error: %s", securacv_ota_error_str(error));
            break;

        case SECURACV_OTA_IDLE:
            break;
    }
}

// ============================================================================
// SYSTEM INFO
// ============================================================================

/**
 * @brief Print system information
 */
static void print_system_info(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "SecuraCV Canary OTA Demo");
    ESP_LOGI(TAG, "===========================================");

    // Chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: ESP32-S3, %d cores, WiFi%s%s",
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    // Flash size
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Flash: %lu MB %s", (unsigned long)(flash_size / (1024 * 1024)),
                 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "(embedded)" : "(external)");
    }

    // Partition info
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        ESP_LOGI(TAG, "Running from: %s (0x%lx)",
                 running->label, (unsigned long)running->address);
    }

    // App version
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "App version: %s", app_desc->version);
    ESP_LOGI(TAG, "OTA version: %s", securacv_ota_get_version());
    ESP_LOGI(TAG, "Compile time: %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, "IDF version: %s", app_desc->idf_ver);
    ESP_LOGI(TAG, "===========================================");
}

// ============================================================================
// MAIN APPLICATION
// ============================================================================

void app_main(void)
{
    esp_err_t err;

    // Print banner
    print_system_info();

    // ========================================================================
    // Step 1: Initialize NVS
    // ========================================================================
    ESP_LOGI(TAG, "Initializing NVS flash...");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // ========================================================================
    // Step 2: Initialize WiFi
    // ========================================================================
    ESP_LOGI(TAG, "Initializing WiFi...");

    err = wifi_sta_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return;
    }

    // Connect to WiFi (using NVS credentials or Kconfig defaults)
    ESP_LOGI(TAG, "Connecting to WiFi...");
    err = wifi_sta_connect_from_nvs(WIFI_CONNECT_TIMEOUT_MS);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Please configure WiFi SSID and password in sdkconfig");
        ESP_LOGE(TAG, "  pio run -t menuconfig -> WiFi Configuration");

        // Continue anyway for OTA self-test (it will fail gracefully)
    } else {
        char ip_str[16];
        wifi_sta_get_ip(ip_str);
        ESP_LOGI(TAG, "WiFi connected! IP: %s", ip_str);
    }

    // ========================================================================
    // Step 3: Initialize OTA and run boot self-test
    // ========================================================================
    ESP_LOGI(TAG, "Initializing OTA engine...");

    // Register self-test functions
    securacv_selftest_t test_nvs = {
        .name = "NVS storage",
        .fn = selftest_nvs,
        .required = true,
    };
    securacv_ota_register_selftest(&test_nvs);

    securacv_selftest_t test_partition = {
        .name = "Partition table",
        .fn = selftest_partition,
        .required = true,
    };
    securacv_ota_register_selftest(&test_partition);

    securacv_selftest_t test_wifi = {
        .name = "WiFi connectivity",
        .fn = selftest_wifi,
        .required = false,  // Don't rollback if WiFi fails
    };
    securacv_ota_register_selftest(&test_wifi);

    // Run boot self-test (validates OTA if we just updated)
    ESP_LOGI(TAG, "Running boot self-test...");
    err = securacv_ota_boot_self_test();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Boot self-test failed: %s", esp_err_to_name(err));
        // Don't return - continue to show status
    }

    // Configure OTA engine
    securacv_ota_config_t ota_config = SECURACV_OTA_CONFIG_DEFAULT;

#ifdef SECURACV_OTA_MANIFEST_URL
    ota_config.manifest_url = SECURACV_OTA_MANIFEST_URL;
#else
    ota_config.manifest_url = "https://operacanary.com/api/v1/firmware/manifest.json";
#endif

#ifndef SECURACV_OTA_SKIP_CERT_VERIFY
    ota_config.server_cert_pem = server_root_ca_pem;
#endif

    ota_config.on_progress = ota_progress_callback;
    ota_config.auto_reboot = true;

    err = securacv_ota_init(&ota_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA init failed: %s", esp_err_to_name(err));
        return;
    }

    // ========================================================================
    // Step 4: Check for updates
    // ========================================================================
    if (wifi_sta_is_connected()) {
        ESP_LOGI(TAG, "Checking for firmware updates...");
        err = securacv_ota_check_and_install();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA check failed to start: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Skipping OTA check - no WiFi connection");
    }

    // ========================================================================
    // Step 5: Main loop
    // ========================================================================
    ESP_LOGI(TAG, "Entering main loop...");
    ESP_LOGI(TAG, "Device is running. OTA updates will be checked in background.");

    uint32_t loop_count = 0;
    while (1) {
        // Log status every 30 seconds
        if (loop_count % 30 == 0) {
            securacv_ota_state_t ota_state = securacv_ota_get_state();
            ESP_LOGI(TAG, "Status: OTA=%s, WiFi=%s, heap=%lu",
                     securacv_ota_state_str(ota_state),
                     wifi_sta_is_connected() ? "connected" : "disconnected",
                     (unsigned long)esp_get_free_heap_size());
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        loop_count++;
    }
}
