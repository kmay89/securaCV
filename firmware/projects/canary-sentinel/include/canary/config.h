#pragma once
#include <stdint.h>
#include <cstddef>

// Project-level composition header for canary-sentinel. Pulls in the active
// preset (configs/canary-sentinel/<preset>/config.h via -I, the SENT_*/FEATURE_
// macros) and derives the housekeeping + network constants the loop and the
// net stack consume. Mirrors the role canary-sense's include/canary/config.h
// plays: the Phase 1a net layer (src/net, src/runtime_config.cpp,
// src/diagnostics.cpp) is canary-sense's, pinned byte-identical by
// firmware/scripts/check_sentinel_net_sync.sh, and reads the SAME constant
// names from here that it reads from canary-sense's config.h.
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
static constexpr uint32_t RF_SCAN_MS      = 10000;  // aggregate WiFi/BLE scan window (Phase 1b)
static constexpr uint32_t CSI_WINDOW_MS   = 1000;   // CSI 1 Hz feature window (Phase 1b)

// Health publish cadence (retained; carries the pubkey HA TOFU-pins on,
// plus heap/uptime/firmware). Also published on every broker reconnect.
static constexpr uint32_t HEALTH_PUBLISH_MS = 60000;

// -------------------- MQTT / HA --------------------
// Not preset data: every preset publishes the same topic schema to the same
// discovery prefix, so these live here rather than in configs/.
static constexpr const char* HA_DISCOVERY_PREFIX = "homeassistant";
static constexpr size_t MQTT_BUFFER_BYTES        = 1536;

// -------------------- WiFi robustness / power --------------------
// Same values as canary-sense / canary-vision: the supervisor that consumes
// them (src/net/wifi_mgr.cpp) is canary-sense's, byte for byte, and the rules
// themselves live once in common/network/wifi_join_policy.h.
static constexpr uint32_t WIFI_BOOT_TIMEOUT_MS  = 30000;   // blocking boot connect
static constexpr uint32_t WIFI_RETRY_BASE_MS    = 2000;    // backoff base (doubles)
static constexpr uint32_t WIFI_RETRY_MAX_MS     = 30000;   // backoff cap
static constexpr uint32_t WIFI_OUTAGE_REBOOT_MS = 300000;  // 5 min outage -> reboot

// Power policy. Modem sleep saves current at some latency cost; off on a
// mains-powered guardian. TX power in quarter-dBm (8..84); -1 keeps the
// radio default.
static constexpr bool   WIFI_POWER_SAVE    = false;
static constexpr int8_t WIFI_TX_POWER_QDBM = -1;

// -------------------- Heap health (diagnostics) --------------------
// Same thresholds as canary-sense's (and the ESP32-S3 tree's) heap monitor.
static constexpr uint32_t HEAP_WARN_BYTES      = 30000;
static constexpr uint32_t HEAP_CRITICAL_BYTES  = 15000;
static constexpr uint32_t HEAP_EMERGENCY_BYTES = 10000;
static constexpr uint32_t HEAP_HYSTERESIS      = 5000;

// -------------------- Software updates (signed pull-OTA) --------------------
// Shared engine at firmware/common/ota — same manifest format, signature
// scheme and HA update-entity UX as canary-sense. A distinct OTA product per
// PRESET, not just per tier — the preset is compile-time data, so a door image
// must never install on a window, hallway or Heavy unit, nor a Lite (C3) image
// on a Standard (C6) head; the engine refuses on product mismatch. Every env
// in envs/platformio/canary-sentinel.ini sets both macros by build flag; the
// default below is a fallback for an env that forgets, which no env uses.
//
// NO release publishes these manifests yet (the project is compile-gated in
// CI, not released, until its bench checklist is green); each channel is
// declared unpublished in firmware/scripts/check_ota_channels.py, and a
// unit's daily check finds no manifest until a release signs one.
#ifndef SECURACV_OTA_PRODUCT
#define SECURACV_OTA_PRODUCT "securacv-canary-sentinel"
#endif
static constexpr const char* OTA_PRODUCT = SECURACV_OTA_PRODUCT;
#ifndef SECURACV_OTA_MANIFEST_URL
#define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-sentinel.json"
#endif
