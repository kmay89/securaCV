#pragma once
#include <stdint.h>
#include <cstddef>

// Project-level composition header: pulls in the flavor configuration
// (configs/canary-sense/<flavor>/config.h via -I, CS_* macros) and derives
// the network/OTA/diagnostics constants the net stack consumes. Mirrors the
// role canary-vision's include/canary/config.h plays, so the two projects'
// net layers stay line-for-line comparable.
// Angle brackets on purpose: a quoted include would search this header's own
// directory first and hit THIS file (also named config.h); <config.h> skips
// the current-file directory and resolves via the -I paths straight to
// configs/canary-sense/<flavor>/config.h.
#include <config.h>

// -------------------- Identity --------------------
static constexpr const char* DEVICE_TYPE   = CS_DEVICE_TYPE;
static constexpr const char* DEVICE_ID     = CS_DEVICE_ID;  // first-boot seed only
static constexpr const char* MANUFACTURER  = CS_MANUFACTURER;
static constexpr const char* MODEL         = CS_MODEL;

// -------------------- Timing --------------------
static constexpr uint32_t HEARTBEAT_MS = CS_HEARTBEAT_MS;

// Ambient light (BH1750) sample cadence — lux feeds tamper corroboration
// ("lights-out + presence"), not a high-rate channel.
static constexpr uint32_t LUX_SAMPLE_MS = 5000;

// -------------------- MQTT / HA --------------------
static constexpr const char* HA_DISCOVERY_PREFIX = CS_HA_DISCOVERY_PREFIX;
static constexpr size_t MQTT_BUFFER_BYTES        = CS_MQTT_BUFFER_BYTES;

// -------------------- WiFi robustness / power --------------------
// STA supervision (S3-tree parity): non-blocking reconnect with exponential
// backoff, then a reboot as the recovery of last resort. Same constants as
// canary-vision.
static constexpr uint32_t WIFI_BOOT_TIMEOUT_MS  = 30000;   // blocking boot connect
static constexpr uint32_t WIFI_RETRY_BASE_MS    = 2000;    // backoff base (doubles)
static constexpr uint32_t WIFI_RETRY_MAX_MS     = 30000;   // backoff cap
static constexpr uint32_t WIFI_OUTAGE_REBOOT_MS = 300000;  // 5 min outage -> reboot

// Power policy. Modem sleep saves ~20 mA at some latency cost; off by
// default on a mains-powered witness. TX power in quarter-dBm (8..84);
// -1 keeps the radio default.
static constexpr bool   WIFI_POWER_SAVE    = false;
static constexpr int8_t WIFI_TX_POWER_QDBM = -1;

// -------------------- Heap health (diagnostics) --------------------
// Same thresholds as the ESP32-S3 tree's securacv_diagnostics heap monitor.
static constexpr uint32_t HEAP_WARN_BYTES      = 30000;
static constexpr uint32_t HEAP_CRITICAL_BYTES  = 15000;
static constexpr uint32_t HEAP_EMERGENCY_BYTES = 10000;
static constexpr uint32_t HEAP_HYSTERESIS      = 5000;

// -------------------- Software updates (signed pull-OTA) --------------------
// Shared engine at firmware/common/ota — same manifest format, signature
// scheme, and HA update-entity UX as canary, canary-wap, and canary-vision.
// Each flavor is a distinct OTA product with its own manifest (the wellbeing
// env overrides both via build flags): a presence-only manifest can never
// install a wellbeing image or vice versa — the engine refuses on product
// mismatch.
#ifndef SECURACV_OTA_PRODUCT
#define SECURACV_OTA_PRODUCT "securacv-canary-sense"
#endif
static constexpr const char* OTA_PRODUCT = SECURACV_OTA_PRODUCT;
#ifndef SECURACV_OTA_MANIFEST_URL
#define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-sense.json"
#endif
