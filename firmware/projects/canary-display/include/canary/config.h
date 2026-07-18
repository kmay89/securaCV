#pragma once
#include <stdint.h>
#include <cstddef>

// Project-level composition header: pulls in the flavor configuration
// (configs/canary-display/<flavor>/config.h via -I, CD_* macros) and derives
// the network/OTA/diagnostics constants the net stack consumes. Mirrors the
// role canary-sense's include/canary/config.h plays, so the projects' net
// layers stay line-for-line comparable.
// Angle brackets on purpose: a quoted include would search this header's own
// directory first and hit THIS file (also named config.h); <config.h> skips
// the current-file directory and resolves via the -I paths straight to
// configs/canary-display/<flavor>/config.h.
//
// Per-site overrides (CD_TZ today) live in secrets/secrets.h next to the
// credentials, so pull it in AHEAD of the flavor config — its #defines must
// win over the flavor defaults' #ifndef fallbacks. Same include ladder as
// runtime_config.cpp. (#843 review catch: the README documented the CD_TZ
// override but nothing on the config include path actually read secrets.h,
// so non-UTC quiet hours silently ran on UTC.)
#if __has_include("secrets.h")
  #include "secrets.h"
#elif __has_include("secrets/secrets.h")
  #include "secrets/secrets.h"
#else
  #include "secrets.ci.h"
#endif
#include <config.h>

// -------------------- Identity --------------------
static constexpr const char* DEVICE_TYPE   = CD_DEVICE_TYPE;
static constexpr const char* DEVICE_ID     = CD_DEVICE_ID;  // first-boot seed only
static constexpr const char* MANUFACTURER  = CD_MANUFACTURER;
static constexpr const char* MODEL         = CD_MODEL;

// -------------------- Timing --------------------
static constexpr uint32_t HEARTBEAT_MS = CD_HEARTBEAT_MS;

// Health publish cadence (retained; heap/uptime/firmware — a display has no
// witness pubkey to pin, but fleet tooling still wants the liveness row).
static constexpr uint32_t HEALTH_PUBLISH_MS = 60000;

// Fleet-rediscovery deadline: broker dark this long (WiFi healthy) -> ask
// the fleet again and rebind (see canary/net/discovery.h).
static constexpr uint32_t BROKER_REDISCOVER_MS = CD_BROKER_REDISCOVER_MS;

// -------------------- MQTT / HA --------------------
static constexpr const char* HA_DISCOVERY_PREFIX = CD_HA_DISCOVERY_PREFIX;
static constexpr size_t MQTT_BUFFER_BYTES        = CD_MQTT_BUFFER_BYTES;

// -------------------- Care wave (display_care_wave.md) --------------------
// Escalation-on-no-ack: a Tier-1 condition unacknowledged this long fires
// ONE securacv/fleet/escalation event for automations to widen the circle.
#ifndef CD_ESCALATE_UNACKED_MS
#define CD_ESCALATE_UNACKED_MS 900000UL   // 15 min
#endif
// Per-site emergency contact (secrets.h override — it's personal data, so
// it lives with the credentials, never in a committed config). When set,
// the dash shows it during an unacked Tier-1: a panel dispatches, a witness
// display INFORMS the person standing in front of it.
#ifndef CD_EMERGENCY_CONTACT
#define CD_EMERGENCY_CONTACT ""
#endif
static constexpr uint32_t ESCALATE_UNACKED_MS = CD_ESCALATE_UNACKED_MS;
static constexpr const char* EMERGENCY_CONTACT = CD_EMERGENCY_CONTACT;

// -------------------- Nightstand wave --------------------
// Site latitude/longitude for on-device sunrise/sunset (suncalc.h). Like
// the emergency contact this is personal data: secrets.h only, never a
// committed config. 999 = unset — sun lines simply don't render.
#ifndef CD_LAT
#define CD_LAT 999.0
#endif
#ifndef CD_LON
#define CD_LON 999.0
#endif
// Which room name marks the bedside witness for the comfort line (substring
// match, case-insensitive — "bed" catches Bedroom/bedside/Kids bedroom).
#ifndef CD_BEDSIDE_ROOM
#define CD_BEDSIDE_ROOM "bed"
#endif
// Night tap-to-peek brightness: bright enough to read the red clock,
// nowhere near the daytime blast (the Echo Show bedside complaint).
#ifndef CD_BRIGHT_PEEK
#define CD_BRIGHT_PEEK 28
#endif

// -------------------- WiFi robustness / power --------------------
// STA supervision (S3-tree parity): non-blocking reconnect with exponential
// backoff, then a reboot as the recovery of last resort. Same constants as
// canary-vision.
static constexpr uint32_t WIFI_BOOT_TIMEOUT_MS  = 30000;   // blocking boot connect
static constexpr uint32_t WIFI_RETRY_BASE_MS    = 2000;    // backoff base (doubles)
static constexpr uint32_t WIFI_RETRY_MAX_MS     = 30000;   // backoff cap
static constexpr uint32_t WIFI_OUTAGE_REBOOT_MS = 300000;  // 5 min outage -> reboot

// Power policy. Modem sleep would add tens of ms latency to pushed alerts —
// exactly the thing a status display exists to avoid — so it stays off.
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
// scheme, and HA update-entity UX as the other Canary variants. Each flavor
// is a distinct OTA product (set via build flags in canary-display.ini): a
// watch image can never install on a dash or vice versa — the engine
// refuses on product mismatch.
// Per-flavor OTA identity. PlatformIO passes these as -D build flags (see
// envs/platformio/canary-display.ini); when it doesn't (e.g. the Arduino
// build, which has no per-env flags), derive them from the flavor macro so a
// watch never polls/accepts a dash image and vice versa. The flavor config is
// already included above, so CD_FLAVOR_* is known here.
#ifndef SECURACV_OTA_PRODUCT
#  if defined(CD_FLAVOR_WATCH)
#    define SECURACV_OTA_PRODUCT "securacv-canary-display-watch"
#  elif defined(CD_FLAVOR_DASH)
#    define SECURACV_OTA_PRODUCT "securacv-canary-display-dash"
#  else
#    define SECURACV_OTA_PRODUCT "securacv-canary-display"
#  endif
#endif
static constexpr const char* OTA_PRODUCT = SECURACV_OTA_PRODUCT;
#ifndef SECURACV_OTA_MANIFEST_URL
#  if defined(CD_FLAVOR_WATCH)
#    define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-display-watch.json"
#  elif defined(CD_FLAVOR_DASH)
#    define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-display-dash.json"
#  else
#    define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-display.json"
#  endif
#endif
