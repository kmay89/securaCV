/**
 * @file config.h
 * @brief AMOLED 2.41 flavor configuration for Canary Display (450x600 AMOLED
 *        portrait — the flagship glance glass)
 *
 * The "Canary Glance AMOLED": the Waveshare ESP32-S3-Touch-AMOLED-2.41 in its
 * metal case (RM690B0 450x600 QSPI AMOLED + FT6336 touch, QMI8658 IMU,
 * PCF85063 RTC, SDMMC microSD, Li-po + soft power latch). The same nightstand
 * app (CD_FLAVOR_NIGHTSTAND: portrait face + witness column + honest night)
 * on the prettiest glass in the fleet: true black costs no power on AMOLED,
 * so the face's Quiet Glass ground is literally free, night mode is genuinely
 * dark, and the severity colors get an emissive panel to sing on. The full
 * tap/long-press ladder works by finger; the case's BOOT button carries the
 * button grammar and the PWR button adds wake + hold-to-power-off.
 *
 * One board carries this flavor (its own PlatformIO env + OTA product):
 *   - waveshare-esp32s3-amoled241 (dual-core S3R8, 16 MB flash, 8 MB octal
 *     PSRAM — the same budget as the Touch-1.69, with far more glass)
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CD_DEVICE_TYPE          "canary-nightstand"
#define CD_DEVICE_ID            "canary_glance_amoled_001"
#define CD_MANUFACTURER         "SecuraCV"
#define CD_MODEL                "Canary Glance AMOLED (Waveshare ESP32-S3-Touch-AMOLED-2.41)"

// Flavor selector consumed by the HAL/UI translation units. This RIDES the
// nightstand flavor (portrait face + witness column + shared modal surfaces,
// all geometry-parametric via pins.h) the way the Touch-1.69 does — but
// CD_AMOLED_GLASS swaps the panel HAL: display_amoled241.cpp (RM690B0 QSPI +
// FT6336 + power latch) compiles in and the ST7789 display_1in47.cpp
// compiles out. It also selects the dash's across-the-room type ladder
// (character.cpp): 450x600 at 2.41" is ~310 ppi, so the small-glass sizes
// would render physically tiny.
#define CD_FLAVOR_NIGHTSTAND    1
#define CD_AMOLED_GLASS         1

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_DISPLAY             1   // RM690B0 450x600 portrait AMOLED face
#define FEATURE_TOUCH               1   // FT6336 capacitive touch — the full
                                        // tap/long-press ladder, by finger
#define FEATURE_AMBIENT_LED         0   // no WS2812 on this board; the glass
                                        // (wash + emission ladder) is the beacon
#define FEATURE_BACKLIGHT_DIM       0   // this flag means PWM dimming, which
                                        // this glass structurally lacks
                                        // (HAS_BACKLIGHT_PWM 0) — night
                                        // dimming still works: the HAL maps
                                        // both intents onto panel command 0x51
#define FEATURE_WIFI_STA            1
#define FEATURE_MQTT                1   // subscribe to the fleet, publish own status
#define FEATURE_CHAIN_VERIFY        1   // on-device Ed25519 verify + TOFU pinning
#define FEATURE_MDNS_DISCOVERY      1   // fleet discovery: find/gossip the broker
#define FEATURE_PROOF_QR            1   // proof QR compiled (reachable once the
                                        // BOOT-button gestures land — follow-up)
#define FEATURE_ACK_SYNC            1   // household ack-sync    (spec 2)
#define FEATURE_PRESENCE_WAKE       1   // illumination ladder   (spec 3)
#ifndef FEATURE_CHIME  // -D overridable: the emulator forces the chime on for
                       // every display flavor; with BUZZER_PIN -1 the engine
                       // no-ops there too — this SKU simply has no piezo
#define FEATURE_CHIME               0   // no piezo pad — boot signature and
                                        // knocks stay visual
#endif
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
#define FEATURE_RTC                 1   // battery-backed PCF85063: trusted time
                                        // before/without SNTP
#define RTC_CHIP_PCF85063           1   // 7-byte time block starts at reg 0x04,
                                        // NOT the PCF8563 base 0x02 (rtc_pcf.h)
// ── Nightstand wave (display_nightstand.md + display_nightstand_line.md) ──
// The bedside contract holds on a glass this good: emission floor instead of
// a backlight floor, gentle wake, comfort words.
#define FEATURE_NIGHT_BLACKOUT      1   // first-boot seed: dark-when-safe at night
                                        // (true black IS this panel's off state)
#define FEATURE_COMFORT_WORDS       1   // bedroom temp/humidity as sleep words
#define FEATURE_HUB_WEATHER         1   // hub-republished forecast
#define FEATURE_WAKE_ALARM          1   // on-device two-phase gentle wake (visual
                                        // ramp; there is no piezo half here)
#define FEATURE_LANTERN             1   // the honest night light: this glass is
                                        // the lamp, summoned by a tap on the
                                        // affordance or a BOOT double-press
// ── Flagship storage: the SD deep archive (fleet/sd_archive.cpp) ──
// Unlike the dash family's expander-latched TF slot, this board's microSD is
// a dedicated SDMMC 1-bit slot with DAT3 on a native GPIO — the cleanest slot
// in the display line, so the time machine's deep tier ships ON by default.
// Absence stays honest: no card = one calm log line, archive on next insert.
// #ifndef-guarded because the env ALSO passes -DFEATURE_SD_STORAGE=1, and it
// must: the core-2 base's deep+ LDF evaluates conditionals with command-line
// defines only, so a flag living solely in this header leaves SD_MMC.h
// unresolvable at link time (the dash-sd env documents the same mechanism).
#ifndef FEATURE_SD_STORAGE
#define FEATURE_SD_STORAGE          1
#endif

// Features NOT used by this device — a display witnesses nothing itself.
#define FEATURE_VISION_AI           0
#define FEATURE_CAMERA_PEEK         0
#define FEATURE_MMWAVE_RADAR        0
#define FEATURE_WIFI_AP             0
#define FEATURE_HTTP_SERVER         0
#define FEATURE_MESH_NETWORK        0
#define FEATURE_BLUETOOTH           0   // no BLE services/pairing; passive scan rides CHIRP
#define FEATURE_HA_DISCOVERY        0
#define FEATURE_GNSS                0

// ============================================================================
// FLEET MODEL
// ============================================================================

// The portrait face's severity-ordered witness column shows what its scaled
// rows actually fit — 5 full-size rows on this glass (ROWS in
// portrait_ui.cpp: the proportional face keeps its physical row height
// rather than shrinking type to force a count; a taller glass may grow to
// 8). The worst witness always floats to the top row and a muted "+N more"
// covers the rest; the fleet model itself tracks up to this many.
#define CD_FLEET_MAX_DEVICES    12

#define CD_STALE_AFTER_MS       180000    // 3 min  -> amber
#define CD_LOST_AFTER_MS        600000    // 10 min -> red
#define CD_ACK_HOLD_MS          3600000   // 1 h

#define CD_EVENT_LOG_CAP        16

// Time machine (spec 7): the roomiest internal window in the portrait line
// (16 MB flash), before the SD deep archive even starts.
#define CD_JOURNAL_CAP          64
#define CD_JOURNAL_MAX_BYTES    65536     // 64 KB flash ceiling (compacts to ring)

// ============================================================================
// UI / NIGHT MODE
// ============================================================================

#define CD_UI_FRAME_MS          100     // ~10 fps content tick (LVGL animates
                                        // the breath every pass regardless)
#define CD_BRIGHT_DAY           255     // panel-command brightness (0x51), 8-bit
#define CD_BRIGHT_AMBIENT       100     // idle daytime dim (illumination ladder)
#define CD_BRIGHT_NIGHT         3       // near-dark glance floor — AMOLED
                                        // emission goes genuinely low
#define CD_TOUCH_WAKE_MS        15000   // full brightness after a wake, then re-dim
#define CD_LONGPRESS_MS         900     // acknowledge gesture (touch long-press)

// The case PWR button (a readable GPIO on this board): short press = wake /
// peek, held this long = honest power-off (panel to sleep, latch dropped,
// deep sleep with wake on the same button for USB-powered desks).
#define CD_PWR_OFF_HOLD_MS      3000

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
                                        // than one that is late (see the
                                        // touch169 config for the full story).
                                        // secrets.h CD_TZ still wins, as does
                                        // POST /api/tz and the settings sheet.
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
