/**
 * @file securacv_ota.c
 * @brief SecuraCV Canary OTA Update Engine Implementation
 *
 * This file implements the OTA update engine for the SecuraCV Canary device.
 * It uses ESP-IDF's esp_https_ota APIs for secure firmware downloads with
 * SHA256 verification and automatic rollback support.
 *
 * ARCHITECTURE:
 * - OTA operations run in a dedicated FreeRTOS task to avoid blocking
 * - State machine drives the update process
 * - Progress is reported via user-registered callback
 * - Self-test validation runs at boot to confirm OTA success
 *
 * SECURITY:
 * - All HTTP communication is over TLS (HTTPS)
 * - Server certificate verification (configurable CA)
 * - SHA256 hash verification of downloaded firmware
 * - ESP-IDF rollback protection prevents bricked devices
 */

#include "securacv_ota.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

// ============================================================================
// LOGGING
// ============================================================================

static const char *TAG = "securacv_ota";

// ============================================================================
// CONSTANTS
// ============================================================================

#define OTA_TASK_STACK_SIZE     8192
#define OTA_TASK_PRIORITY       (tskIDLE_PRIORITY + 2)
#define OTA_TASK_NAME           "ota_task"

#define MAX_SELF_TESTS          16
#define HTTP_BUFFER_SIZE        1024
#define SHA256_DIGEST_LENGTH    32

// ============================================================================
// INTERNAL STATE
// ============================================================================

/**
 * @brief Internal OTA engine state
 */
typedef struct {
    // Configuration (copied from init)
    securacv_ota_config_t config;

    // Runtime state
    securacv_ota_state_t state;
    securacv_ota_error_t last_error;
    uint8_t progress_percent;

    // Manifest from last check
    securacv_ota_manifest_t manifest;
    bool manifest_valid;
    bool update_available;

    // Task management
    TaskHandle_t task_handle;
    SemaphoreHandle_t mutex;
    bool task_should_abort;

    // Self-tests
    securacv_selftest_t self_tests[MAX_SELF_TESTS];
    size_t num_self_tests;

    // Initialization flag
    bool initialized;

    // Operation mode (check only or check+install)
    bool install_mode;
} ota_context_t;

static ota_context_t s_ctx = {0};

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void ota_task(void *arg);
static esp_err_t ota_fetch_manifest(void);
static esp_err_t ota_parse_manifest(const char *json_data);
static esp_err_t ota_download_and_flash(void);
static esp_err_t ota_verify_sha256(const esp_partition_t *partition, size_t image_size, const char *expected_hex);
static void ota_set_state(securacv_ota_state_t state);
static void ota_set_error(securacv_ota_error_t error);
static void ota_report_progress(uint8_t percent);
static bool parse_version(const char *version_str, int *major, int *minor, int *patch);
static void hex_to_bytes(const char *hex, uint8_t *bytes, size_t byte_len);

// ============================================================================
// PUBLIC API - INITIALIZATION
// ============================================================================

esp_err_t securacv_ota_init(const securacv_ota_config_t *config)
{
    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "OTA already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL || config->manifest_url == NULL) {
        ESP_LOGE(TAG, "Invalid configuration: manifest_url required");
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize context
    memset(&s_ctx, 0, sizeof(s_ctx));

    // Copy configuration
    s_ctx.config = *config;

    // Set defaults for optional fields
    if (s_ctx.config.http_timeout_ms == 0) {
        s_ctx.config.http_timeout_ms = 30000;
    }
    if (s_ctx.config.download_buffer_size == 0) {
        s_ctx.config.download_buffer_size = 4096;
    }

    // Create mutex for thread-safe state access
    s_ctx.mutex = xSemaphoreCreateMutex();
    if (s_ctx.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_ctx.state = SECURACV_OTA_IDLE;
    s_ctx.initialized = true;

    ESP_LOGI(TAG, "OTA engine initialized");
    ESP_LOGI(TAG, "  Manifest URL: %s", config->manifest_url);
    ESP_LOGI(TAG, "  Running version: %s", SECURACV_FW_VERSION_STRING);

    return ESP_OK;
}

esp_err_t securacv_ota_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_OK;
    }

    // Abort any running operation
    securacv_ota_abort();

    // Wait for task to stop
    if (s_ctx.task_handle != NULL) {
        // Give task time to clean up
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Delete mutex
    if (s_ctx.mutex != NULL) {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
    }

    s_ctx.initialized = false;
    ESP_LOGI(TAG, "OTA engine deinitialized");

    return ESP_OK;
}

// ============================================================================
// PUBLIC API - OTA OPERATIONS
// ============================================================================

esp_err_t securacv_ota_check(void)
{
    if (!s_ctx.initialized) {
        ESP_LOGE(TAG, "OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_ctx.state != SECURACV_OTA_IDLE) {
        xSemaphoreGive(s_ctx.mutex);
        ESP_LOGW(TAG, "OTA operation already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.install_mode = false;
    s_ctx.task_should_abort = false;

    xSemaphoreGive(s_ctx.mutex);

    // Create OTA task
    BaseType_t ret = xTaskCreate(
        ota_task,
        OTA_TASK_NAME,
        OTA_TASK_STACK_SIZE,
        NULL,
        OTA_TASK_PRIORITY,
        &s_ctx.task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OTA check started");
    return ESP_OK;
}

esp_err_t securacv_ota_check_and_install(void)
{
    if (!s_ctx.initialized) {
        ESP_LOGE(TAG, "OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_ctx.state != SECURACV_OTA_IDLE) {
        xSemaphoreGive(s_ctx.mutex);
        ESP_LOGW(TAG, "OTA operation already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.install_mode = true;
    s_ctx.task_should_abort = false;

    xSemaphoreGive(s_ctx.mutex);

    // Create OTA task
    BaseType_t ret = xTaskCreate(
        ota_task,
        OTA_TASK_NAME,
        OTA_TASK_STACK_SIZE,
        NULL,
        OTA_TASK_PRIORITY,
        &s_ctx.task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OTA check and install started");
    return ESP_OK;
}

esp_err_t securacv_ota_abort(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_ctx.state == SECURACV_OTA_IDLE) {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_OK;
    }

    s_ctx.task_should_abort = true;
    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "OTA abort requested");
    return ESP_OK;
}

// ============================================================================
// PUBLIC API - SELF-TEST
// ============================================================================

esp_err_t securacv_ota_register_selftest(const securacv_selftest_t *test)
{
    if (test == NULL || test->fn == NULL || test->name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ctx.num_self_tests >= MAX_SELF_TESTS) {
        ESP_LOGE(TAG, "Maximum self-tests reached (%d)", MAX_SELF_TESTS);
        return ESP_ERR_NO_MEM;
    }

    s_ctx.self_tests[s_ctx.num_self_tests] = *test;
    s_ctx.num_self_tests++;

    ESP_LOGI(TAG, "Registered self-test: %s (required=%d)", test->name, test->required);
    return ESP_OK;
}

esp_err_t securacv_ota_boot_self_test(void)
{
    ESP_LOGI(TAG, "Running boot self-test validation...");

    // Check if we need to validate an OTA update
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not get OTA state: %s", esp_err_to_name(err));
        // Not an OTA partition or error - continue normally
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Running partition: %s, OTA state: %d", running->label, ota_state);

    // Check if this is a pending validation (new OTA image)
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New OTA firmware pending validation");

        bool all_passed = true;
        bool any_required_failed = false;

        // Run all registered self-tests
        for (size_t i = 0; i < s_ctx.num_self_tests; i++) {
            const securacv_selftest_t *test = &s_ctx.self_tests[i];
            ESP_LOGI(TAG, "Running self-test: %s", test->name);

            bool passed = test->fn(test->name);

            if (passed) {
                ESP_LOGI(TAG, "  PASSED: %s", test->name);
            } else {
                ESP_LOGE(TAG, "  FAILED: %s (required=%d)", test->name, test->required);
                all_passed = false;
                if (test->required) {
                    any_required_failed = true;
                }
            }
        }

        if (any_required_failed) {
            ESP_LOGE(TAG, "Required self-test(s) failed - initiating rollback!");

            // Mark the app as invalid and trigger rollback
            esp_ota_mark_app_invalid_rollback_and_reboot();

            // Should not reach here - device will reboot
            return ESP_ERR_OTA_ROLLBACK_FAILED;
        }

        if (!all_passed) {
            ESP_LOGW(TAG, "Some optional self-tests failed, but continuing");
        }

        // All required tests passed - mark app as valid
        ESP_LOGI(TAG, "Self-tests passed - marking firmware as valid");
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "OTA validation complete - firmware confirmed");
    } else if (ota_state == ESP_OTA_IMG_VALID) {
        ESP_LOGI(TAG, "Firmware already validated");
    } else {
        ESP_LOGI(TAG, "No pending OTA validation needed");
    }

    return ESP_OK;
}

// ============================================================================
// PUBLIC API - STATE QUERIES
// ============================================================================

securacv_ota_state_t securacv_ota_get_state(void)
{
    if (!s_ctx.initialized || s_ctx.mutex == NULL) {
        return SECURACV_OTA_IDLE;
    }

    securacv_ota_state_t state;

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        state = s_ctx.state;
        xSemaphoreGive(s_ctx.mutex);
    } else {
        state = s_ctx.state;  // Best effort without lock
    }

    return state;
}

securacv_ota_error_t securacv_ota_get_last_error(void)
{
    return s_ctx.last_error;
}

const char *securacv_ota_get_version(void)
{
    return SECURACV_FW_VERSION_STRING;
}

const securacv_ota_manifest_t *securacv_ota_get_manifest(void)
{
    if (!s_ctx.manifest_valid) {
        return NULL;
    }
    return &s_ctx.manifest;
}

bool securacv_ota_update_available(void)
{
    return s_ctx.manifest_valid && s_ctx.update_available;
}

uint8_t securacv_ota_get_progress(void)
{
    return s_ctx.progress_percent;
}

// ============================================================================
// PUBLIC API - UTILITIES
// ============================================================================

int securacv_version_compare(const char *a, const char *b)
{
    int a_major, a_minor, a_patch;
    int b_major, b_minor, b_patch;

    if (!parse_version(a, &a_major, &a_minor, &a_patch)) {
        return 0;
    }
    if (!parse_version(b, &b_major, &b_minor, &b_patch)) {
        return 0;
    }

    if (a_major != b_major) return (a_major > b_major) ? 1 : -1;
    if (a_minor != b_minor) return (a_minor > b_minor) ? 1 : -1;
    if (a_patch != b_patch) return (a_patch > b_patch) ? 1 : -1;

    return 0;
}

const char *securacv_ota_error_str(securacv_ota_error_t error)
{
    switch (error) {
        case SECURACV_OTA_ERR_NONE:             return "No error";
        case SECURACV_OTA_ERR_NETWORK:          return "Network error";
        case SECURACV_OTA_ERR_MANIFEST_FETCH:   return "Failed to fetch manifest";
        case SECURACV_OTA_ERR_MANIFEST_PARSE:   return "Failed to parse manifest";
        case SECURACV_OTA_ERR_MANIFEST_INVALID: return "Invalid manifest";
        case SECURACV_OTA_ERR_NO_UPDATE:        return "No update available";
        case SECURACV_OTA_ERR_DOWNLOAD_FAILED:  return "Download failed";
        case SECURACV_OTA_ERR_SHA256_MISMATCH:  return "SHA256 verification failed";
        case SECURACV_OTA_ERR_SIGNATURE_INVALID: return "Signature verification failed";
        case SECURACV_OTA_ERR_FLASH_WRITE:      return "Flash write failed";
        case SECURACV_OTA_ERR_FLASH_READ:       return "Flash read failed";
        case SECURACV_OTA_ERR_PARTITION:        return "Partition error";
        case SECURACV_OTA_ERR_VERSION_ROLLBACK: return "Version rollback rejected";
        case SECURACV_OTA_ERR_SELF_TEST_FAILED: return "Self-test failed";
        case SECURACV_OTA_ERR_ALREADY_RUNNING:  return "OTA already running";
        case SECURACV_OTA_ERR_NOT_INITIALIZED:  return "OTA not initialized";
        case SECURACV_OTA_ERR_OUT_OF_MEMORY:    return "Out of memory";
        default:                                return "Unknown error";
    }
}

const char *securacv_ota_state_str(securacv_ota_state_t state)
{
    switch (state) {
        case SECURACV_OTA_IDLE:        return "Idle";
        case SECURACV_OTA_CHECKING:    return "Checking";
        case SECURACV_OTA_DOWNLOADING: return "Downloading";
        case SECURACV_OTA_VERIFYING:   return "Verifying";
        case SECURACV_OTA_FLASHING:    return "Flashing";
        case SECURACV_OTA_REBOOTING:   return "Rebooting";
        case SECURACV_OTA_ERROR:       return "Error";
        default:                       return "Unknown";
    }
}

// ============================================================================
// INTERNAL - OTA TASK
// ============================================================================

/**
 * @brief Main OTA task function
 *
 * This task handles the full OTA workflow:
 * 1. Fetch manifest
 * 2. Parse and validate manifest
 * 3. Compare versions
 * 4. Download firmware (if install_mode)
 * 5. Verify SHA256
 * 6. Flash to OTA partition
 * 7. Reboot (if auto_reboot)
 */
static void ota_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "OTA task started");

    // Phase 1: Fetch manifest
    ota_set_state(SECURACV_OTA_CHECKING);

    esp_err_t err = ota_fetch_manifest();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Manifest fetch failed");
        goto task_exit;
    }

    // Check if abort was requested
    if (s_ctx.task_should_abort) {
        ESP_LOGI(TAG, "OTA aborted by user");
        ota_set_state(SECURACV_OTA_IDLE);
        goto task_exit;
    }

    // Check version comparison
    int cmp = securacv_version_compare(s_ctx.manifest.version, SECURACV_FW_VERSION_STRING);

    if (cmp > 0) {
        ESP_LOGI(TAG, "Update available: %s -> %s",
                 SECURACV_FW_VERSION_STRING, s_ctx.manifest.version);
        s_ctx.update_available = true;
    } else if (cmp == 0) {
        ESP_LOGI(TAG, "Already running latest version: %s", SECURACV_FW_VERSION_STRING);
        s_ctx.update_available = false;
        if (!s_ctx.config.skip_version_check) {
            ota_set_error(SECURACV_OTA_ERR_NO_UPDATE);
            goto task_exit;
        }
    } else {
        ESP_LOGW(TAG, "Server has older version: %s < %s",
                 s_ctx.manifest.version, SECURACV_FW_VERSION_STRING);
        s_ctx.update_available = false;
        ota_set_error(SECURACV_OTA_ERR_NO_UPDATE);
        goto task_exit;
    }

    // Check minimum version requirement
    if (strlen(s_ctx.manifest.min_version) > 0) {
        int min_cmp = securacv_version_compare(SECURACV_FW_VERSION_STRING, s_ctx.manifest.min_version);
        if (min_cmp < 0) {
            ESP_LOGE(TAG, "Running version %s is below minimum %s",
                     SECURACV_FW_VERSION_STRING, s_ctx.manifest.min_version);
            // Still allow update - we need to upgrade!
        }
    }

    // If check-only mode, we're done
    if (!s_ctx.install_mode) {
        ESP_LOGI(TAG, "Check complete - install not requested");
        ota_set_state(SECURACV_OTA_IDLE);
        goto task_exit;
    }

    // Phase 2: Download and flash
    err = ota_download_and_flash();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Download/flash failed");
        goto task_exit;
    }

    // Phase 3: Reboot (if auto_reboot)
    if (s_ctx.config.auto_reboot) {
        ota_set_state(SECURACV_OTA_REBOOTING);
        ESP_LOGI(TAG, "OTA complete - rebooting in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        ESP_LOGI(TAG, "OTA complete - reboot required");
        ota_set_state(SECURACV_OTA_IDLE);
    }

task_exit:
    s_ctx.task_handle = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
// INTERNAL - MANIFEST FETCH AND PARSE
// ============================================================================

/**
 * @brief HTTP event handler for manifest fetch
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer = NULL;
    static int output_len = 0;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // Accumulate response data
            if (output_buffer == NULL) {
                output_buffer = (char *)malloc(HTTP_BUFFER_SIZE);
                if (output_buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate HTTP buffer");
                    return ESP_ERR_NO_MEM;
                }
                output_len = 0;
            }

            // Copy data if there's space
            int copy_len = evt->data_len;
            if (output_len + copy_len < HTTP_BUFFER_SIZE - 1) {
                memcpy(output_buffer + output_len, evt->data, copy_len);
                output_len += copy_len;
                output_buffer[output_len] = '\0';
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            if (output_buffer != NULL) {
                // Store response in user_data
                char **response = (char **)evt->user_data;
                if (response != NULL && *response == NULL) {
                    *response = output_buffer;
                    output_buffer = NULL;
                } else {
                    free(output_buffer);
                    output_buffer = NULL;
                }
                output_len = 0;
            }
            break;

        case HTTP_EVENT_DISCONNECTED:
            if (output_buffer != NULL) {
                free(output_buffer);
                output_buffer = NULL;
                output_len = 0;
            }
            break;

        default:
            break;
    }

    return ESP_OK;
}

/**
 * @brief Fetch manifest JSON from server
 */
static esp_err_t ota_fetch_manifest(void)
{
    char *response = NULL;
    esp_err_t err = ESP_FAIL;

    ESP_LOGI(TAG, "Fetching manifest from: %s", s_ctx.config.manifest_url);

    esp_http_client_config_t http_config = {
        .url = s_ctx.config.manifest_url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = s_ctx.config.http_timeout_ms,
        .disable_auto_redirect = false,
    };

    // Add certificate if provided
    if (s_ctx.config.server_cert_pem != NULL) {
        http_config.cert_pem = s_ctx.config.server_cert_pem;
    }

#ifdef SECURACV_OTA_SKIP_CERT_VERIFY
    http_config.skip_cert_common_name_check = true;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        ota_set_error(SECURACV_OTA_ERR_NETWORK);
        return ESP_FAIL;
    }

    err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status: %d", status);

        if (status == 200 && response != NULL) {
            ESP_LOGD(TAG, "Manifest response: %s", response);
            err = ota_parse_manifest(response);
        } else {
            ESP_LOGE(TAG, "HTTP request failed: status=%d", status);
            ota_set_error(SECURACV_OTA_ERR_MANIFEST_FETCH);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        ota_set_error(SECURACV_OTA_ERR_NETWORK);
    }

    if (response != NULL) {
        free(response);
    }

    esp_http_client_cleanup(client);
    return err;
}

/**
 * @brief Parse manifest JSON
 */
static esp_err_t ota_parse_manifest(const char *json_data)
{
    cJSON *root = cJSON_Parse(json_data);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse manifest JSON");
        ota_set_error(SECURACV_OTA_ERR_MANIFEST_PARSE);
        return ESP_FAIL;
    }

    // Reset manifest
    memset(&s_ctx.manifest, 0, sizeof(s_ctx.manifest));
    s_ctx.manifest_valid = false;

    // Required fields
    cJSON *product = cJSON_GetObjectItem(root, "product");
    cJSON *version = cJSON_GetObjectItem(root, "version");
    cJSON *url = cJSON_GetObjectItem(root, "url");
    cJSON *sha256 = cJSON_GetObjectItem(root, "sha256");

    if (!cJSON_IsString(product) || !cJSON_IsString(version) ||
        !cJSON_IsString(url) || !cJSON_IsString(sha256)) {
        ESP_LOGE(TAG, "Manifest missing required fields");
        cJSON_Delete(root);
        ota_set_error(SECURACV_OTA_ERR_MANIFEST_INVALID);
        return ESP_FAIL;
    }

    // Verify product matches
#ifdef SECURACV_DEVICE_PRODUCT
    if (strcmp(product->valuestring, SECURACV_DEVICE_PRODUCT) != 0) {
        ESP_LOGE(TAG, "Product mismatch: expected %s, got %s",
                 SECURACV_DEVICE_PRODUCT, product->valuestring);
        cJSON_Delete(root);
        ota_set_error(SECURACV_OTA_ERR_MANIFEST_INVALID);
        return ESP_FAIL;
    }
#endif

    // Copy required fields
    strncpy(s_ctx.manifest.product, product->valuestring, sizeof(s_ctx.manifest.product) - 1);
    strncpy(s_ctx.manifest.version, version->valuestring, sizeof(s_ctx.manifest.version) - 1);
    strncpy(s_ctx.manifest.url, url->valuestring, sizeof(s_ctx.manifest.url) - 1);
    strncpy(s_ctx.manifest.sha256, sha256->valuestring, sizeof(s_ctx.manifest.sha256) - 1);

    // Optional fields
    cJSON *min_version = cJSON_GetObjectItem(root, "min_version");
    if (cJSON_IsString(min_version)) {
        strncpy(s_ctx.manifest.min_version, min_version->valuestring,
                sizeof(s_ctx.manifest.min_version) - 1);
    }

    cJSON *size = cJSON_GetObjectItem(root, "size");
    if (cJSON_IsNumber(size)) {
        s_ctx.manifest.size = (uint32_t)size->valuedouble;
    }

    cJSON *release_notes = cJSON_GetObjectItem(root, "release_notes");
    if (cJSON_IsString(release_notes)) {
        strncpy(s_ctx.manifest.release_notes, release_notes->valuestring,
                sizeof(s_ctx.manifest.release_notes) - 1);
    }

    cJSON *release_url = cJSON_GetObjectItem(root, "release_url");
    if (cJSON_IsString(release_url)) {
        strncpy(s_ctx.manifest.release_url, release_url->valuestring,
                sizeof(s_ctx.manifest.release_url) - 1);
    }

    cJSON_Delete(root);

    s_ctx.manifest_valid = true;

    ESP_LOGI(TAG, "Manifest parsed successfully:");
    ESP_LOGI(TAG, "  Product: %s", s_ctx.manifest.product);
    ESP_LOGI(TAG, "  Version: %s", s_ctx.manifest.version);
    ESP_LOGI(TAG, "  Size: %lu bytes", (unsigned long)s_ctx.manifest.size);

    return ESP_OK;
}

// ============================================================================
// INTERNAL - DOWNLOAD AND FLASH
// ============================================================================

/**
 * @brief Download firmware and flash to OTA partition
 */
static esp_err_t ota_download_and_flash(void)
{
    esp_err_t err;
    esp_https_ota_handle_t https_ota_handle = NULL;
    size_t image_size = 0;

    ota_set_state(SECURACV_OTA_DOWNLOADING);
    ota_report_progress(0);

    ESP_LOGI(TAG, "Downloading firmware from: %s", s_ctx.manifest.url);

    // Configure HTTP client
    esp_http_client_config_t http_config = {
        .url = s_ctx.manifest.url,
        .timeout_ms = s_ctx.config.http_timeout_ms,
        .buffer_size = s_ctx.config.download_buffer_size,
        .buffer_size_tx = 1024,
    };

    if (s_ctx.config.server_cert_pem != NULL) {
        http_config.cert_pem = s_ctx.config.server_cert_pem;
    }

#ifdef SECURACV_OTA_SKIP_CERT_VERIFY
    http_config.skip_cert_common_name_check = true;
#endif

    // Configure OTA
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .partial_http_download = true,
        .max_http_request_size = s_ctx.config.download_buffer_size,
    };

    // Begin OTA
    err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        ota_set_error(SECURACV_OTA_ERR_DOWNLOAD_FAILED);
        return err;
    }

    // Get image info
    esp_app_desc_t new_app_info;
    err = esp_https_ota_get_img_desc(https_ota_handle, &new_app_info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "New firmware: %s (version %s)",
                 new_app_info.project_name, new_app_info.version);
    }

    // Get total size for progress calculation
    int content_length = esp_https_ota_get_image_len_read(https_ota_handle);
    int total_size = (s_ctx.manifest.size > 0) ? s_ctx.manifest.size : 1;  // Avoid div by zero

    // Download loop
    int bytes_read = 0;
    int last_progress = 0;

    while (1) {
        // Check for abort request
        if (s_ctx.task_should_abort) {
            ESP_LOGI(TAG, "Download aborted by user");
            esp_https_ota_abort(https_ota_handle);
            ota_set_state(SECURACV_OTA_IDLE);
            return ESP_ERR_TIMEOUT;
        }

        err = esp_https_ota_perform(https_ota_handle);

        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            // Update progress
            bytes_read = esp_https_ota_get_image_len_read(https_ota_handle);
            int progress = (bytes_read * 100) / total_size;
            if (progress > 100) progress = 100;

            if (progress != last_progress) {
                ota_report_progress((uint8_t)progress);
                last_progress = progress;

                if (progress % 10 == 0) {
                    ESP_LOGI(TAG, "Download progress: %d%% (%d/%d bytes)",
                             progress, bytes_read, total_size);
                }
            }
            continue;
        }

        if (err == ESP_OK) {
            // Download complete
            break;
        }

        // Error occurred
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
        ota_set_error(SECURACV_OTA_ERR_DOWNLOAD_FAILED);
        return err;
    }

    // Get final image size
    image_size = esp_https_ota_get_image_len_read(https_ota_handle);
    ESP_LOGI(TAG, "Download complete: %zu bytes", image_size);

    // Verify before finishing
    ota_set_state(SECURACV_OTA_VERIFYING);

    // Get the OTA partition that was written to
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        esp_https_ota_abort(https_ota_handle);
        ota_set_error(SECURACV_OTA_ERR_PARTITION);
        return ESP_FAIL;
    }

    // Verify SHA256
    err = ota_verify_sha256(update_partition, image_size, s_ctx.manifest.sha256);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHA256 verification failed!");
        esp_https_ota_abort(https_ota_handle);
        ota_set_error(SECURACV_OTA_ERR_SHA256_MISMATCH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SHA256 verification passed");

    // Finish OTA (commit the update)
    ota_set_state(SECURACV_OTA_FLASHING);

    err = esp_https_ota_finish(https_ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "OTA image validation failed");
        }
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        ota_set_error(SECURACV_OTA_ERR_FLASH_WRITE);
        return err;
    }

    ESP_LOGI(TAG, "OTA update written successfully");
    return ESP_OK;
}

/**
 * @brief Verify SHA256 hash of downloaded firmware
 */
static esp_err_t ota_verify_sha256(const esp_partition_t *partition, size_t image_size, const char *expected_hex)
{
    ESP_LOGI(TAG, "Verifying SHA256...");

    // Convert expected hex to bytes
    if (strlen(expected_hex) != 64) {
        ESP_LOGE(TAG, "Invalid SHA256 hex length: %zu", strlen(expected_hex));
        return ESP_FAIL;
    }

    uint8_t expected[SHA256_DIGEST_LENGTH];
    hex_to_bytes(expected_hex, expected, SHA256_DIGEST_LENGTH);

    // Calculate SHA256 of flash content
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256, not SHA-224

    // Read and hash in chunks
    const size_t chunk_size = 4096;
    uint8_t *buffer = (uint8_t *)malloc(chunk_size);
    if (buffer == NULL) {
        mbedtls_sha256_free(&ctx);
        return ESP_ERR_NO_MEM;
    }

    size_t remaining = image_size;
    size_t offset = 0;

    while (remaining > 0) {
        size_t to_read = (remaining < chunk_size) ? remaining : chunk_size;

        esp_err_t err = esp_partition_read(partition, offset, buffer, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read partition at offset %zu: %s",
                     offset, esp_err_to_name(err));
            free(buffer);
            mbedtls_sha256_free(&ctx);
            return err;
        }

        mbedtls_sha256_update(&ctx, buffer, to_read);

        offset += to_read;
        remaining -= to_read;
    }

    free(buffer);

    uint8_t computed[SHA256_DIGEST_LENGTH];
    mbedtls_sha256_finish(&ctx, computed);
    mbedtls_sha256_free(&ctx);

    // Compare hashes
    if (memcmp(computed, expected, SHA256_DIGEST_LENGTH) != 0) {
        ESP_LOGE(TAG, "SHA256 mismatch!");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, expected, SHA256_DIGEST_LENGTH, ESP_LOG_ERROR);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, computed, SHA256_DIGEST_LENGTH, ESP_LOG_ERROR);
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ============================================================================
// INTERNAL - STATE MANAGEMENT
// ============================================================================

static void ota_set_state(securacv_ota_state_t state)
{
    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_ctx.state = state;
        xSemaphoreGive(s_ctx.mutex);
    }

    ESP_LOGI(TAG, "OTA state: %s", securacv_ota_state_str(state));

    // Notify callback
    if (s_ctx.config.on_progress != NULL) {
        s_ctx.config.on_progress(state, s_ctx.progress_percent, s_ctx.last_error,
                                 s_ctx.config.user_data);
    }
}

static void ota_set_error(securacv_ota_error_t error)
{
    s_ctx.last_error = error;

    ESP_LOGE(TAG, "OTA error: %s", securacv_ota_error_str(error));

    ota_set_state(SECURACV_OTA_ERROR);

    // Return to idle after error
    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_ctx.state = SECURACV_OTA_IDLE;
        xSemaphoreGive(s_ctx.mutex);
    }
}

static void ota_report_progress(uint8_t percent)
{
    s_ctx.progress_percent = percent;

    // Notify callback (don't spam on every byte)
    if (s_ctx.config.on_progress != NULL) {
        s_ctx.config.on_progress(s_ctx.state, percent, SECURACV_OTA_ERR_NONE,
                                 s_ctx.config.user_data);
    }
}

// ============================================================================
// INTERNAL - UTILITIES
// ============================================================================

static bool parse_version(const char *version_str, int *major, int *minor, int *patch)
{
    if (version_str == NULL) return false;

    *major = 0;
    *minor = 0;
    *patch = 0;

    int parsed = sscanf(version_str, "%d.%d.%d", major, minor, patch);
    return (parsed >= 2);  // At least major.minor
}

static void hex_to_bytes(const char *hex, uint8_t *bytes, size_t byte_len)
{
    for (size_t i = 0; i < byte_len; i++) {
        unsigned int val;
        sscanf(hex + (i * 2), "%2x", &val);
        bytes[i] = (uint8_t)val;
    }
}
