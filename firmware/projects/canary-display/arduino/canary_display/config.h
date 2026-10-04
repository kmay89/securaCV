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
  #include "secrets.h"
#else
  #include "secrets.ci.h"
#endif
#include "flavor_config.h"

// -------------------- Dev playground (bench mode) --------------------
// FEATURE_PLAYGROUND boots the guided peripheral bench mode instead of the
// fleet face (docs/hardware/dev_playground_43b.md): main.cpp branches into
// canary::playground and never initializes WiFi/MQTT/OTA. Off everywhere
// by default; the canary-display-playground PlatformIO env sets it via
// -D, and the Arduino path sets it in flavor_local.h
// (./setup.sh arduino playground). Dash flavor + 4.3B pins only —
// feature_sanity enforces the board contract.
#ifndef FEATURE_PLAYGROUND
#define FEATURE_PLAYGROUND 0
#endif

// FEATURE_DEVMODE keeps that same peripheral test suite in the SHIPPED 4.3B
// dash firmware, reachable on-glass from Settings -> "dev mode": it sets an NVS
// flag and reboots into the very same network-silent playground, and an
// on-glass "exit dev mode" clears the flag and reboots back to the fleet face.
// So one binary is BOTH a fleet witness and a bench/test device — without ever
// letting the bench UI coexist with a live network stack. Off by default; the
// canary-display-dash-b env sets it. 4.3B (isolated DI/DO) only, exactly like
// FEATURE_PLAYGROUND — feature_sanity enforces the board contract.
#ifndef FEATURE_DEVMODE
#define FEATURE_DEVMODE 0
#endif

// NOTE: the "compile the peripheral bench" guard and the dev-mode NVS latch are
// written out INLINE at each use site, NOT as macros here — the playground and
// settings translation units include the flavor <config.h>, not this
// composition header, so a macro defined here wouldn't be visible to them (and
// the linker would silently drop the bench). The guard everywhere is:
//     ((FEATURE_PLAYGROUND) || (FEATURE_DEVMODE)) [&& CD_FLAVOR_DASH]
// — both are -D command-line flags, so they resolve in every TU — and the latch
// is Preferences("securacv").{get,put}Bool("devmode", ...).

// -------------------- Fleet-link band: LAN multicast --------------------
// Listen for presence beacons on the home WiFi this display is already joined
// to (src/net/fleet_udp.cpp). Still no broker and no hub — a multicast
// datagram is addressed to a group, not to a host — and this is the only band
// that reaches past direct radio range: BLE (chirp_scan) and ESP-NOW
// (espnow_peer) both stop at whatever the radio can hear.
//
// Every band decodes through the same parser and lands in the same
// FleetModel::on_beacon, and a witness is keyed on its fingerprint suffix, so
// enabling this alongside BLE adds coverage rather than duplicate devices or
// duplicate alerts. Default ON; flip OFF per board with -DFEATURE_FLEET_UDP=0
// if a size guard vetoes it — the module and its call sites compile out.
#ifndef FEATURE_FLEET_UDP
#define FEATURE_FLEET_UDP 1
#endif

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
// Backlight level while the LANTERN is lit (care/lantern.h). The lamp's
// color and depth are DRAWN — this only has to keep the panel awake, so the
// dim warm floor is the right default on a PWM board and any nonzero value
// is simply "on" where the backlight is binary (the 7" CH422G line).
#ifndef CD_BRIGHT_LANTERN
#define CD_BRIGHT_LANTERN CD_BRIGHT_PEEK
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
// A gear-carrying image must never identify as a plain flavor: the plain
// image has no gears (and, on the 4.3B, a different pin map), so accepting
// its OTA would silently strip both. When the build system passes no
// explicit product (the Arduino path — including the release train's
// `--profile modes` build), derive the modes identity from the gear flags
// themselves. flavor_local.h always defines them as 0/1, and #if treats an
// undefined macro as 0, so this is safe on every path.
#if (FEATURE_DEVMODE || FEATURE_DEMO_MODE || FEATURE_DEBUG_MODE || FEATURE_ARCADE)
#  define CD_GEARS_COMPILED_IDENTITY 1
#endif
#ifndef SECURACV_OTA_PRODUCT
#  if defined(CD_FLAVOR_WATCH) && defined(CD_GEARS_COMPILED_IDENTITY)
     // CI-compile flavor, never released: polls a manifest that does not
     // exist, so it can never cross-grade to the gearless watch image.
#    define SECURACV_OTA_PRODUCT "securacv-canary-display-watch-modes"
#  elif defined(CD_FLAVOR_WATCH)
#    define SECURACV_OTA_PRODUCT "securacv-canary-display-watch"
#  elif defined(CD_FLAVOR_DASH) && defined(CD_GEARS_COMPILED_IDENTITY)
#    define SECURACV_OTA_PRODUCT "securacv-canary-display-dash-modes"
#  elif defined(CD_FLAVOR_DASH)
#    define SECURACV_OTA_PRODUCT "securacv-canary-display-dash"
#  elif defined(CD_FLAVOR_NIGHTSTAND)
     // The two nightstand boards (c6/s3) each get a board-specific product
     // via the PlatformIO env -D; this is the Arduino-path fallback only.
#    define SECURACV_OTA_PRODUCT "securacv-canary-display-nightstand"
#  else
#    define SECURACV_OTA_PRODUCT "securacv-canary-display"
#  endif
#endif
static constexpr const char* OTA_PRODUCT = SECURACV_OTA_PRODUCT;
#ifndef SECURACV_OTA_MANIFEST_URL
#  if defined(CD_FLAVOR_WATCH) && defined(CD_GEARS_COMPILED_IDENTITY)
#    define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-display-watch-modes.json"
#  elif defined(CD_FLAVOR_WATCH)
#    define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-display-watch.json"
#  elif defined(CD_FLAVOR_DASH) && defined(CD_GEARS_COMPILED_IDENTITY)
#    define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-display-dash-modes.json"
#  elif defined(CD_FLAVOR_DASH)
#    define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-display-dash.json"
#  elif defined(CD_FLAVOR_NIGHTSTAND)
#    define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-display-nightstand.json"
#  else
#    define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-display.json"
#  endif
#endif
