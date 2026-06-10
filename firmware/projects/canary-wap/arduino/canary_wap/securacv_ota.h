/**
 * @file securacv_ota.h
 * @brief SecuraCV signed pull-OTA engine (shared by canary + canary-wap)
 *
 * Over-the-air firmware updates for SecuraCV Canary devices:
 *
 * - Manifest-based version checking over HTTPS (GitHub Releases by default,
 *   any HTTPS server, or an explicitly enabled local HTTP server)
 * - SHA-256 + Ed25519 release-signature verification before flashing
 * - Dual-partition A/B update scheme with automatic rollback
 * - Self-test validation after OTA to prevent bricking
 * - Anti-rollback version floor persisted in NVS
 * - Progress reporting via callback interface
 *
 * SECURITY MODEL:
 * - Public URLs require HTTPS with certificate-bundle verification.
 *   http:// is permitted only for private-network hosts AND only when the
 *   user has explicitly enabled the local-update-server option (the Ed25519
 *   signature — not the transport — is what protects image integrity).
 * - Firmware images are verified against the manifest SHA-256, then the
 *   Ed25519 release signature over (image_size_LE32 || sha256) — the same
 *   scheme as BLE OTA (ble_ota.h), so one release key signs every channel.
 * - An all-zero release public key fail-closes: installs are refused.
 * - A monotonically increasing NVS version floor rejects replayed older
 *   firmware even when validly signed.
 *
 * USAGE:
 * 1. Call securacv_ota_register_selftest() for each boot-validation probe
 * 2. Call securacv_ota_boot_self_test() early in setup() — it confirms or
 *    rolls back a freshly applied update
 * 3. Call securacv_ota_init() after WiFi is connected
 * 4. Call securacv_ota_check() / securacv_ota_check_and_install() on the
 *    24-hour schedule or on user demand
 * 5. Monitor progress via the registered callback
 *
 * This is the canonical engine at firmware/common/ota/. The canary-wap
 * Arduino sketch carries a committed copy (kept in sync by
 * firmware/scripts/check_ota_sync.sh) so GitHub zip downloads compile
 * out of the box.
 *
 * @copyright MIT License
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef SECURACV_OTA_HOST_BUILD
#include "esp_err.h"
#else
// Host-test builds compile the pure-logic subset without ESP-IDF.
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// OTA STATE AND ERROR TYPES
// ============================================================================

/**
 * @brief OTA engine state machine states
 *
 * Progress through states:
 *   IDLE -> CHECKING -> DOWNLOADING -> VERIFYING -> FLASHING -> REBOOTING
 * On error: any state -> ERROR -> IDLE
 */
typedef enum {
    SECURACV_OTA_IDLE,          /**< No OTA operation in progress */
    SECURACV_OTA_CHECKING,      /**< Fetching and parsing manifest from server */
    SECURACV_OTA_DOWNLOADING,   /**< Downloading firmware binary */
    SECURACV_OTA_VERIFYING,     /**< Verifying SHA-256 + Ed25519 signature */
    SECURACV_OTA_FLASHING,      /**< Committing verified image to inactive OTA partition */
    SECURACV_OTA_REBOOTING,     /**< Reboot scheduled, waiting for completion */
    SECURACV_OTA_ERROR,         /**< OTA failed, see error code for details */
} securacv_ota_state_t;

/**
 * @brief OTA error codes
 */
typedef enum {
    SECURACV_OTA_ERR_NONE = 0,              /**< No error */
    SECURACV_OTA_ERR_NETWORK,               /**< Network connectivity error */
    SECURACV_OTA_ERR_MANIFEST_FETCH,        /**< Failed to fetch manifest from server */
    SECURACV_OTA_ERR_MANIFEST_PARSE,        /**< Failed to parse manifest JSON */
    SECURACV_OTA_ERR_MANIFEST_INVALID,      /**< Manifest missing required fields or wrong product */
    SECURACV_OTA_ERR_NO_UPDATE,             /**< No update available (running latest version) */
    SECURACV_OTA_ERR_DOWNLOAD_FAILED,       /**< Firmware download failed */
    SECURACV_OTA_ERR_SHA256_MISMATCH,       /**< Downloaded image SHA-256 doesn't match manifest */
    SECURACV_OTA_ERR_SIGNATURE_INVALID,     /**< Ed25519 release signature verification failed */
    SECURACV_OTA_ERR_SIZE_MISMATCH,         /**< Downloaded image size doesn't match manifest */
    SECURACV_OTA_ERR_FLASH_WRITE,           /**< Failed to write firmware to OTA partition */
    SECURACV_OTA_ERR_FLASH_READ,            /**< Failed to read OTA partition for verification */
    SECURACV_OTA_ERR_PARTITION,             /**< OTA partition not found or invalid */
    SECURACV_OTA_ERR_VERSION_ROLLBACK,      /**< Update rejected: version older than minimum */
    SECURACV_OTA_ERR_SELF_TEST_FAILED,      /**< Post-OTA self-test failed, rollback initiated */
    SECURACV_OTA_ERR_ALREADY_RUNNING,       /**< OTA operation already in progress */
    SECURACV_OTA_ERR_NOT_INITIALIZED,       /**< OTA engine not initialized */
    SECURACV_OTA_ERR_OUT_OF_MEMORY,         /**< Memory allocation failed */
    SECURACV_OTA_ERR_URL_POLICY,            /**< URL rejected: https required (or enable local server option) */
    SECURACV_OTA_ERR_PUBKEY_MISSING,        /**< Release public key not provisioned (all zeros) */
    SECURACV_OTA_ERR_DEFERRED,              /**< Install deferred by the device (busy / low battery) */
} securacv_ota_error_t;

/**
 * @brief Result of comparing a manifest version against the device floor
 */
typedef enum {
    SECURACV_OTA_DECISION_UPDATE,       /**< Manifest version is newer — update available */
    SECURACV_OTA_DECISION_UP_TO_DATE,   /**< Already running this version */
    SECURACV_OTA_DECISION_ROLLBACK,     /**< Manifest version is older — reject */
} securacv_ota_decision_t;

// ============================================================================
// MANIFEST STRUCTURE
// ============================================================================

/**
 * @brief Firmware manifest information (schema v1)
 *
 * Produced and signed by firmware/scripts/ota_release.py. Example:
 * {
 *   "manifest_version": 1,
 *   "product": "securacv-canary",
 *   "version": "2.2.0",
 *   "min_version": "2.1.0",
 *   "url": "https://github.com/.../canary-2.2.0.bin",
 *   "sha256": "a1b2c3d4...",                    (64 hex)
 *   "size": 1048576,
 *   "signature": "f00d...",                     (128 hex, Ed25519 over size_LE32||sha256)
 *   "signing_key_id": "8c0f3a...",              (16 hex, sha256(pubkey) prefix)
 *   "release_notes": "Improved detection accuracy",
 *   "release_url": "https://github.com/.../releases/tag/fw-v2.2.0"
 * }
 */
typedef struct {
    char product[32];           /**< Product identifier (must match config.product) */
    char version[16];           /**< New firmware version string (e.g., "2.2.0") */
    char min_version[16];       /**< Minimum version required to update (for gap-skip) */
    char url[256];              /**< URL to download firmware binary */
    char sha256[65];            /**< Hex-encoded SHA-256 hash of firmware binary */
    uint32_t size;              /**< Firmware binary size in bytes */
    char signature[129];        /**< Hex-encoded Ed25519 signature over (size_LE32||sha256) */
    char signing_key_id[33];    /**< Release key identifier (informational) */
    char release_notes[512];    /**< Human-readable changelog text */
    char release_url[128];      /**< URL to full release notes page */
} securacv_ota_manifest_t;

// ============================================================================
// CALLBACKS
// ============================================================================

/**
 * @brief OTA progress callback function type
 *
 * Invoked whenever OTA state changes or download progress updates.
 * Called from the OTA task context — must be non-blocking. Set a flag or
 * post to a queue; publish MQTT / write SD from the main loop instead.
 *
 * @param state Current OTA state
 * @param percent Download progress (0-100), only valid during DOWNLOADING
 * @param error Error code, only valid when state is SECURACV_OTA_ERROR
 * @param user_data User-provided context pointer from configuration
 */
typedef void (*securacv_ota_progress_cb_t)(
    securacv_ota_state_t state,
    uint8_t percent,
    securacv_ota_error_t error,
    void *user_data
);

/**
 * @brief Install-permission hook
 *
 * Called before a download starts and again before the reboot. Return
 * false to defer the install — e.g. while a witness record is being
 * written, an alert is active, or the battery is too low. Write a short
 * plain-language reason into @p reason (shown to the user as-is).
 *
 * @param reason Output buffer for the deferral reason
 * @param reason_len Size of @p reason
 * @return true to proceed, false to defer
 */
typedef bool (*securacv_ota_can_install_cb_t)(char *reason, size_t reason_len);

/**
 * @brief Pre-reboot hook
 *
 * Called immediately before the post-install restart. Use it to persist
 * witness-chain state, flush SD writes, and quiesce sensing pipelines —
 * mirror what the device's manual-reboot path does.
 */
typedef void (*securacv_ota_before_reboot_cb_t)(void);

// ============================================================================
// SELF-TEST INTERFACE
// ============================================================================

/**
 * @brief Self-test function type
 *
 * User-provided functions to validate system health after OTA.
 *
 * @param test_name Human-readable name of the test (for logging)
 * @return true if test passed, false if test failed
 */
typedef bool (*securacv_selftest_fn_t)(const char *test_name);

/**
 * @brief Self-test registration structure
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
 * Pass to securacv_ota_init(). All string/pointer fields must remain valid
 * for the lifetime of the OTA engine.
 */
typedef struct {
    const char *product;                /**< Product id matched against manifest (e.g. "securacv-canary") */
    const char *current_version;        /**< Running firmware version (e.g. FW_VERSION_STRING) */
    const char *manifest_url;           /**< Compiled-in default manifest URL (NVS can override) */
    const uint8_t *release_pubkey;      /**< 32-byte Ed25519 release public key (all-zero = installs disabled) */
    const char *server_cert_pem;        /**< Optional TLS root CA PEM (NULL = certificate bundle) */
    securacv_ota_progress_cb_t on_progress;     /**< Progress callback (can be NULL) */
    void *user_data;                    /**< User context passed to callback */
    securacv_ota_can_install_cb_t can_install;  /**< Install-permission hook (can be NULL) */
    securacv_ota_before_reboot_cb_t on_before_reboot;  /**< Pre-reboot flush hook (can be NULL) */
    bool skip_version_check;            /**< Force update even if same version (dev only!) */
    bool auto_reboot;                   /**< Automatically reboot after successful install */
    uint32_t http_timeout_ms;           /**< HTTP request timeout in milliseconds (default: 30000) */
    uint32_t download_buffer_size;      /**< Download chunk size in bytes (default: 4096) */
} securacv_ota_config_t;

/**
 * @brief Default OTA configuration initializer
 */
#define SECURACV_OTA_CONFIG_DEFAULT { \
    .product = NULL, \
    .current_version = NULL, \
    .manifest_url = NULL, \
    .release_pubkey = NULL, \
    .server_cert_pem = NULL, \
    .on_progress = NULL, \
    .user_data = NULL, \
    .can_install = NULL, \
    .on_before_reboot = NULL, \
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
 * Call once at startup, after NVS and WiFi are initialized.
 * product, current_version, manifest_url, and release_pubkey are required.
 *
 * @param config Configuration structure (copied, so can be stack-allocated)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t securacv_ota_init(const securacv_ota_config_t *config);

/**
 * @brief Deinitialize the OTA subsystem
 *
 * Requests an abort and waits (bounded) for the worker task to exit.
 * @return ESP_OK on success; ESP_ERR_TIMEOUT if the worker is still
 *         busy (e.g. blocked in a network call) — the engine stays
 *         initialized, retry after the operation finishes.
 */
esp_err_t securacv_ota_deinit(void);

// ============================================================================
// OTA OPERATIONS
// ============================================================================

/**
 * @brief Check for available firmware update (non-blocking)
 *
 * Fetches the manifest and compares versions. Results are delivered via
 * the progress callback; query with securacv_ota_update_available() and
 * securacv_ota_get_manifest().
 *
 * @return ESP_OK if check started, ESP_ERR_INVALID_STATE if already running
 */
esp_err_t securacv_ota_check(void);

/**
 * @brief Check for update and install if available (non-blocking)
 *
 * Full pipeline: manifest fetch -> version compare -> can_install gate ->
 * download -> SHA-256 verify -> Ed25519 verify -> commit -> reboot.
 *
 * @return ESP_OK if operation started, ESP_ERR_INVALID_STATE if already running
 */
esp_err_t securacv_ota_check_and_install(void);

/**
 * @brief Abort an in-progress OTA operation
 */
esp_err_t securacv_ota_abort(void);

// ============================================================================
// BOOT VALIDATION (SELF-TEST)
// ============================================================================

/**
 * @brief Register a self-test function (max 16)
 *
 * Call before securacv_ota_boot_self_test().
 */
esp_err_t securacv_ota_register_selftest(const securacv_selftest_t *test);

/**
 * @brief Run boot self-test validation
 *
 * Call early in setup() after critical subsystems are initialized. If the
 * device just booted into a new OTA image, runs all registered self-tests:
 * pass -> firmware marked valid (rollback cancelled); any required test
 * fails -> firmware marked invalid and the device reboots into the
 * previous version.
 *
 * @return ESP_OK if validation passed or wasn't needed.
 * @note May not return if rollback is triggered (device reboots).
 */
esp_err_t securacv_ota_boot_self_test(void);

/**
 * @brief Whether the current boot is running a freshly applied OTA image
 *
 * True from boot until securacv_ota_boot_self_test() confirms the image.
 * After a confirmed OTA boot, securacv_ota_just_updated() stays true so
 * the integration layer can emit its "update applied" witness event once.
 */
bool securacv_ota_just_updated(void);

// ============================================================================
// PERSISTED SETTINGS (NVS)
// ============================================================================

/**
 * @brief Override the manifest URL (persisted in NVS)
 *
 * Pass NULL or "" to clear the override and return to the compiled-in
 * default. The URL must satisfy securacv_ota_url_allowed() at check time.
 */
esp_err_t securacv_ota_set_manifest_url(const char *url);

/**
 * @brief Get the effective manifest URL (NVS override or compiled default)
 */
esp_err_t securacv_ota_get_manifest_url(char *buf, size_t buf_len);

/** @brief True if the manifest URL is an NVS override (not the default). */
bool securacv_ota_manifest_url_is_override(void);

/** @brief Persisted auto-update opt-in (default false). */
esp_err_t securacv_ota_set_auto_update(bool enabled);
bool securacv_ota_get_auto_update(void);

/** @brief Persisted allow-local-http opt-in (default false). */
esp_err_t securacv_ota_set_local_http_allowed(bool allowed);
bool securacv_ota_get_local_http_allowed(void);

// ============================================================================
// STATE AND STATUS QUERIES
// ============================================================================

/** @brief Get current OTA state (thread-safe). */
securacv_ota_state_t securacv_ota_get_state(void);

/** @brief Get last error code (valid when state is SECURACV_OTA_ERROR). */
securacv_ota_error_t securacv_ota_get_last_error(void);

/** @brief Get currently running firmware version string. */
const char *securacv_ota_get_version(void);

/**
 * @brief Get manifest information from last check
 * @return Pointer valid until the next check, or NULL if no check performed
 */
const securacv_ota_manifest_t *securacv_ota_get_manifest(void);

/** @brief True if the last manifest check found a newer version. */
bool securacv_ota_update_available(void);

/** @brief Download progress 0-100 during download, 0 otherwise. */
uint8_t securacv_ota_get_progress(void);

/** @brief Unix time of the last successful manifest check (0 = never). */
uint32_t securacv_ota_get_last_check_time(void);

// ============================================================================
// PURE-LOGIC UTILITIES (host-testable)
// ============================================================================

/**
 * @brief Compare two semantic version strings ("MAJOR.MINOR.PATCH")
 * @return -1 if a < b, 0 if a == b (or unparseable), 1 if a > b
 */
int securacv_version_compare(const char *a, const char *b);

/**
 * @brief Decide whether a manifest version is an update against the floor
 *
 * The effective floor is max(running_version, nvs_floor) — the NVS floor
 * is the monotonic anti-rollback record.
 *
 * @param manifest_version Version offered by the manifest
 * @param running_version  Currently running firmware version
 * @param nvs_floor        NVS-persisted minimum version ("" if none)
 */
securacv_ota_decision_t securacv_ota_update_decision(
    const char *manifest_version,
    const char *running_version,
    const char *nvs_floor);

/**
 * @brief URL transport policy
 *
 * https:// URLs are always allowed. http:// URLs are allowed only when
 * @p allow_http_local is true AND the host is a private/loopback IPv4
 * literal (10/8, 172.16/12, 192.168/16, 127/8, 169.254/16), "localhost",
 * or a name under .local/.lan/.internal/.home.arpa. Everything else is
 * rejected — image integrity comes from the Ed25519 signature, but public
 * transport still requires TLS to protect metadata and resist tampering.
 */
bool securacv_ota_url_allowed(const char *url, bool allow_http_local);

/**
 * @brief Build the 36-byte Ed25519-signed message for a firmware image
 *
 * Layout: image_size as uint32 little-endian || sha256 digest (32 bytes).
 * Identical to the BLE OTA scheme (ble_ota.h) and ota_release.py.
 *
 * @param image_size Firmware image size in bytes
 * @param sha256 32-byte SHA-256 digest of the image
 * @param out 36-byte output buffer
 */
void securacv_ota_build_signed_message(uint32_t image_size,
                                       const uint8_t sha256[32],
                                       uint8_t out[36]);

/**
 * @brief Convert a hex string to bytes
 * @return true if @p hex is exactly 2*byte_len valid hex chars
 */
bool securacv_ota_hex_to_bytes(const char *hex, uint8_t *bytes, size_t byte_len);

/** @brief Static technical description of an error (for logs/diagnostics). */
const char *securacv_ota_error_str(securacv_ota_error_t error);

/** @brief Static technical description of a state (for logs/diagnostics). */
const char *securacv_ota_state_str(securacv_ota_state_t state);

/**
 * @brief Plain-language error message for the primary UI
 *
 * Short, jargon-free sentences suitable for the web dashboard and the
 * Home Assistant update entity. Technical detail belongs in logs via
 * securacv_ota_error_str().
 */
const char *securacv_ota_friendly_error(securacv_ota_error_t error);

/**
 * @brief Plain-language progress message for the primary UI
 */
const char *securacv_ota_friendly_state(securacv_ota_state_t state);

#ifdef __cplusplus
}
#endif
