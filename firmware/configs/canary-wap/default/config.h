/**
 * @file config.h
 * @brief Default configuration for Canary WAP
 *
 * This configuration is for the standard Wireless Access Point witness
 * device with full feature set: GPS, SD storage, WiFi AP, mesh network,
 * Bluetooth, and RF presence detection.
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CONFIG_DEVICE_TYPE          "canary_wap"
#define CONFIG_DEVICE_ID_PREFIX     "canary-s3-"
#define CONFIG_MANUFACTURER         "SecuraCV"
#define CONFIG_MODEL                "Canary WAP (ESP32-S3)"

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_SD_STORAGE          1   // Enable SD card storage
#define FEATURE_WIFI_AP             1   // Enable WiFi Access Point
#define FEATURE_WIFI_STA            1   // Enable WiFi Station (connect to home)
#define FEATURE_HTTP_SERVER         1   // Enable HTTP API server
#define FEATURE_CAMERA_PEEK         1   // Enable camera preview streaming
#define FEATURE_TAMPER_GPIO         0   // Enable tamper detection pin
#define FEATURE_WATCHDOG            1   // Enable hardware watchdog
#define FEATURE_STATE_LOG           1   // Log state transitions
#define FEATURE_MESH_NETWORK        1   // Enable Opera mesh network
#define FEATURE_BLUETOOTH           1   // Enable Bluetooth Low Energy
#define FEATURE_RF_PRESENCE         1   // Enable RF presence detection
#define FEATURE_GNSS                1   // Enable GPS/GNSS
#define FEATURE_CHIRP               1   // Enable Chirp community witness network

// ============================================================================
// DEBUG FLAGS
// ============================================================================

#ifndef DEBUG_NMEA
#define DEBUG_NMEA                  0   // Print raw NMEA sentences
#endif
#ifndef DEBUG_CBOR
#define DEBUG_CBOR                  0   // Print CBOR hex dump
#endif
#ifndef DEBUG_CHAIN
#define DEBUG_CHAIN                 0   // Print chain operations
#endif
#ifndef DEBUG_VERIFY
#define DEBUG_VERIFY                0   // Print signature verification
#endif
#ifndef DEBUG_HTTP
#define DEBUG_HTTP                  0   // Print HTTP request details
#endif
#ifndef DEBUG_MESH
#define DEBUG_MESH                  0   // Print mesh network details
#endif
#ifndef DEBUG_BLE
#define DEBUG_BLE                   0   // Print BLE details
#endif
#ifndef DEBUG_CHIRP
#define DEBUG_CHIRP                 0   // Print chirp channel details
#endif

// ============================================================================
// TIMING & PRIVACY
// ============================================================================

#define CONFIG_RECORD_INTERVAL_MS       1000    // Record emission rate
#define CONFIG_TIME_BUCKET_MS           5000    // Time coarsening (privacy)
#define CONFIG_FIX_LOST_TIMEOUT_MS      3000    // GPS fix timeout
#define CONFIG_VERIFY_INTERVAL_SEC      60      // Self-verify interval
#define CONFIG_WATCHDOG_TIMEOUT_SEC     8       // Watchdog timeout
#define CONFIG_SD_PERSIST_INTERVAL      10      // Records between SD persists

// ============================================================================
// WIFI AP CONFIGURATION
// ============================================================================

#define CONFIG_AP_SSID_PREFIX           "SecuraCV-"
#define CONFIG_AP_PASSWORD_DEFAULT      "witness2026"
#define CONFIG_AP_CHANNEL               1
#define CONFIG_AP_MAX_CLIENTS           4
#define CONFIG_AP_HIDDEN                0

// ============================================================================
// HTTP SERVER CONFIGURATION
// ============================================================================

#define CONFIG_HTTP_PORT                80
#define CONFIG_HTTP_MAX_CONNECTIONS     4
#define CONFIG_HTTP_ENABLE_CORS         1
#define CONFIG_HTTP_ENABLE_AUTH         0

// ============================================================================
// GNSS CONFIGURATION
// ============================================================================

#define CONFIG_GNSS_BAUD                9600
#define CONFIG_GNSS_TIMEOUT_MS          10000

// ============================================================================
// MOTION DETECTION (HYSTERESIS)
// ============================================================================

#define CONFIG_MOVING_THRESHOLD_MPS     0.8f    // Speed to detect moving
#define CONFIG_STATIC_THRESHOLD_MPS     0.4f    // Speed to detect stationary
#define CONFIG_SPEED_EMA_ALPHA          0.15f   // EMA smoothing factor
#define CONFIG_STATE_HYSTERESIS_MS      2000    // State change debounce

// ============================================================================
// MESH NETWORK CONFIGURATION
// ============================================================================

#define CONFIG_MESH_CHANNEL             0       // 0 = auto
#define CONFIG_MESH_AUTO_CONNECT        1
#define CONFIG_MESH_DISCOVERABLE        1
#define CONFIG_MESH_HEARTBEAT_MS        30000
#define CONFIG_MESH_DISCOVERY_MS        60000

// ============================================================================
// BLUETOOTH CONFIGURATION
// ============================================================================

#define CONFIG_BLE_DEVICE_NAME          "SecuraCV-Canary"
#define CONFIG_BLE_TX_POWER             0       // dBm
#define CONFIG_BLE_PAIRABLE             1
#define CONFIG_BLE_REQUIRE_BONDING      0
#define CONFIG_BLE_ADV_INTERVAL_MS      100

// ============================================================================
// RF PRESENCE CONFIGURATION
// ============================================================================

#define CONFIG_RF_MODE                  3       // WiFi + BLE
#define CONFIG_RF_SAMPLE_INTERVAL_MS    10000
#define CONFIG_RF_PRESENCE_THRESHOLD    1
#define CONFIG_RF_CROWD_THRESHOLD       10
#define CONFIG_RF_PRESENCE_TIMEOUT_MS   30000

// ============================================================================
// STORAGE CONFIGURATION
// ============================================================================

#define CONFIG_SD_SPI_FREQ_FAST         4000000
#define CONFIG_SD_SPI_FREQ_SLOW         1000000

// ============================================================================
// CHIRP CHANNEL CONFIGURATION
// ============================================================================

#define CONFIG_CHIRP_AUTO_RELAY         1       // Auto-relay validated chirps
#define CONFIG_CHIRP_MIN_URGENCY        0       // Minimum urgency (0=info)

// ============================================================================
// SERIAL/USB CONFIGURATION
// ============================================================================

#define CONFIG_SERIAL_BAUD              115200
#define CONFIG_SERIAL_CDC_WAIT_MS       2500
#define CONFIG_BOOT_BUTTON_HOLD_MS      1200
