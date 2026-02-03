/**
 * @file version.h
 * @brief Firmware version and protocol version definitions
 *
 * Centralized version information for firmware builds.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// FIRMWARE VERSION
// ============================================================================

#define FW_VERSION_MAJOR    2
#define FW_VERSION_MINOR    0
#define FW_VERSION_PATCH    1

#define FW_VERSION_STRING   "2.0.1"

// Build timestamp (set by build system)
#ifndef FW_BUILD_DATE
#define FW_BUILD_DATE       __DATE__
#endif

#ifndef FW_BUILD_TIME
#define FW_BUILD_TIME       __TIME__
#endif

// ============================================================================
// PROTOCOL VERSIONS
// ============================================================================

// Privacy Witness Kernel protocol
#define PWK_PROTOCOL_VERSION        "pwk:v0.3.0"

// Chain algorithm
#define CHAIN_ALGORITHM             "sha256-domain-sep"

// Signature algorithm
#define SIGNATURE_ALGORITHM         "ed25519"

// Ruleset for verification
#define RULESET_ID                  "securacv:canary:v1.0"

// ============================================================================
// PRODUCT INFO
// ============================================================================

#define PRODUCT_NAME                "SecuraCV Canary"
#define PRODUCT_MANUFACTURER        "SecuraCV"

// Device type identifier
#define DEVICE_TYPE_CANARY          "canary"
#define DEVICE_TYPE_CANARY_VISION   "canary_vision"
#define DEVICE_TYPE_CANARY_WAP      "canary_wap"

// ============================================================================
// VERSION FUNCTIONS
// ============================================================================

/**
 * @brief Get firmware version string
 * @return Version string (e.g., "2.0.1")
 */
const char* fw_version_string(void);

/**
 * @brief Get full version info string
 * @return Full version info with build date
 */
const char* fw_version_full(void);

/**
 * @brief Check if version is at least specified
 * @param major Major version
 * @param minor Minor version
 * @param patch Patch version
 * @return true if current version >= specified
 */
static inline int fw_version_at_least(int major, int minor, int patch) {
    if (FW_VERSION_MAJOR > major) return 1;
    if (FW_VERSION_MAJOR < major) return 0;
    if (FW_VERSION_MINOR > minor) return 1;
    if (FW_VERSION_MINOR < minor) return 0;
    return FW_VERSION_PATCH >= patch;
}

#ifdef __cplusplus
}
#endif
