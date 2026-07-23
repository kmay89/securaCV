/**
 * @file feature_sanity.h
 * @brief Compile-time cross-check of config feature flags against board
 *        capability flags — fail the build, not the field device.
 *
 * Pattern borrowed from Marlin's SanityCheck.h: a misconfiguration should
 * die at compile time with a message that names the exact flag to fix and
 * where to fix it, instead of surfacing as a runtime mystery ("SD writes
 * failing", "BLE refused") on someone's bench.
 *
 * Include this ONCE per project, after both the board pin map
 * (boards/<id>/pins/pins.h — defines HAS_*) and the configuration
 * (configs/<app>/<config>/config.h — defines FEATURE_*) are visible:
 *
 *     #include "pins.h"              // board capabilities
 *     #include "config.h"     // feature selection
 *     #include "feature_sanity.h"
 *
 * Every check fires only when BOTH sides of the comparison are defined, so
 * the header is safe to adopt incrementally: a tree that doesn't define a
 * given HAS_* flag simply skips that check. The flip side: an undefined
 * capability flag is an unchecked one — boards must define the baseline
 * HAS_* set (CI enforces this via scripts/check_board_registry.py).
 *
 * Rules for adding checks (keep the Marlin discipline):
 *   - The message names the offending flag, the conflicting board fact,
 *     and the two ways out (disable the feature, or pick capable hardware).
 *   - Checks are preprocessor-only. No includes, no code, no side effects.
 */

#pragma once

// ─── Storage ────────────────────────────────────────────────────────────

#if defined(FEATURE_SD_STORAGE) && FEATURE_SD_STORAGE && defined(HAS_SD_CARD) && !HAS_SD_CARD
  #error "FEATURE_SD_STORAGE=1 but this board has no SD slot (HAS_SD_CARD=0). Disable FEATURE_SD_STORAGE in your configs/<app>/<config>/config.h, or select a board with an SD slot (see firmware/boards/boards.json)."
#endif

// ─── Fieldbus ───────────────────────────────────────────────────────────

#if defined(FEATURE_RS485) && FEATURE_RS485 && defined(HAS_CAN_RS485) && !HAS_CAN_RS485
  #error "FEATURE_RS485=1 but this board has no RS485 transceiver (HAS_CAN_RS485=0). Disable FEATURE_RS485 in your config, or select a board with the RS485/CAN terminal (e.g. waveshare-esp32s3-lcd43b; see firmware/boards/boards.json)."
#endif

#if defined(FEATURE_CAN) && FEATURE_CAN && defined(HAS_CAN_RS485) && !HAS_CAN_RS485
  #error "FEATURE_CAN=1 but this board has no CAN/TWAI transceiver (HAS_CAN_RS485=0). Disable FEATURE_CAN in your config, or select a board with the RS485/CAN terminal (e.g. waveshare-esp32s3-lcd43b; see firmware/boards/boards.json)."
#endif

// ─── Camera ─────────────────────────────────────────────────────────────

#if defined(FEATURE_CAMERA_PEEK) && FEATURE_CAMERA_PEEK && defined(HAS_CAMERA) && !HAS_CAMERA
  #error "FEATURE_CAMERA_PEEK=1 but this board has no camera (HAS_CAMERA=0). Disable FEATURE_CAMERA_PEEK in your config, or select a camera board like xiao-esp32s3-sense (see firmware/boards/boards.json)."
#endif

#if defined(FEATURE_CAMERA_PEEK) && FEATURE_CAMERA_PEEK && defined(HAS_PSRAM) && !HAS_PSRAM
  #error "FEATURE_CAMERA_PEEK=1 but this board has no PSRAM (HAS_PSRAM=0). The camera framebuffer lives in PSRAM; without it the heap starves (BLE refused, SD writes failing). Disable FEATURE_CAMERA_PEEK or select a PSRAM board."
#endif

// ─── Radios ─────────────────────────────────────────────────────────────

#if defined(FEATURE_BLUETOOTH) && FEATURE_BLUETOOTH && defined(HAS_BLE) && !HAS_BLE
  #error "FEATURE_BLUETOOTH=1 but this board has no BLE radio (HAS_BLE=0). Disable FEATURE_BLUETOOTH in your config, or select a BLE-capable board (see firmware/boards/boards.json)."
#endif

#if defined(FEATURE_BLE) && FEATURE_BLE && defined(HAS_BLE) && !HAS_BLE
  #error "FEATURE_BLE=1 but this board has no BLE radio (HAS_BLE=0). Disable FEATURE_BLE in your config, or select a BLE-capable board (see firmware/boards/boards.json)."
#endif

#if defined(HAS_WIFI) && !HAS_WIFI
  #if defined(FEATURE_WIFI_AP) && FEATURE_WIFI_AP
    #error "FEATURE_WIFI_AP=1 but this board has no WiFi radio (HAS_WIFI=0). Disable FEATURE_WIFI_AP in your config, or select a WiFi-capable board."
  #endif
  #if defined(FEATURE_WIFI_STA) && FEATURE_WIFI_STA
    #error "FEATURE_WIFI_STA=1 but this board has no WiFi radio (HAS_WIFI=0). Disable FEATURE_WIFI_STA in your config, or select a WiFi-capable board."
  #endif
  #if defined(FEATURE_HTTP_SERVER) && FEATURE_HTTP_SERVER
    #error "FEATURE_HTTP_SERVER=1 requires a WiFi radio (HAS_WIFI=0 on this board). Disable FEATURE_HTTP_SERVER in your config, or select a WiFi-capable board."
  #endif
  #if defined(FEATURE_MQTT) && FEATURE_MQTT
    #error "FEATURE_MQTT=1 requires a WiFi radio (HAS_WIFI=0 on this board). Disable FEATURE_MQTT in your config, or select a WiFi-capable board."
  #endif
  #if defined(FEATURE_HA_MQTT) && FEATURE_HA_MQTT
    #error "FEATURE_HA_MQTT=1 requires a WiFi radio (HAS_WIFI=0 on this board). Disable FEATURE_HA_MQTT in your config, or select a WiFi-capable board."
  #endif
  #if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
    #error "FEATURE_MESH_NETWORK=1 requires a WiFi radio (HAS_WIFI=0 on this board). Disable FEATURE_MESH_NETWORK in your config, or select a WiFi-capable board."
  #endif
  #if defined(FEATURE_WIFI_PRESENCE) && FEATURE_WIFI_PRESENCE
    #error "FEATURE_WIFI_PRESENCE=1 requires a WiFi radio (HAS_WIFI=0 on this board). Disable FEATURE_WIFI_PRESENCE in your config, or select a WiFi-capable board."
  #endif
  #if defined(FEATURE_CSI) && FEATURE_CSI
    #error "FEATURE_CSI=1 requires a WiFi radio (HAS_WIFI=0 on this board) — CSI sensing reads WiFi channel state. Disable FEATURE_CSI in your config, or select a WiFi-capable board."
  #endif
  #if defined(FEATURE_CHIRP) && FEATURE_CHIRP
    #error "FEATURE_CHIRP=1 requires a WiFi radio (HAS_WIFI=0 on this board). Disable FEATURE_CHIRP in your config, or select a WiFi-capable board."
  #endif
#endif

// ─── Display ────────────────────────────────────────────────────────────

#if defined(FEATURE_DISPLAY) && FEATURE_DISPLAY && defined(HAS_DISPLAY) && !HAS_DISPLAY
  #error "FEATURE_DISPLAY=1 but this board has no display (HAS_DISPLAY=0). Disable FEATURE_DISPLAY in your config, or select a display board (see firmware/boards/boards.json)."
#endif

#if defined(FEATURE_TOUCH) && FEATURE_TOUCH && defined(HAS_TOUCH) && !HAS_TOUCH
  #error "FEATURE_TOUCH=1 but this board has no touch controller (HAS_TOUCH=0). Disable FEATURE_TOUCH in your config, or select a touch-capable board."
#endif

#if defined(FEATURE_BACKLIGHT_DIM) && FEATURE_BACKLIGHT_DIM && defined(HAS_BACKLIGHT_PWM) && !HAS_BACKLIGHT_PWM
  #error "FEATURE_BACKLIGHT_DIM=1 but this board's backlight is not PWM-dimmable (HAS_BACKLIGHT_PWM=0 — e.g. the Waveshare 4.3's CH422G is on/off only). Set FEATURE_BACKLIGHT_DIM=0 in your config."
#endif

// ─── Dev playground (bench mode) ────────────────────────────────────────
//
// The playground's whole safety story is the 4.3B's terminal block: every
// external wire lands on an isolated, buffered, or bused interface. So it
// deliberately breaks the "both sides defined" convention: an UNDEFINED
// HAS_ISOLATED_IO means the selected board has no isolated DI/DO and the
// mode must not build — failing loud beats a bench build whose DI/DO
// stations silently drive the wrong expander bits.

// FEATURE_DEVMODE reboots into that same peripheral bench from Settings, so it
// carries the identical board contract — the terminal block is still the whole
// safety story. Both flags are gated here together.
#if (defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND) || \
    (defined(FEATURE_DEVMODE) && FEATURE_DEVMODE)
  #if !defined(HAS_ISOLATED_IO) || !HAS_ISOLATED_IO
    #error "FEATURE_PLAYGROUND / FEATURE_DEVMODE require a board with isolated DI/DO (HAS_ISOLATED_IO=1 — the Waveshare ESP32-S3-Touch-LCD-4.3B, boards/waveshare-esp32s3-lcd43b). Build the canary-display-playground or canary-display-dash-b env, or in the Arduino IDE pick the 'Waveshare ESP32-S3-Touch-LCD-4.3B' board (see docs/hardware/dev_playground_43b.md)."
  #endif
  #if defined(HAS_DISPLAY) && !HAS_DISPLAY
    #error "FEATURE_PLAYGROUND / FEATURE_DEVMODE but this board has no display (HAS_DISPLAY=0) — the bench is a guided on-glass mode. Select the 4.3B display board."
  #endif
  #if defined(HAS_TOUCH) && !HAS_TOUCH
    #error "FEATURE_PLAYGROUND / FEATURE_DEVMODE but this board has no touch controller (HAS_TOUCH=0) — the bench's station cards are tap-driven. Select the 4.3B display board."
  #endif
#endif

// ─── On-glass modes: demo / debug / arcade ──────────────────────────────
//
// The bench's siblings (docs/hardware/display_modes.md) are tap-driven
// full-screen faces: they need glass and touch, nothing more — the
// isolated-IO contract belongs to the bench pair above, not here. These
// follow the standard both-sides-defined convention (adoption-safe on
// boards that predate the flags).
#if (defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE) ||   \
    (defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE) || \
    (defined(FEATURE_ARCADE) && FEATURE_ARCADE)
  #if defined(HAS_DISPLAY) && !HAS_DISPLAY
    #error "FEATURE_DEMO_MODE / FEATURE_DEBUG_MODE / FEATURE_ARCADE but this board has no display (HAS_DISPLAY=0) — the modes are full-screen faces. Build a canary-display modes env (see docs/hardware/display_modes.md), or drop the mode flag."
  #endif
  #if defined(HAS_TOUCH) && !HAS_TOUCH
    #error "FEATURE_DEMO_MODE / FEATURE_DEBUG_MODE / FEATURE_ARCADE but this board has no touch controller (HAS_TOUCH=0) — every mode is tap-driven and exits by long-press. Pick a touch display board, or drop the mode flag."
  #endif
#endif

// ─── Acoustic alarm listener (the mic-bearing dash) ─────────────────────
//
// FEATURE_MIC_ALARM is the 4.3C's alarm-pattern listener. It deliberately
// breaks the "both sides defined" convention the same way the playground
// does: an UNDEFINED HAS_MICROPHONE means the selected board never declared
// a mic, and a listening feature must not build on silence about the one
// capability that IS the privacy surface — failing loud beats a mic build
// landing on a board whose map never admitted to microphones.
#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM
  #if !defined(HAS_MICROPHONE) || !HAS_MICROPHONE
    #error "FEATURE_MIC_ALARM=1 but this board declares no microphone (HAS_MICROPHONE=1 required — the Waveshare ESP32-S3-Touch-LCD-4.3C, boards/waveshare-esp32s3-lcd43c). Build the canary-display-dash-mic env, or drop the flag (see docs/hardware/display_mic_variant.md)."
  #endif
#endif

// ─── Sensors & inputs ───────────────────────────────────────────────────

#if defined(FEATURE_GNSS) && FEATURE_GNSS && defined(HAS_GNSS_UART) && !HAS_GNSS_UART
  #error "FEATURE_GNSS=1 but this board has no GNSS UART wiring (HAS_GNSS_UART=0). Disable FEATURE_GNSS in your config, or add the GNSS UART pins to your board's pins.h (see firmware/PORTING.md)."
#endif

#if defined(FEATURE_TAMPER_GPIO) && FEATURE_TAMPER_GPIO && defined(HAS_TAMPER_INPUT) && !HAS_TAMPER_INPUT
  #error "FEATURE_TAMPER_GPIO=1 but this board has no tamper input (HAS_TAMPER_INPUT=0). Disable FEATURE_TAMPER_GPIO in your config, or define the tamper pin in your board's pins.h."
#endif

#if defined(FEATURE_VISION_AI) && FEATURE_VISION_AI && defined(HAS_VISION_AI) && !HAS_VISION_AI
  #error "FEATURE_VISION_AI=1 but this board is not a supported Vision AI host (HAS_VISION_AI=0). Disable FEATURE_VISION_AI in your config, or select a Vision AI host board (see firmware/boards/boards.json)."
#endif
