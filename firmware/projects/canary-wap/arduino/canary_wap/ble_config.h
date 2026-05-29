/*
 * SecuraCV Canary — BLE Configuration
 *
 * All BLE constants, UUIDs, chirp protocol definitions, and configuration.
 * No implementation — definitions only.
 *
 * Security Design:
 * - BLE-only (no Bluetooth Classic — reduces binary blob surface)
 * - Never broadcasts identifiable device info (no MAC in cleartext ads)
 * - BLE treated as optional hardware (graceful degradation if absent)
 * - All BLE code gated behind FEATURE_BLE compile flag
 */

#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

#include <Arduino.h>

// ════════════════════════════════════════════════════════════════
// BLE FEATURE FLAGS
// ════════════════════════════════════════════════════════════════
// Set FEATURE_BLE to 0 to compile without ANY BLE code (removes binary blobs)
// When 0, all BLE headers become no-ops
#ifndef FEATURE_BLE
#define FEATURE_BLE           1
#endif

// NimBLE presence check.
//
// A device build that wants BLE but can't see <NimBLEDevice.h> must FAIL
// LOUDLY — silently disabling BLE is the documented past bug (devices shipped
// with "BLE unavailable in this build" because the library wasn't installed).
// Only an explicit host/CI compile-check build (-DSECURACV_HOST_BUILD or the
// CI's -DVALIDATION_BUILD) may stub BLE out to a no-op so off-target / no-radio
// builds still link. A real device build sets neither, so the #error fires.
#if (FEATURE_BLE || (defined(FEATURE_BLUETOOTH) && FEATURE_BLUETOOTH)) && !__has_include(<NimBLEDevice.h>)
  #if defined(SECURACV_HOST_BUILD) || defined(VALIDATION_BUILD)
    #undef  FEATURE_BLE
    #define FEATURE_BLE 0
    #ifdef FEATURE_BLUETOOTH
      #undef  FEATURE_BLUETOOTH
      #define FEATURE_BLUETOOTH 0
    #endif
    #ifdef SECURACV_EMIT_BUILD_BANNER
      #pragma message "NimBLEDevice.h not found — BLE stubbed out (host/validation build)"
    #endif
  #else
    #error "NimBLE-Arduino not found but BLE is enabled. Install it (PlatformIO: lib_deps = h2zero/NimBLE-Arduino@^2.3.8 ; Arduino: arduino-cli lib install \"NimBLE-Arduino\"), or build a non-BLE profile (-DBUILD_PROFILE_MINIMAL), or set -DSECURACV_HOST_BUILD for off-target tests."
  #endif
#endif

// FEATURE_BLE_STATUS gates the BLE GATT status service (ble_status_api.h).
// Requires FEATURE_BLUETOOTH (the NimBLE server it registers on). Auto-
// disable if NimBLE is missing or the BLUETOOTH channel is off.
#if defined(FEATURE_BLE_STATUS) && FEATURE_BLE_STATUS
  #if !defined(FEATURE_BLUETOOTH) || !FEATURE_BLUETOOTH
    #undef  FEATURE_BLE_STATUS
    #define FEATURE_BLE_STATUS 0
  #endif
#endif

// Sub-feature flags (only relevant if FEATURE_BLE == 1)
#define FEATURE_BLE_OPERA     1   // Server/advertising mode (presence)
#define FEATURE_BLE_CHIRP     1   // Broadcast alert mode (connectionless)
#define FEATURE_BLE_NEARBY    1   // Scanner/discovery mode (client)

// ════════════════════════════════════════════════════════════════
// BLE UUIDs — SecuraCV Canary Service
// ════════════════════════════════════════════════════════════════
#define SCV_SERVICE_UUID          "a1b2c3d4-e5f6-7890-abcd-ef0123456001"
#define SCV_CHAR_DEVICE_INFO_UUID "a1b2c3d4-e5f6-7890-abcd-ef0123456002"
#define SCV_CHAR_WITNESS_UUID     "a1b2c3d4-e5f6-7890-abcd-ef0123456003"
#define SCV_CHAR_COMMAND_UUID     "a1b2c3d4-e5f6-7890-abcd-ef0123456004"

// ════════════════════════════════════════════════════════════════
// BLE ADVERTISING / SCAN SETTINGS
// ════════════════════════════════════════════════════════════════
#define BLE_DEVICE_NAME_PREFIX    "SCV-"
#define BLE_ADV_INTERVAL_MS       500
#define BLE_SCAN_INTERVAL         100    // in 0.625ms units
#define BLE_SCAN_WINDOW           99     // in 0.625ms units
#define BLE_SCAN_DURATION_SEC     5
#define BLE_SCAN_PERIOD_SEC       30     // how often to run a scan cycle
#define BLE_ACTIVE_SCAN           true   // false for stealth mode

// ════════════════════════════════════════════════════════════════
// CHIRP PROTOCOL
// ════════════════════════════════════════════════════════════════
#define CHIRP_COMPANY_ID          0xFFFF  // Use 0xFFFF for testing
#define CHIRP_MIN_INTERVAL_MS     10000   // Rate limit: 1 chirp per 10 seconds
#define CHIRP_HEARTBEAT_INTERVAL_MS 300000 // Heartbeat every 5 minutes

// Chirp message types
enum ChirpType : uint8_t {
    CHIRP_ALERT     = 0x01,  // Manual or sensor-triggered alert
    CHIRP_HEARTBEAT = 0x02,  // Periodic "I'm alive" (every 5 min)
    CHIRP_TAMPER    = 0x03,  // Tamper event detected
    CHIRP_WITNESS   = 0x04,  // New witness record created
    CHIRP_BOOT      = 0x05,  // Device just booted
};

// Chirp type to string
inline const char* chirpTypeName(ChirpType t) {
    switch (t) {
        case CHIRP_ALERT:     return "alert";
        case CHIRP_HEARTBEAT: return "heartbeat";
        case CHIRP_TAMPER:    return "tamper";
        case CHIRP_WITNESS:   return "witness";
        case CHIRP_BOOT:      return "boot";
        default:              return "unknown";
    }
}

// ════════════════════════════════════════════════════════════════
// NEARBY DEVICE TRACKING
// ════════════════════════════════════════════════════════════════
#define NEARBY_MAX_CANARIES       20     // Max tracked Canary devices
#define NEARBY_RSSI_HISTORY       10     // RSSI readings to keep per device
#define NEARBY_EXPIRY_SEC         120    // Remove device after 2 min without contact
#define NEARBY_SCAN_TASK_STACK    4096   // FreeRTOS task stack size
#define NEARBY_SCAN_TASK_PRIORITY 1      // Lower than main loop

// ════════════════════════════════════════════════════════════════
// RSSI THRESHOLDS
// ════════════════════════════════════════════════════════════════
#define RSSI_EXCELLENT  -50
#define RSSI_GOOD       -70
#define RSSI_FAIR       -85
#define RSSI_WEAK       -100

// RSSI to human-readable quality
inline const char* rssiQuality(int rssi) {
    if (rssi >= RSSI_EXCELLENT) return "excellent";
    if (rssi >= RSSI_GOOD)      return "good";
    if (rssi >= RSSI_FAIR)      return "fair";
    return "weak";
}

// ════════════════════════════════════════════════════════════════
// CHIRP PAYLOAD STRUCTURE
// ════════════════════════════════════════════════════════════════
// Manufacturer data layout (17 bytes total):
//   [0-1]   Company ID (0xFFFF)           — set by NimBLE
//   [2]     Chirp type (uint8_t)
//   [3-6]   Coarse timestamp (uint32_t, epoch/3600 = hour bucket)
//   [7-14]  Chain hash prefix (8 bytes)
//   [15-16] Device ID prefix (2 bytes)
#define CHIRP_PAYLOAD_SIZE  15  // Excluding 2-byte company ID (handled by NimBLE)

// Nearby Canary device tracking structure
struct NearbyCanary {
    char deviceIdPrefix[5];              // 4-char hex prefix + null
    int8_t rssiHistory[NEARBY_RSSI_HISTORY];
    uint8_t rssiIndex;
    uint8_t rssiCount;
    unsigned long lastSeenMs;
    ChirpType lastChirpType;
    uint8_t chainHashPrefix[8];          // First 8 bytes of their chain hash
    bool active;
};

#endif // BLE_CONFIG_H
