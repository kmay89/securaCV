/**
 * @file config.h
 * @brief Touch 1.69 flavor configuration for Canary Display (240x280 touch portrait)
 *
 * The "Canary Nightstand Touch": the Waveshare ESP32-S3-Touch-LCD-1.69
 * (ST7789V2 240x280 portrait IPS + CST816T capacitive touch, QMI8658 IMU,
 * PCF85063 RTC, 3.7 V battery with onboard charging). The same nightstand
 * app (CD_FLAVOR_NIGHTSTAND: portrait face + witness column + honest night)
 * on glass you can TOUCH: the standardized tap/long-press ladder the watch
 * and dash speak (wake, peek, acknowledge, the commissioning wizard) works
 * with a finger instead of the BOOT button. No WS2812 on this board — the
 * glass itself is the whole ambient surface. It subscribes to the Canaries'
 * MQTT topics and shows; it never senses (no camera, no microphone, by
 * construction).
 *
 * One board carries this flavor (its own PlatformIO env + OTA product):
 *   - waveshare-esp32s3-touch-lcd169 (dual-core S3R8, 16 MB flash, 8 MB
 *     octal PSRAM — the roomiest budget in the nightstand family)
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CD_DEVICE_TYPE          "canary-nightstand"
#define CD_DEVICE_ID            "canary_nightstand_touch_001"
#define CD_MANUFACTURER         "SecuraCV"
#define CD_MODEL                "Canary Nightstand Touch (Waveshare ESP32-S3-Touch-LCD-1.69)"

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
#define FEATURE_TOUCH               1   // CST816T capacitive touch — the full
                                        // tap/long-press ladder, by finger
#define FEATURE_AMBIENT_LED         0   // no WS2812 on this board; the glass
                                        // (wash + backlight ladder) is the beacon
#define FEATURE_BACKLIGHT_DIM       1   // LEDC PWM night dimming (board supports it)
#define FEATURE_WIFI_STA            1
#define FEATURE_MQTT                1   // subscribe to the fleet, publish own status
#define FEATURE_CHAIN_VERIFY        1   // on-device Ed25519 verify + TOFU pinning
#define FEATURE_MDNS_DISCOVERY      1   // fleet discovery: find/gossip the broker
#define FEATURE_PROOF_QR            1   // proof QR compiled (reachable once the
                                        // BOOT-button gestures land — follow-up)
#define FEATURE_ACK_SYNC            1   // household ack-sync    (spec 2)
#define FEATURE_PRESENCE_WAKE       1   // illumination ladder   (spec 3)
#ifndef FEATURE_CHIME  // -D overridable so the emulator (not real hardware) can force the chime on
#define FEATURE_CHIME               1   // populated buzzer (GPIO42, vendor pinout) —
                                        // sounds the "canary wakes" boot chime on power-on
#endif
// Overridable (#ifndef): the C6 board's env compiles these two OUT
// (-DFEATURE_CHIRP_SCAN=0 -DFEATURE_FLEET_LINK=0). Not a preference — an
// OTA-slot budget: the 4 MB C6 has 0x1E0000 A/B slots and the BLE stack
// (libble_app + the NimBLE host) costs ~300 KB the full portrait image
// doesn't have. The S3 stick (16 MB) keeps both. Off-grid BLE on the C6
// returns with the size work, not by flag-flipping here.
#ifndef FEATURE_CHIRP_SCAN
#define FEATURE_CHIRP_SCAN          1   // off-grid BLE chirp fallback (spec 6)
#endif
#define FEATURE_BLE5_SCAN           0   // BLE 5 ext/Coded-PHY long-range scan; bench-gated
#ifndef FEATURE_FLEET_LINK
#define FEATURE_FLEET_LINK          1   // off-grid BLE GATT pull of WAP status (spec 6)
#endif
#define FEATURE_TIME_MACHINE        1   // proof-carrying event journal + history (spec 7)
#define FEATURE_TIME_MACHINE_PERSIST 0  // LittleFS durability; bench-gated
#define FEATURE_ONBOARDING          1   // first-boot SoftAP wizard + on-glass guide
#define FEATURE_CARE                1   // attention policy (display_care_wave.md)
#define FEATURE_RHYTHM              1   // whole-home morning-rhythm baseline
#define FEATURE_WATCHDOG            1
#define FEATURE_SNTP                1   // clock + quiet-hours source
#define FEATURE_RTC                 1   // battery-backed PCF85063A: trusted time
                                        // before/without SNTP, and a correct
                                        // day/night volume for the boot chime at
                                        // cold power-on (else it errs quiet)
#define RTC_CHIP_PCF85063           1   // this board's RTC is a PCF85063A — its
                                        // 7-byte time block starts at reg 0x04,
                                        // NOT the PCF8563 base 0x02 (rtc_pcf.h)
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
#define FEATURE_LANTERN             1   // the honest night light: this glass is
                                        // the lamp, summoned by a tap on the
                                        // affordance (this board HAS touch) or
                                        // a BOOT double-press, and timed out

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
#define CD_LONGPRESS_MS         900     // acknowledge gesture (touch long-press)

// Lantern seeds (runtime prefs live in NVS; see care/lantern.h). Warm
// Lantern scene, 15 minutes, no auto schedule — "lantern hours" trades away
// the dark-means-safe signal, so it stays opt-in.
#define CD_LANTERN_SCENE        6       // look_engine kScenes[6] = "Lantern"
#define CD_LANTERN_MINUTES      15
#define CD_LANTERN_AUTO         0

// Quiet hours (local time; requires SNTP). First-boot seeds — the settings
// surface owns the runtime schedule (glass_settings.h).
#define CD_QUIET_START_HOUR     22
#define CD_QUIET_END_HOUR       7
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

#define CD_HEARTBEAT_MS         30000
#define CD_WATCHDOG_TIMEOUT_SEC 30

#define CD_BROKER_REDISCOVER_MS 120000  // 2 min

#define CD_HEARTBEAT_UI_MS      60000   // all-verified pulse cadence (day only)
#define CD_PRESENCE_WAKE_MS     10000   // Notice+ event fresher than this wakes
#define CD_CHIME_REVOICE_MS     30000   // Tier-1 repeats until acked
#define CD_MQTT_BUFFER_BYTES    2048
#define CD_HA_DISCOVERY_PREFIX  "homeassistant"
