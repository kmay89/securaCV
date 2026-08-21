/*
 * SecuraCV Canary — Centralized Configuration
 *
 * Feature flags and debug flags are passed via platformio.ini build_flags.
 * This file provides defaults if they're not defined externally.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef CANARY_CONFIG_H
#define CANARY_CONFIG_H

// ════════════════════════════════════════════════════════════════
// FEATURE FLAG DEFAULTS (if not set by build system)
// ════════════════════════════════════════════════════════════════

#ifndef FEATURE_SD_STORAGE
  #define FEATURE_SD_STORAGE    1
#endif
#ifndef FEATURE_WIFI_AP
  #define FEATURE_WIFI_AP       1
#endif
#ifndef FEATURE_HTTP_SERVER
  #define FEATURE_HTTP_SERVER   1
#endif
#ifndef FEATURE_CAMERA_PEEK
  #define FEATURE_CAMERA_PEEK   1
#endif
#ifndef FEATURE_TAMPER_GPIO
  #define FEATURE_TAMPER_GPIO   0
#endif
#ifndef FEATURE_WATCHDOG
  #define FEATURE_WATCHDOG      1
#endif
#ifndef FEATURE_STATE_LOG
  #define FEATURE_STATE_LOG     1
#endif
#ifndef FEATURE_OTA_UPDATE
  #define FEATURE_OTA_UPDATE    1
#endif
#ifndef FEATURE_OTA_PULL
  #define FEATURE_OTA_PULL      0   // Signed pull-OTA engine (firmware/common/ota)
#endif
#ifndef FEATURE_HA_MQTT
  #define FEATURE_HA_MQTT       0
#endif
#ifndef FEATURE_HA_DISCOVERY
  #define FEATURE_HA_DISCOVERY  0
#endif
#ifndef FEATURE_MESH_NETWORK
  #define FEATURE_MESH_NETWORK  0
#endif
#ifndef FEATURE_BLUETOOTH
  #define FEATURE_BLUETOOTH     0
#endif
#ifndef FEATURE_BLE
  #define FEATURE_BLE           0   // BLE Discovery (Opera/Chirp/Nearby)
#endif
#ifndef FEATURE_SYS_MONITOR
  #define FEATURE_SYS_MONITOR   0
#endif
#ifndef FEATURE_WIFI_PRESENCE
  #define FEATURE_WIFI_PRESENCE 0   // WiFi probe request presence detection
#endif
#ifndef FEATURE_AUDIBLE_CHIRP
  #define FEATURE_AUDIBLE_CHIRP 0   // Local audible/visual alert tones
#endif
#ifndef FEATURE_RF_PRESENCE
  #define FEATURE_RF_PRESENCE   0   // RF presence detection
#endif
#ifndef FEATURE_CHIRP
  #define FEATURE_CHIRP         0   // Chirp community witness network
#endif
#ifndef FEATURE_GNSS
  #define FEATURE_GNSS          1   // GPS/GNSS telemetry (enabled by default)
#endif
#ifndef FEATURE_CSI
  #define FEATURE_CSI           0   // WiFi CSI motion / breathing / micro-activity sensing
#endif
#ifndef FEATURE_ACOUSTIC_EVENTS
  #define FEATURE_ACOUSTIC_EVENTS  0   // PDM mic + T3 smoke / T4 CO cadence detection
#endif
#ifndef FEATURE_ACOUSTIC_TRANSIENTS
  #define FEATURE_ACOUSTIC_TRANSIENTS 0  // Phase 2b: knock / doorbell / glass-break (opt-in)
#endif
#ifndef FEATURE_TOUCH
  #define FEATURE_TOUCH            0   // Capacitive touch (silent panic / tamper / approach)
#endif
#ifndef FEATURE_DEEP_SLEEP
  #define FEATURE_DEEP_SLEEP       0   // ESP32-S3 native deep-sleep arming (opt-in)
#endif
#ifndef FEATURE_IR_RMT
  #define FEATURE_IR_RMT           0   // RMT IR remote-control activity detection
#endif
#ifndef FEATURE_TEMP_TAMPER
  #define FEATURE_TEMP_TAMPER      0   // Internal die-temp drift tamper detection
#endif
#ifndef FEATURE_SENSING_WITNESS
  #define FEATURE_SENSING_WITNESS  0   // Sign emergency/security sensing events into witness chain
#endif
#ifndef FEATURE_VISION_DETECT
  #define FEATURE_VISION_DETECT    0   // 3-layer cascaded vision detection (motion + person)
#endif
#ifndef FEATURE_VISION_TFLITE
  #define FEATURE_VISION_TFLITE    0   // Layer 3: TFLite person classifier (opt-in, ~250KB flash)
#endif
#ifndef FEATURE_POWER_MONITOR
  #define FEATURE_POWER_MONITOR    1   // Battery voltage + charge state + SoC (ADC or inference)
#endif
#ifndef FEATURE_POWER_POLICY
  #define FEATURE_POWER_POLICY     1   // Runtime power mode engine (auto sleep/throttle by battery)
#endif
#ifndef FEATURE_THERMAL_WATCHDOG
  #define FEATURE_THERMAL_WATCHDOG 1   // Passive die-temp observer + NVS thermal history (never actuates)
#endif
#ifndef FEATURE_SETUP_WIZARD
  #define FEATURE_SETUP_WIZARD     1   // First-time setup detection, captive portal, device naming
#endif
#ifndef FEATURE_DIAGNOSTICS
  #define FEATURE_DIAGNOSTICS      1   // Heap monitoring, SD health, self-test, feature degradation
#endif
#ifndef FEATURE_DATA_MGMT
  #define FEATURE_DATA_MGMT        1   // SD log rotation, chain backup/restore, integrity verification
#endif
#ifndef FEATURE_BLE_STATUS
  #define FEATURE_BLE_STATUS       1   // BLE GATT status service (battery, health, chain over BLE)
#endif
#ifndef FEATURE_USB_ONBOARD
  #define FEATURE_USB_ONBOARD      0   // USB-OTG onboarding: consented HID help-launch + read-only SD-over-MSC + guided recovery/unseal (opt-in; needs ARDUINO_USB_MODE=0, enabled in [env:usb-onboard])
#endif
#ifndef USB_ONBOARD_AUTOLAUNCH
  #define USB_ONBOARD_AUTOLAUNCH   0   // Hands-off auto-open of the help page a few seconds after plug-in, with NO button press. This is genuine HID auto-typing (BadUSB-shaped) — leave OFF unless you accept that; the frictionless defaults are the START-HERE drive file + a one-tap BOOT press.
#endif
#ifndef FEATURE_TEST_CONSOLE
  #define FEATURE_TEST_CONSOLE     0   // Serial DEMO/MUTATE test commands (camera peek, mic beep, BLE advertise, pairing…). OFF in every shipping image: the console's 't' run-all is read-only (Tier::Diag) and always available, but the powerful demo/mutating commands are dev/test-only and never in a production binary (security + flash). See docs/design/test_console.md.
#endif
#ifndef FEATURE_CONSOLE_THEME
  #define FEATURE_CONSOLE_THEME    1   // The themed serial console: the 'l' identity banner (device fingerprint as drunken-bishop randomart) via the host-tested scene engine (common/ui/*). Safe by default — probes the terminal (ESC[6n) and stays 7-bit ASCII unless it answers, so it never becomes garbage. Small flash cost (pure header composition); compile out if the budget is tight. See docs/design/serial_console_theming.md.
#endif

// ════════════════════════════════════════════════════════════════
// DEBUG FLAG DEFAULTS
// ════════════════════════════════════════════════════════════════

#ifndef DEBUG_NMEA
  #define DEBUG_NMEA            0
#endif
#ifndef DEBUG_CBOR
  #define DEBUG_CBOR            0
#endif
#ifndef DEBUG_CHAIN
  #define DEBUG_CHAIN           0
#endif
#ifndef DEBUG_VERIFY
  #define DEBUG_VERIFY          0
#endif
#ifndef DEBUG_HTTP
  #define DEBUG_HTTP            0
#endif

// ════════════════════════════════════════════════════════════════
// VERSION & PROTOCOL (must match PWK expectations)
// ════════════════════════════════════════════════════════════════

#define DEVICE_TYPE           "canary"
#define FIRMWARE_VERSION      "2.4.12"

// Optional build-time provenance for the 'f' fingerprint command. CI/PlatformIO
// can inject the short commit with -DFIRMWARE_GIT_HASH=\"abc1234\"; when it
// isn't injected the build timestamp (__DATE__/__TIME__) still identifies the
// image, so the field degrades honestly rather than lying.
#ifndef FIRMWARE_GIT_HASH
#define FIRMWARE_GIT_HASH     "not-embedded"
#endif
#define RULESET_ID            "securacv:canary:v1.0"
#define PROTOCOL_VERSION      "pwk:v0.3.0"
#define CHAIN_ALGORITHM       "sha256-domain-sep"
#define SIGNATURE_ALGORITHM   "ed25519"

// The owner-help page the USB onboarding keyboard is allowed to open. This is
// the ONLY URL the HID keyboard can ever type — it is both the compile-time
// payload and the run-time allow-list origin (usb_onboard::is_allowed_help_url).
// Must be https and must match the origin served by the securacv_website repo.
#ifndef SECURACV_HELP_URL_BASE
  #define SECURACV_HELP_URL_BASE  "https://securacv.com/canary"
#endif

// ════════════════════════════════════════════════════════════════
// PULL-OTA (signed firmware updates — firmware/common/ota)
// ════════════════════════════════════════════════════════════════

// Product id matched against the manifest's "product" field.
#ifndef SECURACV_OTA_PRODUCT
  #define SECURACV_OTA_PRODUCT  "securacv-canary"
#endif

// Compiled-in default manifest URL. The firmware-release workflow
// publishes manifest-canary.json on every fw-v* GitHub Release; the
// "latest" alias keeps this URL stable across releases. Users can
// point at a local server instead via Settings (stored in NVS).
#ifndef SECURACV_OTA_MANIFEST_URL
  #define SECURACV_OTA_MANIFEST_URL \
    "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary.json"
#endif

// ════════════════════════════════════════════════════════════════
// HARDWARE PIN DEFINITIONS
// ════════════════════════════════════════════════════════════════

// SD card SPI (XIAO ESP32S3 Sense) - can be overridden by build flags
#ifndef SD_CS_PIN
  #define SD_CS_PIN    21
#endif
#ifndef SD_SCK_PIN
  #define SD_SCK_PIN   7
#endif
#ifndef SD_MISO_PIN
  #define SD_MISO_PIN  8
#endif
#ifndef SD_MOSI_PIN
  #define SD_MOSI_PIN  9
#endif

// GPS UART
#ifndef GPS_RX_PIN
  #define GPS_RX_PIN   44
#endif
#ifndef GPS_TX_PIN
  #define GPS_TX_PIN   43
#endif
#define GPS_BAUD       9600

// PDM microphone (XIAO ESP32-S3 Sense built-in MSM261D3526H1CPM)
#ifndef MIC_PIN_CLK
  #define MIC_PIN_CLK    42
#endif
#ifndef MIC_PIN_DATA
  #define MIC_PIN_DATA   41
#endif

// Tamper detection
#define TAMPER_GPIO    2

// Boot button
#define BOOT_BUTTON_GPIO  0

// ════════════════════════════════════════════════════════════════
// CAMERA CONFIG (XIAO ESP32S3 Sense OV2640)
// ════════════════════════════════════════════════════════════════

// Defaults are the XIAO ESP32-S3 Sense; board-specialized envs (e.g. the
// AI-Thinker ESP32-CAM / Freenove S3 CAM ports) override the whole set via
// -DCAM_PIN_* build flags, same pattern as the SD/GPS/MIC pins above.
#if FEATURE_CAMERA_PEEK
  #ifndef CAM_PIN_PWDN
    #define CAM_PIN_PWDN    -1
  #endif
  #ifndef CAM_PIN_RESET
    #define CAM_PIN_RESET   -1
  #endif
  #ifndef CAM_PIN_XCLK
    #define CAM_PIN_XCLK    10
  #endif
  #ifndef CAM_PIN_SIOD
    #define CAM_PIN_SIOD    40
  #endif
  #ifndef CAM_PIN_SIOC
    #define CAM_PIN_SIOC    39
  #endif
  #ifndef CAM_PIN_D7
    #define CAM_PIN_D7      48
  #endif
  #ifndef CAM_PIN_D6
    #define CAM_PIN_D6      11
  #endif
  #ifndef CAM_PIN_D5
    #define CAM_PIN_D5      12
  #endif
  #ifndef CAM_PIN_D4
    #define CAM_PIN_D4      14
  #endif
  #ifndef CAM_PIN_D3
    #define CAM_PIN_D3      16
  #endif
  #ifndef CAM_PIN_D2
    #define CAM_PIN_D2      18
  #endif
  #ifndef CAM_PIN_D1
    #define CAM_PIN_D1      17
  #endif
  #ifndef CAM_PIN_D0
    #define CAM_PIN_D0      15
  #endif
  #ifndef CAM_PIN_VSYNC
    #define CAM_PIN_VSYNC   38
  #endif
  #ifndef CAM_PIN_HREF
    #define CAM_PIN_HREF    47
  #endif
  #ifndef CAM_PIN_PCLK
    #define CAM_PIN_PCLK    13
  #endif
#endif

// ════════════════════════════════════════════════════════════════
// WIFI AP DEFAULTS
// ════════════════════════════════════════════════════════════════

// Fallback only — production uses device-unique password derived from pubkey
#define AP_PASSWORD_DEFAULT  "witness2026"
#define AP_CHANNEL           1
#define AP_MAX_CONNECTIONS   1    // Hardened: max 1 client for security isolation

// Radio defaults applied once at network bring-up. Pinning the PHY to HT20 +
// 11bgn keeps the WiFi-CSI subcarrier count constant — an HT40 association or
// rate renegotiation would change it and destabilize the fixed 32-dim CSI
// feature vector. The country code sets the correct regulatory channel set / TX
// ceiling; with 802.11d enabled the STA adapts it to the associated AP, so the
// world-safe "01" default never blocks a router on ch 12/13.
#ifndef CANARY_WIFI_COUNTRY
  #define CANARY_WIFI_COUNTRY  "01"   // world-safe; 802.11d adapts to the AP
#endif
#ifndef CANARY_WIFI_TX_QDBM
  #define CANARY_WIFI_TX_QDBM  78     // quarter-dBm (~19.5 dBm); ESP32-S3 range 8..84
#endif

// ════════════════════════════════════════════════════════════════
// BLE DEFAULTS
// ════════════════════════════════════════════════════════════════

// BLE TX power in dBm (NimBLE 2.x setPower takes dBm directly, NOT an
// ESP_PWR_LVL_* index). +9 dBm is the S3 maximum and matches canary-wap's
// F5-audited NVS default; the XIAO ESP32S3 has no onboard antenna (U.FL +
// external rod only), so the historically-effective +9 is the safe default.
#define BLE_TX_POWER_DBM     9

// ════════════════════════════════════════════════════════════════
// TIMING & COARSENING
// ════════════════════════════════════════════════════════════════

#define RECORD_INTERVAL_MS       1000    // Record emission rate
#define TIME_BUCKET_MS           5000    // Time coarsening bucket
#define FIX_LOST_TIMEOUT_MS      3000    // GPS fix timeout
#define VERIFY_INTERVAL_SEC      60      // Self-verify every N seconds
#define WATCHDOG_TIMEOUT_SEC     8       // Hardware watchdog
#define SD_PERSIST_INTERVAL      10      // Persist every N records

// ════════════════════════════════════════════════════════════════
// MOTION DETECTION WITH HYSTERESIS
// ════════════════════════════════════════════════════════════════

#define MOVING_THRESHOLD_MPS     0.8f
#define STATIC_THRESHOLD_MPS     0.4f
#define SPEED_EMA_ALPHA          0.15f
#define STATE_HYSTERESIS_MS      2000

// ════════════════════════════════════════════════════════════════
// THERMAL THRESHOLDS (die temperature, °C)
// Single source for the camera actuator (securacv_camera) and the
// thermal watchdog's shadow classifier (securacv_thermal_watchdog)
// so the two can never silently drift apart. Rationale and Seeed
// source data: docs/esp32s3_thermal_review.md.
// ════════════════════════════════════════════════════════════════

#define THERMAL_THROTTLE_TEMP_C   70     // enter adaptive (throttled) mode
#define THERMAL_PAUSE_TEMP_C      80     // enter protective pause
#define THERMAL_RECOVER_MARGIN_C   5     // recover this far below entry

// ════════════════════════════════════════════════════════════════
// USB CDC & OPERATOR INTERFACE
// ════════════════════════════════════════════════════════════════

#define SERIAL_CDC_WAIT_MS       2500
#define BOOT_BUTTON_HOLD_MS      2000
#define BOOT_SHORT_PRESS_MS      50      // Debounce floor for short press
#define BOOT_LONG_PRESS_MS       5000    // Hold for factory reset
#define BOOT_MEDIUM_PRESS_MS     2000    // Medium hold for info print

// ════════════════════════════════════════════════════════════════
// SD CARD SPI SPEEDS
// ════════════════════════════════════════════════════════════════
//
// FAST is the normal operating clock; the storage driver falls back to SLOW
// (and retries the whole SD.begin ladder) if a card fails to init at FAST, so
// a card that can't sustain FAST degrades gracefully rather than failing to
// mount. 20 MHz is well within SD-SPI limits on the short XIAO-Sense expansion
// traces (SPI mode tops out at 25 MHz per the SD spec) and cuts the
// synchronous per-record FAT-append window on the loop task ~5x vs the old
// 4 MHz. Hardware bench validation across a range of cards is recommended
// before treating 20 MHz as verified — see firmware/CONFIG_CHANGES.md.
#ifndef SD_SPI_FAST
  #define SD_SPI_FAST            20000000  // 20 MHz normal operation
#endif
#ifndef SD_SPI_SLOW
  #define SD_SPI_SLOW            1000000   // 1 MHz init/recovery fallback
#endif

// ════════════════════════════════════════════════════════════════
// WIFI PROVISIONING
// ════════════════════════════════════════════════════════════════

#define WIFI_CONNECT_TIMEOUT_MS      30000
#define WIFI_RECONNECT_INTERVAL_MS   30000

// ════════════════════════════════════════════════════════════════
// NVS KEYS
// ════════════════════════════════════════════════════════════════

#define NVS_MAIN_NS       "securacv"
#define NVS_KEY_PRIV      "privkey"
#define NVS_KEY_SEQ       "seq"
#define NVS_KEY_BOOTS     "boots"
#define NVS_KEY_CHAIN     "chain"
#define NVS_KEY_TAMPER    "tamper"
#define NVS_KEY_LOGSEQ    "logseq"
// The day this device's key was born, and whether that day may be called a
// birthday — written exactly once, never overwritten. See
// firmware/common/identity/birth_day.h for why a device that can restate its
// own birth day is a device that launders its age.
#define NVS_KEY_BORN      "born_day"
#define NVS_KEY_BORN_EX   "born_exact"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_WIFI_EN   "wifi_en"
#define NVS_KEY_TOKEN     "api_token"
#define NVS_KEY_BATT_CAP  "batt_cap"
#define NVS_KEY_BATT_CYC  "batt_cycles"
#define NVS_KEY_BATT_MAX  "batt_max_mv"
#define NVS_KEY_BATT_MIN  "batt_min_mv"
#define NVS_KEY_BATT_RT   "batt_rt_min"   // total runtime on battery (minutes)
#define NVS_KEY_BATT_SOC  "batt_soc_min"  // all-time lowest SoC %
#define NVS_KEY_BATT_BO   "batt_bo_cnt"   // brownout reset count
#define NVS_KEY_BATT_FC   "batt_fc_ms"    // last full charge millis()
#define NVS_KEY_SETUP_OK  "setup_ok"
#define NVS_KEY_DEV_NAME  "dev_name"

// ════════════════════════════════════════════════════════════════
// MQTT (Home Assistant)
// ════════════════════════════════════════════════════════════════

#ifndef MQTT_PORT
  #define MQTT_PORT          1883
#endif
#ifndef MQTT_TOPIC_PREFIX
  #define MQTT_TOPIC_PREFIX  "securacv"
#endif

// ════════════════════════════════════════════════════════════════
// RATE LIMITING
// ════════════════════════════════════════════════════════════════

#define RATE_LIMIT_WINDOW_MS     60000   // 1 minute window
#define RATE_LIMIT_MAX_REQUESTS  120     // Max requests per window
#define RATE_LIMIT_MAX_ACTIONS   30      // Max POST actions per window

// ════════════════════════════════════════════════════════════════
// ZONE ID
// ════════════════════════════════════════════════════════════════

#define ZONE_ID              "zone:mobile"
#define DEVICE_ID_PREFIX     "canary-s3-"

#endif // CANARY_CONFIG_H
