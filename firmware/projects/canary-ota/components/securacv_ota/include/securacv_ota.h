/**
 * @file securacv_ota.h
 * @brief SecuraCV Canary OTA Update Engine
 *
 * Provides over-the-air firmware update capabilities for the SecuraCV Canary
 * privacy witness device. This component implements:
 *
 * - Manifest-based version checking over HTTPS
 * - Secure firmware download with SHA256 verification
 * - Dual-partition A/B update scheme with automatic rollback
 * - Self-test validation after OTA to prevent bricking
 * - Progress reporting via callback interface
 *
 * SECURITY MODEL:
 * - All downloads occur over HTTPS with TLS certificate verification
 * - Firmware images are verified against SHA256 hash from manifest
 * - ESP-IDF's Secure Boot v2 provides bootloader-level signature verification (Phase 3)
 * - Rollback protection ensures only validated firmware stays active
 *
 * USAGE:
 * 1. Call securacv_ota_init() after WiFi is connected
 * 2. Call securacv_ota_boot_self_test() early in app_main() to handle OTA validation
 * 3. Use securacv_ota_check_and_install() to trigger update check
 * 4. Monitor progress via the registered callback
 *
 * @author ERRERlabs
 * @copyright MIT License
 */

#pragma once

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// VERSION INFORMATION
// ============================================================================

/**
 * @brief Firmware version numbers
 *
 * These are compiled into the firmware and used for version comparison
 * during OTA updates. Update these values for each release.
 */
#define SECURACV_FW_VERSION_MAJOR  1
#define SECURACV_FW_VERSION_MINOR  0
#define SECURACV_FW_VERSION_PATCH  0
#define SECURACV_FW_VERSION_STRING "1.0.0"

// ============================================================================
// OTA STATE AND ERROR TYPES
// ============================================================================

/**
 * @brief OTA engine state machine states
 *
 * These states represent the current phase of an OTA operation.
 * Progress through states: IDLE -> CHECKING -> DOWNLOADING -> VERIFYING -> FLASHING -> REBOOTING
 * On error: any state -> ERROR -> IDLE
 */
typedef enum {
    SECURACV_OTA_IDLE,          /**< No OTA operation in progress */
    SECURACV_OTA_CHECKING,      /**< Fetching and parsing manifest from server */
    SECURACV_OTA_DOWNLOADING,   /**< Downloading firmware binary */
    SECURACV_OTA_VERIFYING,     /**< Verifying SHA256 hash of downloaded image */
    SECURACV_OTA_FLASHING,      /**< Writing verified image to inactive OTA partition */
    SECURACV_OTA_REBOOTING,     /**< Reboot scheduled, waiting for completion */
    SECURACV_OTA_ERROR,         /**< OTA failed, see error code for details */
} securacv_ota_state_t;

/**
 * @brief OTA error codes
 *
 * Detailed error codes for diagnosing OTA failures.
 */
typedef enum {
    SECURACV_OTA_ERR_NONE = 0,              /**< No error */
    SECURACV_OTA_ERR_NETWORK,               /**< Network connectivity error */
    SECURACV_OTA_ERR_MANIFEST_FETCH,        /**< Failed to fetch manifest from server */
    SECURACV_OTA_ERR_MANIFEST_PARSE,        /**< Failed to parse manifest JSON */
    SECURACV_OTA_ERR_MANIFEST_INVALID,      /**< Manifest missing required fields or wrong product */
    SECURACV_OTA_ERR_NO_UPDATE,             /**< No update available (running latest version) */
    SECURACV_OTA_ERR_DOWNLOAD_FAILED,       /**< Firmware download failed */
    SECURACV_OTA_ERR_SHA256_MISMATCH,       /**< Downloaded image SHA256 doesn't match manifest */
    SECURACV_OTA_ERR_SIGNATURE_INVALID,     /**< Firmware signature verification failed (Phase 3) */
    SECURACV_OTA_ERR_FLASH_WRITE,           /**< Failed to write firmware to OTA partition */
    SECURACV_OTA_ERR_FLASH_READ,            /**< Failed to read OTA partition for verification */
    SECURACV_OTA_ERR_PARTITION,             /**< OTA partition not found or invalid */
    SECURACV_OTA_ERR_VERSION_ROLLBACK,      /**< Update rejected: version older than minimum */
    SECURACV_OTA_ERR_SELF_TEST_FAILED,      /**< Post-OTA self-test failed, rollback initiated */
    SECURACV_OTA_ERR_ALREADY_RUNNING,       /**< OTA operation already in progress */
    SECURACV_OTA_ERR_NOT_INITIALIZED,       /**< OTA engine not initialized */
    SECURACV_OTA_ERR_OUT_OF_MEMORY,         /**< Memory allocation failed */
} securacv_ota_error_t;

// ============================================================================
// MANIFEST STRUCTURE
// ============================================================================

/**
 * @brief Firmware manifest information
 *
 * This structure contains all information from the OTA manifest JSON file.
 * The manifest is fetched from the configured URL before each update.
 *
 * Example manifest JSON:
 * {
 *   "product": "securacv-canary",
 *   "version": "1.3.0",
 *   "min_version": "1.0.0",
 *   "url": "https://operacanary.com/firmware/canary-1.3.0.bin",
 *   "sha256": "a1b2c3d4e5f6...",
 *   "size": 1048576,
 *   "release_notes": "Improved detection accuracy",
 *   "release_url": "https://operacanary.com/changelog#1.3.0"
 * }
 */
typedef struct {
    char product[32];           /**< Product identifier (must match SECURACV_DEVICE_PRODUCT) */
    char version[16];           /**< New firmware version string (e.g., "1.3.0") */
    char min_version[16];       /**< Minimum version required to update (for gap-skip) */
    char url[256];              /**< HTTPS URL to download firmware binary */
    char sha256[65];            /**< Hex-encoded SHA256 hash of firmware binary */
    uint32_t size;              /**< Firmware binary size in bytes */
    char release_notes[512];    /**< Human-readable changelog text */
    char release_url[128];      /**< URL to full release notes page */
} securacv_ota_manifest_t;

// ============================================================================
// PROGRESS CALLBACK
// ============================================================================

/**
 * @brief OTA progress callback function type
 *
 * This callback is invoked whenever OTA state changes or download progress updates.
 * The callback is called from the OTA task context, so it should be non-blocking.
 *
 * @param state Current OTA state
 * @param percent Download progress (0-100), only valid during DOWNLOADING state
 * @param error Error code, only valid when state is SECURACV_OTA_ERROR
 * @param user_data User-provided context pointer from configuration
 *
 * IMPORTANT: This callback is invoked from a FreeRTOS task context.
 * Do not perform blocking operations or heavy processing in the callback.
 * If you need to take action on state changes, set a flag or post to a queue.
 */
typedef void (*securacv_ota_progress_cb_t)(
    securacv_ota_state_t state,
    uint8_t percent,
    securacv_ota_error_t error,
    void *user_data
);

// ============================================================================
// SELF-TEST INTERFACE
// ============================================================================

/**
 * @brief Self-test function type
 *
 * User-provided functions to validate system health after OTA.
 * Return true if the test passes, false if it fails.
 *
 * @param test_name Human-readable name of the test (for logging)
 * @return true if test passed, false if test failed
 *
 * Example tests:
 * - NVS storage accessible
 * - Ed25519 private key loads correctly
 * - Hash chain head is valid
 * - WiFi connects successfully
 * - Camera initializes
 */
typedef bool (*securacv_selftest_fn_t)(const char *test_name);

/**
 * @brief Self-test registration structure
 *
 * Used to register custom self-test functions that run after OTA
 * to validate the new firmware before confirming the update.
 */
typedef struct {
    const char *name;           /**< Test name for logging */
    securacv_selftest_fn_t fn;  /**< Test function pointer */
    bool required;              /**< If true, failure triggers rollback */
} securacv_selftest_t;

// ============================================================================
// CONFIGURATION
// ============================================================================

/**
 * @brief OTA engine configuration
 *
 * Pass this structure to securacv_ota_init() to configure the OTA engine.
 * All string pointers must remain valid for the lifetime of the OTA engine.
 */
typedef struct {
    const char *manifest_url;           /**< HTTPS URL to firmware manifest JSON */
    const char *server_cert_pem;        /**< TLS root CA certificate in PEM format (NULL to use system CAs) */
    securacv_ota_progress_cb_t on_progress;  /**< Progress callback (can be NULL) */
    void *user_data;                    /**< User context passed to callback */
    bool skip_version_check;            /**< Force update even if same version (dev only!) */
    bool auto_reboot;                   /**< Automatically reboot after successful download (default: true) */
    uint32_t http_timeout_ms;           /**< HTTP request timeout in milliseconds (default: 30000) */
    uint32_t download_buffer_size;      /**< Download chunk size in bytes (default: 4096) */
} securacv_ota_config_t;

/**
 * @brief Default OTA configuration
 *
 * Use this to initialize a configuration structure with sensible defaults.
 */
#define SECURACV_OTA_CONFIG_DEFAULT { \
    .manifest_url = NULL, \
    .server_cert_pem = NULL, \
    .on_progress = NULL, \
    .user_data = NULL, \
    .skip_version_check = false, \
    .auto_reboot = true, \
    .http_timeout_ms = 30000, \
    .download_buffer_size = 4096, \
}

// ============================================================================
// INITIALIZATION AND SHUTDOWN
// ============================================================================

/**
 * @brief Initialize the OTA subsystem
 *
 * Must be called once at startup, after NVS and WiFi are initialized.
 * This function:
 * - Validates the configuration
 * - Creates the OTA task
 * - Initializes internal state
 *
 * @param config Configuration structure (copied, so can be stack-allocated)
 * @return ESP_OK on success, error code on failure
 *
 * @note The manifest_url field in config must not be NULL.
 * @note Call securacv_ota_deinit() to clean up resources if needed.
 */
esp_err_t securacv_ota_init(const securacv_ota_config_t *config);

/**
 * @brief Deinitialize the OTA subsystem
 *
 * Stops any in-progress OTA operation and frees resources.
 * Safe to call even if init was never called or failed.
 *
 * @return ESP_OK on success
 */
esp_err_t securacv_ota_deinit(void);

// ============================================================================
// OTA OPERATIONS
// ============================================================================

/**
 * @brief Check for available firmware update
 *
 * Fetches the manifest from the configured URL and compares versions.
 * This is non-blocking: spawns a FreeRTOS task to do the work.
 * Results are delivered via the progress callback.
 *
 * If an update is available, the manifest information can be retrieved
 * via securacv_ota_get_manifest().
 *
 * @return ESP_OK if check started, ESP_ERR_INVALID_STATE if already running
 */
esp_err_t securacv_ota_check(void);

/**
 * @brief Check for update and install if available
 *
 * This is the main entry point for OTA updates. It:
 * 1. Fetches the manifest
 * 2. Compares versions
 * 3. Downloads the new firmware (if newer version available)
 * 4. Verifies SHA256 hash
 * 5. Writes to inactive OTA partition
 * 6. Reboots into new firmware (if auto_reboot is true)
 *
 * This is non-blocking: spawns a FreeRTOS task to do the work.
 * Progress is reported via the progress callback.
 *
 * @return ESP_OK if operation started, ESP_ERR_INVALID_STATE if already running
 */
esp_err_t securacv_ota_check_and_install(void);

/**
 * @brief Abort an in-progress OTA operation
 *
 * Cancels any running OTA download or check. The OTA engine returns
 * to IDLE state. If a partial download exists, it is discarded.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not running
 */
esp_err_t securacv_ota_abort(void);

// ============================================================================
// BOOT VALIDATION (SELF-TEST)
// ============================================================================

/**
 * @brief Register a self-test function
 *
 * Self-tests run after an OTA update to validate that the new firmware
 * is working correctly. If any required test fails, the firmware is
 * marked invalid and the device reboots back to the previous version.
 *
 * Call this before securacv_ota_boot_self_test() to register tests.
 *
 * @param test Self-test definition
 * @return ESP_OK on success, ESP_ERR_NO_MEM if max tests reached
 *
 * @note Maximum 16 self-tests can be registered.
 */
esp_err_t securacv_ota_register_selftest(const securacv_selftest_t *test);

/**
 * @brief Run boot self-test validation
 *
 * Call this early in app_main() after critical subsystems are initialized
 * but before starting normal operation.
 *
 * This function checks if we just booted from a new OTA partition:
 * - If NOT a new OTA boot: Returns immediately with ESP_OK
 * - If new OTA boot: Runs all registered self-tests
 *   - All tests pass: Marks firmware valid, cancels rollback, returns ESP_OK
 *   - Any required test fails: Marks firmware invalid, reboots to previous version
 *
 * The rollback happens automatically via ESP-IDF's app rollback feature.
 *
 * @return ESP_OK if validation passed or not needed
 *         ESP_ERR_OTA_ROLLBACK_FAILED if rollback was needed but failed
 *
 * @note This function may not return if rollback is triggered (device reboots).
 */
esp_err_t securacv_ota_boot_self_test(void);

// ============================================================================
// STATE AND STATUS QUERIES
// ============================================================================

/**
 * @brief Get current OTA state
 *
 * Thread-safe state query.
 *
 * @return Current OTA state
 */
securacv_ota_state_t securacv_ota_get_state(void);

/**
 * @brief Get last error code
 *
 * Returns the error code from the most recent failed operation.
 * Only meaningful when state is SECURACV_OTA_ERROR.
 *
 * @return Last error code, or SECURACV_OTA_ERR_NONE if no error
 */
securacv_ota_error_t securacv_ota_get_last_error(void);

/**
 * @brief Get currently running firmware version string
 *
 * @return Version string (e.g., "1.0.0"), never NULL
 */
const char *securacv_ota_get_version(void);

/**
 * @brief Get manifest information from last check
 *
 * Returns information about the firmware found during the last
 * manifest check. Only valid after a successful check operation.
 *
 * @return Pointer to manifest structure, or NULL if no check performed
 *
 * @note The returned pointer is valid until the next check operation.
 */
const securacv_ota_manifest_t *securacv_ota_get_manifest(void);

/**
 * @brief Check if an update is available
 *
 * Convenience function to check if the last manifest check found
 * a newer version than what's currently running.
 *
 * @return true if update available, false otherwise
 */
bool securacv_ota_update_available(void);

/**
 * @brief Get download progress percentage
 *
 * @return Progress 0-100 during download, 0 otherwise
 */
uint8_t securacv_ota_get_progress(void);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * @brief Compare two semantic version strings
 *
 * Compares versions in "MAJOR.MINOR.PATCH" format.
 *
 * @param a First version string
 * @param b Second version string
 * @return -1 if a < b, 0 if a == b, 1 if a > b
 *
 * Examples:
 *   "1.0.0" vs "1.0.1" -> -1
 *   "1.2.0" vs "1.1.9" -> 1
 *   "2.0.0" vs "1.9.9" -> 1
 */
int securacv_version_compare(const char *a, const char *b);

/**
 * @brief Get human-readable error description
 *
 * @param error Error code
 * @return Static string describing the error
 */
const char *securacv_ota_error_str(securacv_ota_error_t error);

/**
 * @brief Get human-readable state description
 *
 * @param state OTA state
 * @return Static string describing the state
 */
const char *securacv_ota_state_str(securacv_ota_state_t state);

#ifdef __cplusplus
}
#endif
