/**
 * @file config.h
 * @brief Dash flavor configuration for Canary Display (4.3" dashboard)
 *
 * The "Canary Dash": Waveshare ESP32-S3-Touch-LCD-4.3 (800x480 IPS, GT911
 * touch). A front-door/kitchen wall or desk dashboard rendering a witness
 * card grid plus an event timeline — it subscribes to the Canaries' MQTT
 * topics and shows; it never senses (no camera, no microphone, by
 * construction).
 */

#pragma once

// ============================================================================
// DEVICE IDENTITY
// ============================================================================

#define CD_DEVICE_TYPE          "canary-dash"
#define CD_DEVICE_ID            "canary_dash_001"
#define CD_MANUFACTURER         "SecuraCV"
#define CD_MODEL                "Canary Dash (Waveshare ESP32-S3-Touch-LCD-4.3)"

// Flavor selector consumed by the HAL/UI translation units.
#define CD_FLAVOR_DASH          1

// ============================================================================
// FEATURE FLAGS
// ============================================================================

#define FEATURE_DISPLAY             1   // 800x480 card grid + event timeline
#define FEATURE_TOUCH               1   // GT911: tap = wake, long-press = ack
#define FEATURE_BACKLIGHT_DIM       0   // CH422G backlight is ON/OFF only (no PWM)
#define FEATURE_WIFI_STA            1
#define FEATURE_MQTT                1   // subscribe to the fleet, publish own status
#define FEATURE_CHAIN_VERIFY        1   // on-device Ed25519 verify + TOFU pinning
#define FEATURE_MDNS_DISCOVERY      1   // fleet discovery: find/gossip the broker
#define FEATURE_PROOF_QR            1   // tap-for-proof QR (trailblazer spec 1)
#define FEATURE_QR_COMMISSION       1   // "add a canary" SCV1 QR minting — the
                                        // surface has always compiled here
                                        // (transparency-sheet doorway); the
                                        // flag lets /api/device SAY so
#define FEATURE_ACK_SYNC            1   // household ack-sync    (spec 2)
#define FEATURE_PRESENCE_WAKE       1   // illumination ladder   (spec 3)
#ifndef FEATURE_CHIME  // -D overridable so the emulator (not real hardware) can force the chime on
#define FEATURE_CHIME               0   // piezo unpopulated; engine compiled (spec 5)
#endif
// The four below are #ifndef-guarded because compile-verification envs set
// them with -D. That guard is LOAD-BEARING, not style: a plain #define here
// silently REDEFINES the command-line value back (GCC warns, file wins), so an
// env like canary-display-dash-vault was compiling the no-op stubs while
// claiming to verify the persistence body. Any flag an env -Ds must be
// #ifndef-guarded here, like FEATURE_CHIME above.
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
// ── Nightstand wave (display_nightstand.md) — the dash is a WALL panel:
// it gets the data lines but not the bedside behaviors (its backlight is
// on/off only, and a hallway has no wake alarm) ──
#define FEATURE_NIGHT_BLACKOUT      0   // dash night = dark theme + scheduled off
#define FEATURE_COMFORT_WORDS       1
#define FEATURE_HUB_WEATHER         1
#define FEATURE_WAKE_ALARM          0

// Features NOT used by this device — a display witnesses nothing itself.
#define FEATURE_VISION_AI           0
#define FEATURE_CAMERA_PEEK         0
#define FEATURE_MMWAVE_RADAR        0
#ifndef FEATURE_SD_STORAGE  // -D overridable (canary-display-dash-sd env)
#define FEATURE_SD_STORAGE          0   // TF-slot deep archive (fleet/sd_archive.cpp,
                                        // SDMMC 1-bit); bench-gated (like CHIME)
#endif
#define FEATURE_WIFI_AP             0
#define FEATURE_HTTP_SERVER         0
#define FEATURE_MESH_NETWORK        0
#define FEATURE_BLUETOOTH           0   // no BLE services/pairing; passive scan rides FEATURE_CHIRP_SCAN
#define FEATURE_HA_DISCOVERY        0
#define FEATURE_GNSS                0

// ============================================================================
// FLEET MODEL
// ============================================================================

// 800x480 fits a readable 4x4 card grid alongside the timeline column.
#define CD_FLEET_MAX_DEVICES    16

#define CD_STALE_AFTER_MS       180000    // 3 min  -> amber
#define CD_LOST_AFTER_MS        600000    // 10 min -> red
#define CD_ACK_HOLD_MS          3600000   // 1 h

#define CD_EVENT_LOG_CAP        24

// Time machine (spec 7): proof-carrying journal depth and flash budget. The
// dash has the panel space to scrub a deeper window than the watch. History
// modal reads the RAM ring; FEATURE_TIME_MACHINE_PERSIST mirrors to LittleFS.
#define CD_JOURNAL_CAP          64
#define CD_JOURNAL_MAX_BYTES    98304     // 96 KB flash ceiling (compacts to ring)

// ============================================================================
// UI / NIGHT MODE
// ============================================================================

#define CD_UI_FRAME_MS          200     // 5 fps refresh tick (direct RGB draw)
#define CD_BRIGHT_DAY           255     // kept for API symmetry; panel is on/off
#define CD_BRIGHT_AMBIENT       255     // backlight is on/off; ambient == on
#define CD_BRIGHT_NIGHT         0       // quiet hours = dark theme + backlight OFF
#define CD_TOUCH_WAKE_MS        20000   // backlight back on after a tap, then off
#define CD_LONGPRESS_MS         900     // acknowledge gesture

// First-boot seeds — the on-glass settings surface owns the runtime
// schedule (glass_settings.h).
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

// Broker dark past this (WiFi fine) -> re-ask the fleet via mDNS and rebind
// (self-heals a broker that moved DHCP lease; also the retry cadence for a
// never-configured unit waiting for its first referral).
#define CD_BROKER_REDISCOVER_MS 120000  // 2 min

// Trailblazer wave 1 (display_trailblazer_spec.md)
#define CD_HEARTBEAT_UI_MS      60000   // all-verified pulse cadence (day only)
#define CD_PRESENCE_WAKE_MS     10000   // Notice+ event fresher than this wakes
#define CD_CHIME_REVOICE_MS     30000   // Tier-1 repeats until acked
#define CD_MQTT_BUFFER_BYTES    2048
#define CD_HA_DISCOVERY_PREFIX  "homeassistant"
