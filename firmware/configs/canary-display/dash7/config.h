/**
 * @file config.h
 * @brief Dash 7 flavor configuration for Canary Display (7" big glass)
 *
 * The "Canary Dash 7": Waveshare ESP32-S3-Touch-LCD-7 (7" 800x480 IPS, GT911
 * 5-point touch, CH422G expander). Electrically the 4.3" Canary Dash at 7" —
 * SAME 800x480 canvas, SAME RGB HAL / GT911 / CH422G — so it reuses the Dash
 * flavor (CD_FLAVOR_DASH) and the dash_ui layout wholesale; the physical
 * glass is simply larger (roomier targets, more whitespace). It differs from
 * the 4.3" only in board pins and its own OTA product.
 *
 * The new capability is real 5-point multitouch (the GT911 reports up to 5);
 * the full-report wiring + gestures ride the dash HAL/UI (display_dash.cpp,
 * dash_ui.cpp) — see docs/hardware/display_nightstand_line.md §6.
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CD_DEVICE_TYPE          "canary-dash"
#define CD_DEVICE_ID            "canary_dash7_001"
#define CD_MANUFACTURER         "SecuraCV"
#define CD_MODEL                "Canary Dash 7 (Waveshare ESP32-S3-Touch-LCD-7)"

// Reuses the Dash flavor selector: same 800x480 canvas, same HAL/UI. The
// only per-board differences (pins, OTA product) come from the PlatformIO
// env, not this macro.
#define CD_FLAVOR_DASH          1

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_DISPLAY             1   // 800x480 card grid + event timeline
#define FEATURE_TOUCH               1   // GT911 5-point: tap = wake, long-press = ack
#define FEATURE_BACKLIGHT_DIM       0   // CH422G backlight is ON/OFF only (no PWM)
#define FEATURE_WIFI_STA            1
#define FEATURE_MQTT                1
#define FEATURE_CHAIN_VERIFY        1
#define FEATURE_MDNS_DISCOVERY      1
#define FEATURE_PROOF_QR            1
#define FEATURE_ACK_SYNC            1
#define FEATURE_PRESENCE_WAKE       1
#define FEATURE_CHIME               0
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
// ── Nightstand wave — the 7" is a WALL/desk panel like the 4.3": data lines,
// not bedside behaviors (backlight on/off only, no wake alarm) ──
#define FEATURE_NIGHT_BLACKOUT      0
#define FEATURE_COMFORT_WORDS       1
#define FEATURE_HUB_WEATHER         1
#define FEATURE_STANDALONE_WEATHER  1   // hub-less homes may OPT IN to the glass
                                        // fetching its own coarse forecast
                                        // (net/wx_direct.h - three gates, off by
                                        // default, never a hub fallback)
#define FEATURE_WAKE_ALARM          0

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

// The 7" has even more room than the 4.3" for the card grid + timeline.
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

#define CD_UI_FRAME_MS          200     // 5 fps refresh tick (direct RGB draw)
#define CD_BRIGHT_DAY           255
#define CD_BRIGHT_AMBIENT       255     // backlight is on/off; ambient == on
#define CD_BRIGHT_NIGHT         0       // quiet hours = dark theme + backlight OFF
#define CD_TOUCH_WAKE_MS        20000
#define CD_LONGPRESS_MS         900

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

#define CD_HEARTBEAT_UI_MS      60000
#define CD_PRESENCE_WAKE_MS     10000
#define CD_CHIME_REVOICE_MS     30000
#define CD_MQTT_BUFFER_BYTES    2048
#define CD_HA_DISCOVERY_PREFIX  "homeassistant"
