/**
 * @file config.h
 * @brief Wellbeing configuration for Canary Sense (presence + vitals)
 *
 * Adds the P1-gated breathing/heart-rate wellbeing channel on top of the
 * default presence flavor. As with the default, the actual compile-time switch
 * for vitals code is the -DCANARY_SENSE_VITALS build flag set in
 * envs/platformio/canary-sense.ini (the canary-sense-wellbeing env) — this
 * header is the human-readable mirror plus the wellbeing-specific tuning.
 *
 * Privacy reminder (design doc §2.2): vitals are wellbeing signals, NOT medical
 * data. They never enter the sealed log, are never precise-timestamped, ride
 * the health/status channel only, and BPM numerics require the P1 opt-in flag
 * to surface as HA entities. Vitals are hard-suppressed unless exactly one
 * target is present.
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CS_DEVICE_TYPE          "canary_sense"
#define CS_DEVICE_ID            "canary_sense_001"
#define CS_MANUFACTURER         "SecuraCV"
#define CS_MODEL                "Canary Sense Wellbeing (XIAO ESP32-C6 + MR60BHA2)"

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

// Vitals are ON in the wellbeing flavor. The real gate is -DCANARY_SENSE_VITALS
// at the env level (canary-sense-wellbeing); this is the readable mirror.
#define FEATURE_VITALS              1

// P1 opt-in: BPM numerics only surface as HA entities when this is set. The
// P0 "breathing confirmed" binary lock is always available when VITALS is on.
#define FEATURE_VITALS_BPM_P1       1

// Features NOT used by this device
#define FEATURE_VISION_AI           0
#define FEATURE_CAMERA_PEEK         0
#define FEATURE_SD_STORAGE          0
#define FEATURE_WIFI_AP             0
#define FEATURE_HTTP_SERVER         0
#define FEATURE_MESH_NETWORK        0
#define FEATURE_BLUETOOTH           0
#define FEATURE_GNSS                0

// ============================================================================
// RADAR PRESENCE FSM (maps onto securacv::mmwave::PresenceConfig)
// ============================================================================

#define CS_PRESENT_DEBOUNCE_MS  300     // sustained target before "present"
#define CS_CLEAR_TIMEOUT_MS     1500    // no target before "clear"
#define CS_RADAR_STALL_MS       5000    // no frame at all before "unknown"

#define CS_RANGE_NEAR_CM        150     // <= -> near
#define CS_RANGE_MID_CM         350     // <= -> mid, else far

// ============================================================================
// VITALS LOCK FSM (maps onto securacv::mmwave::VitalsConfig)
// ============================================================================

#define CS_VITALS_LOCK_MS       4000    // sustained vitals before "locked"
#define CS_VITALS_LOST_MS       6000    // no vitals before "lost"
#define CS_BREATH_MIN_BPM       6       // plausible breathing band
#define CS_BREATH_MAX_BPM       30
#define CS_HEART_MIN_BPM        40      // plausible heart-rate band
#define CS_HEART_MAX_BPM        130

// ============================================================================
// TIMING
// ============================================================================

#define CS_HEARTBEAT_MS         5000    // Status heartbeat interval

// ============================================================================
// MQTT CONFIGURATION
// ============================================================================

#define CS_MQTT_BUFFER_BYTES    1536
#define CS_HA_DISCOVERY_PREFIX  "homeassistant"

#define CS_TOPIC_EVENTS         "securacv/%s/events"
#define CS_TOPIC_STATE          "securacv/%s/state"
#define CS_TOPIC_STATUS         "securacv/%s/status"
