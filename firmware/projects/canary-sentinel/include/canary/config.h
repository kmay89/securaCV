#pragma once
#include <stdint.h>
#include <cstddef>

// Project-level composition header for canary-sentinel. Pulls in the active
// preset (configs/canary-sentinel/<preset>/config.h via -I, the SENT_*/FEATURE_
// macros) and derives the housekeeping constants the loop consumes. Mirrors the
// role canary-sense's include/canary/config.h plays so the two projects' net
// layers stay comparable when the Phase 1 net stack is wired in.
//
// Angle brackets on purpose (same trick as canary-sense): a quoted include
// would find THIS file first (also named config.h); <config.h> skips the
// current directory and resolves via -I straight to the preset.
#include <config.h>

// -------------------- Identity --------------------
static constexpr const char* DEVICE_TYPE  = SENT_DEVICE_TYPE;
static constexpr const char* DEVICE_ID    = SENT_DEVICE_ID;  // first-boot seed only
static constexpr const char* MANUFACTURER = SENT_MANUFACTURER;
static constexpr const char* MODEL        = SENT_MODEL;
static constexpr const char* TIER         = SENT_TIER;

// -------------------- Timing --------------------
static constexpr uint32_t HEARTBEAT_MS        = SENT_HEARTBEAT_MS;
static constexpr uint32_t WATCHDOG_TIMEOUT_SEC = SENT_WATCHDOG_TIMEOUT_SEC;

// Per-channel sample cadences. The fusion tick runs at the fastest of these;
// each channel only re-observes on its own cadence and decays between.
static constexpr uint32_t FUSION_TICK_MS  = 100;   // engine evaluate() cadence
static constexpr uint32_t PIR_POLL_MS     = 100;
static constexpr uint32_t RADAR_POLL_MS   = 100;
static constexpr uint32_t LIGHT_SAMPLE_MS = 1000;
static constexpr uint32_t RF_SCAN_MS      = 10000;  // aggregate WiFi/BLE scan window
static constexpr uint32_t CSI_WINDOW_MS   = 1000;   // CSI 1 Hz feature window

// -------------------- Signed witness / publish (Phase 1 hook) --------------------
// The coarse FusionResult transitions are what get signed + published, reusing
// the exact witness + net stack canary-sense proves (common/identity,
// common/witness, MQTT + HA discovery, signed pull-OTA). Phase 0 emits them to
// the console; Phase 1 lights up the network path. Kept as a macro so a distinct
// OTA product string lands here when that phase is wired.
#ifndef SECURACV_OTA_PRODUCT
#define SECURACV_OTA_PRODUCT "securacv-canary-sentinel"
#endif
static constexpr const char* OTA_PRODUCT = SECURACV_OTA_PRODUCT;
