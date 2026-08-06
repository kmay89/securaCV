#pragma once
#include <stdint.h>
#include <cstddef>

// Pull in the flavor configuration (configs/canary-vision/<flavor>/config.h
// via -I): the FEATURE_* flags feature_sanity.h cross-checks against the
// board's HAS_* capabilities. Without this, the sanity checks silently
// no-op in the real build (every check skips when FEATURE_* is undefined) —
// same include ladder canary-sense and canary-display use.
// Angle brackets on purpose: a quoted include would search this header's own
// directory first and hit THIS file (also named config.h); <config.h> skips
// the current-file directory and resolves via the -I paths straight to
// configs/canary-vision/<flavor>/config.h.
#include <config.h>

// -------------------- Fleet-link presence beacon (BLE) --------------------
// Advertise-only NimBLE presence beacon (src/net/fleet_beacon_adv.cpp) so a
// canary-display finds this witness directly over BLE — no broker, no shared
// WiFi. Default ON; guarded so CI can flip it OFF per board with
// -DFEATURE_FLEET_BEACON=0 if the OTA-slot size guard (0x140000) vetoes the
// added NimBLE stack. The whole module + its call sites compile out when 0.
#ifndef FEATURE_FLEET_BEACON
#define FEATURE_FLEET_BEACON 1
#endif

// -------------------- Fleet-link presence beacon (LAN multicast) ----------
// The same beacon, carried over the home WiFi this device is already joined to
// (src/net/fleet_udp.cpp). Still no broker, no hub, no pairing — a
// multicast datagram is addressed to a group, not to a host, so there is
// nothing to discover or log into. This is the band that reaches ACROSS a
// house: BLE and ESP-NOW stop at direct radio range.
//
// It carries bytes built by src/net/fleet_beacon_payload.cpp — the same ones
// the BLE carrier advertises — so enabling or disabling either band never
// changes what the beacon says, only how far it goes. Default ON; guarded so
// CI can flip it OFF per board with -DFEATURE_FLEET_UDP=0 if the OTA-slot size
// guard vetoes it. The module + its call sites compile out when 0.
#ifndef FEATURE_FLEET_UDP
#define FEATURE_FLEET_UDP 1
#endif

// -------------------- Fleet-link roster scanner (BLE) --------------------
// The RX twin of the beacon (src/net/fleet_roster_scan.cpp): a low-duty passive
// BLE scan that hears the OTHER Canaries' presence beacons + chirps and keeps a
// shared fleet roster (last-heartbeat + battery/health/chain), so every Canary
// — not just the display — tracks its siblings. Default ON; guarded so CI can
// flip it OFF per board with -DFEATURE_FLEET_ROSTER=0 if the OTA-slot size
// guard vetoes the added scan path. The module + its call sites compile out
// when 0. (The scanner shares NimBLE with the beacon; enabling it without the
// beacon is fine, but the pair is the intended configuration.)
#ifndef FEATURE_FLEET_ROSTER
#define FEATURE_FLEET_ROSTER 1
#endif

// -------------------- Identity --------------------
// Canonical device type: lowercase, hyphenated — must match HA's
// DEVICE_TYPE_MODALITY map and the mDNS `dt` TXT key. (HA canonicalises
// the legacy "canary_vision" spelling, but new payloads publish canonical.)
static constexpr const char* DEVICE_TYPE = "canary-vision";
static constexpr const char* DEVICE_ID   = "canary_vision_001";  // change per unit

static constexpr const char* MANUFACTURER = "SecuraCV";
// Board-neutral: the same app ships on ESP32-C3 DevKit, XIAO ESP32-C3,
// and XIAO ESP32-S3 hosts (see firmware/envs/platformio/canary-vision.ini).
static constexpr const char* MODEL        = "Canary Vision (Grove Vision AI V2)";

// -------------------- Vision semantics --------------------
// NOTE: PERSON_TARGET is model-dependent in SSCMA.
// Keep as 0 if your loaded model uses class 0 for "person".
//
// These four constants are FIRST-BOOT SEEDS only: the live values are
// NVS-backed and adjustable from Home Assistant (number entities) — see
// canary/detect_config.h. Changing the loaded model never needs a rebuild.
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

// -------------------- WiFi robustness / power --------------------
// STA supervision (S3-tree parity): non-blocking reconnect with exponential
// backoff, then a reboot as the recovery of last resort.
static constexpr uint32_t WIFI_BOOT_TIMEOUT_MS  = 30000;   // blocking boot connect
static constexpr uint32_t WIFI_RETRY_BASE_MS    = 2000;    // backoff base (doubles)
static constexpr uint32_t WIFI_RETRY_MAX_MS     = 30000;   // backoff cap
static constexpr uint32_t WIFI_OUTAGE_REBOOT_MS = 300000;  // 5 min outage -> reboot

// Power policy. Modem sleep (WIFI_PS_MIN_MODEM) saves ~20 mA at the cost of
// some broker-to-device latency; off by default on a mains-powered witness.
// TX power is in quarter-dBm (8..84); -1 keeps the radio default.
static constexpr bool   WIFI_POWER_SAVE    = false;
static constexpr int8_t WIFI_TX_POWER_QDBM = -1;

// -------------------- Aim assist (bench/aiming) --------------------
// Boxes-only live channel for the Lovelace aim card: coordinates and
// scores at a usable cadence, never pixels. Off by default; a switch in
// HA turns it on; it turns itself off after AIM_AUTO_OFF_MS so a
// forgotten toggle can't stream box telemetry forever.
static constexpr uint32_t AIM_PUBLISH_MS      = 200;     // detection frames (~5 Hz)
static constexpr uint32_t AIM_IDLE_PUBLISH_MS = 1000;    // empty frames (clears the box)
static constexpr uint32_t AIM_AUTO_OFF_MS     = 600000;  // 10 min

// -------------------- Heap health (diagnostics) --------------------
// Same thresholds as the ESP32-S3 tree's securacv_diagnostics heap monitor.
// Levels escalate immediately and de-escalate only past +HYSTERESIS, so the
// degradation state can't flap around a boundary.
static constexpr uint32_t HEAP_WARN_BYTES      = 30000;
static constexpr uint32_t HEAP_CRITICAL_BYTES  = 15000;
static constexpr uint32_t HEAP_EMERGENCY_BYTES = 10000;
static constexpr uint32_t HEAP_HYSTERESIS      = 5000;

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
