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
// HARDWARE TARGET SELECTION — Uncomment exactly ONE
// ════════════════════════════════════════════════════════════════════════════

// #define HARDWARE_XIAO_ESP32S3   // XIAO ESP32-S3 Sense (dual-core, camera, PSRAM)
#define HARDWARE_XIAO_ESP32C3   // XIAO ESP32-C3 (single-core RISC-V, BLE 5.0 only)

// ════════════════════════════════════════════════════════════════════════════
// BUILD PROFILE SELECTION — Uncomment exactly ONE
// ════════════════════════════════════════════════════════════════════════════

// #define BUILD_PROFILE_MINIMAL   // Fastest build: crypto + GPS only (~45s)
// #define BUILD_PROFILE_DEV       // Development: + WiFi + HTTP + SD (~90s)
#define BUILD_PROFILE_FULL      // Full features: + Mesh + BLE (~150s)


// Security guardrails
// FULL is treated as release-grade: no debug/developer credential fallbacks.
#if defined(BUILD_PROFILE_FULL)
  #define SECURACV_RELEASE_BUILD 1
#else
  #define SECURACV_RELEASE_BUILD 0
#endif
// Sanity checks
// Compiler output guidance
#if SECURACV_RELEASE_BUILD
  #pragma message("[SecuraCV] Release build: AP password is device-unique and printed in provisioning/boot serial output.")
#else
  #pragma message("[SecuraCV] Non-release build: device-unique AP password is preferred; debug fallback may be generated if derivation fails.")
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
  #define HW_HAS_BT_CLASSIC     0   // C3 supports BLE only, no Classic Bluetooth
  #define HW_CPU_CORES          1
  #define HW_MAX_GPIO           21
#elif defined(HARDWARE_XIAO_ESP32S3)
  // ESP32-S3: Dual-core Xtensa LX7, BLE + Classic BT, camera, PSRAM
  #define HW_HAS_CAMERA         1
  #define HW_HAS_PSRAM          1
  #define HW_HAS_BT_CLASSIC     1
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
  #define FEATURE_SYS_MONITOR   0
  #define FEATURE_WIFI_PRESENCE 0   // WiFi probe request presence detection
  #define FEATURE_AUDIBLE_CHIRP 0   // Local audible/visual alert tones

  #define DEBUG_NMEA            0
  #define DEBUG_CBOR            0
  #define DEBUG_CHAIN           1   // Useful for minimal testing
  #define DEBUG_VERIFY          0
  #define DEBUG_HTTP            0

#elif defined(BUILD_PROFILE_DEV)
  // ── DEV: WiFi + HTTP + SD for web UI testing ──
  // Use for: Testing HTTP API, web dashboard, SD storage
  // Skips: Camera, Mesh, BLE (biggest compile time savers)

  #define FEATURE_SD_STORAGE    1
  #define FEATURE_WIFI_AP       1
  #define FEATURE_HTTP_SERVER   1
  #define FEATURE_CAMERA_PEEK   0   // Skip camera (saves ~20s)
  #define FEATURE_TAMPER_GPIO   0
  #define FEATURE_WATCHDOG      1
  #define FEATURE_STATE_LOG     1
  #define FEATURE_MESH_NETWORK  0   // Skip mesh (saves ~15s)
  #define FEATURE_BLUETOOTH     0   // Skip BLE (saves ~25s)
  #define FEATURE_BLE           0   // Skip BLE Discovery (saves ~25s)
  #define FEATURE_SYS_MONITOR   1
  #define FEATURE_WIFI_PRESENCE 1   // WiFi probe request presence detection
  #define FEATURE_AUDIBLE_CHIRP 1   // Local audible/visual alert tones

  #define DEBUG_NMEA            0
  #define DEBUG_CBOR            0
  #define DEBUG_CHAIN           0
  #define DEBUG_VERIFY          0
  #define DEBUG_HTTP            1   // Useful for API testing

#elif defined(BUILD_PROFILE_FULL)
  // ── FULL: All hardware-supported features enabled ──
  // Use for: Production builds, full integration testing
  // Note: Camera and Classic BT auto-disabled on ESP32-C3 (hardware limitation)

  #define FEATURE_SD_STORAGE    1
  #define FEATURE_WIFI_AP       1
  #define FEATURE_HTTP_SERVER   1
  #define FEATURE_CAMERA_PEEK   HW_HAS_CAMERA    // ESP32-C3 has no camera interface
  #define FEATURE_TAMPER_GPIO   0
  #define FEATURE_WATCHDOG      1
  #define FEATURE_STATE_LOG     1
  #define FEATURE_MESH_NETWORK  1
  #define FEATURE_BLUETOOTH     HW_HAS_BT_CLASSIC // ESP32-C3 has no Classic Bluetooth
  #define FEATURE_BLE           1   // BLE Discovery (Opera/Chirp/Nearby) — works on both S3 and C3
  #define FEATURE_SYS_MONITOR   1
  #define FEATURE_WIFI_PRESENCE 1   // WiFi probe request presence detection
  #define FEATURE_AUDIBLE_CHIRP 1   // Local audible/visual alert tones

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
