/**
 * @file config.h
 * @brief Watch flavor configuration for Canary Display (glance puck)
 *
 * The "Canary Watch Station": XIAO ESP32-S3 seated in the Seeed Round
 * Display for XIAO (1.28" 240x240 GC9A01 + CST816S touch). A bedside/desk
 * glance puck that renders fleet status as a witness ring — it subscribes
 * to the Canaries' MQTT topics and shows; it never senses (no camera, no
 * microphone, by construction).
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CD_DEVICE_TYPE          "canary-watch"
#define CD_DEVICE_ID            "canary_watch_001"
#define CD_MANUFACTURER         "SecuraCV"
#define CD_MODEL                "Canary Watch Station (XIAO ESP32-S3 + Round Display)"

// Flavor selector consumed by the HAL/UI translation units.
#define CD_FLAVOR_WATCH         1

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_DISPLAY             1   // GC9A01 glance ring UI
#define FEATURE_TOUCH               1   // CST816S: tap = wake/page, long-press = ack
#define FEATURE_BACKLIGHT_DIM       1   // LEDC PWM night dimming (board supports it)
#define FEATURE_WIFI_STA            1
#define FEATURE_MQTT                1   // subscribe to the fleet, publish own status
#define FEATURE_CHAIN_VERIFY        1   // on-device Ed25519 verify + TOFU pinning
#define FEATURE_MDNS_DISCOVERY      1   // fleet discovery: find/gossip the broker
#define FEATURE_PROOF_QR            1   // tap-for-proof QR (trailblazer spec 1)
#define FEATURE_QR_COMMISSION       1   // "add a canary" SCV1 QR minting — the
                                        // surface has always compiled here
                                        // (empty-nest long-press + settings
                                        // row); the flag lets /api/device
                                        // finally SAY so (glass_web reads it)
#define FEATURE_ACK_SYNC            1   // household ack-sync    (spec 2)
#define FEATURE_PRESENCE_WAKE       1   // illumination ladder   (spec 3)
#ifndef FEATURE_CHIME  // -D overridable so the emulator (not real hardware) can force the chime on
#define FEATURE_CHIME               0   // piezo unpopulated; engine compiled (spec 5)
#endif
// #ifndef-guarded like the dash config: any flag a compile-verification env
// sets with -D must be overridable, or the file's #define silently wins the
// redefinition and the env verifies stubs (see the dash config's note).
#ifndef FEATURE_CHIRP_SCAN
#define FEATURE_CHIRP_SCAN          1   // off-grid BLE chirp fallback (spec 6)
#endif
#ifndef FEATURE_BLE5_SCAN
#define FEATURE_BLE5_SCAN           0   // BLE 5 ext/Coded-PHY long-range scan; bench-gated (spec 6, like CHIME)
#endif
#ifndef FEATURE_FLEET_LINK
#define FEATURE_FLEET_LINK          1   // off-grid BLE GATT pull of WAP status (spec 6)
#endif
#define FEATURE_TIME_MACHINE        1   // proof-carrying event journal + history UI (spec 7)
#ifndef FEATURE_TIME_MACHINE_PERSIST
#define FEATURE_TIME_MACHINE_PERSIST 0  // LittleFS durability; bench-gated (like CHIME)
#endif
#define FEATURE_ONBOARDING          1   // first-boot SoftAP wizard + on-glass guide
#define FEATURE_CARE                1   // attention policy: night-silent maintenance,
                                        // morning summary, per-witness mute, Roll Call,
                                        // escalation-on-no-ack (display_care_wave.md)
#define FEATURE_RHYTHM              1   // whole-home morning-rhythm baseline (learned
                                        // on-device, never uploaded; wellbeing line)
#define FEATURE_WATCHDOG            1
#define FEATURE_SNTP                1   // clock + quiet-hours source
// ── Nightstand wave (display_nightstand.md) ──
#define FEATURE_NIGHT_BLACKOUT      0   // bench verdict: LCD backlight bleed makes
                                        // "off" a glowing gray panel - worse than a
                                        // calm dim glow. Now just the first-boot
                                        // seed for the runtime "at night" choice
                                        // (settings surface, glass_settings.h).
#define FEATURE_COMFORT_WORDS       1   // bedroom temp/humidity as sleep-science words
#define FEATURE_HUB_WEATHER         1   // hub-republished forecast (securacv/env/weather)
#define FEATURE_WAKE_ALARM          1   // on-device two-phase gentle wake (needs the
                                        // piezo populated to be audible — ramp works now)

// Features NOT used by this device — a display witnesses nothing itself.
#define FEATURE_VISION_AI           0
#define FEATURE_CAMERA_PEEK         0
#define FEATURE_MMWAVE_RADAR        0
#define FEATURE_SD_STORAGE          0   // slot exists, but it SHARES the panel's SPI
                                        // pins and the GC9A01 runs on Arduino_GFX's
                                        // private bus handle - no shared arbiter, so
                                        // the deep archive is dash-only for now
                                        // (fleet/sd_archive.h has the full story;
                                        // sd_archive.cpp #errors if forced on here)
#define FEATURE_WIFI_AP             0
#define FEATURE_HTTP_SERVER         0
#define FEATURE_MESH_NETWORK        0
#define FEATURE_BLUETOOTH           0   // no BLE services/pairing; passive scan rides FEATURE_CHIRP_SCAN
#define FEATURE_HA_DISCOVERY        0   // the display consumes; HA already sees the fleet
#define FEATURE_GNSS                0

// ============================================================================
// FLEET MODEL
// ============================================================================

// The glance ring renders one arc segment per witness; past 8 the segments
// get too small to read across a room, so the watch caps lower than dash.
#define CD_FLEET_MAX_DEVICES    8

// Staleness ladder (per-witness, driven off last MQTT activity):
//   fresh -> stale (amber) -> lost (red, treated like offline).
// Canaries heartbeat status every 5-30 s and health every 60 s, so 3 min
// of silence is genuinely abnormal.
#define CD_STALE_AFTER_MS       180000    // 3 min  -> amber
#define CD_LOST_AFTER_MS        600000    // 10 min -> red

// How long an acknowledged (quieted) alert keeps its residual chip before
// the ack itself expires and full emphasis returns if the condition persists.
#define CD_ACK_HOLD_MS          3600000   // 1 h

// Event ring buffer shown on the events page.
#define CD_EVENT_LOG_CAP        16

// Time machine (spec 7): proof-carrying journal depth and flash budget. Each
// record carries the verbatim signed chain head (~360 B), so the watch — the
// smaller device — keeps a modest window. The history page reads the RAM ring;
// FEATURE_TIME_MACHINE_PERSIST mirrors it to LittleFS so it survives reboots.
#define CD_JOURNAL_CAP          32
#define CD_JOURNAL_MAX_BYTES    49152     // 48 KB flash ceiling (compacts to ring)

// ============================================================================
// UI / NIGHT MODE
// ============================================================================

#define CD_UI_FRAME_MS          100     // ~10 fps render tick (canvas flush)
#define CD_BRIGHT_DAY           255     // LEDC duty (8-bit)
#define CD_BRIGHT_AMBIENT       100     // idle daytime dim (illumination ladder)
#define CD_BRIGHT_NIGHT         3       // near-dark glance floor (bench: 10 was
                                        // visibly bright at 3 a.m.; 3 is the dimmest
                                        // stable duty on this backlight)
#define CD_TOUCH_WAKE_MS        15000   // full brightness after a tap, then re-dim
#define CD_LONGPRESS_MS         900     // acknowledge gesture

// Quiet hours (local time; requires SNTP). First-boot seeds — the on-glass
// settings surface owns the runtime schedule (glass_settings.h).
#define CD_QUIET_START_HOUR     22
#define CD_QUIET_END_HOUR       7
// POSIX TZ string for local time. Default UTC; set yours in secrets.h
// (e.g. "EST5EDT,M3.2.0,M11.1.0") — it's location-ish data, so it lives
// with the other per-site values rather than in a committed header.
#ifndef CD_TZ
#define CD_TZ                   "EST5EDT,M3.2.0,M11.1.0"  // America/New_York
                                        // A wall clock that is wrong is worse
                                        // than one that is late: UTC0 shipped a
                                        // glass that read 3 a.m. at 11 a.m., which
                                        // also drove the face into night mode in
                                        // daylight. The web learner is OFF by
                                        // default on purpose (it would send the
                                        // household IP to a geolocation service),
                                        // so this seed IS the answer until someone
                                        // sets one. secrets.h CD_TZ still wins, as
                                        // does POST /api/tz and the settings sheet.
#endif

// ============================================================================
// TIMING / MQTT
// ============================================================================

#define CD_HEARTBEAT_MS         30000   // own retained status cadence
#define CD_WATCHDOG_TIMEOUT_SEC 30      // > worst bounded block (MQTT TCP timeout)

// Broker dark past this (WiFi fine) -> re-ask the fleet via mDNS and rebind
// (self-heals a broker that moved DHCP lease; also the retry cadence for a
// never-configured unit waiting for its first referral).
#define CD_BROKER_REDISCOVER_MS 120000  // 2 min

// Trailblazer wave 1 (display_trailblazer_spec.md)
#define CD_HEARTBEAT_UI_MS      60000   // all-verified pulse cadence (day only)
#define CD_PRESENCE_WAKE_MS     10000   // Notice+ event fresher than this wakes
#define CD_CHIME_REVOICE_MS     30000   // Tier-1 repeats until acked
#define CD_MQTT_BUFFER_BYTES    2048    // inbound state/sensing payloads are chunky
#define CD_HA_DISCOVERY_PREFIX  "homeassistant"
