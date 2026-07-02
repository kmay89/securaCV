/**
 * @file config.h
 * @brief Default configuration for Canary Sense (presence-only)
 *
 * Presence / target-count / lux witness on the Seeed MR60BHA2 kit (XIAO
 * ESP32-C6 host). Vitals are compiled OUT in this flavor — note that the actual
 * vitals build switch is the -DCANARY_SENSE_VITALS build flag set per
 * environment in envs/platformio/canary-sense.ini, NOT this header. The
 * FEATURE_VITALS=0 line here is the human-readable mirror of that build
 * decision and must stay consistent with the env.
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CS_DEVICE_TYPE          "canary-sense"  // canonical; must match HA DEVICE_TYPE_CANARY_SENSE
#define CS_DEVICE_ID            "canary_sense_001"
#define CS_MANUFACTURER         "SecuraCV"
#define CS_MODEL                "Canary Sense (XIAO ESP32-C6 + MR60BHA2)"

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_MMWAVE_RADAR        1   // MR60BHA2 presence/count over UART
#define FEATURE_AMBIENT_LIGHT       1   // BH1750 lux (tamper corroboration)
#define FEATURE_STATUS_LED          1   // WS2812 status/identify
#define FEATURE_WIFI_STA            1   // Enable WiFi Station
#define FEATURE_MQTT                1   // Enable MQTT publishing
#define FEATURE_HA_DISCOVERY        1   // Enable Home Assistant Discovery
#define FEATURE_WATCHDOG            1   // Enable hardware watchdog

// Vitals are OFF in the default flavor. The real gate is -DCANARY_SENSE_VITALS
// at the env level; this is the readable mirror (see wellbeing/config.h).
#define FEATURE_VITALS              0

// Features NOT used by this device
#define FEATURE_VISION_AI           0
#define FEATURE_CAMERA_PEEK         0
#define FEATURE_SD_STORAGE          0
#define FEATURE_WIFI_AP             0
#define FEATURE_HTTP_SERVER         0
#define FEATURE_MESH_NETWORK        0
#define FEATURE_BLUETOOTH           0   // BLE available but not used
#define FEATURE_GNSS                0

// ============================================================================
// RADAR PRESENCE FSM (maps onto securacv::mmwave::PresenceConfig)
// ============================================================================

#define CS_PRESENT_DEBOUNCE_MS  300     // sustained target before "present"
#define CS_CLEAR_TIMEOUT_MS     1500    // no target before "clear"
#define CS_RADAR_STALL_MS       5000    // no frame at all before "unknown"

// Coarse range bands (privacy-safe; raw distance never exported)
#define CS_RANGE_NEAR_CM        150     // <= -> near
#define CS_RANGE_MID_CM         350     // <= -> mid, else far

// ============================================================================
// TIMING
// ============================================================================

#define CS_HEARTBEAT_MS         5000    // Status heartbeat interval

// Task watchdog timeout. Wider than canary-wap's 8 s because this loop's
// worst bounded block (one MQTT connect attempt against a dead broker,
// TCP timeout) can run into double digits; must stay above that.
#define CS_WATCHDOG_TIMEOUT_SEC 30

// ============================================================================
// MQTT CONFIGURATION
// ============================================================================

#define CS_MQTT_BUFFER_BYTES    1536
#define CS_HA_DISCOVERY_PREFIX  "homeassistant"

// Topic patterns (device_id substituted at runtime)
#define CS_TOPIC_EVENTS         "securacv/%s/events"
#define CS_TOPIC_STATE          "securacv/%s/state"
#define CS_TOPIC_STATUS         "securacv/%s/status"
