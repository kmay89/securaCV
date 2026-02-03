/**
 * @file config.h
 * @brief Mobile configuration for Canary WAP
 *
 * Optimized configuration for mobile/portable use with extended
 * battery life and reduced power consumption.
 */

#pragma once

// Include default config as base
#include "../default/config.h"

// ============================================================================
// OVERRIDE DEVICE IDENTITY
// ============================================================================

#undef CONFIG_DEVICE_TYPE
#define CONFIG_DEVICE_TYPE          "canary_wap_mobile"

#undef CONFIG_MODEL
#define CONFIG_MODEL                "Canary WAP Mobile (ESP32-S3)"

// ============================================================================
// POWER OPTIMIZATION OVERRIDES
// ============================================================================

// Reduce record frequency for battery life
#undef CONFIG_RECORD_INTERVAL_MS
#define CONFIG_RECORD_INTERVAL_MS       5000    // 5 seconds (was 1s)

// Increase time bucket for less precise (more private) timestamps
#undef CONFIG_TIME_BUCKET_MS
#define CONFIG_TIME_BUCKET_MS           10000   // 10 seconds (was 5s)

// Reduce mesh activity
#undef CONFIG_MESH_HEARTBEAT_MS
#define CONFIG_MESH_HEARTBEAT_MS        60000   // 1 minute (was 30s)

#undef CONFIG_MESH_DISCOVERY_MS
#define CONFIG_MESH_DISCOVERY_MS        120000  // 2 minutes (was 60s)

// Reduce RF scanning
#undef CONFIG_RF_SAMPLE_INTERVAL_MS
#define CONFIG_RF_SAMPLE_INTERVAL_MS    30000   // 30 seconds (was 10s)

// Reduce BLE advertising frequency
#undef CONFIG_BLE_ADV_INTERVAL_MS
#define CONFIG_BLE_ADV_INTERVAL_MS      500     // 500ms (was 100ms)

// Lower BLE TX power
#undef CONFIG_BLE_TX_POWER
#define CONFIG_BLE_TX_POWER             (-12)   // -12 dBm (was 0)

// ============================================================================
// DISABLE NON-ESSENTIAL FEATURES
// ============================================================================

// Disable camera preview to save power (still captures for witness)
#undef FEATURE_CAMERA_PEEK
#define FEATURE_CAMERA_PEEK             0

// Keep core features enabled
// FEATURE_SD_STORAGE = 1
// FEATURE_WIFI_AP = 1
// FEATURE_HTTP_SERVER = 1
// FEATURE_GNSS = 1
// FEATURE_WATCHDOG = 1
// FEATURE_MESH_NETWORK = 1
// FEATURE_BLUETOOTH = 1
// FEATURE_RF_PRESENCE = 1
