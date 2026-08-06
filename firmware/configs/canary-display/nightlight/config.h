/**
 * @file config.h
 * @brief Nightlight flavor configuration for Canary Display (C3 1.47" pocket)
 *
 * The "Canary Nightlight": the Waveshare ESP32-C3-LCD-1.47 in the pocket
 * case (canary_c3_lcd147.scad), turned toward a kid's bedside. A 7-segment
 * clock over a soft lamp wash, with a canary companion who visits on the
 * household's rhythm — up with you in the morning, a little song at midday,
 * winding down in the evening, asleep beside the clock at night. Made to be
 * flashed in the Flasher, joined to WiFi on the glass, and tuned from the
 * iPhone app — no other tools.
 *
 * Structure: this RIDES the nightstand flavor (CD_FLAVOR_NIGHTSTAND — the
 * ST7789 portrait HAL, provisioning, settings, splash) and swaps the face,
 * exactly the way the Nightstand 7 rides the dash (CD_NIGHTSTAND7):
 * CD_NIGHTLIGHT compiles portrait_ui.cpp out and nightlight_ui.cpp in.
 *
 * Two deliberate personality departures from the nightstand, both product
 * decisions rather than drift — the honest-night reasoning is spelled out
 * at each flag:
 *
 *   1. The lamp defaults ON through quiet hours (CD_LANTERN_AUTO NIGHT).
 *      A nightlight that ships dark is a clock. This does NOT trade away
 *      the honest-signal rule, because this flavor never renders safety as
 *      light in the first place: the lamp is decor (a look-engine scene),
 *      the clock is information, and the lantern model's attention veto
 *      still takes the glass back — and puts the lamp out — the instant
 *      anything wants attention. Light here never means "safe"; it only
 *      ever means "the lamp you asked for is on."
 *
 *   2. Brightness is CAPPED AT 50% duty in the HAL (CD_BL_MAX_PCT). The
 *      pocket case is closed PETG with a lamp duty cycle; the cap is a
 *      heat budget, enforced at the expander PWM register underneath every
 *      settings path, so no stored preference and no API call can exceed
 *      it. A can't, not a won't.
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CD_DEVICE_TYPE          "canary-nightlight"
#define CD_DEVICE_ID            "canary_nightlight_001"
#define CD_MANUFACTURER         "SecuraCV"
#define CD_MODEL                "Canary Nightlight (ESP32-C3 1.47\" ST7789T)"

// Flavor selectors: the nightstand's HAL + shared surfaces, the nightlight's
// face and personality on top.
#define CD_FLAVOR_NIGHTSTAND    1
#define CD_NIGHTLIGHT           1

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_DISPLAY             1   // ST7789T 180x320 portrait face
#define FEATURE_TOUCH               0   // no touch panel on this board
#define FEATURE_AMBIENT_LED         0   // NO WS2812 on the C3 board — the
                                        // glass itself is the whole lamp
#define FEATURE_BACKLIGHT_DIM       1   // EXIO PWM (I2C register, 8-bit)
#define FEATURE_WIFI_STA            1
#define FEATURE_MQTT                1   // joins a fleet if the household has
                                        // one; idles standalone otherwise
                                        // (placeholder broker = standalone,
                                        // already the honest-dark case)
#define FEATURE_CHAIN_VERIFY        1   // same trust engine as its siblings
#define FEATURE_MDNS_DISCOVERY      1   // how the iPhone app finds it
#define FEATURE_PROOF_QR            0   // size cut (the C6 lean build's next
                                        // scoped cut, taken here from day 1 —
                                        // a kid's clock mints no proof QRs)
#define FEATURE_ACK_SYNC            1
#define FEATURE_PRESENCE_WAKE       0   // a bedside lamp must not flare when
                                        // someone walks past a hall canary
#ifndef FEATURE_CHIME  // -D overridable so the emulator can force the chime on
#define FEATURE_CHIME               0   // no piezo pad on this board
#endif
// The 4 MB C3 carries the same OTA-slot budget as the 4 MB C6 nightstand
// (0x1E0000 A/B slots): the BLE stack is compiled out. Off-grid BLE returns
// with the size work, not by flag-flipping here.
#ifndef FEATURE_CHIRP_SCAN
#define FEATURE_CHIRP_SCAN          0
#endif
#define FEATURE_BLE5_SCAN           0
#ifndef FEATURE_FLEET_LINK
#define FEATURE_FLEET_LINK          0
#endif
#define FEATURE_TIME_MACHINE        1   // proof-carrying journal (spec 7)
#define FEATURE_TIME_MACHINE_PERSIST 0
#define FEATURE_ONBOARDING          1   // first-boot SoftAP wizard + join QR —
                                        // the whole "zero other tools" story
#define FEATURE_CARE                1   // attention policy (quiet, honest)
#define FEATURE_RHYTHM              1
#define FEATURE_WATCHDOG            1
#define FEATURE_SNTP                1   // the clock IS the product
#define FEATURE_NIGHT_BLACKOUT      0   // nightlight departure: night keeps
                                        // the calibrated glow — a clock a
                                        // kid can read at 3 a.m. The honesty
                                        // veto still outranks everything.
#define FEATURE_COMFORT_WORDS       0   // no comfort sensors in this room
#define FEATURE_HUB_WEATHER         0   // size cut; the face has no forecast
#define FEATURE_WAKE_ALARM          1   // two-phase gentle wake (visual ramp)
#define FEATURE_LANTERN             1   // the lamp — see the header note: on
                                        // this flavor it runs through quiet
                                        // hours by default (the product)
#define FEATURE_AUTO_ORIENT         1   // the QMI8658 follows real movement:
                                        // stand it on any edge and the clock
                                        // rights itself — commits only from
                                        // SETTLED gravity (io/orientation.h),
                                        // so a carry or a shake never flips
                                        // it. Triple-press = manual override
                                        // (turns auto off, like touching a
                                        // climate dial leaves AUTO).

// Features NOT used by this device — a display witnesses nothing itself.
#define FEATURE_VISION_AI           0
#define FEATURE_CAMERA_PEEK         0
#define FEATURE_MMWAVE_RADAR        0
#define FEATURE_SD_STORAGE          0   // TF slot exists; unused in v0.1
#define FEATURE_WIFI_AP             0
#define FEATURE_HTTP_SERVER         0
#define FEATURE_MESH_NETWORK        0
#define FEATURE_BLUETOOTH           0
#define FEATURE_HA_DISCOVERY        0
#define FEATURE_GNSS                0

// ============================================================================
// THE HEAT CEILING
// ============================================================================

// Hard backlight-duty ceiling, percent of full. Enforced in the HAL at the
// EXIO PWM register (hal/display_1in47.cpp::exio_backlight) — the single
// sink every brightness path funnels through. Closed PETG pocket case +
// lamp duty cycle = a thermal budget; 50% keeps the panel and the case
// comfortable to the touch. Do not raise without bench thermals.
#define CD_BL_MAX_PCT           50

// ============================================================================
// FLEET MODEL
// ============================================================================

// Same portrait-glass cap as the nightstand: if this nightlight ever joins
// a real fleet, the honest machinery is all still here.
#define CD_FLEET_MAX_DEVICES    8

#define CD_STALE_AFTER_MS       180000    // 3 min  -> amber
#define CD_LOST_AFTER_MS        600000    // 10 min -> red
#define CD_ACK_HOLD_MS          3600000   // 1 h

#define CD_EVENT_LOG_CAP        16

#define CD_JOURNAL_CAP          32
#define CD_JOURNAL_MAX_BYTES    49152

// ============================================================================
// UI / NIGHT MODE
// ============================================================================

#define CD_UI_FRAME_MS          100
#define CD_BRIGHT_DAY           255     // pre-cap request; the HAL ceiling
#define CD_BRIGHT_AMBIENT       140     // makes day-full 50% actual duty
#define CD_BRIGHT_NIGHT         3
// The lamp's backlight rung: the nightstand keeps its lantern at the dim
// peek floor (a fleet glass moonlighting as a lamp); here the lamp is the
// product, so it earns a real glow — still under the HAL's 50% ceiling.
#define CD_BRIGHT_LANTERN       120
#define CD_TOUCH_WAKE_MS        15000
#define CD_LONGPRESS_MS         900

// The lamp seeds (runtime prefs live in NVS; see care/lantern.h). THE
// nightlight departure: auto mode ships as LANTERN_AUTO_NIGHT — the lamp
// burns through quiet hours out of the box, warm Lantern scene, because
// that is the product being bought. The scene ring (BOOT double-press, the
// app, or a tap while lit) walks every look-engine scene: Lantern's warm
// orange dim, the full Rainbow sweep, Moonbeam's bright white, and the
// rest. The attention veto extinguishes it the instant anything is wrong.
#define CD_LANTERN_SCENE        6       // look_engine kScenes[6] = "Lantern"
#define CD_LANTERN_MINUTES      15      // a daytime summon still times out
#define CD_LANTERN_AUTO         1       // LANTERN_AUTO_NIGHT (see above)

// Hallway mode stays available but OFF — the lantern auto mode above
// already covers the nightlight's night; Hallway's rise/ebb dwell is an
// opt-in refinement from the app or settings surface.
#define CD_HALLWAY_ON           0
#define CD_HALLWAY_LEVEL        1
#define CD_HALLWAY_RISE_MIN     6
#define CD_HALLWAY_EBB_MIN      12
#define CD_HALLWAY_SCENE        6
#define CD_HALLWAY_BEACON       0       // no WS2812 on this board anyway

// Quiet hours (local time; requires SNTP). Seeded for a kid's room —
// the settings surface and the app own the runtime schedule.
#define CD_QUIET_START_HOUR     20
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
