/**
 * @file config.h
 * @brief Default configuration for Canary Vision
 *
 * This configuration is for the Vision AI presence detection device
 * using Grove Vision AI V2 sensor and ESP32-C3.
 *
 * What lives here is what something reads (sweep F182): the FEATURE_* flags
 * (feature_sanity.h checks them against the board; the Lab's workshop.json
 * copies them), the watchdog timeout, which this file owns, and a copy of
 * the device type, which the fleet figures read here
 * (canary-local/tools/figures/gen_figures.mjs). The settings it used to
 * repeat are set in the project's own headers:
 * projects/canary-vision/include/canary/config.h (identity, detection and
 * dwell defaults, voxel grid, frame size, timing, MQTT buffer, discovery
 * prefix; the flasher catalog, canary-local/tools/gen_flash.py, reads its
 * detection defaults there) and include/canary/topics.h (the topic
 * patterns). scripts/tests/test_vision_flavor_config_readers.py fails on a
 * CONFIG_* here that nothing reads, and holds the device type equal to
 * config.h's.
 */

#pragma once

// ============================================================================
// DEVICE TYPE (a copy for the fleet figures)
// ============================================================================

// gen_figures.mjs keys each flavor's figure by the `#define ...DEVICE_TYPE`
// in its config.h (walkConfigs); without it the Vision boards serve no device
// type in figures.json. The firmware publishes include/canary/config.h's
// DEVICE_TYPE, which owns the value.
#define CONFIG_DEVICE_TYPE          "canary-vision"  // canonical: lowercase, hyphenated

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_VISION_AI           1   // Enable Vision AI sensor
#define FEATURE_WIFI_STA            1   // Enable WiFi Station
#define FEATURE_MQTT                1   // Enable MQTT publishing
#define FEATURE_HA_DISCOVERY        1   // Enable Home Assistant Discovery
#define FEATURE_WATCHDOG            1   // Enable hardware watchdog
// Wider than wap's 8 s because the loop's worst bounded block is one MQTT
// connect attempt against a dead broker (canary-sense parity).
#define CV_WATCHDOG_TIMEOUT_SEC     30

// Features NOT available on C3
#define FEATURE_SD_STORAGE          0
#define FEATURE_WIFI_AP             0
#define FEATURE_HTTP_SERVER         0
#define FEATURE_CAMERA_PEEK         0
#define FEATURE_MESH_NETWORK        0
#define FEATURE_BLUETOOTH           0   // BLE available but not used
#define FEATURE_RF_PRESENCE         0
#define FEATURE_GNSS                0
