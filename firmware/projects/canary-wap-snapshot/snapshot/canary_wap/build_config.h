/*
 * SecuraCV Canary — Build Configuration
 *
 * Select a build profile to control compile time and feature set.
 * Uncomment ONE profile below, then recompile.
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
// BUILD PROFILE SELECTION — Uncomment exactly ONE
// ════════════════════════════════════════════════════════════════════════════

// #define BUILD_PROFILE_MINIMAL   // Fastest build: crypto + GPS only (~45s)
// #define BUILD_PROFILE_DEV       // Development: + WiFi + HTTP + SD (~90s)
#define BUILD_PROFILE_FULL      // Full features: + Camera + Mesh + BLE (~150s)

// Sanity check: exactly one profile must be selected
#if (defined(BUILD_PROFILE_MINIMAL) + defined(BUILD_PROFILE_DEV) + defined(BUILD_PROFILE_FULL)) != 1
  #error "Exactly one build profile must be selected in build_config.h."
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
  #define FEATURE_SYS_MONITOR   0

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
  #define FEATURE_SYS_MONITOR   1

  #define DEBUG_NMEA            0
  #define DEBUG_CBOR            0
  #define DEBUG_CHAIN           0
  #define DEBUG_VERIFY          0
  #define DEBUG_HTTP            1   // Useful for API testing

#elif defined(BUILD_PROFILE_FULL)
  // ── FULL: All features enabled ──
  // Use for: Production builds, full integration testing

  #define FEATURE_SD_STORAGE    1
  #define FEATURE_WIFI_AP       1
  #define FEATURE_HTTP_SERVER   1
  #define FEATURE_CAMERA_PEEK   1
  #define FEATURE_TAMPER_GPIO   0
  #define FEATURE_WATCHDOG      1
  #define FEATURE_STATE_LOG     1
  #define FEATURE_MESH_NETWORK  1
  #define FEATURE_BLUETOOTH     1
  #define FEATURE_SYS_MONITOR   1

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

#endif // SECURACV_BUILD_CONFIG_H
