#pragma once
#include <stdint.h>
#include <cstddef>

// -------------------- Identity --------------------
static constexpr const char* DEVICE_TYPE = "canary_vision";
static constexpr const char* DEVICE_ID   = "canary_vision_001";  // change per unit

static constexpr const char* MANUFACTURER = "SecuraCV";
// Board-neutral: the same app ships on ESP32-C3 DevKit, XIAO ESP32-C3,
// and XIAO ESP32-S3 hosts (see firmware/envs/platformio/canary-vision.ini).
static constexpr const char* MODEL        = "Canary Vision (Grove Vision AI V2)";

// -------------------- Vision semantics --------------------
// NOTE: PERSON_TARGET is model-dependent in SSCMA.
// Keep as 0 if your loaded model uses class 0 for "person".
static constexpr int PERSON_TARGET = 0;
static constexpr int SCORE_MIN     = 70;       // 0–100
static constexpr uint32_t LOST_TIMEOUT_MS = 1500;

// Dwell
static constexpr uint32_t DWELL_START_MS      = 10000;
static constexpr uint32_t DWELL_END_GRACE_MS  = 0;

// Interaction heuristic
static constexpr uint32_t INTERACTION_AFTER_LEAVE_WINDOW_MS = 3000;
static constexpr uint32_t ZONE_INTERACTION_MS               = 2500;

// Voxel grid
static constexpr uint8_t VOXEL_COLS = 3;
static constexpr uint8_t VOXEL_ROWS = 3;

// Frame dims (common SSCMA models)
static constexpr int FRAME_W = 240;
static constexpr int FRAME_H = 240;

// Timing
static constexpr uint32_t INVOKE_PERIOD_MS = 100;
static constexpr uint32_t HEARTBEAT_MS     = 5000;

// MQTT / HA
static constexpr const char* HA_DISCOVERY_PREFIX = "homeassistant";
static constexpr size_t MQTT_BUFFER_BYTES        = 1536;  // discovery payloads > 256

// -------------------- Software updates (signed pull-OTA) --------------------
// Shared engine at firmware/common/ota — same manifest format, signature
// scheme, and HA update-entity UX as canary and canary-wap.
// Each host board is a distinct OTA product with its own manifest (the
// XIAO envs override both via build flags), so a manifest for one board
// can never install another board's image — the engine refuses on
// product mismatch.
#ifndef SECURACV_OTA_PRODUCT
#define SECURACV_OTA_PRODUCT "securacv-canary-vision"
#endif
static constexpr const char* OTA_PRODUCT = SECURACV_OTA_PRODUCT;
#ifndef SECURACV_OTA_MANIFEST_URL
#define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-vision.json"
#endif
