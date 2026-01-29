#pragma once
#include <stdint.h>

// -------------------- Identity --------------------
static constexpr const char* DEVICE_TYPE = "canary_vision";
static constexpr const char* DEVICE_ID   = "canary_vision_001";  // change per unit

static constexpr const char* MANUFACTURER = "SecuraCV";
static constexpr const char* MODEL        = "Canary Vision (ESP32-C3 + Grove Vision AI V2)";

// -------------------- Vision semantics --------------------
// NOTE: PERSON_TARGET is model-dependent in SSCMA.
// Keep as 0 if your loaded model uses class 0 for "person".
static constexpr int PERSON_TARGET = 0;
static constexpr int SCORE_MIN     = 70;       // 0–100
static constexpr uint32_t LOST_TIMEOUT_MS = 1500;

// Dwell
static constexpr uint32_t DWELL_START_MS      = 10'000;
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
