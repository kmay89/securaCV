/**
 * @file config.h
 * @brief Nightstand flavor configuration for Canary Display (1.47" portrait)
 *
 * The "Canary Nightstand": a Waveshare ESP32 1.47" board (ST7789 172x320
 * portrait IPS) with ONE onboard WS2812 addressable RGB LED — made to live
 * on a nightstand or a desk. Color is its primary language: the LED is an
 * across-the-room state beacon that breathes with the fleet, and the tall
 * glass carries the living canary and a vertical witness column. It
 * subscribes to the Canaries' MQTT topics and shows; it never senses (no
 * camera, no microphone, by construction).
 *
 * Two boards share this flavor (each its own PlatformIO env + OTA product):
 *   - waveshare-esp32c6-lcd147  (single-core C6, no PSRAM — renders lean)
 *   - waveshare-esp32s3-lcd147  (dual-core S3, 8 MB PSRAM — can double-buffer)
 * Same look, two budgets. Neither board has a touch panel (HAS_TOUCH 0), so
 * the ambient LED + the BOOT button are the whole input surface.
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CD_DEVICE_TYPE          "canary-nightstand"
#define CD_DEVICE_ID            "canary_nightstand_001"
#define CD_MANUFACTURER         "SecuraCV"
#define CD_MODEL                "Canary Nightstand (ESP32 1.47\" ST7789 + WS2812)"

// Flavor selector consumed by the HAL/UI translation units. A brand-new
// guard (not watch/dash): the 172x320 portrait face is its own layout, and
// display_1in47.cpp is its own panel HAL. Shared modal surfaces
// (settings/commission/onboarding/splash) treat the nightstand as a small
// portrait sibling of the watch.
#define CD_FLAVOR_NIGHTSTAND    1

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_DISPLAY             1   // ST7789 172x320 portrait face
#define FEATURE_TOUCH               0   // no touch panel on the 1.47" boards
#define FEATURE_AMBIENT_LED         1   // WS2812 across-room state beacon (RMT)
#define FEATURE_BACKLIGHT_DIM       1   // LEDC PWM night dimming (board supports it)
#define FEATURE_WIFI_STA            1
#define FEATURE_MQTT                1   // subscribe to the fleet, publish own status
#define FEATURE_CHAIN_VERIFY        1   // on-device Ed25519 verify + TOFU pinning
#define FEATURE_MDNS_DISCOVERY      1   // fleet discovery: find/gossip the broker
#define FEATURE_PROOF_QR            1   // proof QR compiled (reachable once the
                                        // BOOT-button gestures land — follow-up)
#define FEATURE_ACK_SYNC            1   // household ack-sync    (spec 2)
#define FEATURE_PRESENCE_WAKE       1   // illumination ladder   (spec 3)
#define FEATURE_CHIME               0   // no piezo on these boards; engine compiled
#define FEATURE_CHIRP_SCAN          1   // off-grid BLE chirp fallback (spec 6)
#define FEATURE_BLE5_SCAN           0   // BLE 5 ext/Coded-PHY long-range scan; bench-gated
#define FEATURE_FLEET_LINK          1   // off-grid BLE GATT pull of WAP status (spec 6)
#define FEATURE_TIME_MACHINE        1   // proof-carrying event journal + history (spec 7)
#define FEATURE_TIME_MACHINE_PERSIST 0  // LittleFS durability; bench-gated
#define FEATURE_ONBOARDING          1   // first-boot SoftAP wizard + on-glass guide
#define FEATURE_CARE                1   // attention policy (display_care_wave.md)
#define FEATURE_RHYTHM              1   // whole-home morning-rhythm baseline
#define FEATURE_WATCHDOG            1
#define FEATURE_SNTP                1   // clock + quiet-hours source
// ── Nightstand wave (display_nightstand.md + display_nightstand_line.md) ──
// This IS the bedside device the wave was written for: PWM backlight floor,
// gentle wake, comfort words — plus the new ambient LED beacon.
#define FEATURE_NIGHT_BLACKOUT      1   // first-boot seed: dark-when-safe at night
                                        // (the honest two-channel night-light —
                                        // the LED is a pure attention beacon)
#define FEATURE_COMFORT_WORDS       1   // bedroom temp/humidity as sleep words
#define FEATURE_HUB_WEATHER         1   // hub-republished forecast
#define FEATURE_WAKE_ALARM          1   // on-device two-phase gentle wake (visual
                                        // ramp works without a piezo)

// Features NOT used by this device — a display witnesses nothing itself.
#define FEATURE_VISION_AI           0
#define FEATURE_CAMERA_PEEK         0
#define FEATURE_MMWAVE_RADAR        0
#define FEATURE_SD_STORAGE          0   // slot exists; event cache is a follow-up
#define FEATURE_WIFI_AP             0
#define FEATURE_HTTP_SERVER         0
#define FEATURE_MESH_NETWORK        0
#define FEATURE_BLUETOOTH           0   // no BLE services/pairing; passive scan rides CHIRP
#define FEATURE_HA_DISCOVERY        0
#define FEATURE_GNSS                0

// ============================================================================
// FLEET MODEL
// ============================================================================

// The vertical witness column shows one row per Canary; on the narrow 172 px
// glass more than ~8 rows get too short to read from bed, so it caps like the
// watch. The worst-severity witness always floats to the top row.
#define CD_FLEET_MAX_DEVICES    8

#define CD_STALE_AFTER_MS       180000    // 3 min  -> amber
#define CD_LOST_AFTER_MS        600000    // 10 min -> red
#define CD_ACK_HOLD_MS          3600000   // 1 h

#define CD_EVENT_LOG_CAP        16

// Time machine (spec 7): modest window like the watch (the smaller sibling).
#define CD_JOURNAL_CAP          32
#define CD_JOURNAL_MAX_BYTES    49152     // 48 KB flash ceiling (compacts to ring)

// ============================================================================
// UI / NIGHT MODE
// ============================================================================

#define CD_UI_FRAME_MS          100     // ~10 fps content tick (LVGL animates
                                        // the breath every pass regardless)
#define CD_BRIGHT_DAY           255     // LEDC duty (8-bit)
#define CD_BRIGHT_AMBIENT       100     // idle daytime dim (illumination ladder)
#define CD_BRIGHT_NIGHT         3       // near-dark glance floor (same as watch)
#define CD_TOUCH_WAKE_MS        15000   // full brightness after a wake, then re-dim
#define CD_LONGPRESS_MS         900     // acknowledge gesture (BOOT button — follow-up)

// Quiet hours (local time; requires SNTP). First-boot seeds — the settings
// surface owns the runtime schedule (glass_settings.h).
#define CD_QUIET_START_HOUR     22
#define CD_QUIET_END_HOUR       7
#ifndef CD_TZ
#define CD_TZ                   "UTC0"
#endif

// ============================================================================
// TIMING / MQTT
// ============================================================================

#define CD_HEARTBEAT_MS         30000
#define CD_WATCHDOG_TIMEOUT_SEC 30

#define CD_BROKER_REDISCOVER_MS 120000  // 2 min

#define CD_HEARTBEAT_UI_MS      60000   // all-verified pulse cadence (day only)
#define CD_PRESENCE_WAKE_MS     10000   // Notice+ event fresher than this wakes
#define CD_CHIME_REVOICE_MS     30000   // Tier-1 repeats until acked
#define CD_MQTT_BUFFER_BYTES    2048
#define CD_HA_DISCOVERY_PREFIX  "homeassistant"
