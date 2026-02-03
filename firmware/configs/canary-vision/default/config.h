/**
 * @file config.h
 * @brief Default configuration for Canary Vision
 *
 * This configuration is for the Vision AI presence detection device
 * using Grove Vision AI V2 sensor and ESP32-C3.
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CONFIG_DEVICE_TYPE          "canary_vision"
#define CONFIG_DEVICE_ID            "canary_vision_001"
#define CONFIG_MANUFACTURER         "SecuraCV"
#define CONFIG_MODEL                "Canary Vision (ESP32-C3 + Grove Vision AI V2)"

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_VISION_AI           1   // Enable Vision AI sensor
#define FEATURE_WIFI_STA            1   // Enable WiFi Station
#define FEATURE_MQTT                1   // Enable MQTT publishing
#define FEATURE_HA_DISCOVERY        1   // Enable Home Assistant Discovery
#define FEATURE_WATCHDOG            1   // Enable hardware watchdog

// Features NOT available on C3
#define FEATURE_SD_STORAGE          0
#define FEATURE_WIFI_AP             0
#define FEATURE_HTTP_SERVER         0
#define FEATURE_CAMERA_PEEK         0
#define FEATURE_MESH_NETWORK        0
#define FEATURE_BLUETOOTH           0   // BLE available but not used
#define FEATURE_RF_PRESENCE         0
#define FEATURE_GNSS                0

// ============================================================================
// VISION AI CONFIGURATION
// ============================================================================

#define CONFIG_PERSON_TARGET        0       // SSCMA class ID for person
#define CONFIG_SCORE_MIN            70      // Minimum detection confidence
#define CONFIG_LOST_TIMEOUT_MS      1500    // Time before presence lost

// ============================================================================
// DWELL DETECTION
// ============================================================================

#define CONFIG_DWELL_START_MS       10000   // Time to confirm dwelling
#define CONFIG_DWELL_END_GRACE_MS   0       // Grace period before dwell ends

// ============================================================================
// INTERACTION DETECTION
// ============================================================================

#define CONFIG_INTERACTION_AFTER_LEAVE_WINDOW_MS    3000
#define CONFIG_ZONE_INTERACTION_MS                  2500

// ============================================================================
// VOXEL GRID
// ============================================================================

#define CONFIG_VOXEL_COLS           3
#define CONFIG_VOXEL_ROWS           3

// ============================================================================
// FRAME DIMENSIONS
// ============================================================================

#define CONFIG_FRAME_W              240
#define CONFIG_FRAME_H              240

// ============================================================================
// TIMING
// ============================================================================

#define CONFIG_INVOKE_PERIOD_MS     100     // Vision sample interval
#define CONFIG_HEARTBEAT_MS         5000    // Status heartbeat interval

// ============================================================================
// MQTT CONFIGURATION
// ============================================================================

#define CONFIG_MQTT_BUFFER_BYTES    1536
#define CONFIG_HA_DISCOVERY_PREFIX  "homeassistant"

// Topic patterns (device_id substituted at runtime)
#define CONFIG_TOPIC_EVENTS         "securacv/%s/events"
#define CONFIG_TOPIC_STATE          "securacv/%s/state"
#define CONFIG_TOPIC_STATUS         "securacv/%s/status"
