/*
 * SecuraCV Canary — Build Configuration
 *
 * Select a build profile AND hardware target to control compile time
 * and feature set. Uncomment ONE profile and ONE target below, then recompile.
 *
 * ARDUINO IDE BUILD SPEED TIPS:
 * ─────────────────────────────
 * 1. File > Preferences > Enable "Aggressively cache compiled core"
 * 2. Use MINIMAL profile during development iteration
 * 3. Don't close Arduino IDE between builds (keeps cache warm)
 * 4. Avoid "Verify" if you're going to "Upload" anyway
 */

#ifndef SECURACV_BUILD_CONFIG_H
#define SECURACV_BUILD_CONFIG_H

// ════════════════════════════════════════════════════════════════════════════
// WEB ASSET STRATEGY
// ════════════════════════════════════════════════════════════════════════════
// Ship the large dashboard/settings/companion HTML as pre-gzipped byte arrays
// (web_assets_gz.h) and compile the raw PROGMEM literals out of the binary —
// saves ~336 KB of app-partition flash. Defined here, a globally included
// config header, so EVERY translation unit that pulls in web_ui.h /
// csi_dashboard_html.h / companion_pwa.h sees it and never compiles its own
// uncompressed duplicate. Those asset headers #include build_config.h
// themselves, so the guard holds regardless of include order.
#define CANARY_WEB_ASSETS_GZIPPED 1

// ────────────────────────────────────────────────────────────────────────────
// GPS PRECISION COARSENING (Invariant III)
// ────────────────────────────────────────────────────────────────────────────
// Decimal places lat/lon are rounded to before any operator-facing emission
// (HTTP/serial). This is a PER-DEPLOYMENT privacy knob — tune it to the install's
// circumstances (e.g. population density / how identifying a precise fix is):
//   2 dp ≈ 1.1 km   3 dp ≈ 110 m (default)   4 dp ≈ 11 m   5 dp ≈ 1.1 m
// Lower = more privacy / less locate precision; higher = the reverse.
// Build-time default; can be overridden at runtime via the "gps_prec" NVS key.
#ifndef GPS_COARSEN_DECIMALS
#define GPS_COARSEN_DECIMALS 3
#endif

// ════════════════════════════════════════════════════════════════════════════
// HARDWARE TARGET SELECTION — Uncomment exactly ONE
// ════════════════════════════════════════════════════════════════════════════

// May be selected externally via -DHARDWARE_XIAO_ESP32S3 / -DHARDWARE_XIAO_ESP32C3
// (e.g. PlatformIO build_flags). Arduino IDE users: leave the default below.
#if !defined(HARDWARE_XIAO_ESP32S3) && !defined(HARDWARE_XIAO_ESP32C3)
#define HARDWARE_XIAO_ESP32S3   // XIAO ESP32-S3 Sense (dual-core, camera, PSRAM)
// #define HARDWARE_XIAO_ESP32C3   // XIAO ESP32-C3 (single-core RISC-V, BLE 5.0 only)
#endif

// ════════════════════════════════════════════════════════════════════════════
// BUILD PROFILE SELECTION — Uncomment exactly ONE
// ════════════════════════════════════════════════════════════════════════════

// May be selected externally via -DBUILD_PROFILE_MINIMAL / _DEV / _FULL
// (e.g. PlatformIO build_flags). Arduino IDE users: leave the default below.
#if !defined(BUILD_PROFILE_MINIMAL) && !defined(BUILD_PROFILE_DEV) && !defined(BUILD_PROFILE_FULL)
// #define BUILD_PROFILE_MINIMAL   // Fastest build: crypto + GPS only (~45s)
// #define BUILD_PROFILE_DEV       // Development: + WiFi + HTTP + SD (~90s)
#define BUILD_PROFILE_FULL      // Full features: + Mesh + BLE (~150s)
#endif


// Security guardrails
// FULL is treated as release-grade: no debug/developer credential fallbacks.
#if defined(BUILD_PROFILE_FULL)
  #define SECURACV_RELEASE_BUILD 1
#else
  #define SECURACV_RELEASE_BUILD 0
#endif
// Sanity checks
// Compiler output guidance — only emit once, from the main sketch TU.
// (canary_wap.ino defines SECURACV_EMIT_BUILD_BANNER before including this
// header so the note isn't printed by every .cpp that pulls it in.)
#ifdef SECURACV_EMIT_BUILD_BANNER
  #if SECURACV_RELEASE_BUILD
    #pragma message("[SecuraCV] Release build: AP password is device-unique and printed in provisioning/boot serial output.")
  #else
    #pragma message("[SecuraCV] Non-release build: device-unique AP password is preferred; debug fallback may be generated if derivation fails.")
  #endif
#endif
#if (defined(BUILD_PROFILE_MINIMAL) + defined(BUILD_PROFILE_DEV) + defined(BUILD_PROFILE_FULL)) != 1
  #error "Exactly one build profile must be selected in build_config.h."
#endif
#if (defined(HARDWARE_XIAO_ESP32S3) + defined(HARDWARE_XIAO_ESP32C3)) != 1
  #error "Exactly one hardware target must be selected in build_config.h."
#endif

// ════════════════════════════════════════════════════════════════════════════
// HARDWARE CAPABILITY FLAGS (derived from target selection)
// ════════════════════════════════════════════════════════════════════════════
#if defined(HARDWARE_XIAO_ESP32C3)
  // ESP32-C3: Single-core RISC-V, BLE 5.0 only, no camera, no PSRAM
  // GPIO range: 0-21 only. Strapping pins: GPIO 2, 8, 9
  #define HW_HAS_CAMERA         0
  #define HW_HAS_PSRAM          0
  #define HW_HAS_BT_CLASSIC     0   // C3 has no Classic Bluetooth radio
  #define HW_HAS_BLE            1   // C3 supports Bluetooth Low Energy 5.0
  #define HW_CPU_CORES          1
  #define HW_MAX_GPIO           21
#elif defined(HARDWARE_XIAO_ESP32S3)
  // ESP32-S3: Dual-core Xtensa LX7, BLE 5.0 only (no Classic), camera, PSRAM
  // Reference: ESP32-S3 datasheet §1.1 — "Bluetooth LE 5.0", no BR/EDR.
  #define HW_HAS_CAMERA         1
  #define HW_HAS_PSRAM          1
  #define HW_HAS_BT_CLASSIC     0   // S3 has no Classic Bluetooth radio
  #define HW_HAS_BLE            1   // S3 supports Bluetooth Low Energy 5.0
  #define HW_CPU_CORES          2
  #define HW_MAX_GPIO           48
#endif

// ════════════════════════════════════════════════════════════════════════════
// PROFILE DEFINITIONS — Do not edit below unless customizing
// ════════════════════════════════════════════════════════════════════════════

#if defined(BUILD_PROFILE_MINIMAL)
  // ── MINIMAL: Core witness functionality only ──
  // Use for: Testing crypto, GPS, chain logic
  // Skips: WiFi, HTTP, SD, Camera, Mesh, BLE

  #define FEATURE_SD_STORAGE    0
  #define FEATURE_WIFI_AP       0
  #define FEATURE_HTTP_SERVER   0
  #define FEATURE_CAMERA_PEEK   0
  #define FEATURE_TAMPER_GPIO   0
  #define FEATURE_WATCHDOG      1
  #define FEATURE_STATE_LOG     1
  #define FEATURE_MESH_NETWORK  0
  #define FEATURE_BLUETOOTH     0
  #define FEATURE_BLE           0   // BLE Discovery (Opera/Chirp/Nearby)
  #define FEATURE_BLE_SCAN      0   // BLE Scout — paired-beacon room attribution (PR 5)
  #define FEATURE_BLE_STATUS    0   // BLE GATT status service (battery/health/chain)
  #define FEATURE_SYS_MONITOR   0
  #define FEATURE_WIFI_PRESENCE 0   // WiFi probe request presence detection
  #define FEATURE_AUDIBLE_CHIRP 0   // Local audible/visual alert tones
  #define FEATURE_DATA_MGMT     0   // SD log rotation, chain backup/restore
  #define FEATURE_POWER_MONITOR 1   // Battery voltage + SoC monitoring (always on)
  #define FEATURE_POWER_POLICY  0   // Smart battery power modes (DEV/FULL only)
  #define FEATURE_QR_PROVISION  0   // Camera-based WiFi QR provisioning

  #define DEBUG_NMEA            0
  #define DEBUG_CBOR            0
  #define DEBUG_CHAIN           1   // Useful for minimal testing
  #define DEBUG_VERIFY          0
  #define DEBUG_HTTP            0

#elif defined(BUILD_PROFILE_DEV)
  // ── DEV: WiFi + HTTP + SD + BLE pairing for full app testing ──
  // Use for: Testing HTTP API, web dashboard, SD storage, BLE pairing flow.
  // Skips: Camera, Mesh (biggest remaining compile time savers).
  //
  // BLE was previously disabled here for build speed, but the dashboard's
  // Bluetooth panel ships with the build, so users would land on the
  // pairing/scan UI and see "BLE unavailable in this build". Including the
  // pairing channel in DEV keeps the panel honest while still letting MINIMAL
  // be the no-radio fast-iteration target.

  #define FEATURE_SD_STORAGE    1
  #define FEATURE_WIFI_AP       1
  #define FEATURE_HTTP_SERVER   1
  #define FEATURE_CAMERA_PEEK   0   // Skip camera (saves ~20s)
  #define FEATURE_TAMPER_GPIO   0
  #define FEATURE_WATCHDOG      1
  #define FEATURE_STATE_LOG     1
  #define FEATURE_MESH_NETWORK  0   // Skip mesh (saves ~15s)
  #define FEATURE_BLUETOOTH     HW_HAS_BLE   // BLE pairing channel (NimBLE)
  #define FEATURE_BLE           0   // Skip Opera/Chirp/Nearby discovery (saves ~25s)
  #define FEATURE_BLE_SCAN      0   // BLE Scout disabled by default — opt-in via FULL build
  #define FEATURE_BLE_STATUS    FEATURE_BLUETOOTH   // BLE GATT status service (needs NimBLE server)
  #define FEATURE_SYS_MONITOR   1
  #define FEATURE_WIFI_PRESENCE 1   // WiFi probe request presence detection
  #define FEATURE_AUDIBLE_CHIRP 1   // Local audible/visual alert tones
  #define FEATURE_DATA_MGMT     1   // SD log rotation, chain backup/restore
  #define FEATURE_POWER_MONITOR 1   // Battery voltage + SoC monitoring
  #define FEATURE_POWER_POLICY  1   // Smart battery power modes
  #define FEATURE_QR_PROVISION  0   // Camera may not be in DEV build

  #define DEBUG_NMEA            0
  #define DEBUG_CBOR            0
  #define DEBUG_CHAIN           0
  #define DEBUG_VERIFY          0
  #define DEBUG_HTTP            1   // Useful for API testing

#elif defined(BUILD_PROFILE_FULL)
  // ── FULL: All hardware-supported features enabled ──
  // Use for: Production builds, full integration testing
  // Note: Camera auto-disabled on ESP32-C3 (no DVP camera interface).
  //       BLE works on both S3 and C3; neither variant has Classic Bluetooth.

  #define FEATURE_SD_STORAGE    1
  #define FEATURE_WIFI_AP       1
  #define FEATURE_HTTP_SERVER   1
  #define FEATURE_CAMERA_PEEK   HW_HAS_CAMERA    // ESP32-C3 has no camera interface
  #define FEATURE_TAMPER_GPIO   0
  #define FEATURE_WATCHDOG      1
  #define FEATURE_STATE_LOG     1
  #define FEATURE_MESH_NETWORK  1
  // FEATURE_BLUETOOTH gates the BLE pairing/advertising channel (NimBLE).
  // This is BLE, not Classic Bluetooth — gate on HW_HAS_BLE so it works on
  // both XIAO ESP32-S3 and XIAO ESP32-C3 (both BLE 5.0 capable).
  #define FEATURE_BLUETOOTH     HW_HAS_BLE
  #define FEATURE_BLE           1   // BLE Discovery (Opera/Chirp/Nearby) — works on both S3 and C3
  #define FEATURE_BLE_SCAN      HW_HAS_BLE   // BLE Scout — paired-beacon room attribution
  #define FEATURE_BLE_STATUS    1   // BLE GATT status service (battery/health/chain)
  #define FEATURE_SYS_MONITOR   1
  #define FEATURE_WIFI_PRESENCE 1   // WiFi probe request presence detection
  #define FEATURE_AUDIBLE_CHIRP 1   // Local audible/visual alert tones
  #define FEATURE_DATA_MGMT     1   // SD log rotation, chain backup/restore
  #define FEATURE_POWER_MONITOR 1   // Battery voltage + SoC monitoring
  #define FEATURE_POWER_POLICY  1   // Smart battery power modes
  #define FEATURE_QR_PROVISION  HW_HAS_CAMERA   // Camera-based WiFi QR provisioning

  #define DEBUG_NMEA            0
  #define DEBUG_CBOR            0
  #define DEBUG_CHAIN           0
  #define DEBUG_VERIFY          0
  #define DEBUG_HTTP            0

#endif

// ════════════════════════════════════════════════════════════════════════════
// PROFILE INFO (for runtime logging)
// ════════════════════════════════════════════════════════════════════════════

#if defined(BUILD_PROFILE_MINIMAL)
  #define BUILD_PROFILE_NAME "MINIMAL"
#elif defined(BUILD_PROFILE_DEV)
  #define BUILD_PROFILE_NAME "DEV"
#elif defined(BUILD_PROFILE_FULL)
  #define BUILD_PROFILE_NAME "FULL"
#endif

#if defined(HARDWARE_XIAO_ESP32C3)
  #define HARDWARE_TARGET_NAME "XIAO_ESP32C3"
#elif defined(HARDWARE_XIAO_ESP32S3)
  #define HARDWARE_TARGET_NAME "XIAO_ESP32S3"
#endif

#endif // SECURACV_BUILD_CONFIG_H
