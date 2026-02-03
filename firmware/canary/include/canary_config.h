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
#define FIRMWARE_VERSION      "2.1.0"
#define RULESET_ID            "securacv:canary:v1.0"
#define PROTOCOL_VERSION      "pwk:v0.3.0"
#define CHAIN_ALGORITHM       "sha256-domain-sep"
#define SIGNATURE_ALGORITHM   "ed25519"

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

// Tamper detection
#define TAMPER_GPIO    2

// Boot button
#define BOOT_BUTTON_GPIO  0

// ════════════════════════════════════════════════════════════════
// CAMERA CONFIG (XIAO ESP32S3 Sense OV2640)
// ════════════════════════════════════════════════════════════════

#if FEATURE_CAMERA_PEEK
  #define CAM_PIN_PWDN    -1
  #define CAM_PIN_RESET   -1
  #define CAM_PIN_XCLK    10
  #define CAM_PIN_SIOD    40
  #define CAM_PIN_SIOC    39
  #define CAM_PIN_D7      48
  #define CAM_PIN_D6      11
  #define CAM_PIN_D5      12
  #define CAM_PIN_D4      14
  #define CAM_PIN_D3      16
  #define CAM_PIN_D2      18
  #define CAM_PIN_D1      17
  #define CAM_PIN_D0      15
  #define CAM_PIN_VSYNC   38
  #define CAM_PIN_HREF    47
  #define CAM_PIN_PCLK    13
#endif

// ════════════════════════════════════════════════════════════════
// WIFI AP DEFAULTS
// ════════════════════════════════════════════════════════════════

#define AP_PASSWORD_DEFAULT  "witness2026"
#define AP_CHANNEL           1
#define AP_MAX_CONNECTIONS   4

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
// USB CDC & OPERATOR INTERFACE
// ════════════════════════════════════════════════════════════════

#define SERIAL_CDC_WAIT_MS       2500
#define BOOT_BUTTON_HOLD_MS      1200

// ════════════════════════════════════════════════════════════════
// SD CARD SPI SPEEDS
// ════════════════════════════════════════════════════════════════

#define SD_SPI_FAST              4000000   // 4 MHz
#define SD_SPI_SLOW              1000000   // 1 MHz fallback

// ════════════════════════════════════════════════════════════════
// WIFI PROVISIONING
// ════════════════════════════════════════════════════════════════

#define WIFI_CONNECT_TIMEOUT_MS      15000
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
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_WIFI_EN   "wifi_en"

// ════════════════════════════════════════════════════════════════
// MQTT (Home Assistant)
// ════════════════════════════════════════════════════════════════

#if FEATURE_HA_MQTT
  #define MQTT_SERVER        "homeassistant.local"
  #define MQTT_PORT          1883
  #define MQTT_USER          "securacv"
  #define MQTT_TOPIC_PREFIX  "securacv/canary"
#endif

// ════════════════════════════════════════════════════════════════
// ZONE ID
// ════════════════════════════════════════════════════════════════

#define ZONE_ID              "zone:mobile"
#define DEVICE_ID_PREFIX     "canary-s3-"

#endif // CANARY_CONFIG_H
