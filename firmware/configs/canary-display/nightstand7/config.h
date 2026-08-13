/**
 * @file config.h
 * @brief Nightstand 7 flavor configuration for Canary Display (7" bedside glass)
 *
 * The "Canary Nightstand 7": the Waveshare ESP32-S3-Touch-LCD-7 (7" 800x480
 * IPS, GT911 5-point touch, CH422G expander) living on a NIGHTSTAND, not a
 * wall. Electrically it is the Dash 7 — same RGB HAL / GT911 / CH422G, so it
 * keeps CD_FLAVOR_DASH for the panel + shared surfaces — but its standing
 * face is the bedside one (nightstand7_ui.cpp): a big clock hero, the day's
 * weather + comfort lines, the living canary on a quiet fleet strip, and a
 * clock-focused red night face. CD_NIGHTSTAND7 selects that face and the
 * bedside behaviors below.
 *
 * Night on this glass is RENDERED, not dimmed: the CH422G backlight is
 * on/off only (HAS_BACKLIGHT_PWM 0), so quiet hours mean near-black pixels
 * + the red-shifted clock, with the true blackout (backlight off, tap to
 * peek) as the NIGHT_SCREEN_OFF choice. The honesty veto is unchanged: a
 * Warn+ condition or a dead link keeps the glass glowing its state.
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CD_DEVICE_TYPE          "canary-nightstand7"
#define CD_DEVICE_ID            "canary_nightstand7_001"
#define CD_MANUFACTURER         "SecuraCV"
#define CD_MODEL                "Canary Nightstand 7 (Waveshare ESP32-S3-Touch-LCD-7)"

// Panel/HAL family selector: the 7" bedside glass IS a dash-family panel
// (display_dash.cpp, GT911, CH422G, the 800x480 modal surfaces)...
#define CD_FLAVOR_DASH          1
// ...but this sub-flavor swaps the standing face for the bedside one and
// compiles the wall dashboard out (dash_ui.cpp is guarded on it).
#define CD_NIGHTSTAND7          1

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_DISPLAY             1   // 800x480 bedside face (nightstand7_ui)
#define FEATURE_TOUCH               1   // GT911: tap = wake/lantern, long-press = ack
#define FEATURE_BACKLIGHT_DIM       0   // CH422G backlight is ON/OFF only (no PWM)
#define FEATURE_WIFI_STA            1
#define FEATURE_MQTT                1
#define FEATURE_CHAIN_VERIFY        1
#define FEATURE_MDNS_DISCOVERY      1
#define FEATURE_PROOF_QR            1
#define FEATURE_ACK_SYNC            1
#define FEATURE_PRESENCE_WAKE       1
#define FEATURE_CHIME               0   // no piezo on this board
#define FEATURE_CHIRP_SCAN          1
#define FEATURE_BLE5_SCAN           0
#define FEATURE_FLEET_LINK          1
#define FEATURE_TIME_MACHINE        1
#define FEATURE_TIME_MACHINE_PERSIST 0
#define FEATURE_ONBOARDING          1
#define FEATURE_CARE                1
#define FEATURE_RHYTHM              1
#define FEATURE_WATCHDOG            1
#define FEATURE_SNTP                1
// ── Nightstand wave — this 7" IS a bedside device: the full set ──
#define FEATURE_NIGHT_BLACKOUT      1   // dark-when-safe night (rendered dark +
                                        // scheduled backlight-off; honesty veto
                                        // in main.cpp keeps the glow on Warn+)
#define FEATURE_COMFORT_WORDS       1   // bedroom temp/humidity as sleep words
#define FEATURE_HUB_WEATHER         1   // hub-republished forecast + wx alerts
#define FEATURE_WAKE_ALARM          1   // two-phase gentle wake; the sunrise is
                                        // RENDERED on-glass (backlight is binary)
#define FEATURE_LANTERN             1
#define FEATURE_STANDALONE_WEATHER  1   // hub-less homes may OPT IN to the glass
                                        // fetching its own coarse forecast
                                        // (net/wx_direct.h - three gates, off by
                                        // default, never a hub fallback)   // user-summoned, timed-out night light
                                        // (display_nightstand_line.md §4)

// Features NOT used by this device — a display witnesses nothing itself.
#define FEATURE_VISION_AI           0
#define FEATURE_CAMERA_PEEK         0
#define FEATURE_MMWAVE_RADAR        0
#define FEATURE_SD_STORAGE          0
#define FEATURE_WIFI_AP             0
#define FEATURE_HTTP_SERVER         0
#define FEATURE_MESH_NETWORK        0
#define FEATURE_BLUETOOTH           0
#define FEATURE_HA_DISCOVERY        0
#define FEATURE_GNSS                0

// ============================================================================
// FLEET MODEL
// ============================================================================

// The bedside strip shows chips, not cards; 16 keeps parity with the dash7
// so a household never outgrows the bedroom glass before the wall one.
#define CD_FLEET_MAX_DEVICES    16

#define CD_STALE_AFTER_MS       180000    // 3 min  -> amber
#define CD_LOST_AFTER_MS        600000    // 10 min -> red
#define CD_ACK_HOLD_MS          3600000   // 1 h

#define CD_EVENT_LOG_CAP        24

#define CD_JOURNAL_CAP          64
#define CD_JOURNAL_MAX_BYTES    98304     // 96 KB flash ceiling (compacts to ring)

// ============================================================================
// UI / NIGHT MODE
// ============================================================================

#define CD_UI_FRAME_MS          200     // 5 fps content tick (LVGL animates the
                                        // wash/breath every pass regardless)
#define CD_BRIGHT_DAY           255
#define CD_BRIGHT_AMBIENT       255     // backlight is on/off; ambient == on
#define CD_BRIGHT_NIGHT         0       // quiet hours = dark render + backlight
                                        // OFF when the user chose blackout
#define CD_TOUCH_WAKE_MS        20000
#define CD_LONGPRESS_MS         900

#define CD_QUIET_START_HOUR     22
#define CD_QUIET_END_HOUR       7
#ifndef CD_TZ
#define CD_TZ                   "UTC0"
#endif

// Lantern seeds (runtime prefs live in NVS; see care/lantern.h): the warm
// Lantern scene, a 15-minute timeout, auto-schedule off (the honest default —
// dark-when-safe stays meaningful unless the user opts into lantern hours).
#define CD_LANTERN_SCENE        6       // look_engine kScenes[6] = "Lantern"
#define CD_LANTERN_MINUTES      15
#define CD_LANTERN_AUTO         0

// ============================================================================
// TIMING / MQTT
// ============================================================================

#define CD_HEARTBEAT_MS         30000
#define CD_WATCHDOG_TIMEOUT_SEC 30

#define CD_BROKER_REDISCOVER_MS 120000  // 2 min

#define CD_HEARTBEAT_UI_MS      60000
#define CD_PRESENCE_WAKE_MS     10000
#define CD_CHIME_REVOICE_MS     30000
#define CD_MQTT_BUFFER_BYTES    2048
#define CD_HA_DISCOVERY_PREFIX  "homeassistant"
