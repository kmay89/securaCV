/**
 * @file securacv_ota.cpp
 * @brief SecuraCV signed pull-OTA engine implementation
 *
 * Ported from firmware/projects/canary-ota/components/securacv_ota/ and
 * extended with Ed25519 release-signature verification, certificate-bundle
 * TLS, URL transport policy, NVS-persisted settings, retry with backoff,
 * and install-permission / pre-reboot hooks. Compiles against both
 * Arduino-ESP32 core 2.x (IDF 4.4) and 3.x (IDF 5.x).
 *
 * ARCHITECTURE:
 * - OTA operations run in a dedicated FreeRTOS task to avoid blocking
 * - State machine drives the update process
 * - Progress is reported via user-registered callback
 * - Self-test validation runs at boot to confirm or roll back an update
 *
 * The pure-logic subset (version compare, update decision, URL policy,
 * signed-message layout, hex parsing, message strings) compiles on the
 * host under -DSECURACV_OTA_HOST_BUILD for unit testing.
 */

#include "securacv_ota.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ============================================================================
// SECURITY: Compile-time guard against cert-skip in release builds
// ============================================================================
// SECURACV_OTA_SKIP_CERT_VERIFY disables TLS certificate verification,
// enabling MITM attacks on OTA downloads. It MUST only be used in
// development builds with self-signed certs and mock_ota_server.py.
#if defined(SECURACV_OTA_SKIP_CERT_VERIFY) && \
    !(defined(SECURACV_DEBUG_BUILD) || defined(SECURACV_BUILD_DEV))
  #error "SECURACV_OTA_SKIP_CERT_VERIFY is set in a non-dev build. " \
         "This disables TLS certificate verification and MUST NOT be used in " \
         "release/production firmware."
#endif

// ============================================================================
// PURE LOGIC — compiled on device and host
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

// Prerelease ranking at an equal major.minor.patch: a "dev.N" build is
// older than "rc.N", which is older than a stable build — so promoting a
// commit (retagging it stable) is offered to channel testers as a real
// update, and the anti-rollback floor stays coherent across channels
// (docs/RELEASE_PROCESS.md). Variant suffixes ("2.2.0-wap") are NOT
// prerelease markers: only explicit "dev."/"rc." dot-number segments
// count, wherever they sit after the numeric triple.
static void parse_prerelease(const char *v, int *band, int *num)
{
    *band = 3;  /* stable */
    *num  = 0;
    if (v == NULL) return;
    const char *p = strstr(v, "-rc.");
    if (p == NULL) p = strstr(v, ".rc.");
    if (p != NULL) {
        *band = 2;
        *num  = atoi(p + 4);
        return;
    }
    p = strstr(v, "-dev.");
    if (p == NULL) p = strstr(v, ".dev.");
    if (p != NULL) {
        *band = 1;
        *num  = atoi(p + 5);
    }
}

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

    int a_band, a_pre, b_band, b_pre;
    parse_prerelease(a, &a_band, &a_pre);
    parse_prerelease(b, &b_band, &b_pre);
    if (a_band != b_band) return (a_band > b_band) ? 1 : -1;
    if (a_pre  != b_pre)  return (a_pre  > b_pre)  ? 1 : -1;

    return 0;
}

securacv_ota_decision_t securacv_ota_update_decision(
    const char *manifest_version,
    const char *running_version,
    const char *nvs_floor)
{
    // The effective floor is whichever is higher: the running firmware or
    // the NVS-persisted monotonic minimum. A validly signed but older image
    // replayed from a hostile mirror is rejected here.
    const char *floor = running_version;
    if (nvs_floor != NULL && nvs_floor[0] != '\0' &&
        securacv_version_compare(nvs_floor, running_version) > 0) {
        floor = nvs_floor;
    }

    int cmp = securacv_version_compare(manifest_version, floor);
    if (cmp > 0)  return SECURACV_OTA_DECISION_UPDATE;
    if (cmp == 0) return SECURACV_OTA_DECISION_UP_TO_DATE;
    return SECURACV_OTA_DECISION_ROLLBACK;
}

// IPv4 literal parser for the URL policy. Returns true and fills octets
// only when host is a full dotted-quad literal.
static bool parse_ipv4(const char *host, size_t host_len, int octets[4])
{
    int value = 0, ndigits = 0, noctets = 0;
    for (size_t i = 0; i <= host_len; i++) {
        char c = (i < host_len) ? host[i] : '.';
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            ndigits++;
            if (ndigits > 3 || value > 255) return false;
        } else if (c == '.') {
            if (ndigits == 0 || noctets >= 4) return false;
            octets[noctets++] = value;
            value = 0;
            ndigits = 0;
        } else {
            return false;
        }
    }
    return noctets == 4;
}

static bool host_suffix_is(const char *host, size_t host_len, const char *suffix)
{
    size_t suffix_len = strlen(suffix);
    if (host_len < suffix_len) return false;
    return strncasecmp(host + host_len - suffix_len, suffix, suffix_len) == 0;
}

bool securacv_ota_url_allowed(const char *url, bool allow_http_local)
{
    if (url == NULL || url[0] == '\0') return false;

    if (strncasecmp(url, "https://", 8) == 0) {
        return true;
    }
    if (strncasecmp(url, "http://", 7) != 0) {
        return false;  // Unknown scheme
    }
    if (!allow_http_local) {
        return false;
    }

    // Extract host: between "http://" and the first '/', ':' or end.
    const char *host = url + 7;
    size_t host_len = 0;
    while (host[host_len] != '\0' && host[host_len] != '/' &&
           host[host_len] != ':' && host[host_len] != '?') {
        host_len++;
    }
    if (host_len == 0) return false;

    if (host_len == 9 && strncasecmp(host, "localhost", 9) == 0) return true;

    int o[4];
    if (parse_ipv4(host, host_len, o)) {
        if (o[0] == 10) return true;                                   // 10/8
        if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return true;      // 172.16/12
        if (o[0] == 192 && o[1] == 168) return true;                   // 192.168/16
        if (o[0] == 127) return true;                                  // loopback
        if (o[0] == 169 && o[1] == 254) return true;                   // link-local
        return false;  // Public IPv4 literal over http: refused
    }

    // Private-use name suffixes (mDNS / router-local / RFC 8375).
    if (host_suffix_is(host, host_len, ".local")) return true;
    if (host_suffix_is(host, host_len, ".lan")) return true;
    if (host_suffix_is(host, host_len, ".internal")) return true;
    if (host_suffix_is(host, host_len, ".home.arpa")) return true;

    return false;
}

void securacv_ota_build_signed_message(uint32_t image_size,
                                       const uint8_t sha256[32],
                                       uint8_t out[36])
{
    // Identical layout to ble_ota.cpp and ota_release.py:
    // image_size as uint32 little-endian || sha256 digest.
    out[0] = (uint8_t)(image_size & 0xFF);
    out[1] = (uint8_t)((image_size >> 8) & 0xFF);
    out[2] = (uint8_t)((image_size >> 16) & 0xFF);
    out[3] = (uint8_t)((image_size >> 24) & 0xFF);
    memcpy(out + 4, sha256, 32);
}

// Append one field + NUL separator to the canonical manifest message.
static bool manifest_msg_append(uint8_t *out, size_t cap, size_t *pos, const char *field)
{
    size_t len = strlen(field);
    if (*pos + len + 1 > cap) return false;
    memcpy(out + *pos, field, len);
    *pos += len;
    out[(*pos)++] = '\0';
    return true;
}

bool securacv_ota_build_manifest_message(const securacv_ota_manifest_t *m,
                                         uint8_t *out, size_t cap,
                                         size_t *out_len)
{
    // Byte-identical to ota_release.py's manifest_signed_message():
    // "scv-manifest-v1\0" then every field (lowercased sha, decimal size)
    // followed by a NUL separator. Absent optional fields are empty.
    if (m == NULL || out == NULL || out_len == NULL) return false;

    static const char prefix[] = "scv-manifest-v1";
    size_t pos = 0;
    if (!manifest_msg_append(out, cap, &pos, prefix)) return false;
    if (!manifest_msg_append(out, cap, &pos, m->product)) return false;
    if (!manifest_msg_append(out, cap, &pos, m->version)) return false;
    if (!manifest_msg_append(out, cap, &pos, m->min_version)) return false;
    if (!manifest_msg_append(out, cap, &pos, m->url)) return false;

    char sha_lower[65];
    for (size_t i = 0; i < sizeof(sha_lower) - 1 && m->sha256[i] != '\0'; i++) {
        char c = m->sha256[i];
        sha_lower[i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
        sha_lower[i + 1] = '\0';
    }
    if (m->sha256[0] == '\0') sha_lower[0] = '\0';
    if (!manifest_msg_append(out, cap, &pos, sha_lower)) return false;

    char size_dec[12];
    snprintf(size_dec, sizeof(size_dec), "%lu", (unsigned long)m->size);
    if (!manifest_msg_append(out, cap, &pos, size_dec)) return false;

    if (!manifest_msg_append(out, cap, &pos, m->release_notes)) return false;
    if (!manifest_msg_append(out, cap, &pos, m->release_url)) return false;

    *out_len = pos;
    return true;
}

bool securacv_ota_hex_to_bytes(const char *hex, uint8_t *bytes, size_t byte_len)
{
    if (hex == NULL || strlen(hex) != byte_len * 2) return false;

    for (size_t i = 0; i < byte_len * 2; i++) {
        char c = hex[i];
        uint8_t nibble;
        if (c >= '0' && c <= '9')      nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else return false;

        if (i % 2 == 0) bytes[i / 2] = nibble << 4;
        else            bytes[i / 2] |= nibble;
    }
    return true;
}

const char *securacv_ota_error_str(securacv_ota_error_t error)
{
    switch (error) {
        case SECURACV_OTA_ERR_NONE:              return "No error";
        case SECURACV_OTA_ERR_NETWORK:           return "Network error";
        case SECURACV_OTA_ERR_MANIFEST_FETCH:    return "Failed to fetch manifest";
        case SECURACV_OTA_ERR_MANIFEST_PARSE:    return "Failed to parse manifest";
        case SECURACV_OTA_ERR_MANIFEST_INVALID:  return "Invalid manifest";
        case SECURACV_OTA_ERR_NO_UPDATE:         return "No update available";
        case SECURACV_OTA_ERR_DOWNLOAD_FAILED:   return "Download failed";
        case SECURACV_OTA_ERR_SHA256_MISMATCH:   return "SHA-256 verification failed";
        case SECURACV_OTA_ERR_SIGNATURE_INVALID: return "Ed25519 signature verification failed";
        case SECURACV_OTA_ERR_SIZE_MISMATCH:     return "Image size mismatch";
        case SECURACV_OTA_ERR_FLASH_WRITE:       return "Flash write failed";
        case SECURACV_OTA_ERR_FLASH_READ:        return "Flash read failed";
        case SECURACV_OTA_ERR_PARTITION:         return "Partition error";
        case SECURACV_OTA_ERR_VERSION_ROLLBACK:  return "Version rollback rejected";
        case SECURACV_OTA_ERR_SELF_TEST_FAILED:  return "Self-test failed";
        case SECURACV_OTA_ERR_ALREADY_RUNNING:   return "OTA already running";
        case SECURACV_OTA_ERR_NOT_INITIALIZED:   return "OTA not initialized";
        case SECURACV_OTA_ERR_OUT_OF_MEMORY:     return "Out of memory";
        case SECURACV_OTA_ERR_URL_POLICY:        return "URL rejected by transport policy";
        case SECURACV_OTA_ERR_PUBKEY_MISSING:    return "Release public key not provisioned";
        case SECURACV_OTA_ERR_DEFERRED:          return "Install deferred by device";
        case SECURACV_OTA_ERR_MANIFEST_SIG:      return "Manifest signature missing or invalid";
        default:                                 return "Unknown error";
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

const char *securacv_ota_friendly_error(securacv_ota_error_t error)
{
    // Plain-language strings for the primary UI — keep these readable for
    // everyone (they're held to the microcopy lint standard). Technical
    // detail lives in securacv_ota_error_str() and the logs.
    switch (error) {
        case SECURACV_OTA_ERR_NONE:
            return "";
        case SECURACV_OTA_ERR_NETWORK:
        case SECURACV_OTA_ERR_MANIFEST_FETCH:
            return "Could not reach the update server. Check the network and try again.";
        case SECURACV_OTA_ERR_MANIFEST_PARSE:
        case SECURACV_OTA_ERR_MANIFEST_INVALID:
            return "The update info from the server looks wrong. Try again later.";
        case SECURACV_OTA_ERR_NO_UPDATE:
            return "Your Canary is up to date.";
        case SECURACV_OTA_ERR_DOWNLOAD_FAILED:
            return "The download stopped before it finished. Try again.";
        case SECURACV_OTA_ERR_SHA256_MISMATCH:
        case SECURACV_OTA_ERR_SIGNATURE_INVALID:
        case SECURACV_OTA_ERR_SIZE_MISMATCH:
            return "The update file failed its security check, so it was not installed.";
        case SECURACV_OTA_ERR_FLASH_WRITE:
        case SECURACV_OTA_ERR_FLASH_READ:
        case SECURACV_OTA_ERR_PARTITION:
            return "The update could not be saved. Your Canary is still on its current software.";
        case SECURACV_OTA_ERR_VERSION_ROLLBACK:
            return "That update is older than the software already installed, so it was skipped.";
        case SECURACV_OTA_ERR_SELF_TEST_FAILED:
            return "The update did not pass its health check, so the previous software was restored.";
        case SECURACV_OTA_ERR_ALREADY_RUNNING:
            return "An update is already in progress.";
        case SECURACV_OTA_ERR_URL_POLICY:
            return "The update address must use https. Local-only servers need the local update option turned on.";
        case SECURACV_OTA_ERR_PUBKEY_MISSING:
            return "Updates are turned off on this build because no release key is set.";
        case SECURACV_OTA_ERR_DEFERRED:
            return "The update was postponed. It will be offered again later.";
        case SECURACV_OTA_ERR_MANIFEST_SIG:
            return "The update info failed its security check, so it was ignored.";
        default:
            return "Something went wrong with the update. Your Canary is still on its current software.";
    }
}

const char *securacv_ota_friendly_state(securacv_ota_state_t state)
{
    switch (state) {
        case SECURACV_OTA_IDLE:        return "";
        case SECURACV_OTA_CHECKING:    return "Checking for updates…";
        case SECURACV_OTA_DOWNLOADING: return "Downloading update…";
        case SECURACV_OTA_VERIFYING:   return "Checking the update file…";
        case SECURACV_OTA_FLASHING:    return "Installing…";
        case SECURACV_OTA_REBOOTING:   return "Restarting…";
        case SECURACV_OTA_ERROR:       return "Update failed.";
        default:                       return "";
    }
}

bool securacv_ota_channel_manifest_url(const char *base_url,
                                       securacv_ota_channel_t channel,
                                       char *buf, size_t buf_len)
{
    if (base_url == NULL || buf == NULL || buf_len == 0) return false;
    if (channel != SECURACV_OTA_CHANNEL_DEV) return false;

    // The one canonical stable-channel segment (docs/RELEASE_PROCESS.md):
    // GitHub guarantees releases/latest never points at a prerelease, and
    // the repo's release guard keeps it on the firmware — so this rewrite
    // is the exact inverse of how the dev channel is published (the
    // fw-dev-latest prerelease mirrors every manifest a dev release cuts).
    static const char k_release_seg[] = "/releases/latest/download/";
    static const char k_dev_seg[]     = "/releases/download/fw-dev-latest/";

    const char *seg = strstr(base_url, k_release_seg);
    if (seg == NULL) return false;

    const size_t head = (size_t)(seg - base_url);
    const char *tail = seg + (sizeof(k_release_seg) - 1);
    const size_t need = head + (sizeof(k_dev_seg) - 1) + strlen(tail) + 1;
    if (need > buf_len) return false;

    memcpy(buf, base_url, head);
    memcpy(buf + head, k_dev_seg, sizeof(k_dev_seg) - 1);
    strcpy(buf + head + (sizeof(k_dev_seg) - 1), tail);
    return true;
}

#ifndef SECURACV_OTA_HOST_BUILD
// ============================================================================
// DEVICE IMPLEMENTATION
// ============================================================================

// Arduino.h MUST come before any ESP-IDF networking header in this TU.
// lwip's inet.h defines INADDR_NONE as a macro; if it lands before the
// core's IPAddress.h (extern IPAddress INADDR_NONE), that declaration
// macro-expands into a parse error. Including Arduino.h first pins the
// safe order regardless of what ArduinoJson/Crypto pull in later.
#include <Arduino.h>

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
#include "esp_system.h"
#if __has_include("esp_random.h")
#include "esp_random.h"
#endif
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include <time.h>

#include <ArduinoJson.h>
#include <Ed25519.h>

// Certificate bundle for public HTTPS endpoints (GitHub Releases). The
// attach hook's NAME varies across Arduino-ESP32 platform packagings —
// upstream IDF exposes esp_crt_bundle_attach, while some core builds
// rename it to arduino_esp_crt_bundle_attach to avoid an IDF symbol clash
// (and which name a given platform/chip combination ships is a patch-level
// detail, not a core-version one). Declare both as weak and dispatch to
// whichever this build's libraries actually provide — link-time feature
// detection instead of unwinnable version pinning. If neither exists,
// TLS setup fails closed (manifest fetch reports a network error).
extern "C" {
esp_err_t esp_crt_bundle_attach(void *conf) __attribute__((weak));
esp_err_t arduino_esp_crt_bundle_attach(void *conf) __attribute__((weak));
}

static esp_err_t scv_crt_bundle_attach(void *conf)
{
    // Prefer the IDF entry point: it always carries the embedded default
    // root bundle. The arduino_* variant is the fallback for the core
    // builds that renamed it.
    if (esp_crt_bundle_attach != NULL) {
        return esp_crt_bundle_attach(conf);
    }
    if (arduino_esp_crt_bundle_attach != NULL) {
        return arduino_esp_crt_bundle_attach(conf);
    }
    return ESP_FAIL;
}

// House rule (regression_check.sh, LESSONS_LEARNED.md): always the non-_ret
// mbedtls names. They exist on both Arduino core lines — void-returning
// (deprecated) on core 2.x / mbedtls 2.28, int-returning on core 3.x /
// mbedtls 3.x — so calls must not inspect the return value.

// ============================================================================
// ROLLBACK OWNERSHIP — keep the Arduino core from auto-confirming images
// ============================================================================
// The Arduino core's initArduino() runs BEFORE setup() and, by default,
// marks a pending OTA image valid immediately (weak verifyRollbackLater()
// returns false → esp_ota_mark_app_valid_cancel_rollback() fires microseconds
// into the first boot). That silently defeats both protections this engine
// promises:
//   - our boot self-tests would run after the image is already confirmed,
//     so a failed required probe could never trigger a rollback, and
//   - a crash-looping image would never revert, because the bootloader's
//     PENDING_VERIFY safety net only acts on unconfirmed images.
// Overriding the weak hook tells the core: the application owns validation.
// The image then stays PENDING_VERIFY until securacv_ota_boot_self_test()
// confirms it — and if the new firmware crashes (or hangs into a watchdog
// reset) at ANY point before that confirmation, the bootloader boots the
// previous firmware on the very next start. One bad boot, automatic recovery.
#if defined(CONFIG_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
extern "C" bool verifyRollbackLater(void)
{
    return true;
}
#endif

static const char *TAG = "securacv_ota";

#define OTA_TASK_STACK_SIZE     8192
#define OTA_TASK_PRIORITY       (tskIDLE_PRIORITY + 2)
#define OTA_TASK_NAME           "ota_task"

#define MAX_SELF_TESTS          16
#define HTTP_BUFFER_SIZE        4096  // Large enough for manifest JSON with release notes
#define SHA256_DIGEST_LENGTH    32
#define MANIFEST_FETCH_ATTEMPTS 3

// ============================================================================
// INTERNAL STATE
// ============================================================================

typedef struct {
    // Configuration (copied from init)
    securacv_ota_config_t config;

    // Runtime state
    securacv_ota_state_t state;
    securacv_ota_error_t last_error;
    uint8_t progress_percent;
    uint32_t last_check_time;

    // Manifest from last check
    securacv_ota_manifest_t manifest;
    bool manifest_valid;
    bool update_available;

    // Conditional-GET cache (RAM only — consistent with manifest cache)
    char etag[80];

    // Task management
    TaskHandle_t task_handle;
    SemaphoreHandle_t mutex;
    volatile bool task_should_abort;

    // Self-tests
    securacv_selftest_t self_tests[MAX_SELF_TESTS];
    size_t num_self_tests;

    // Initialization flag
    bool initialized;

    // Operation mode (check only or check+install)
    bool install_mode;
} ota_context_t;

static ota_context_t s_ctx = {};
static bool s_just_updated = false;

// ============================================================================
// NVS PERSISTED SETTINGS
// ============================================================================

#define NVS_NAMESPACE        "securacv_ota"
#define NVS_KEY_MIN_VERSION  "min_ver"
#define NVS_KEY_MANIFEST_URL "manifest_url"
#define NVS_KEY_AUTO_UPDATE  "auto_upd"
#define NVS_KEY_HTTP_LOCAL   "http_local"
#define NVS_KEY_PENDING_VER  "pend_ver"
#define NVS_KEY_CHANNEL      "channel"

static esp_err_t nvs_store_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    if (value == NULL || value[0] == '\0') {
        err = nvs_erase_key(handle, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_str(handle, key, value);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_load_str(const char *key, char *buf, size_t buf_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, key, buf, &buf_len);
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_store_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static uint8_t nvs_load_u8(const char *key, uint8_t fallback)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return fallback;
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

esp_err_t securacv_ota_set_manifest_url(const char *url)
{
    if (url != NULL && url[0] != '\0' &&
        !securacv_ota_url_allowed(url, securacv_ota_get_local_http_allowed())) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_store_str(NVS_KEY_MANIFEST_URL, url);
}

esp_err_t securacv_ota_get_manifest_url(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) return ESP_ERR_INVALID_ARG;
    if (nvs_load_str(NVS_KEY_MANIFEST_URL, buf, buf_len) == ESP_OK && buf[0] != '\0') {
        // An explicit URL override names an exact server — it wins outright,
        // channel setting included (see the header's channel contract).
        return ESP_OK;
    }
    if (s_ctx.config.manifest_url == NULL) {
        buf[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    if (securacv_ota_channel_manifest_url(s_ctx.config.manifest_url,
                                          securacv_ota_get_channel(),
                                          buf, buf_len)) {
        return ESP_OK;
    }
    strncpy(buf, s_ctx.config.manifest_url, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return ESP_OK;
}

bool securacv_ota_manifest_url_is_override(void)
{
    // Size the probe buffer to the real maximum URL, not a token 8 bytes: an
    // override is a full https URL, so nvs_get_str used to return
    // ESP_ERR_NVS_INVALID_LENGTH and leave `buf` UNWRITTEN, after which the
    // old `buf[0]` read uninitialized stack for every real override. Treat a
    // too-long stored value as proof an override exists rather than reading an
    // untouched buffer. An empty string is never persisted (nvs_store_str
    // erases the key for one), so the key being present at all IS the override.
    char buf[sizeof(s_ctx.manifest.url)];
    esp_err_t err = nvs_load_str(NVS_KEY_MANIFEST_URL, buf, sizeof(buf));
    if (err == ESP_ERR_NVS_INVALID_LENGTH) return true;
    return err == ESP_OK && buf[0] != '\0';
}

esp_err_t securacv_ota_set_channel(securacv_ota_channel_t channel)
{
    if (channel != SECURACV_OTA_CHANNEL_RELEASE &&
        channel != SECURACV_OTA_CHANNEL_DEV) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_store_u8(NVS_KEY_CHANNEL, (uint8_t)channel);
}

securacv_ota_channel_t securacv_ota_get_channel(void)
{
    // Anything unrecognized in NVS reads as RELEASE: fail toward the
    // signed stable channel, never toward dev.
    return nvs_load_u8(NVS_KEY_CHANNEL, 0) == 1 ? SECURACV_OTA_CHANNEL_DEV
                                                : SECURACV_OTA_CHANNEL_RELEASE;
}

esp_err_t securacv_ota_set_auto_update(bool enabled)
{
    return nvs_store_u8(NVS_KEY_AUTO_UPDATE, enabled ? 1 : 0);
}

bool securacv_ota_get_auto_update(void)
{
    return nvs_load_u8(NVS_KEY_AUTO_UPDATE, 0) != 0;
}

esp_err_t securacv_ota_set_local_http_allowed(bool allowed)
{
    return nvs_store_u8(NVS_KEY_HTTP_LOCAL, allowed ? 1 : 0);
}

bool securacv_ota_get_local_http_allowed(void)
{
    return nvs_load_u8(NVS_KEY_HTTP_LOCAL, 0) != 0;
}

esp_err_t securacv_ota_mark_pending_install(const char *version)
{
    if (version == NULL || version[0] == '\0') return ESP_ERR_INVALID_ARG;
    return nvs_store_str(NVS_KEY_PENDING_VER, version);
}

bool securacv_ota_take_pending_version(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) return false;
    buf[0] = '\0';
    if (nvs_load_str(NVS_KEY_PENDING_VER, buf, buf_len) == ESP_OK && buf[0] != '\0') {
        nvs_store_str(NVS_KEY_PENDING_VER, NULL);  // erase: consume exactly once
        return true;
    }

    // Migration: firmware released before this engine recorded the target
    // in the Preferences "securacv" namespace as "ota_target" (written by
    // the OLD image at reboot time). Honor it once so the very first OTA
    // out of a pre-migration build still gets its outcome witnessed.
    {
        nvs_handle_t handle;
        if (nvs_open("securacv", NVS_READWRITE, &handle) == ESP_OK) {
            size_t len = buf_len;
            if (nvs_get_str(handle, "ota_target", buf, &len) == ESP_OK && buf[0] != '\0') {
                nvs_erase_key(handle, "ota_target");
                nvs_commit(handle);
                nvs_close(handle);
                return true;
            }
            nvs_close(handle);
        }
    }
    buf[0] = '\0';
    return false;
}

/**
 * @brief Persist a minimum acceptable firmware version in NVS.
 *
 * The stored version is a monotonically increasing floor. Any OTA
 * payload whose version is <= this floor is rejected, even if a
 * validly-signed older firmware is presented after a physical rollback.
 */
static esp_err_t nvs_store_min_version(const char *version_str)
{
    return nvs_store_str(NVS_KEY_MIN_VERSION, version_str);
}

static esp_err_t nvs_load_min_version(char *buf, size_t buf_len)
{
    return nvs_load_str(NVS_KEY_MIN_VERSION, buf, buf_len);
}

/** True while this boot's image is still awaiting self-test confirmation. */
static bool running_image_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

/**
 * @brief Raise the monotonic anti-rollback floor to @p version (if higher).
 *
 * Must only be called for a CONFIRMED image — raising the floor while the
 * running image is still pending self-test would, after a rollback,
 * permanently block re-offering that version.
 */
static void ota_raise_floor_to(const char *version)
{
    if (version == NULL || version[0] == '\0') return;

    char stored[16] = {0};
    esp_err_t err = nvs_load_min_version(stored, sizeof(stored));
    if (err != ESP_OK || securacv_version_compare(version, stored) > 0) {
        nvs_store_min_version(version);
        ESP_LOGI(TAG, "NVS min firmware version updated to %s", version);
    } else {
        ESP_LOGI(TAG, "NVS min firmware version: %s", stored);
    }
}

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void ota_task(void *arg);
static esp_err_t ota_fetch_manifest(void);
static esp_err_t ota_parse_manifest(const char *json_data);
static esp_err_t ota_download_and_flash(void);
static esp_err_t ota_verify_sha256(const esp_partition_t *partition, size_t image_size,
                                   const char *expected_hex, uint8_t computed_out[32]);
static void ota_set_state(securacv_ota_state_t state);
static void ota_set_error(securacv_ota_error_t error);
static void ota_report_progress(uint8_t percent);

static bool pubkey_provisioned(const uint8_t *pubkey)
{
    if (pubkey == NULL) return false;
    for (int i = 0; i < 32; i++) {
        if (pubkey[i] != 0) return true;
    }
    return false;
}

// ============================================================================
// PUBLIC API - INITIALIZATION
// ============================================================================

esp_err_t securacv_ota_init(const securacv_ota_config_t *config)
{
    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "OTA already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL || config->manifest_url == NULL ||
        config->product == NULL || config->current_version == NULL) {
        ESP_LOGE(TAG, "Invalid configuration: product, current_version and manifest_url required");
        return ESP_ERR_INVALID_ARG;
    }

    // Preserve self-tests registered before init.
    securacv_selftest_t saved_tests[MAX_SELF_TESTS];
    size_t saved_num = s_ctx.num_self_tests;
    memcpy(saved_tests, s_ctx.self_tests, sizeof(saved_tests));

    memset(&s_ctx, 0, sizeof(s_ctx));
    memcpy(s_ctx.self_tests, saved_tests, sizeof(saved_tests));
    s_ctx.num_self_tests = saved_num;

    s_ctx.config = *config;

    if (s_ctx.config.http_timeout_ms == 0) {
        s_ctx.config.http_timeout_ms = 30000;
    }
    if (s_ctx.config.download_buffer_size == 0) {
        s_ctx.config.download_buffer_size = 4096;
    }

    s_ctx.mutex = xSemaphoreCreateMutex();
    if (s_ctx.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_ctx.state = SECURACV_OTA_IDLE;
    s_ctx.initialized = true;

    // Persist the currently-running version as the NVS minimum floor.
    // Monotonic: once v2.2 runs, NVS records 2.2 and any future OTA to
    // <= 2.2 is rejected. Deferred while this boot's image is still
    // pending self-test: if the self-test later fails and the device
    // rolls back, a pre-raised floor would block ever retrying that
    // version. securacv_ota_boot_self_test() raises it on confirmation.
    if (running_image_pending_verify()) {
        ESP_LOGI(TAG, "Image pending self-test — anti-rollback floor raise deferred");
    } else {
        ota_raise_floor_to(s_ctx.config.current_version);
    }

    if (!pubkey_provisioned(s_ctx.config.release_pubkey)) {
        ESP_LOGW(TAG, "Release public key is all zeros — OTA installs are disabled (fail-closed)");
    }

    ESP_LOGI(TAG, "OTA engine initialized");
    ESP_LOGI(TAG, "  Product: %s", s_ctx.config.product);
    ESP_LOGI(TAG, "  Running version: %s", s_ctx.config.current_version);
    ESP_LOGI(TAG, "  Default manifest URL: %s", s_ctx.config.manifest_url);

    return ESP_OK;
}

esp_err_t securacv_ota_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_OK;
    }

    securacv_ota_abort();

    // Wait for the worker to notice the abort flag and exit (it clears
    // task_handle on its way out). Force-deleting a task mid-TLS-handshake
    // or mid-flash-write would leak or corrupt, and deleting the mutex
    // under a live task is undefined behavior — so if the task doesn't
    // exit in time (it may be blocked in a network call for up to the
    // HTTP timeout), refuse the teardown and let the caller retry.
    int wait_ms = 5000;
    while (s_ctx.task_handle != NULL && wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms -= 50;
    }
    if (s_ctx.task_handle != NULL) {
        ESP_LOGW(TAG, "OTA task still running — deinit refused, retry after it finishes");
        return ESP_ERR_TIMEOUT;
    }

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

static esp_err_t ota_start_task(bool install_mode)
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

    s_ctx.install_mode = install_mode;
    s_ctx.task_should_abort = false;

    xSemaphoreGive(s_ctx.mutex);

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

    ESP_LOGI(TAG, "OTA %s started", install_mode ? "check and install" : "check");
    return ESP_OK;
}

esp_err_t securacv_ota_check(void)
{
    return ota_start_task(false);
}

esp_err_t securacv_ota_check_and_install(void)
{
    return ota_start_task(true);
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

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not get OTA state: %s", esp_err_to_name(err));
        // Not an OTA partition or error - continue normally
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Running partition: %s, OTA state: %d", running->label, ota_state);

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New OTA firmware pending validation");

        bool all_passed = true;
        bool any_required_failed = false;

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

            esp_ota_mark_app_invalid_rollback_and_reboot();

            // Should not reach here - device will reboot
            return ESP_FAIL;
        }

        if (!all_passed) {
            ESP_LOGW(TAG, "Some optional self-tests failed, but continuing");
        }

        ESP_LOGI(TAG, "Self-tests passed - marking firmware as valid");
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
            return err;
        }

        s_just_updated = true;
        // The image is confirmed — NOW raise the anti-rollback floor.
        // When init() ran first (canary order), it deferred this while
        // we were pending verify; when init() runs after (WAP order),
        // current_version is still NULL here and init() raises it.
        ota_raise_floor_to(s_ctx.config.current_version);
        ESP_LOGI(TAG, "OTA validation complete - firmware confirmed");
    } else if (ota_state == ESP_OTA_IMG_VALID) {
        ESP_LOGI(TAG, "Firmware already validated");
    } else {
        ESP_LOGI(TAG, "No pending OTA validation needed");
    }

    return ESP_OK;
}

bool securacv_ota_just_updated(void)
{
    return s_just_updated;
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
    return (s_ctx.config.current_version != NULL) ? s_ctx.config.current_version : "0.0.0";
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

uint32_t securacv_ota_get_last_check_time(void)
{
    return s_ctx.last_check_time;
}

// ============================================================================
// INTERNAL - OTA TASK
// ============================================================================

/**
 * @brief Main OTA task function
 *
 * 1. Fetch manifest (with retry/backoff)
 * 2. Parse and validate manifest (product, signature fields)
 * 3. Compare versions against the anti-rollback floor
 * 4. Gate on can_install (if install_mode)
 * 5. Download firmware
 * 6. Verify SHA-256, then the Ed25519 release signature
 * 7. Commit to the inactive OTA partition
 * 8. Flush device state and reboot (if auto_reboot)
 */
static void ota_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "OTA task started");

    ota_set_state(SECURACV_OTA_CHECKING);

    esp_err_t err = ota_fetch_manifest();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Manifest fetch failed");
        goto task_exit;
    }

    if (s_ctx.task_should_abort) {
        ESP_LOGI(TAG, "OTA aborted by user");
        ota_set_state(SECURACV_OTA_IDLE);
        goto task_exit;
    }

    s_ctx.last_check_time = (uint32_t)time(NULL);

    // Anti-rollback: manifest version against max(running, NVS floor).
    {
        char nvs_min[16] = {0};
        nvs_load_min_version(nvs_min, sizeof(nvs_min));

        securacv_ota_decision_t decision = securacv_ota_update_decision(
            s_ctx.manifest.version, s_ctx.config.current_version, nvs_min);

        switch (decision) {
            case SECURACV_OTA_DECISION_UPDATE:
                ESP_LOGI(TAG, "Update available: %s -> %s",
                         s_ctx.config.current_version, s_ctx.manifest.version);
                s_ctx.update_available = true;
                break;
            case SECURACV_OTA_DECISION_UP_TO_DATE:
                ESP_LOGI(TAG, "Already running latest version: %s", s_ctx.manifest.version);
                s_ctx.update_available = false;
                if (!s_ctx.config.skip_version_check) {
                    s_ctx.last_error = SECURACV_OTA_ERR_NONE;  // Up to date is not an error
                    ota_set_state(SECURACV_OTA_IDLE);
                    goto task_exit;
                }
                break;
            case SECURACV_OTA_DECISION_ROLLBACK:
                ESP_LOGW(TAG, "SECURITY: Version rollback rejected: %s", s_ctx.manifest.version);
                s_ctx.update_available = false;
                ota_set_error(SECURACV_OTA_ERR_VERSION_ROLLBACK);
                goto task_exit;
        }
    }

    // min_version is informational for gap-skips: log if we're below it.
    if (strlen(s_ctx.manifest.min_version) > 0) {
        if (securacv_version_compare(s_ctx.config.current_version,
                                     s_ctx.manifest.min_version) < 0) {
            ESP_LOGW(TAG, "Running version %s is below manifest min_version %s",
                     s_ctx.config.current_version, s_ctx.manifest.min_version);
        }
    }

    if (!s_ctx.install_mode) {
        ESP_LOGI(TAG, "Check complete - install not requested");
        ota_set_state(SECURACV_OTA_IDLE);
        goto task_exit;
    }

    // Fail-closed: an unprovisioned key means installs are disabled.
    if (!pubkey_provisioned(s_ctx.config.release_pubkey)) {
        ESP_LOGE(TAG, "Install refused: release public key not provisioned");
        ota_set_error(SECURACV_OTA_ERR_PUBKEY_MISSING);
        goto task_exit;
    }

    // The manifest must carry a signature.
    if (strlen(s_ctx.manifest.signature) != 128) {
        ESP_LOGE(TAG, "Install refused: manifest has no valid signature field");
        ota_set_error(SECURACV_OTA_ERR_SIGNATURE_INVALID);
        goto task_exit;
    }

    // Give the device a veto before we spend bandwidth and flash cycles.
    if (s_ctx.config.can_install != NULL) {
        char reason[96] = {0};
        if (!s_ctx.config.can_install(reason, sizeof(reason))) {
            ESP_LOGW(TAG, "Install deferred by device: %s", reason);
            ota_set_error(SECURACV_OTA_ERR_DEFERRED);
            goto task_exit;
        }
    }

    err = ota_download_and_flash();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Download/flash failed");
        goto task_exit;
    }

    if (s_ctx.config.auto_reboot) {
        // Conditions can change during a long download — re-check the veto
        // before pulling the rug out.
        if (s_ctx.config.can_install != NULL) {
            char reason[96] = {0};
            if (!s_ctx.config.can_install(reason, sizeof(reason))) {
                ESP_LOGW(TAG, "Reboot deferred by device: %s — update staged, reboot to apply", reason);
                ota_set_error(SECURACV_OTA_ERR_DEFERRED);
                goto task_exit;
            }
        }

        ota_set_state(SECURACV_OTA_REBOOTING);
        ESP_LOGI(TAG, "OTA complete - rebooting in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (s_ctx.config.on_before_reboot != NULL) {
            s_ctx.config.on_before_reboot();
        }
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

typedef struct {
    char *body;
    int body_len;
    char etag[sizeof(s_ctx.etag)];
} manifest_response_t;

/**
 * @brief HTTP event handler for manifest fetch
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    manifest_response_t *resp = (manifest_response_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (resp != NULL && evt->header_key != NULL && evt->header_value != NULL &&
                strcasecmp(evt->header_key, "ETag") == 0) {
                strncpy(resp->etag, evt->header_value, sizeof(resp->etag) - 1);
                resp->etag[sizeof(resp->etag) - 1] = '\0';
            }
            break;

        case HTTP_EVENT_ON_DATA:
            if (resp == NULL) break;
            if (resp->body == NULL) {
                resp->body = (char *)malloc(HTTP_BUFFER_SIZE);
                if (resp->body == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate HTTP buffer");
                    return ESP_ERR_NO_MEM;
                }
                resp->body_len = 0;
            }
            if (resp->body_len + evt->data_len < HTTP_BUFFER_SIZE - 1) {
                memcpy(resp->body + resp->body_len, evt->data, evt->data_len);
                resp->body_len += evt->data_len;
                resp->body[resp->body_len] = '\0';
            }
            break;

        default:
            break;
    }

    return ESP_OK;
}

/**
 * @brief Single manifest fetch attempt.
 * @return ESP_OK (parsed), ESP_ERR_NOT_MODIFIED-like via out flag, or error.
 */
static esp_err_t ota_fetch_manifest_once(const char *url, bool *not_modified)
{
    manifest_response_t resp = {};
    esp_err_t err;
    *not_modified = false;

    esp_http_client_config_t http_config = {};
    http_config.url = url;
    http_config.event_handler = http_event_handler;
    http_config.user_data = &resp;
    http_config.timeout_ms = (int)s_ctx.config.http_timeout_ms;
    http_config.disable_auto_redirect = false;

    if (s_ctx.config.server_cert_pem != NULL) {
        http_config.cert_pem = s_ctx.config.server_cert_pem;
    }
    else {
        http_config.crt_bundle_attach = scv_crt_bundle_attach;
    }

#ifdef SECURACV_OTA_SKIP_CERT_VERIFY
    http_config.skip_cert_common_name_check = true;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    // Conditional GET: cheap 24-hour checks when nothing changed. Only sent
    // while we still hold the manifest the ETag corresponds to (RAM cache).
    if (s_ctx.etag[0] != '\0' && s_ctx.manifest_valid) {
        esp_http_client_set_header(client, "If-None-Match", s_ctx.etag);
    }

    err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Manifest HTTP status: %d", status);

        if (status == 304 && s_ctx.manifest_valid) {
            *not_modified = true;
            err = ESP_OK;
        } else if (status == 200 && resp.body != NULL) {
            err = ota_parse_manifest(resp.body);
            if (err == ESP_OK) {
                strncpy(s_ctx.etag, resp.etag, sizeof(s_ctx.etag) - 1);
                s_ctx.etag[sizeof(s_ctx.etag) - 1] = '\0';
            }
        } else {
            ESP_LOGE(TAG, "Manifest request failed: status=%d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Manifest request failed: %s", esp_err_to_name(err));
    }

    if (resp.body != NULL) {
        free(resp.body);
    }

    esp_http_client_cleanup(client);
    return err;
}

/**
 * @brief Fetch manifest with transport policy check and retry/backoff
 */
static esp_err_t ota_fetch_manifest(void)
{
    char url[sizeof(s_ctx.manifest.url)];
    if (securacv_ota_get_manifest_url(url, sizeof(url)) != ESP_OK) {
        ota_set_error(SECURACV_OTA_ERR_MANIFEST_FETCH);
        return ESP_FAIL;
    }

    if (!securacv_ota_url_allowed(url, securacv_ota_get_local_http_allowed())) {
        ESP_LOGE(TAG, "Manifest URL rejected by transport policy: %s", url);
        ota_set_error(SECURACV_OTA_ERR_URL_POLICY);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Fetching manifest from: %s", url);

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < MANIFEST_FETCH_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            // Exponential backoff with jitter: ~1s, ~2s (+0..500ms).
            uint32_t delay_ms = (1000U << (attempt - 1)) + (esp_random() % 500);
            ESP_LOGI(TAG, "Retrying manifest fetch in %lu ms (attempt %d/%d)",
                     (unsigned long)delay_ms, attempt + 1, MANIFEST_FETCH_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            if (s_ctx.task_should_abort) return ESP_FAIL;
        }

        bool not_modified = false;
        err = ota_fetch_manifest_once(url, &not_modified);
        if (err == ESP_OK) {
            if (not_modified) {
                ESP_LOGI(TAG, "Manifest unchanged since last check (304)");
            }
            return ESP_OK;
        }
    }

    ota_set_error(SECURACV_OTA_ERR_MANIFEST_FETCH);
    return err;
}

/**
 * @brief Parse manifest JSON (schema v1, see ota_release.py)
 */
static esp_err_t ota_parse_manifest(const char *json_data)
{
    JsonDocument doc;
    DeserializationError parse_err = deserializeJson(doc, json_data);
    if (parse_err) {
        ESP_LOGE(TAG, "Failed to parse manifest JSON: %s", parse_err.c_str());
        ota_set_error(SECURACV_OTA_ERR_MANIFEST_PARSE);
        return ESP_FAIL;
    }

    // Reset manifest
    memset(&s_ctx.manifest, 0, sizeof(s_ctx.manifest));
    s_ctx.manifest_valid = false;
    s_ctx.etag[0] = '\0';

    const char *product = doc["product"];
    const char *version = doc["version"];
    const char *url = doc["url"];
    const char *sha256 = doc["sha256"];

    if (product == NULL || version == NULL || url == NULL || sha256 == NULL) {
        ESP_LOGE(TAG, "Manifest missing required fields");
        ota_set_error(SECURACV_OTA_ERR_MANIFEST_INVALID);
        return ESP_FAIL;
    }

    if (strcmp(product, s_ctx.config.product) != 0) {
        ESP_LOGE(TAG, "Product mismatch: expected %s, got %s",
                 s_ctx.config.product, product);
        ota_set_error(SECURACV_OTA_ERR_MANIFEST_INVALID);
        return ESP_FAIL;
    }

    strncpy(s_ctx.manifest.product, product, sizeof(s_ctx.manifest.product) - 1);
    strncpy(s_ctx.manifest.version, version, sizeof(s_ctx.manifest.version) - 1);
    strncpy(s_ctx.manifest.url, url, sizeof(s_ctx.manifest.url) - 1);
    strncpy(s_ctx.manifest.sha256, sha256, sizeof(s_ctx.manifest.sha256) - 1);

    const char *min_version = doc["min_version"];
    if (min_version != NULL) {
        strncpy(s_ctx.manifest.min_version, min_version, sizeof(s_ctx.manifest.min_version) - 1);
    }

    s_ctx.manifest.size = doc["size"] | 0U;

    const char *signature = doc["signature"];
    if (signature != NULL) {
        strncpy(s_ctx.manifest.signature, signature, sizeof(s_ctx.manifest.signature) - 1);
    }

    const char *manifest_signature = doc["manifest_signature"];
    if (manifest_signature != NULL) {
        strncpy(s_ctx.manifest.manifest_signature, manifest_signature,
                sizeof(s_ctx.manifest.manifest_signature) - 1);
    }

    const char *signing_key_id = doc["signing_key_id"];
    if (signing_key_id != NULL) {
        strncpy(s_ctx.manifest.signing_key_id, signing_key_id,
                sizeof(s_ctx.manifest.signing_key_id) - 1);
    }

    const char *release_notes = doc["release_notes"];
    if (release_notes != NULL) {
        strncpy(s_ctx.manifest.release_notes, release_notes,
                sizeof(s_ctx.manifest.release_notes) - 1);
    }

    const char *release_url = doc["release_url"];
    if (release_url != NULL) {
        strncpy(s_ctx.manifest.release_url, release_url,
                sizeof(s_ctx.manifest.release_url) - 1);
    }

    // Manifest signature: nothing in this manifest — not the version shown
    // in Home Assistant, not the release notes, not the download URL — is
    // trusted until the metadata itself verifies against the release key.
    // Without this, a hostile local mirror could forge what users read or
    // where the device fetches. Skipped only on unprovisioned (all-zero
    // key) dev builds, which cannot install anything anyway.
    if (pubkey_provisioned(s_ctx.config.release_pubkey)) {
        uint8_t msig[64];
        uint8_t msg[SECURACV_OTA_MANIFEST_MSG_MAX];
        size_t msg_len = 0;
        if (!securacv_ota_hex_to_bytes(s_ctx.manifest.manifest_signature, msig, sizeof(msig)) ||
            !securacv_ota_build_manifest_message(&s_ctx.manifest, msg, sizeof(msg), &msg_len) ||
            !Ed25519::verify(msig, s_ctx.config.release_pubkey, msg, msg_len)) {
            ESP_LOGE(TAG, "SECURITY: manifest signature missing or invalid — manifest rejected");
            memset(&s_ctx.manifest, 0, sizeof(s_ctx.manifest));
            ota_set_error(SECURACV_OTA_ERR_MANIFEST_SIG);
            return ESP_FAIL;
        }
    }

    s_ctx.manifest_valid = true;

    ESP_LOGI(TAG, "Manifest parsed successfully:");
    ESP_LOGI(TAG, "  Product: %s", s_ctx.manifest.product);
    ESP_LOGI(TAG, "  Version: %s", s_ctx.manifest.version);
    ESP_LOGI(TAG, "  Size: %lu bytes", (unsigned long)s_ctx.manifest.size);
    ESP_LOGI(TAG, "  Signed: %s (key %s)",
             s_ctx.manifest.signature[0] ? "yes" : "NO",
             s_ctx.manifest.signing_key_id[0] ? s_ctx.manifest.signing_key_id : "?");

    return ESP_OK;
}

// ============================================================================
// INTERNAL - DOWNLOAD, VERIFY, FLASH
// ============================================================================

/**
 * @brief Download firmware, verify SHA-256 + Ed25519, commit to OTA partition
 */
static esp_err_t ota_download_and_flash(void)
{
    esp_err_t err;
    esp_https_ota_handle_t https_ota_handle = NULL;
    size_t image_size = 0;

    // The binary URL inherits the same transport policy as the manifest.
    if (!securacv_ota_url_allowed(s_ctx.manifest.url,
                                  securacv_ota_get_local_http_allowed())) {
        ESP_LOGE(TAG, "Firmware URL rejected by transport policy: %s", s_ctx.manifest.url);
        ota_set_error(SECURACV_OTA_ERR_URL_POLICY);
        return ESP_FAIL;
    }

    ota_set_state(SECURACV_OTA_DOWNLOADING);
    ota_report_progress(0);

    ESP_LOGI(TAG, "Downloading firmware from: %s", s_ctx.manifest.url);

    esp_http_client_config_t http_config = {};
    http_config.url = s_ctx.manifest.url;
    http_config.timeout_ms = (int)s_ctx.config.http_timeout_ms;
    http_config.buffer_size = (int)s_ctx.config.download_buffer_size;
    http_config.buffer_size_tx = 1024;

    if (s_ctx.config.server_cert_pem != NULL) {
        http_config.cert_pem = s_ctx.config.server_cert_pem;
    }
    else {
        http_config.crt_bundle_attach = scv_crt_bundle_attach;
    }

#ifdef SECURACV_OTA_SKIP_CERT_VERIFY
    http_config.skip_cert_common_name_check = true;
#endif

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;
    ota_config.partial_http_download = true;
    ota_config.max_http_request_size = (int)s_ctx.config.download_buffer_size;

    err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        ota_set_error(SECURACV_OTA_ERR_DOWNLOAD_FAILED);
        return err;
    }

    esp_app_desc_t new_app_info;
    err = esp_https_ota_get_img_desc(https_ota_handle, &new_app_info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "New firmware: %s (version %s)",
                 new_app_info.project_name, new_app_info.version);
    }

    int total_size = (s_ctx.manifest.size > 0) ? (int)s_ctx.manifest.size : 1;

    int bytes_read = 0;
    int last_progress = 0;

    while (1) {
        if (s_ctx.task_should_abort) {
            ESP_LOGI(TAG, "Download aborted by user");
            esp_https_ota_abort(https_ota_handle);
            ota_set_state(SECURACV_OTA_IDLE);
            return ESP_ERR_TIMEOUT;
        }

        err = esp_https_ota_perform(https_ota_handle);

        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            bytes_read = esp_https_ota_get_image_len_read(https_ota_handle);
            int progress = (int)(((int64_t)bytes_read * 100) / total_size);
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
            break;
        }

        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
        ota_set_error(SECURACV_OTA_ERR_DOWNLOAD_FAILED);
        return err;
    }

    image_size = (size_t)esp_https_ota_get_image_len_read(https_ota_handle);
    ESP_LOGI(TAG, "Download complete: %zu bytes", image_size);

    ota_set_state(SECURACV_OTA_VERIFYING);

    // The signature covers (size || sha256) — the size must match exactly.
    if (s_ctx.manifest.size > 0 && image_size != s_ctx.manifest.size) {
        ESP_LOGE(TAG, "Image size mismatch: manifest=%lu, downloaded=%zu",
                 (unsigned long)s_ctx.manifest.size, image_size);
        esp_https_ota_abort(https_ota_handle);
        ota_set_error(SECURACV_OTA_ERR_SIZE_MISMATCH);
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        esp_https_ota_abort(https_ota_handle);
        ota_set_error(SECURACV_OTA_ERR_PARTITION);
        return ESP_FAIL;
    }

    uint8_t computed_sha[SHA256_DIGEST_LENGTH];
    err = ota_verify_sha256(update_partition, image_size, s_ctx.manifest.sha256, computed_sha);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHA-256 verification failed!");
        esp_https_ota_abort(https_ota_handle);
        ota_set_error(SECURACV_OTA_ERR_SHA256_MISMATCH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SHA-256 verification passed");

    // Ed25519 release signature over (image_size_LE32 || sha256) — the same
    // message and key as BLE OTA, verified against the computed digest of
    // the actual flash contents (not the manifest's claim).
    {
        uint8_t signature[64];
        if (!securacv_ota_hex_to_bytes(s_ctx.manifest.signature, signature, sizeof(signature))) {
            ESP_LOGE(TAG, "Manifest signature is not valid hex");
            esp_https_ota_abort(https_ota_handle);
            ota_set_error(SECURACV_OTA_ERR_SIGNATURE_INVALID);
            return ESP_FAIL;
        }

        uint8_t signed_msg[36];
        securacv_ota_build_signed_message((uint32_t)image_size, computed_sha, signed_msg);

        if (!Ed25519::verify(signature, s_ctx.config.release_pubkey,
                             signed_msg, sizeof(signed_msg))) {
            ESP_LOGE(TAG, "SECURITY: Ed25519 release signature verification FAILED");
            esp_https_ota_abort(https_ota_handle);
            ota_set_error(SECURACV_OTA_ERR_SIGNATURE_INVALID);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Ed25519 release signature verified");

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

    // The boot partition is flipped from this point on — even if the reboot
    // is deferred (battery gate) or comes later for an unrelated reason, the
    // next start boots the new image. Record the target version NOW so the
    // next boot can always tell applied from rolled-back.
    securacv_ota_mark_pending_install(s_ctx.manifest.version);

    ESP_LOGI(TAG, "OTA update written successfully");
    return ESP_OK;
}

/**
 * @brief Verify SHA-256 hash of downloaded firmware against the manifest,
 *        and return the computed digest for signature verification.
 */
static esp_err_t ota_verify_sha256(const esp_partition_t *partition, size_t image_size,
                                   const char *expected_hex, uint8_t computed_out[32])
{
    ESP_LOGI(TAG, "Verifying SHA-256...");

    uint8_t expected[SHA256_DIGEST_LENGTH];
    if (!securacv_ota_hex_to_bytes(expected_hex, expected, SHA256_DIGEST_LENGTH)) {
        ESP_LOGE(TAG, "Invalid SHA-256 hex in manifest");
        return ESP_FAIL;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256, not SHA-224

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

    mbedtls_sha256_finish(&ctx, computed_out);
    mbedtls_sha256_free(&ctx);

    if (memcmp(computed_out, expected, SHA256_DIGEST_LENGTH) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch!");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, expected, SHA256_DIGEST_LENGTH, ESP_LOG_ERROR);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, computed_out, SHA256_DIGEST_LENGTH, ESP_LOG_ERROR);
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

    if (s_ctx.config.on_progress != NULL) {
        s_ctx.config.on_progress(s_ctx.state, percent, SECURACV_OTA_ERR_NONE,
                                 s_ctx.config.user_data);
    }
}

#endif // !SECURACV_OTA_HOST_BUILD
