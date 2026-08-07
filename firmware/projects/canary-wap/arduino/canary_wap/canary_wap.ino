/*
  ╔══════════════════════════════════════════════════════════════════════════════╗
  ║  SecuraCV Canary — Production Witness Device with SD & WAP                   ║
  ║  Version 2.0.1 — SD Storage + WiFi Access Point + Fixed Camera Peek          ║
  ║                                                                              ║
  ║  Privacy Witness Kernel (PWK) Compatible                                     ║
  ║  Hardware: XIAO ESP32-S3 Sense / XIAO ESP32-C3 + L76K GNSS + microSD        ║
  ║  Target:   Set HARDWARE_XIAO_ESP32C3 or HARDWARE_XIAO_ESP32S3 in            ║
  ║            build_config.h to match your board                                ║
  ╚══════════════════════════════════════════════════════════════════════════════╝
  
  SECURITY PROPERTIES:
  ────────────────────
  ✓ Unique device identity from hardware RNG (NVS persistence)
  ✓ Monotonic sequence numbers (persist across reboots, crash-safe)
  ✓ Hash chain with domain separation (tamper-evident)
  ✓ Ed25519 signatures on every record
  ✓ Crypto self-test at boot + periodic verification
  ✓ Time coarsening (5-second buckets, no precise timestamps)
  ✓ Chain state persistence (survives power loss)
  ✓ Boot attestation record (identity proof on first record)
  ✓ Watchdog timer (hardware reset on hang)
  ✓ Tamper detection GPIO (optional enclosure breach sensor)
  ✓ State transition logging with hysteresis (reduces noise)
  ✓ Complete GPS telemetry capture
  
  NEW IN 2.0:
  ───────────
  ✓ SD card storage for witness records (append-only)
  ✓ SD card storage for health/diagnostic logs
  ✓ WiFi Access Point for local monitoring
  ✓ HTTP API for status, logs, export
  ✓ Web UI dashboard
  ✓ Log acknowledgment system (audit trail preserved)
  ✓ PWK-compatible export bundles
  
  FIXED IN 2.0.1:
  ───────────────
  ✓ Camera peek streaming now works (g_peek_active state fix)
  ✓ Added /api/peek/start endpoint
  ✓ Added /api/peek/resolution endpoint for frame size control
  ✓ Proper MJPEG boundary handling
  
  PWK COMPATIBILITY:
  ──────────────────
  ✓ CBOR payloads match event_contract.md
  ✓ Chain hash compatible with log_verify
  ✓ Ruleset versioning for verification compatibility
  ✓ Export format compatible with export_verify
  ✓ Device pubkey location matches database schema
  
  Library requirements:
  - "Crypto" by Rhys Weatherley (Arduino Library Manager)
  - ESP32 Arduino Core 3.x
  - ArduinoJson 7.x
*/

// Emit one-shot build banners (release-mode AP-password notice, NimBLE
// auto-disable notes) only from this main translation unit. Defined before
// any include so transitively-included build_config.h / ble_config.h see it.
#define SECURACV_EMIT_BUILD_BANNER 1

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

#include "esp_system.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_http_server.h"

// Signed pull-OTA engine — committed copy of firmware/common/ota (kept in
// sync by firmware/scripts/check_ota_sync.sh). ota_release_key.h carries
// the shared Ed25519 release public key, the same one ble_ota verifies.
#include "securacv_ota.h"
#include "ota_release_key.h"
// Needed for SO_KEEPALIVE / TCP_KEEP* socket options applied to the
// long-lived MJPEG peek stream so half-open TCP sockets get torn down
// at the kernel layer instead of waiting on a doomed application send.
#include "lwip/sockets.h"
#include "lwip/inet.h"
// TLS self-signed cert generation requires mbedtls x509write support,
// which is not available in all ESP32 Arduino Core builds (e.g., 3.3.7).
// Auto-detect: if the header exists, enable runtime TLS cert generation.
#if __has_include("mbedtls/x509write_crt.h")
  #define SECURACV_HAS_TLS_CERTGEN 1
#else
  #define SECURACV_HAS_TLS_CERTGEN 0
#endif

// esp_https_server.h requires CONFIG_ESP_HTTPS_SERVER_ENABLE in ESP-IDF.
// Guard it so builds without HTTPS support fall back to HTTP-only.
#if __has_include("esp_https_server.h")
  #include "esp_https_server.h"
  #define SECURACV_HAS_HTTPS_SERVER 1
#else
  #define SECURACV_HAS_HTTPS_SERVER 0
#endif
#include "esp_mac.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#if SECURACV_HAS_TLS_CERTGEN
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509write_crt.h"
#endif

#include <WiFi.h>
#include <bootloader_random.h>  // early-entropy source for the first-boot identity keygen
#include <ESPmDNS.h>
#include "mdns.h"               // IDF mDNS C API — delegated hostname for canary.local catch-all
#include "esp_idf_version.h"    // ESP_IDF_VERSION gate for mdns_delegate_hostname_add (>= 4.4)
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

#include <Crypto.h>
#include <Ed25519.h>

#include "esp_camera.h"

#include <boot_banner.h>
#include "log_level.h"
#include "health_log.h"
#include "sd_storage.h"
#include "gnss_time.h"  // NMEA UTC date/time -> validated Unix epoch (GPS-derived system clock)
#include "nvs_store.h"
#include "api_auth.h"
#include "wifi_provisioning_auth.h"  // WifiChangeAuth enum — must precede the
                                     // auto-generated prototype for
                                     // wifi_change_authorize()
#include "config_logic.h"            // runtime device-config clamps (privacy floor)
#include "peek_stream_logic.h"       // pure, host-tested camera-stream value math
#include "csi_mem.h"                 // csi_large_calloc: PSRAM-first big buffers
#include <new>                       // placement-new for the PSRAM GPS ring
#include "catchall_logic.h"          // pure, host-tested canary.local claim decisions
#include "witness_store.h"           // pure, host-tested /WITNESS jsonl format + recovery
#include "fleet_selfreport.h"        // shared /api/fleet body builder (parity by architecture)
#include "birth_day.h"               // pure, host-tested "when was this key born" rules
#include "camera_gate_logic.h"       // pure, host-tested camera standby/peek gating
#include "power_gate_logic.h"        // pure, host-tested round-two power gate decisions
#include "health_store_logic.h"      // pure, host-tested /HEALTH jsonl format
#include "wap_server.h"
// Ship the dashboard/settings/companion HTML as pre-gzipped byte arrays
// instead of the raw PROGMEM literals — saves ~336 KB of app-partition flash.
// The CANARY_WEB_ASSETS_GZIPPED guard (defined in build_config.h, which the
// asset headers self-include) compiles the uncompressed copies out of the
// binary; those files stay the editable source of truth (regen via
// gen_web_assets_gz.py).
#include "web_assets_gz.h"
#include "web_ui.h"
#include "companion_pwa.h"
#include "csi_integration.h"     // Boot the CSI library + HTTP endpoints
#include "csi_mqtt.h"            // Optional MQTT bridge for HA integration
#include "device_signature.h"    // Ed25519 sigs over MQTT publishes (per-device PKI)
#include "csi_event_log.h"       // SD-backed event persistence + MQTT backfill
#include "csi_witness_payload.h" // Builds the witness-chain payload string
#include <ble_events_module.h>   // spec §10 BLE event chokepoint helpers
#include "usb_evidence_drive.h" // USB evidence drive / update drop-zone (opt-in build)
#include "setup_page_html.h"     // Static captive-portal "open canary.local" page
#include "captive_probe.h"       // Pure per-platform connectivity-probe response policy
extern "C" {
#include "qrcodegen.h"           // Vendored Nayuki QR encoder, MIT
}
#include "csi_dashboard_html.h"  // CSI_DASHBOARD_HTML — the Phase-3 headline UI now served at /
#include "mesh_network.h"
#include "mesh_channel_policy.h"  // Channel decision (STA-follow) for MQTT telemetry
#include "airtime_governor.h"     // Rolling airtime stats for MQTT telemetry
#include "bluetooth_channel.h"
#include "bluetooth_api.h"
#include "ble_console.h"
#include "ble_log_export.h"
#include "household_api.h"
#include "device_pseudonym.h"
#include "sys_monitor.h"
#include "hardware_state.h"
#include "selftest_api.h"        // GET /api/selftest — wizard pre-flight aggregator
#include "data_mgmt_api.h"      // SD rotation, chain backup/restore, integrity verify

// ════════════════════════════════════════════════════════════════════════════
// BUILD CONFIGURATION — Edit build_config.h to select profile
// ════════════════════════════════════════════════════════════════════════════
// Build profiles (edit build_config.h to switch):
//   MINIMAL - Fastest build (~45s): crypto + GPS only
//   DEV     - Development (~90s):   + WiFi + HTTP + SD
//   FULL    - All features (~150s): + Camera + Mesh + BLE
// ════════════════════════════════════════════════════════════════════════════

#include "build_config.h"

#if FEATURE_QR_PROVISION
#include "qr_scanner.h"
#include "provision_qr.h"  // shared SCV1/WIFI: grammar (firmware/common —
                           // the display mints these codes; sync-guarded)
#endif

// BLE Discovery Subsystem (Opera/Chirp/Nearby)
#include "ble_config.h"
#include "ble_manager.h"

// BLE GATT Status Service (battery/health/chain over GATT)
#include "ble_status_api.h"

// BLE Scout (CSI room attribution): its NimBLE scan bring-up is completed by
// the post-join-window gate (ble_discovery_start_if_due), NOT by
// csi_integration's early state-only init — see ble_scout_allow_radio().
#if FEATURE_BLE_SCAN
#include "ble_scout.h"
#endif

// WiFi Presence Detection (probe request monitoring)
#include "wifi_presence.h"
#include "wifi_presence_api.h"
#include "rf_presence.h"
#include "rf_presence_api.h"

// Audible Chirp (local alert tones — PWM buzzer / LED blink)
#include "audible_chirp.h"
#include "audible_chirp_api.h"

// Chirp Channel v0.2 REST API (audit C12). Compiled only when mesh is on.
#if FEATURE_MESH_NETWORK
#include "chirp_api.h"
#endif

// Beacon Channel REST API. Compiled only when FEATURE_BEACON_CHANNEL is on.
#if FEATURE_BEACON_CHANNEL
#include "beacon_api.h"
#endif

#include "setup_wizard.h"

// Power monitoring and battery policy engine
#if FEATURE_POWER_MONITOR
#include "power_monitor.h"
#endif
#if FEATURE_POWER_POLICY
#include "power_policy.h"
// Lock the pure header's mode mirror to the real enum so the CSI gate and
// MQTT cadence stretch can't silently desync from power_policy.h.
static_assert((uint8_t)PMODE_PLUGGED_IN     == power_gate::MODE_PLUGGED_IN,     "power_gate mode drift");
static_assert((uint8_t)PMODE_BATTERY_NORMAL == power_gate::MODE_BATTERY_NORMAL, "power_gate mode drift");
static_assert((uint8_t)PMODE_BATTERY_SAVER  == power_gate::MODE_BATTERY_SAVER,  "power_gate mode drift");
static_assert((uint8_t)PMODE_LOW_POWER      == power_gate::MODE_LOW_POWER,      "power_gate mode drift");
static_assert((uint8_t)PMODE_SHUTDOWN       == power_gate::MODE_SHUTDOWN,       "power_gate mode drift");
static_assert((uint8_t)PMODE_USB_ONLY       == power_gate::MODE_USB_ONLY,       "power_gate mode drift");
#endif

// PDM acoustic event detection (T3 smoke / T4 CO alarm cadences)
#if FEATURE_ACOUSTIC_EVENTS
#include "securacv_audio.h"
#include "acoustic_events_module.h"

// Last acoustic event, held for the MQTT /sensing snapshot. Written by
// the audio event callback and read by the loop's MQTT cadence block —
// both run on the main task (the callback fires synchronously from
// audio_process() in loop()), so no atomics are needed. The loop clears
// the event back to "none" after AUDIO_MQTT_EVENT_HOLD_MS, which flips
// HA's smoke/CO/knock/doorbell/glass binary sensors OFF.
static char     g_audio_mqtt_event[20] = "none";
static uint32_t g_audio_mqtt_event_ms  = 0;
static bool     g_audio_mqtt_dirty     = false;
static const uint32_t AUDIO_MQTT_EVENT_HOLD_MS = 30000;
#endif

// Sealed alarm snapshots — opt-in camera frames on life-safety acoustic
// triggers, encrypted to an operator-held key the device never holds
// (write-only escrow; see vault_snapshot.h and docs/sealed_snapshot_vault.md).
// NOTE: only includes and variables here — a function DEFINITION this early
// would become the sketch's first one, moving the Arduino builder's
// auto-prototype insertion point above the .ino's own type declarations
// (FixState, GnssFix, WitnessRecord, ...) and breaking the whole build.
// vault_rotate_dir_hook() is defined next to the vault HTTP handlers.
#if FEATURE_VAULT_SNAPSHOT
#include "vault_logic.h"
#include "vault_snapshot.h"
#include "vault_events_module.h"

// POST /api/vault/test runs on the HTTP task, but request_capture() is
// loop-task-only (cooldown stamps, NVS seq, the job struct and the CSI time
// bucket are all loop-owned). The handler latches this flag; loop() consumes
// it and runs the real request there — same deferral pattern as the HA OTA
// install command.
static volatile bool g_vault_test_pending = false;
#endif

// ════════════════════════════════════════════════════════════════════════════
// VERSION & PROTOCOL (must match PWK expectations)
// ════════════════════════════════════════════════════════════════════════════

static const char* DEVICE_TYPE        = "canary";
static const char* FIRMWARE_VERSION   = "2.4.8-wap";  // Vision runtime config release train
static const char* RULESET_ID         = "securacv:canary:v1.0";
static const char* PROTOCOL_VERSION   = "pwk:v0.3.0";
static const char* CHAIN_ALGORITHM    = "sha256-domain-sep";
static const char* SIGNATURE_ALGORITHM = "ed25519";

// ── Signed pull-OTA ─────────────────────────────────────────────────────────
// Product id matched against the manifest's "product" field, and the
// compiled-in default manifest URL (the firmware-release workflow publishes
// manifest-canary-wap.json on every fw-v* GitHub Release; the "latest"
// alias keeps this URL stable). Users can point at a local server instead
// via Settings (stored in NVS by the engine).
static const char* OTA_PRODUCT = "securacv-canary-wap";
#ifndef SECURACV_OTA_MANIFEST_URL
#define SECURACV_OTA_MANIFEST_URL \
  "https://github.com/kmay89/securaCV/releases/latest/download/manifest-canary-wap.json"
#endif

/* Set by the OTA progress callback (runs on the OTA task, must stay
 * non-blocking); the main loop turns it into an MQTT publish. */
static volatile bool g_ota_publish_pending = false;
static securacv_ota_state_t g_ota_last_seen_state = SECURACV_OTA_IDLE;
static uint32_t g_ota_next_check_ms = 0;

// ════════════════════════════════════════════════════════════════════════════
// DEVICE CONFIG
// ════════════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════════════
// DEVICE ID — derived from hardware target
// ════════════════════════════════════════════════════════════════════════════

#if defined(HARDWARE_XIAO_ESP32C3)
static const char* DEVICE_ID_PREFIX = "canary-c3-";
#else
static const char* DEVICE_ID_PREFIX = "canary-s3-";
#endif
static const char* ZONE_ID   = "zone:mobile";

// ════════════════════════════════════════════════════════════════════════════
// GNSS CONFIG — Pin assignments differ per hardware target
// ════════════════════════════════════════════════════════════════════════════

static const uint32_t GPS_BAUD = 9600;

#if defined(HARDWARE_XIAO_ESP32C3)
// XIAO ESP32-C3 pin mapping (GPIO 0-21 only)
// D6 = GPIO 2, D7 = GPIO 3 on the C3 pinout
// WARNING: GPIO 2 is a strapping pin on C3 — ensure it is not pulled
// high/low externally during boot, or the chip may enter wrong boot mode.
static const int GPS_RX_GPIO = 20;  // XIAO C3 D7 (GPIO 20)
static const int GPS_TX_GPIO = 21;  // XIAO C3 D6 (GPIO 21)
#else
// XIAO ESP32-S3 Sense pin mapping
static const int GPS_RX_GPIO = 44;  // XIAO S3 D7
static const int GPS_TX_GPIO = 43;  // XIAO S3 D6
#endif

// ════════════════════════════════════════════════════════════════════════════
// SD CARD CONFIG — Pin assignments differ per hardware target
// ════════════════════════════════════════════════════════════════════════════

#if defined(HARDWARE_XIAO_ESP32C3)
// XIAO ESP32-C3 SPI pins for SD card
// Using the standard SPI pins on C3 to avoid strapping pins (GPIO 8, 9)
// D0=GPIO2, D1=GPIO3, D2=GPIO4, D3=GPIO5, D8=GPIO8(SCK), D9=GPIO9(MISO), D10=GPIO10(MOSI)
// NOTE: GPIO 8 and 9 are strapping pins on C3. If SD card is connected
// to these pins, ensure pull-up/pull-down resistors match the boot config
// (GPIO 8 HIGH for normal boot, GPIO 9 HIGH for normal boot).
static const int SD_CS_PIN   = 5;   // XIAO C3 D3 (GPIO 5)
static const int SD_SCK_PIN  = 8;   // XIAO C3 D8 (GPIO 8) — strapping pin, needs pull-up
static const int SD_MISO_PIN = 9;   // XIAO C3 D9 (GPIO 9) — strapping pin, needs pull-up
static const int SD_MOSI_PIN = 10;  // XIAO C3 D10 (GPIO 10)
#else
// XIAO ESP32-S3 Sense SPI pins (directly on expansion board)
static const int SD_CS_PIN   = 21;
static const int SD_SCK_PIN  = 7;
static const int SD_MISO_PIN = 8;
static const int SD_MOSI_PIN = 9;
#endif
static const uint32_t SD_SPI_FAST = 4000000;
static const uint32_t SD_SPI_SLOW = 1000000;

// ════════════════════════════════════════════════════════════════════════════
// WIFI AP CONFIG
// ════════════════════════════════════════════════════════════════════════════

static const int   AP_CHANNEL          = 1;
static const int   AP_MAX_CLIENTS      = 1;  // Hardened: max 1 client for security

// Once the STA has held its association to the home network for this long, the
// management SoftAP is torn down so the single 2.4 GHz radio runs STA + BLE —
// Espressif's stable (Y) coexistence combo — instead of AP + STA + BLE, which
// the RF-coexistence support matrix rates C1 (supported but unstable) once a
// client is joined to the AP. The AP is re-raised automatically if the STA
// link drops (see wifi_raise_ap / wifi_drop_ap). See docs/esp32s3_ble_wap_audit.md.
//
// The grace must cover the provisioning handoff: joining a home AP on a
// channel != AP_CHANNEL drags the SoftAP (single radio) to that channel,
// momentarily kicking the provisioning phone. The phone re-associates to the
// same SSID on the new channel within seconds — but then still needs DHCP and
// a few /api/wifi polls before the wizard can render its success card. The
// old 8 s grace lost that race almost every time, so a *successful* join
// looked like "Couldn't connect" on the phone. Two minutes of the C1 combo
// during provisioning only is an acceptable trade for a truthful wizard.
static const uint32_t AP_DROP_GRACE_MS = 120000;

// ════════════════════════════════════════════════════════════════════════════
// CAMERA CONFIG (XIAO ESP32-S3 Sense only — ESP32-C3 has no camera interface)
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_CAMERA_PEEK && HW_HAS_CAMERA
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

// ════════════════════════════════════════════════════════════════════════════
// TIMING & COARSENING
// ════════════════════════════════════════════════════════════════════════════

static const uint32_t RECORD_INTERVAL_MS   = 1000;    // Record emission rate (default)
static const uint32_t TIME_BUCKET_MS       = 5000;    // Time coarsening bucket — the PRIVACY FLOOR (Invariant III)
static const uint32_t FIX_LOST_TIMEOUT_MS  = 3000;    // GPS fix timeout

// ── Operator-configurable runtime settings (Device tab "Save Configuration",
// NVS-persisted). The compile-time constants above are the defaults; the time
// bucket constant is additionally the privacy FLOOR — g_time_bucket_ms may be
// clamped to at least the floor, so event timing is NEVER finer than the
// compile-time minimum (see config_logic.h, Invariant III). ──
static const uint32_t RECORD_INTERVAL_MIN_MS = 250;
static const uint32_t RECORD_INTERVAL_MAX_MS = 60000;
static const uint8_t  LOG_LEVEL_STORE_MAX    = SCV_LOG_WARNING;  // never drop ERROR/CRITICAL
static uint32_t g_record_interval_ms = RECORD_INTERVAL_MS;
static uint32_t g_time_bucket_ms     = TIME_BUCKET_MS;
static uint8_t  g_log_min_level      = SCV_LOG_INFO;
static const uint32_t VERIFY_INTERVAL_SEC  = 60;      // Self-verify every N seconds
static const uint32_t WATCHDOG_TIMEOUT_SEC = 8;       // Watchdog timeout
static const uint32_t SD_PERSIST_INTERVAL  = 10;      // Persist every N records

// ════════════════════════════════════════════════════════════════════════════
// USB CDC & OPERATOR INTERFACE
// ════════════════════════════════════════════════════════════════════════════

static const uint32_t SERIAL_CDC_WAIT_MS   = 2500;
// BOOT button gesture thresholds. The short-press window opens the
// provisioning gate; the medium-hold prints a serial diagnostic; the
// long-hold triggers factory reset. The short-press lower bound (50 ms)
// is the debounce floor — anything quicker is mechanical bounce, not a
// human tap. The upper bound (2000 ms) matches the "< 2 seconds" hint
// already shown in the wizard, so a slightly slow tap still counts.
static const uint32_t BOOT_BUTTON_HOLD_MS  = 2000;
static const int      BOOT_BUTTON_GPIO     = 0;
static const uint32_t BOOT_SHORT_PRESS_MS  = 50;
static const uint32_t BOOT_LONG_PRESS_MS   = 3000;
// Provisioning gate stays open for this long after a BOOT press, so a
// browser tab that polls every 2 s reliably catches it and a captive
// portal interstitial can't time the user out. The gate is still
// single-use: the first successful receipt fetch closes it immediately.
static const uint32_t PROVISIONING_GATE_TTL_MS = 30000;

// ════════════════════════════════════════════════════════════════════════════
// MOTION DETECTION WITH HYSTERESIS
// ════════════════════════════════════════════════════════════════════════════
//
// Mounted cameras sit still in a window: the L76K still emits ~0.3–0.7 m/s of
// "speed" and a few meters of position wander from multipath. If we surface
// that raw, the dashboard looks like the camera is drifting around — which
// kills credibility for evidence chains. The motion filter below holds the
// displayed position to a smoothed anchor whenever the device is at rest, and
// only releases it once we see sustained, unambiguous motion (matching the
// existing FixState hysteresis). This keeps a stolen/moving deployment honest
// (real motion still shows) while making a stationary deployment rock-solid.

static const float    MOVING_THRESHOLD_MPS   = 0.8f;
static const float    STATIC_THRESHOLD_MPS   = 0.4f;
static const float    SPEED_EMA_ALPHA        = 0.15f;
static const uint32_t STATE_HYSTERESIS_MS    = 2000;

// Position lock parameters. STATIONARY_RADIUS_M is the maximum drift from the
// anchor we accept as "still"; HDOP_LOCK_MAX gates the filter to fixes that
// are at least minimally trustworthy (HDOP > 5 means we're in deep multipath
// and should not anchor against that noise).
static const double   STATIONARY_RADIUS_M    = 8.0;
static const double   HDOP_LOCK_MAX          = 5.0;
static const float    ANCHOR_EMA_ALPHA       = 0.10f;
static const uint32_t MOTION_RELEASE_MS      = 3000;  // Sustained motion before unlocking position

// ════════════════════════════════════════════════════════════════════════════
// NVS PERSISTENCE
// ════════════════════════════════════════════════════════════════════════════

// NVS namespace is defined in nvs_store.h as NVS_MAIN_NS
static const char* NVS_KEY_PRIV     = "privkey";
static const char* NVS_KEY_SEQ      = "seq";
static const char* NVS_KEY_BOOTS    = "boots";
static const char* NVS_KEY_CHAIN    = "chain";
static const char* NVS_KEY_TAMPER   = "tamper";
static const char* NVS_KEY_LOGSEQ   = "logseq";
// The day this device's key was born, and whether that day may be called a
// birthday — written exactly once, never overwritten (birth_day.h).
static const char* NVS_KEY_BORN     = "born_day";
static const char* NVS_KEY_BORN_EX  = "born_exact";
static const char* NVS_KEY_WIFI_SSID = "wifi_ssid";
static const char* NVS_KEY_WIFI_PASS = "wifi_pass";
static const char* NVS_KEY_WIFI_EN   = "wifi_en";
// Explicit standalone mode: the user chose "use without home WiFi" in the
// wizard. The device runs permanently on its own SoftAP — no STA join
// attempts, and the AP is never torn down for radio stability. Cleared
// automatically the moment real credentials are saved.
static const char* NVS_KEY_WIFI_AP_ONLY = "wifi_ap_only";
static bool g_wifi_ap_only = false;
static const char* NVS_KEY_API_TKN  = "api_tkn";
static const char* NVS_KEY_TLS_CERT = "tls_cert";
static const char* NVS_KEY_TLS_KEY  = "tls_key";
static const char* NVS_KEY_GPS_PREC = "gps_prec";   // GPS coarsening decimals (runtime override)

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

enum FixState : uint8_t {
  STATE_NO_FIX       = 0,
  STATE_FIX_ACQUIRED = 1,
  STATE_STATIONARY   = 2,
  STATE_MOVING       = 3,
  STATE_FIX_LOST     = 4
};

enum RecordType : uint8_t {
  RECORD_BOOT_ATTESTATION = 0,
  RECORD_WITNESS_EVENT    = 1,
  RECORD_TAMPER_ALERT     = 2,
  RECORD_STATE_CHANGE     = 3,
};

enum GpsFixMode : uint8_t {
  FIX_MODE_NONE = 1,
  FIX_MODE_2D   = 2,
  FIX_MODE_3D   = 3
};

enum WiFiProvState : uint8_t {
  WIFI_PROV_IDLE         = 0,  // Not attempting connection
  WIFI_PROV_SCANNING     = 1,  // Scanning for networks
  WIFI_PROV_CONNECTING   = 2,  // Attempting to connect
  WIFI_PROV_CONNECTED    = 3,  // Connected to home WiFi
  WIFI_PROV_FAILED       = 4,  // Connection failed
  WIFI_PROV_AP_ONLY      = 5   // AP mode only (no home WiFi configured)
};

struct WiFiCredentials {
  char ssid[33];              // Max 32 chars + null
  char password[65];          // Max 64 chars + null
  bool enabled;               // Whether to attempt connection
  bool configured;            // Whether credentials are stored
};

struct WiFiStatus {
  WiFiProvState state;
  bool ap_active;
  bool sta_connected;
  int8_t rssi;
  char sta_ip[16];
  char ap_ip[16];
  uint8_t ap_clients;
  uint32_t connect_attempts;
  uint32_t last_connect_ms;
  uint32_t connected_since_ms;
  char last_fail_reason[48];  // Human-readable reason for the most recent connect failure
};

struct GnssFix {
  bool     valid;
  double   lat;
  double   lon;
  int      quality;
  int      satellites;
  int      sats_in_view;
  double   hdop;
  double   pdop;
  double   vdop;
  double   altitude_m;
  double   geoid_sep_m;
  double   speed_knots;
  double   speed_kmh;
  double   course_deg;
  GpsFixMode fix_mode;
  uint32_t last_update_ms;
  uint32_t last_gga_ms;
  uint32_t last_rmc_ms;
  uint32_t last_gsa_ms;
};

struct GpsUtcTime {
  bool     valid;
  int      year;
  int      month;
  int      day;
  int      hour;
  int      minute;
  int      second;
  int      centisecond;
  uint32_t last_seen_ms;
};

struct WitnessRecord {
  uint32_t    seq;
  uint32_t    time_bucket;
  RecordType  type;
  uint8_t     payload_hash[32];
  uint8_t     prev_hash[32];
  uint8_t     chain_hash[32];
  uint8_t     signature[64];
  size_t      payload_len;
  bool        verified;
};

struct DeviceIdentity {
  uint8_t  privkey[32];
  uint8_t  pubkey[32];
  uint8_t  pubkey_fp[8];
  uint8_t  chain_head[32];
  uint32_t seq;
  uint32_t seq_persisted;
  uint32_t boot_count;
  uint32_t boot_ms;
  uint32_t tamper_count;
  uint32_t log_seq;
  // ── when this key was born ────────────────────────────────────────────────
  // The device id, the AP SSID, the mDNS name and the app's birth certificate
  // all derive from the keypair, so "how old is this Canary" is really "how old
  // is this key". A key is always born before any clock exists (no RTC — the
  // chip boots at the epoch and only learns the date from GPS), so these are
  // filled in on the first believable clock and then never again. See
  // birth_day.h for the three rules.
  uint32_t born_day;      // Days since the Unix epoch; 0 = never dated.
  bool     born_exact;    // False ⇒ first DATED, not born. Not a birthday.
  uint32_t key_born_ms;   // millis() when this boot generated the key…
  bool     key_is_new;    // …meaningful only if this boot is the one that made it.
  bool     initialized;
  bool     tamper_active;
  char     device_id[32];
  char     ap_ssid[32];
  char     mdns_hostname[40];    // unique mDNS host label, e.g. "canary-kitchen" / "canary-aabb"
  // API security fields (added for SAP integration)
  char     api_token_str[36];    // "cv_" + 32 base62 chars + null
  char     ap_password[16];      // device-unique AP password "cv-" + 12 chars (privkey-derived)
  char     fingerprint_hex[17];  // hex-encoded pubkey fingerprint (8 bytes = 16 hex chars)
  bool     first_boot;           // true if keypair was just generated
};

struct SystemHealth {
  uint32_t records_created;
  uint32_t records_verified;
  uint32_t verify_failures;
  uint32_t gps_sentences;
  uint32_t gga_count;
  uint32_t rmc_count;
  uint32_t gsa_count;
  uint32_t gsv_count;
  uint32_t vtg_count;
  uint32_t chain_persists;
  uint32_t state_changes;
  uint32_t tamper_events;
  uint32_t uptime_sec;
  uint32_t free_heap;
  uint32_t min_heap;
  uint32_t gps_lock_ms;
  uint32_t http_requests;
  uint32_t http_errors;
  uint32_t sd_writes;
  uint32_t sd_errors;
  uint32_t logs_stored;
  uint32_t logs_unacked;
  bool     gps_healthy;
  bool     crypto_healthy;
  bool     sd_healthy;
  bool     wifi_active;
};

template <size_t N>
class RingBuffer {
public:
  RingBuffer() : head_(0), tail_(0), count_(0) {}
  bool push(uint8_t v) {
    if (count_ >= N) return false;
    buf_[head_] = v;
    head_ = (head_ + 1) % N;
    count_++;
    return true;
  }
  bool pop(uint8_t &v) {
    if (count_ == 0) return false;
    v = buf_[tail_];
    tail_ = (tail_ + 1) % N;
    count_--;
    return true;
  }
  size_t size() const { return count_; }
private:
  uint8_t buf_[N];
  size_t head_, tail_, count_;
};

// ════════════════════════════════════════════════════════════════════════════
// GLOBALS
// ════════════════════════════════════════════════════════════════════════════

static DeviceIdentity g_device;
static GnssFix        g_fix;
static GpsUtcTime     g_gps_utc;
static FixState       g_state = STATE_NO_FIX;
static FixState       g_pending_state = STATE_NO_FIX;
static uint32_t       g_state_entered_ms = 0;
static uint32_t       g_pending_state_ms = 0;
static WitnessRecord  g_last_record;
static SystemHealth   g_health;

typedef void (*pre_reboot_fn)();
static pre_reboot_fn g_pre_reboot_hook = nullptr;

// NVS access is now encapsulated in NvsManager singleton (see nvs_store.h)

// PSRAM-resident (csi_mem.h): the 2 KB GPS byte ring is pumped and drained
// on the loop task only (Serial1.read -> push in loop(), pop in the NMEA
// parser). Placement-new into a PSRAM allocation at the top of setup();
// NULL (even the fallback failed) disables GPS byte buffering fail-safe.
static RingBuffer<2048>* g_gps_rb = nullptr;
static char g_line_buf[256];
static size_t g_line_len = 0;

static float g_speed_ema = 0.0f;
static uint32_t g_last_record_ms = 0;
static uint32_t g_last_verify_ms = 0;
static uint32_t g_boot_button_press_start = 0;

// ── Motion filter (anchor-lock when stationary) ─────────────────────────────
// display_* are what the API/UI publish; raw values stay in g_fix for the
// witness chain so we never tamper with the underlying truth — only the
// presentation. is_locked is true while the position is held at the anchor.
struct MotionFilter {
  double  display_lat;
  double  display_lon;
  double  display_alt_m;
  float   display_speed_mps;
  bool    is_locked;
  bool    has_anchor;
  double  anchor_lat;
  double  anchor_lon;
  double  anchor_alt_m;
  uint32_t anchor_samples;
  uint32_t motion_candidate_since_ms;
};
static MotionFilter g_motion = {0};

// SD card
static SPIClass g_sd_spi(FSPI);
static bool g_sd_mounted = false;

// HTTP server
static httpd_handle_t g_http_server = nullptr;

// HTTPS server (TLS)
static httpd_handle_t g_https_server = nullptr;
static bool g_tls_enabled = false;

// TLS certificate (DER-encoded, stored in NVS)
static uint8_t* g_tls_cert_der = nullptr;
static size_t   g_tls_cert_der_len = 0;
static uint8_t* g_tls_key_der = nullptr;
static size_t   g_tls_key_der_len = 0;
static char     g_tls_cert_fp_hex[65] = {0};  // SHA256 of cert for pinning

// Provisioning gate (physical BOOT button). Stores the millis() at which
// the press landed; 0 means closed. provisioning_gate_is_open() applies
// the TTL so the gate auto-closes after PROVISIONING_GATE_TTL_MS even if
// the receipt is never fetched. The variable is touched from the main
// loop task (button handler) and the HTTP server task (receipt handler),
// so access goes through __atomic_*_n to match the pattern used for
// other cross-task counters (see csi_integration.cpp g_outbound_bytes).
static uint32_t g_provisioning_gate_opened_at = 0;

static inline bool provisioning_gate_is_open() {
  uint32_t opened = __atomic_load_n(&g_provisioning_gate_opened_at, __ATOMIC_RELAXED);
  if (opened == 0) return false;
  return (millis() - opened) < PROVISIONING_GATE_TTL_MS;
}

// WiFi provisioning state
static WiFiCredentials g_wifi_creds;
static WiFiStatus g_wifi_status;
static bool g_wifi_scan_in_progress = false;
// WiFi STA connect deadline. 30 s matches what canary-ota already uses
// and gives slow APs (captive portal, weak DHCP, distant routers) a
// fair shot at completing association — 15 s was tight enough that
// users at the edge of range saw spurious "Wrong password" wizard
// failures even with correct credentials.
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000;

// Set true once the BLE/Bluetooth init has been ATTEMPTED (regardless of
// outcome), so the self-test can tell "not up yet / skipped in safe mode"
// (SKIP) apart from "init ran and the stack genuinely failed" (FAIL). Read
// by selftest_api.h's probe_bluetooth; non-static so its extern resolves.
volatile bool g_ble_init_attempted = false;

// The ENTIRE Bluetooth/BLE bring-up (stack init + radio activity) is deferred
// out of the provisioning join window and out of setup() (see
// ble_discovery_start_if_due / provisioning_logic::ble_discovery_start_due).
// Two reasons, one per resource:
//   - RADIO: concurrent BLE duty starves a phone's WPA2 handshake to the
//     SoftAP (worst with Discovery's 5 s ~99%-duty active scan).
//   - RAM: the stack costs ~55-65 KB of internal memory that WiFi/lwIP/httpd
//     must get first; deferring until the setup AP is torn down runs init at
//     the point of maximum free internal heap.
// _ready = setup() finished and armed the gate (not safe mode); _started =
// the one-shot bring-up ran (regardless of outcome — the heap guard's
// verdict is recorded in bluetooth_channel::init_fail_reason());
// _ready_ms = boot reference for the settle/max-hold windows.
#if FEATURE_BLE || FEATURE_BLUETOOTH || FEATURE_BLE_SCAN
static bool     g_ble_discovery_ready   = false;
static bool     g_ble_discovery_started = false;
static uint32_t g_ble_discovery_ready_ms = 0;
// AP-only standalone has no STA to wait on, so the join window never "clears"
// via a home-WiFi join. Let the operator's first association to the permanent
// SoftAP land cleanly, then bring BLE up and accept steady-state coexistence.
static const uint32_t BLE_DISCOVERY_AP_ONLY_SETTLE_MS = 45000;
// Fallback so a normal device whose home WiFi is down/gone (STA never connects,
// AP stays up) doesn't leave BLE Discovery — and its Chirp/Nearby offline
// features — disabled forever. After this hold, start regardless and accept
// steady-state coexistence.
static const uint32_t BLE_DISCOVERY_MAX_HOLD_MS = 300000;  // 5 min
#endif

// QR scan-in-progress flag. Lives up here (not in the QR section) because
// the camera power manager and the CSI witness bridge reference it, and
// the Arduino preprocessor hoists prototypes for functions only — a
// variable declared below its first use does not compile.
#if FEATURE_QR_PROVISION
static volatile bool g_qr_scan_active = false;
// Boot scan-to-join (onboarding wave): an unprovisioned canary with a
// working camera scans for a provisioning code on its own — power it on,
// point it at the display, done. True while the scan task was started by
// the loop tick rather than a phone captive-portal session; the phone
// session's pair-token gates don't apply to the display-minted SCV1 path
// (its own expiring token + physical proximity is the auth).
static volatile bool g_qr_auto_scan = false;
static uint32_t g_qr_auto_next_ms = 0;
#endif

// Camera state
#if FEATURE_CAMERA_PEEK
static bool g_camera_initialized = false;
static volatile bool g_peek_active = false;
static framesize_t g_peek_framesize = FRAMESIZE_VGA;
// Real-time camera metrics — all values derived from esp_camera state, never fabricated
static int      g_peek_jpeg_quality   = 0;     // 0-63, lower = better quality (set at init)
static int      g_peek_xclk_freq_hz   = 0;     // sensor clock in Hz
static bool     g_peek_psram_used     = false; // framebuffer in PSRAM vs DRAM
static uint8_t  g_peek_fb_count       = 0;     // configured framebuffer count
static uint16_t g_peek_sensor_pid     = 0;     // sensor product id (e.g. 0x26 = OV2640)
static uint8_t  g_peek_sensor_ver     = 0;
static uint8_t  g_peek_sensor_midh    = 0;
static uint8_t  g_peek_sensor_midl    = 0;
// Init diagnostics — surfaced in /api/peek/status so the UI can show *why*
// the camera failed rather than the silent "unavailable" the user used to see.
static int      g_peek_last_init_err  = 0;     // esp_err_t of the last failed attempt (0 = ok)
static char     g_peek_last_init_label[20] = "never"; // which attempt was tried last
static bool     g_peek_psram_found    = false; // psramFound() result at the most recent init
static uint32_t g_peek_init_count     = 0;     // number of init attempts since boot (incl. retries)
// Frame pacing for MJPEG stream — inverse of target FPS. With PSRAM the stream
// can deliver ~25 fps at VGA; without PSRAM the OV2640 itself caps below that.
static uint32_t g_peek_frame_delay_ms = 40;    // 40 ms ≈ 25 fps target
// Per-stream measurements
static volatile uint32_t g_peek_frame_count       = 0;  // frames sent in current stream
static volatile uint32_t g_peek_last_frame_bytes  = 0;  // most recent frame jpeg size
static volatile uint32_t g_peek_last_frame_ms     = 0;  // millis() of last frame
static volatile uint32_t g_peek_stream_start_ms   = 0;  // millis() when stream began
static volatile uint64_t g_peek_total_bytes       = 0;  // bytes sent in current stream
static volatile uint32_t g_peek_stream_end_ms     = 0;  // millis() when the last stream ended (0 = never/active)
// Rolling 1s window for instantaneous FPS (no fabrication: counted from real frame deliveries)
static volatile uint32_t g_peek_fps_window_start  = 0;
static volatile uint32_t g_peek_fps_window_count  = 0;
static volatile uint32_t g_peek_fps_last          = 0;  // FPS measured over last full 1s window
// Single-stream guard: set (with acquire/release semantics) while the MJPEG
// worker task owns the async request. Only the httpd task sets it (handlers
// never run concurrently on the single-task server) and only the worker
// clears it, after httpd_req_async_handler_complete().
static volatile bool g_peek_stream_task_busy = false;
// Spinlock guarding the metrics block above. The MJPEG stream runs in a
// dedicated worker task (peek_stream_task) while /api/peek/status is served
// by the httpd task, so writer and reader genuinely race — this lock is
// load-bearing, not defensive.
static portMUX_TYPE g_peek_metrics_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

// Health log buffer (circular, most recent entries)
struct HealthLogRingEntry {
  uint32_t seq;
  uint32_t timestamp_ms;
  LogLevel level;
  LogCategory category;
  AckStatus ack_status;
  char message[80];
  char detail[48];
};
static const size_t HEALTH_LOG_RING_SIZE = 100;
// PSRAM-resident (csi_mem.h): 100 entries x 140 B = 14 KB, the single
// biggest static claim on the internal DRAM bank the BLE stack competes
// for, and only ever touched from task context. Allocated at the very top
// of setup() (before the first log_health call); NULL means even the
// internal fallback failed, in which case log_health degrades to
// Serial-only and the ring stays empty (count never grows, so readers
// never dereference it).
static HealthLogRingEntry* g_health_log_ring = nullptr;
static size_t g_health_log_ring_head = 0;
static size_t g_health_log_ring_count = 0;

// Durable tier staging: log_health() runs on ANY task, and every SD
// writer in this firmware is loop-task-only — so entries are formatted
// up front and staged into this small PSRAM ring behind a critical
// section; health_store_drain() (loop) appends them to the per-boot
// /HEALTH file. NULL ring (alloc failure) disables the SD tier only —
// the RAM ring and Serial keep working.
struct HealthPendingLine {
  char line[health_store::HS_LINE_MAX];
  size_t len;
};
static HealthPendingLine* g_health_pending = nullptr;
static size_t g_health_pending_head = 0;   // next slot to write
static size_t g_health_pending_count = 0;  // staged, undrained
static uint32_t g_health_pending_dropped = 0;
static portMUX_TYPE g_health_pending_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_health_sd_warned = false;    // one warning per failure streak

// ════════════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════════════════

static void print_table_header();
static void print_status_bar();
static void print_identity_block();
static void print_time_block();
static void print_gps_block();
static void print_help();
static void print_quick_connect_details(const char* title);
static bool create_witness_record(const uint8_t* payload, size_t len, RecordType type, WitnessRecord* out);
// log_health is declared extern in health_log.h for use by other modules
void log_health(LogLevel level, LogCategory category, const char* message, const char* detail = nullptr);

// WiFi provisioning
static bool wifi_load_credentials();
static bool wifi_save_credentials();
static bool wifi_clear_credentials();
static void wifi_init_provisioning();
static void wifi_connect_to_home();
static void wifi_check_connection();
static void wifi_drop_ap();
static void wifi_raise_ap();
static bool resolve_ap_password(char* out_password, size_t out_len);
static const char* wifi_state_name(WiFiProvState s);
static void claim_catch_all_hostname();
static void schedule_catch_all_claim();
static void generate_mdns_hostname(char* out, size_t cap);

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

static const char* state_name(FixState s) {
  switch (s) {
    case STATE_NO_FIX:       return "NO_FIX";
    case STATE_FIX_ACQUIRED: return "FIX_ACQ";
    case STATE_STATIONARY:   return "STATIC";
    case STATE_MOVING:       return "MOVING";
    case STATE_FIX_LOST:     return "LOST";
    default:                 return "???";
  }
}

static const char* state_name_short(FixState s) {
  switch (s) {
    case STATE_NO_FIX:       return "NOFIX";
    case STATE_FIX_ACQUIRED: return "ACQRD";
    case STATE_STATIONARY:   return "STAT";
    case STATE_MOVING:       return "MOVE";
    case STATE_FIX_LOST:     return "LOST";
    default:                 return "???";
  }
}

static const char* record_type_name(RecordType t) {
  switch (t) {
    case RECORD_BOOT_ATTESTATION: return "BOOT";
    case RECORD_WITNESS_EVENT:    return "EVNT";
    case RECORD_TAMPER_ALERT:     return "TAMP";
    case RECORD_STATE_CHANGE:     return "STCH";
    default:                      return "???";
  }
}

static const char* fix_mode_name(GpsFixMode m) {
  switch (m) {
    case FIX_MODE_NONE: return "None";
    case FIX_MODE_2D:   return "2D";
    case FIX_MODE_3D:   return "3D";
    default:            return "?";
  }
}

static const char* quality_name(int q) {
  switch (q) {
    case 0: return "Inv";
    case 1: return "GPS";
    case 2: return "DGPS";
    case 4: return "RTK";
    case 5: return "FRTK";
    default: return "?";
  }
}

static void fix_init(GnssFix* f) {
  memset(f, 0, sizeof(GnssFix));
  f->hdop = 99.9;
  f->pdop = 99.9;
  f->vdop = 99.9;
  f->fix_mode = FIX_MODE_NONE;
}

static void utc_init(GpsUtcTime* t) {
  memset(t, 0, sizeof(GpsUtcTime));
}

// Convert a fix's GpsUtcTime to a Unix epoch second, gated on RMC-derived
// calendar validity (see gnss_time.h). Returns false — and leaves *out
// untouched — if utc.valid is false or the calendar fields don't pass
// gnss_calendar_valid(). Defense in depth: the RMC parser above already
// screens this before setting valid=true.
static bool gps_utc_to_epoch(const GpsUtcTime& utc, time_t* out) {
  if (!utc.valid) return false;
  return securacv::gnss::gnss_utc_to_unix(utc.year, utc.month, utc.day,
                                           utc.hour, utc.minute, utc.second, out);
}

// ════════════════════════════════════════════════════════════════════════════
// GPS-DERIVED SYSTEM CLOCK
// ════════════════════════════════════════════════════════════════════════════
//
// This sketch has no SNTP path — no WiFi-based time sync exists anywhere in
// canary-wap. Without it the system clock never leaves its post-boot state,
// so every wall-clock-gated feature (the NFPA-72 monthly self-test chirp's
// waking-hours check, its 30-day NVS-persisted cadence) is silently dead: the
// `time(nullptr) >= 1700000000` guard those features already use to mean
// "clock is synced" can never pass. GPS is the one clock source this device
// actually has that isn't derived from an untrusted network peer — the L76K
// reports UTC directly from the satellite constellation. sync_clock_from_gps()
// seeds the system clock from it once RMC carries a validated date/time, and
// re-checks periodically to correct crystal drift over long uptimes — but
// only ever moves the clock from a GPS fix that has already passed the RMC
// status + calendar-validity gate above, and only steps the clock when it
// disagrees by more than a second (so this isn't calling settimeofday() on
// every check once the two clocks agree). g_gps_utc.valid latches true on
// the first good RMC and is never cleared by a later void/stale sentence
// (other diagnostics consumers rely on that latch), so this checks
// last_seen_ms itself — a fix isn't "available" for a resync once GPS has
// been lost, or this would keep replaying a stale epoch every 10 minutes
// and freeze/rewind wall time after ordinary GNSS loss.
static const time_t GPS_CLOCK_FLOOR = 1700000000;  // ~2023-11-14; below this, "unset"
static const uint32_t GPS_CLOCK_RESYNC_INTERVAL_MS = 10UL * 60UL * 1000UL;
static const uint32_t GPS_CLOCK_FIX_STALE_MS = 30UL * 1000UL;  // RMC arrives ~1 Hz

// Offer the current wall clock to the birth-day recorder. Cheap and safe to
// call at any cadence: it returns immediately once a day is recorded (once, for
// the life of the key) or while the clock is still the boot epoch. Defined down
// with the NVS helpers it writes through; declared here because the clock sync
// is the caller and sits above them.
static bool note_wall_clock(uint32_t unix_s);

static void sync_clock_from_gps() {
  static uint32_t s_last_sync_attempt_ms = 0;
  uint32_t now_ms = millis();

  time_t sys_now = time(nullptr);
  bool clock_set = (sys_now >= GPS_CLOCK_FLOOR);

  // The first believable clock is also the first chance this device has ever
  // had to know its own birthday — the key was generated long before any clock
  // existed. Offered here rather than in the loop because this is the one
  // function that knows the clock is real.
  if (clock_set) note_wall_clock((uint32_t)sys_now);

  if (clock_set && (now_ms - s_last_sync_attempt_ms) < GPS_CLOCK_RESYNC_INTERVAL_MS) {
    return;  // already trustworthy and not due for a drift-correction check
  }

  if (!g_gps_utc.valid) return;
  if ((now_ms - g_gps_utc.last_seen_ms) > GPS_CLOCK_FIX_STALE_MS) return;  // GPS lost

  time_t gps_epoch;
  if (!gps_utc_to_epoch(g_gps_utc, &gps_epoch)) return;
  if (gps_epoch < GPS_CLOCK_FLOOR) return;  // receiver clock itself looks unset/wrong

  s_last_sync_attempt_ms = now_ms;
  if (clock_set && llabs((long long)(gps_epoch - sys_now)) < 2) return;

  struct timeval tv = { .tv_sec = gps_epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  Serial.printf("[CLOCK] system clock %s from GPS (epoch=%lld)\n",
                clock_set ? "corrected" : "set", (long long)gps_epoch);
}

static float knots_to_mps(float knots) {
  return knots * 0.514444f;
}

static float knots_to_kmh(float knots) {
  return knots * 1.852f;
}

static void secure_zero(void* p, size_t n) {
  volatile uint8_t* vp = (volatile uint8_t*)p;
  while (n--) *vp++ = 0;
}

static uint32_t time_bucket() {
  return millis() / g_time_bucket_ms;  // runtime bucket, clamped >= TIME_BUCKET_MS floor
}

static uint32_t uptime_seconds() {
  return millis() / 1000;
}

static void format_uptime(char* out, size_t cap, uint32_t secs) {
  uint32_t h = secs / 3600;
  uint32_t m = (secs % 3600) / 60;
  uint32_t s = secs % 60;
  snprintf(out, cap, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
}

static void serial_wait_for_cdc(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (!Serial && (millis() - start < timeout_ms)) {
    delay(10);
  }
}

static void hex_print(const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (d[i] < 16) Serial.print('0');
    Serial.print(d[i], HEX);
  }
}

static void hex_to_str(char* out, const uint8_t* d, size_t n) {
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < n; i++) {
    out[i*2]   = hex[d[i] >> 4];
    out[i*2+1] = hex[d[i] & 0x0F];
  }
  out[n*2] = 0;
}

// Defined below, next to UNAMBIGUOUS_ALPHABET: renders 16 bits into a 4-char
// suffix in the no-confusion alphabet (no 0/O/o or 1/I/i/l/L).
static void unambiguous_suffix16(uint16_t v, char out[5]);

static void generate_device_id(char* out, size_t cap) {
  // Identity suffix from the Ed25519 pubkey fingerprint, never the MAC
  // (event_contract §10: device identification uses the truncated pubkey hash).
  // Requires g_device.pubkey_fp to be populated first (see provision_device()).
  // Encoded in the unambiguous alphabet (no 0/O/o, 1/I/i/l/L) so the handle a
  // user reads off canary.local / a sticker has no confusable glyphs. base-54
  // of 16 bits is injective, so per-device uniqueness matches the old hex form;
  // it stays stable across reflashes because the keypair persists in NVS.
  char suffix[5];
  unambiguous_suffix16((uint16_t)((g_device.pubkey_fp[0] << 8) | g_device.pubkey_fp[1]),
                       suffix);
  // Defense-in-depth: never emit a truncated (ambiguous) handle on overflow.
  if (cap == 0) return;
  if (snprintf(out, cap, "%s%s", DEVICE_ID_PREFIX, suffix) >= (int)cap) out[0] = '\0';
}

static void generate_ap_ssid(char* out, size_t cap) {
  // Suffix from the pubkey fingerprint, never the MAC (event_contract §10),
  // in the same no-confusion alphabet as the device_id.
  char suffix[5];
  unambiguous_suffix16((uint16_t)((g_device.pubkey_fp[0] << 8) | g_device.pubkey_fp[1]),
                       suffix);
  if (cap == 0) return;
  if (snprintf(out, cap, "SecuraCV-%s", suffix) >= (int)cap) out[0] = '\0';
}

// mDNS hostname rules (RFC 6762 §16 / RFC 1123): a single DNS label may only
// contain [a-z0-9-] and must not start or end with a hyphen. Lowercase the
// input and replace any other byte with '-'. Mirrors sanitize_mdns_hostname()
// in firmware/canary/lib/securacv_network/src/securacv_network.cpp so both
// builds advertise the same hostname grammar.
static void sanitize_mdns_label(const char* in, char* out, size_t cap) {
  if (cap == 0) return;
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j < cap - 1; i++) {
    char c = in[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      out[j++] = c;
    } else {
      out[j++] = '-';
    }
  }
  while (j > 0 && out[j - 1] == '-') j--;       // trim trailing hyphens
  size_t start = 0;
  while (start < j && out[start] == '-') start++; // trim leading hyphens
  if (start > 0) { memmove(out, out + start, j - start); j -= start; }
  out[j] = '\0';
}

// Build the unique, stable mDNS hostname this device advertises. A user-set
// friendly name (captured during setup, stored in NVS "dev_name") wins and
// yields a memorable "canary-<name>" (e.g. "canary-kitchen"); otherwise we
// fall back to the pubkey-fingerprint-derived "canary-aabb". This is what makes a second
// Canary "just work": every device owns a distinct <host>.local, while the
// bare "canary.local" stays available as a single-device catch-all (see the
// delegated-hostname claim in wifi_init_provisioning()).
static void generate_mdns_hostname(char* out, size_t cap) {
  if (cap == 0) return;
  const char* friendly = setup_wizard::get_device_name();
  if (friendly && friendly[0]) {
    char label[setup_wizard::DEVICE_NAME_MAX + 1];
    sanitize_mdns_label(friendly, label, sizeof(label));
    if (label[0]) {
      // Reserve room for the "canary-" prefix; total label must stay <= 63.
      snprintf(out, cap, "canary-%s", label);
      return;
    }
  }
  // Fallback suffix from the pubkey fingerprint, never the MAC (event_contract §10).
  snprintf(out, cap, "canary-%02x%02x",
           g_device.pubkey_fp[0], g_device.pubkey_fp[1]);
}

// ════════════════════════════════════════════════════════════════════════════
// SHA-256 WITH DOMAIN SEPARATION
// ════════════════════════════════════════════════════════════════════════════

static void sha256_raw(const uint8_t* data, size_t n, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, data, n);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

static void sha256_domain(const char* domain, const uint8_t* data, size_t n, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  
  mbedtls_sha256_update(&ctx, (const uint8_t*)domain, strlen(domain));
  uint8_t sep = 0x00;
  mbedtls_sha256_update(&ctx, &sep, 1);
  
  if (data && n > 0) {
    mbedtls_sha256_update(&ctx, data, n);
  }
  
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

// ════════════════════════════════════════════════════════════════════════════
// NVS PERSISTENCE (using NvsManager singleton from nvs_store.h)
// ════════════════════════════════════════════════════════════════════════════

// Note: nvs_open_rw(), nvs_open_ro(), and nvs_close() are now provided
// by nvs_store.h as inline functions that delegate to NvsManager::instance()

static bool nvs_load_key(uint8_t priv[32]) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;
  size_t n = nvs.getBytesLength(NVS_KEY_PRIV);
  if (n != 32) { nvs.end(); return false; }
  nvs.getBytes(NVS_KEY_PRIV, priv, 32);
  nvs.end();
  return true;
}

static bool nvs_store_key(const uint8_t priv[32]) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  nvs.putBytes(NVS_KEY_PRIV, priv, 32);
  nvs.end();
  return true;
}

static uint32_t nvs_load_u32(const char* key, uint32_t def = 0) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return def;
  uint32_t v = nvs.getUInt(key, def);
  nvs.end();
  return v;
}

static bool nvs_store_u32(const char* key, uint32_t val) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  nvs.putUInt(key, val);
  nvs.end();
  return true;
}

// Declared up beside sync_clock_from_gps (its only caller); defined here
// because it writes through the two NVS helpers directly above. The decision
// itself lives in birth_day.h — this function owns only the persistence.
static bool note_wall_clock(uint32_t unix_s) {
  birth::Stamp stored;
  stored.day = g_device.born_day;
  stored.exact = g_device.born_exact;

  birth::Observation now;
  now.unix_s = unix_s;
  now.key_age_known = g_device.key_is_new;
  now.key_age_s = g_device.key_is_new
                      ? (millis() - g_device.key_born_ms) / 1000u
                      : 0u;

  birth::Stamp fresh;
  if (!birth::consider(stored, now, &fresh)) return false;

  // Order matters: the day is what `recorded()` tests, so writing it last means
  // a power cut between the two writes leaves no half-stamped birth — the next
  // boot simply tries again.
  nvs_store_u32(NVS_KEY_BORN_EX, fresh.exact ? 1 : 0);
  nvs_store_u32(NVS_KEY_BORN, fresh.day);
  g_device.born_day = fresh.day;
  g_device.born_exact = fresh.exact;

  Serial.printf("[BIRTH] key %s day %lu (UTC)\n",
                fresh.exact ? "born on" : "first dated",
                (unsigned long)fresh.day);
  return true;
}

static bool nvs_load_bytes(const char* key, uint8_t* out, size_t len) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;
  size_t n = nvs.getBytesLength(key);
  if (n != len) { nvs.end(); return false; }
  nvs.getBytes(key, out, len);
  nvs.end();
  return true;
}

static bool nvs_store_bytes(const char* key, const uint8_t* data, size_t len) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  nvs.putBytes(key, data, len);
  nvs.end();
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// ED25519 CRYPTO
// ════════════════════════════════════════════════════════════════════════════

static bool generate_keypair(uint8_t priv[32], uint8_t pub[32]) {
  // Get 32 bytes from hardware RNG. This runs during provisioning, before the
  // radio is brought up (the AP SSID / mDNS name are derived from the key
  // fingerprint, so the keypair must exist first), so the RNG has no RF entropy
  // source yet — gate the one-time draw with bootloader_random_enable()/
  // _disable() to seed it properly (ESP-IDF's documented early-entropy pattern).
  bootloader_random_enable();
  esp_fill_random(priv, 32);
  bootloader_random_disable();
  
  // Derive public key
  Ed25519::derivePublicKey(pub, priv);
  return true;
}

static void sign_message(const uint8_t priv[32], const uint8_t pub[32], 
                         const uint8_t* msg, size_t len, uint8_t sig[64]) {
  Ed25519::sign(sig, priv, pub, msg, len);
}

static bool verify_signature(const uint8_t pub[32], const uint8_t* msg, size_t len, const uint8_t sig[64]) {
  return Ed25519::verify(sig, pub, msg, len);
}

static void compute_fingerprint(const uint8_t pub[32], uint8_t fp[8]) {
  uint8_t hash[32];
  sha256_domain("securacv:pubkey:fingerprint", pub, 32, hash);
  memcpy(fp, hash, 8);
}

// ════════════════════════════════════════════════════════════════════════════
// HMAC-SHA256 (using mbedtls)
// ════════════════════════════════════════════════════════════════════════════

static void hmac_sha256(const uint8_t* key, size_t key_len,
                        const uint8_t* data, size_t data_len,
                        uint8_t out[32]) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, key, key_len);
  mbedtls_md_hmac_update(&ctx, data, data_len);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

// ════════════════════════════════════════════════════════════════════════════
// UNAMBIGUOUS BASE57 ENCODING (unbiased rejection sampling)
// ════════════════════════════════════════════════════════════════════════════

// Drops EVERY case variant of the glyph-confusion classes that bite users
// reading or typing a value by hand: 0/O/o and 1/I/i/l/L. Shared by the API
// token, the WiFi AP password, and the device_id / AP-SSID suffix, so the
// "no confusing characters" rule holds everywhere a human sees an identifier.
// 54 chars; 32 chars × log2(54) ≈ 184 bits of entropy — a clear UX win.
static const char UNAMBIGUOUS_ALPHABET[] =
  "23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz";
static const size_t UNAMBIGUOUS_LEN = sizeof(UNAMBIGUOUS_ALPHABET) - 1;  // 54

// Render 16 bits as a fixed 4-char suffix in the unambiguous alphabet. base-54
// of a 16-bit value is injective (54^4 ≫ 65536), so it preserves the
// uniqueness of the old 4-hex-digit suffix with no confusable glyphs.
static void unambiguous_suffix16(uint16_t v, char out[5]) {
  for (int i = 0; i < 4; i++) {
    out[i] = UNAMBIGUOUS_ALPHABET[v % UNAMBIGUOUS_LEN];
    v = (uint16_t)(v / UNAMBIGUOUS_LEN);
  }
  out[4] = '\0';
}

// Rejection sampling: discard bytes >= 216 (216 = 54*4, evenly divisible)
// This eliminates modular bias entirely.
static void format_api_token_string(const uint8_t* input, size_t in_len,
                                    char* output, size_t out_len) {
  size_t out_idx = 0;
  output[out_idx++] = 'c';
  output[out_idx++] = 'v';
  output[out_idx++] = '_';

  size_t target_chars = 32;
  size_t chars_produced = 0;
  size_t i = 0;

  while (chars_produced < target_chars && out_idx < out_len - 1) {
    uint8_t b;
    if (i < in_len) {
      b = input[i++];
    } else {
      // Extremely unlikely fallback: deterministic byte generation
      b = (uint8_t)(i ^ 0xA5);
      i++;
    }

    if (b < 216) {  // 216 = 54 * 4 → evenly divisible
      output[out_idx++] = UNAMBIGUOUS_ALPHABET[b % UNAMBIGUOUS_LEN];
      chars_produced++;
    }
    // else: reject this byte (biased), try next
  }

  output[out_idx] = '\0';
}

// ════════════════════════════════════════════════════════════════════════════
// HKDF-STYLE API TOKEN DERIVATION (two-step key separation)
// ════════════════════════════════════════════════════════════════════════════

static bool derive_api_token(const uint8_t privkey[32], char* token_str, size_t token_str_len) {
  // ── Step 1: Derive intermediate token-key (key separation) ────────────
  // The Ed25519 signing key NEVER directly touches the token derivation context.
  uint8_t token_key[32];
  hmac_sha256(
    privkey, 32,
    (const uint8_t*)"securacv:token-key-derive:v1", 28,
    token_key
  );

  // ── Step 2: Derive actual API token from intermediate key + MAC ──────
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  uint8_t token_input[27];  // 21 bytes domain + 6 bytes MAC
  memcpy(token_input, "securacv:api-token:v1", 21);
  memcpy(token_input + 21, mac, 6);

  uint8_t token_hash[32];
  hmac_sha256(
    token_key, 32,
    token_input, 27,
    token_hash
  );

  // ── Step 3: Encode as base62 with rejection sampling ─────────────────
  uint8_t token_bytes[24];
  memcpy(token_bytes, token_hash, 24);
  format_api_token_string(token_bytes, 24, token_str, token_str_len);

  // ── Wipe intermediate key material ───────────────────────────────────
  secure_zero(token_key, sizeof(token_key));
  secure_zero(token_hash, sizeof(token_hash));
  secure_zero(token_bytes, sizeof(token_bytes));

  return strlen(token_str) >= 35;  // "cv_" + 32 chars
}

// ════════════════════════════════════════════════════════════════════════════
// DEVICE-UNIQUE AP PASSWORD (derived from pubkey fingerprint)
// ════════════════════════════════════════════════════════════════════════════

static void derive_ap_password(const uint8_t privkey[32], char* password, size_t len) {
  // Derive from the PRIVATE key, not the public fingerprint. The pubkey
  // fingerprint is published (serial banner, device_id suffix, /enroll page),
  // so the old fingerprint-derived password was recomputable by anyone who had
  // seen the device — its entropy was irrelevant because it wasn't secret. HMAC
  // the private key under its own domain label (same key-separation discipline
  // as derive_api_token) so the password is a real per-device secret.
  uint8_t pw_key[32];
  hmac_sha256(privkey, 32,
              (const uint8_t*)"securacv:ap-password:v1", 23,
              pw_key);

  // 12 unambiguous chars ≈ 69 bits — far beyond offline WPA2 (PBKDF2) reach.
  // WPA2-PSK requires 8-63 ASCII chars; "cv-" + 12 = 15 fits ap_password[16].
  char encoded[13];
  size_t chars_produced = 0;
  for (size_t i = 0; chars_produced < 12 && i < 32; i++) {
    uint8_t b = pw_key[i];
    if (b < 216) {  // 216 = 54 * 4, rejection sampling to avoid modulo bias
      encoded[chars_produced++] = UNAMBIGUOUS_ALPHABET[b % UNAMBIGUOUS_LEN];
    }
  }
  // 32 HMAC bytes for 12 chars (reject ~15.6%): exhausting all 32 is
  // astronomically unlikely, but pad deterministically just in case. Use '2'
  // (first char of the unambiguous alphabet) — never '0', which we excluded.
  while (chars_produced < 12) {
    encoded[chars_produced++] = '2';
  }
  encoded[12] = '\0';
  snprintf(password, len, "cv-%s", encoded);
  // Result: "cv-XXXXXXXXXXXX" — 15 chars, a private-key-derived secret
  // Wipe both transient copies of secret material (DCE-safe) — the password
  // lives on only in the caller's buffer (g_device.ap_password) by design.
  secure_zero(encoded, sizeof(encoded));
  secure_zero(pw_key, sizeof(pw_key));
}

// ════════════════════════════════════════════════════════════════════════════
// NVS TOKEN STORAGE
// ════════════════════════════════════════════════════════════════════════════

static bool nvs_store_token(const char* token_str) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  size_t written = nvs.putBytes(NVS_KEY_API_TKN, token_str, strlen(token_str));
  nvs.end();
  return written > 0;
}

static bool nvs_load_token(char* token_str, size_t max_len) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;
  size_t n = nvs.getBytesLength(NVS_KEY_API_TKN);
  if (n == 0 || n >= max_len) { nvs.end(); return false; }
  nvs.getBytes(NVS_KEY_API_TKN, token_str, n);
  token_str[n] = '\0';
  nvs.end();
  return strlen(token_str) >= 35;
}

// ════════════════════════════════════════════════════════════════════════════
// TLS CERTIFICATE GENERATION & MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

static bool tls_load_from_nvs() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;

  size_t cert_len = nvs.getBytesLength(NVS_KEY_TLS_CERT);
  size_t key_len  = nvs.getBytesLength(NVS_KEY_TLS_KEY);

  if (cert_len == 0 || key_len == 0) {
    nvs.end();
    return false;
  }

  g_tls_cert_der = (uint8_t*)malloc(cert_len);
  g_tls_key_der  = (uint8_t*)malloc(key_len);
  if (!g_tls_cert_der || !g_tls_key_der) {
    free(g_tls_cert_der); g_tls_cert_der = nullptr;
    free(g_tls_key_der);  g_tls_key_der = nullptr;
    nvs.end();
    return false;
  }

  nvs.getBytes(NVS_KEY_TLS_CERT, g_tls_cert_der, cert_len);
  nvs.getBytes(NVS_KEY_TLS_KEY,  g_tls_key_der,  key_len);
  g_tls_cert_der_len = cert_len;
  g_tls_key_der_len  = key_len;

  nvs.end();
  return true;
}

static bool tls_store_to_nvs() {
  if (!g_tls_cert_der || !g_tls_key_der) return false;
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  nvs.putBytes(NVS_KEY_TLS_CERT, g_tls_cert_der, g_tls_cert_der_len);
  nvs.putBytes(NVS_KEY_TLS_KEY,  g_tls_key_der,  g_tls_key_der_len);
  nvs.end();
  return true;
}

static void tls_compute_cert_fingerprint() {
  if (!g_tls_cert_der || g_tls_cert_der_len == 0) return;
  uint8_t cert_fp[32];
  sha256_raw(g_tls_cert_der, g_tls_cert_der_len, cert_fp);
  hex_to_str(g_tls_cert_fp_hex, cert_fp, 32);
}

#if SECURACV_HAS_TLS_CERTGEN
static bool tls_generate_self_signed_cert() {
  Serial.println("[TLS] Generating self-signed certificate (RSA-2048)...");
  Serial.println("[TLS] This takes 30-60 seconds on first boot only.");

  int ret;
  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_mpi serial_mpi;

  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_mpi_init(&serial_mpi);

  const char *pers = "securacv_tls_gen";
  ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char*)pers, strlen(pers));
  if (ret != 0) {
    Serial.printf("[TLS] DRBG seed failed: -0x%04X\n", (unsigned)-ret);
    goto cleanup;
  }

  // Generate RSA-2048 key
  ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (ret != 0) {
    Serial.printf("[TLS] PK setup failed: -0x%04X\n", (unsigned)-ret);
    goto cleanup;
  }

  ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random,
                             &ctr_drbg, 2048, 65537);
  if (ret != 0) {
    Serial.printf("[TLS] RSA keygen failed: -0x%04X\n", (unsigned)-ret);
    goto cleanup;
  }
  Serial.println("[TLS] RSA-2048 key generated");

  // Build CN with device fingerprint for pinning
  {
    char subject[80];
    snprintf(subject, sizeof(subject), "CN=securacv-%s,O=SecuraCV,OU=Canary",
             g_device.fingerprint_hex);

    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    ret = mbedtls_x509write_crt_set_subject_name(&crt, subject);
    if (ret != 0) {
      Serial.printf("[TLS] Set subject failed: -0x%04X\n", (unsigned)-ret);
      goto cleanup;
    }
    ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
    if (ret != 0) {
      Serial.printf("[TLS] Set issuer failed: -0x%04X\n", (unsigned)-ret);
      goto cleanup;
    }

    mbedtls_mpi_lset(&serial_mpi, 1);
    mbedtls_x509write_crt_set_serial(&crt, &serial_mpi);

    mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20500101000000");
  }

  // Write certificate to DER
  {
    uint8_t cert_buf[2048];
    ret = mbedtls_x509write_crt_der(&crt, cert_buf, sizeof(cert_buf),
                                     mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret < 0) {
      Serial.printf("[TLS] Cert write failed: -0x%04X\n", (unsigned)-ret);
      goto cleanup;
    }
    // DER output is written to the END of the buffer
    g_tls_cert_der_len = ret;
    g_tls_cert_der = (uint8_t*)malloc(g_tls_cert_der_len);
    if (!g_tls_cert_der) { ret = -1; goto cleanup; }
    memcpy(g_tls_cert_der, cert_buf + sizeof(cert_buf) - ret, ret);
  }

  // Write private key to DER
  {
    uint8_t key_buf[2048];
    ret = mbedtls_pk_write_key_der(&key, key_buf, sizeof(key_buf));
    if (ret < 0) {
      Serial.printf("[TLS] Key write failed: -0x%04X\n", (unsigned)-ret);
      free(g_tls_cert_der); g_tls_cert_der = nullptr;
      goto cleanup;
    }
    g_tls_key_der_len = ret;
    g_tls_key_der = (uint8_t*)malloc(g_tls_key_der_len);
    if (!g_tls_key_der) {
      free(g_tls_cert_der); g_tls_cert_der = nullptr;
      ret = -1; goto cleanup;
    }
    memcpy(g_tls_key_der, key_buf + sizeof(key_buf) - ret, ret);
  }

  Serial.println("[TLS] Certificate generated successfully");
  ret = 0;

cleanup:
  mbedtls_mpi_free(&serial_mpi);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  return (ret == 0);
}
#endif // SECURACV_HAS_TLS_CERTGEN

static bool init_tls_cert() {
  // Try loading from NVS first
  if (tls_load_from_nvs()) {
    tls_compute_cert_fingerprint();
    Serial.println("[TLS] Loaded certificate from NVS");
    Serial.printf("[TLS] Cert fingerprint: %.16s...\n", g_tls_cert_fp_hex);
    return true;
  }

#if SECURACV_HAS_TLS_CERTGEN
  // Generate new self-signed cert
  if (!tls_generate_self_signed_cert()) {
    Serial.println("[TLS] Certificate generation FAILED");
    return false;
  }

  // Store in NVS for reuse across reboots
  if (!tls_store_to_nvs()) {
    Serial.println("[TLS] WARNING: Failed to store cert in NVS");
  }

  tls_compute_cert_fingerprint();
  Serial.printf("[TLS] CN: securacv-%s\n", g_device.fingerprint_hex);
  Serial.printf("[TLS] Cert fingerprint: %.16s...\n", g_tls_cert_fp_hex);
  return true;
#else
  Serial.println("[TLS] Certificate generation not available (mbedtls x509write not compiled in)");
  Serial.println("[TLS] To enable HTTPS, provision a certificate via NVS or use PlatformIO build.");
  return false;
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// CHAIN OPERATIONS
// ════════════════════════════════════════════════════════════════════════════

static void compute_chain_hash(const uint8_t prev[32], const uint8_t payload_hash[32], 
                               uint32_t seq, uint32_t time_bucket, uint8_t out[32]) {
  // Domain-separated: "securacv:fw:chain:v1" || 0x00 || prev || payload_hash || seq (BE) || time_bucket (BE)
  uint8_t buf[32 + 32 + 4 + 4];
  memcpy(buf, prev, 32);
  memcpy(buf + 32, payload_hash, 32);
  buf[64] = (seq >> 24) & 0xFF;
  buf[65] = (seq >> 16) & 0xFF;
  buf[66] = (seq >> 8) & 0xFF;
  buf[67] = seq & 0xFF;
  buf[68] = (time_bucket >> 24) & 0xFF;
  buf[69] = (time_bucket >> 16) & 0xFF;
  buf[70] = (time_bucket >> 8) & 0xFF;
  buf[71] = time_bucket & 0xFF;
  
  sha256_domain("securacv:fw:chain:v1", buf, sizeof(buf), out);
}

static void update_chain(const uint8_t payload_hash[32], uint32_t tb, WitnessRecord* rec) {
  rec->seq = ++g_device.seq;
  rec->time_bucket = tb;
  memcpy(rec->prev_hash, g_device.chain_head, 32);
  memcpy(rec->payload_hash, payload_hash, 32);
  
  compute_chain_hash(rec->prev_hash, payload_hash, rec->seq, tb, rec->chain_hash);
  memcpy(g_device.chain_head, rec->chain_hash, 32);
}

static void persist_chain_state() {
  nvs_store_u32(NVS_KEY_SEQ, g_device.seq);
  nvs_store_bytes(NVS_KEY_CHAIN, g_device.chain_head, 32);
  g_device.seq_persisted = g_device.seq;
  g_health.chain_persists++;
  
  #if DEBUG_CHAIN
  Serial.print("[CHAIN] Persisted seq=");
  Serial.println(g_device.seq);
  #endif
}

// ════════════════════════════════════════════════════════════════════════════
// WITNESS SD PERSISTENCE — /WITNESS/records.jsonl, the durable tier
// ════════════════════════════════════════════════════════════════════════════
//
// Two storage tiers, same shape as the beacon audit log
// (beacon_channel.cpp): SD is the append-only log of record — one
// self-describing JSON line per signed record, never rotated or truncated
// (Invariant IV) — and NVS chain-head/seq is the fast-boot cache persisted
// every SD_PERSIST_INTERVAL records. Best-effort: a device without a card
// keeps chaining in RAM/NVS behind ONE latched health warning per failure
// streak; a successful append re-arms the latch.

static bool g_witness_sd_warned = false;

static bool witness_sd_fail(const char* why) {
  if (!g_witness_sd_warned) {
    g_witness_sd_warned = true;
    char msg[128];
    snprintf(msg, sizeof(msg),
             "witness log: SD tier unavailable (%s) — NVS cache only", why);
    log_health(SCV_LOG_WARNING, SCV_CAT_STORAGE, msg, nullptr);
  }
  return false;
}

static bool sd_append_witness_record(const WitnessRecord* rec) {
#if FEATURE_SD_STORAGE
  if (rec == NULL) return witness_sd_fail("null record");
  if (!g_sd_mounted) return witness_sd_fail("no card");
  if (sd_mount_in_flight()) return witness_sd_fail("mount in flight");
  if (!SD.exists("/WITNESS") && !SD.mkdir("/WITNESS"))
    return witness_sd_fail("mkdir /WITNESS failed");

  char line[witness_store::RECORD_LINE_MAX];
  const size_t n = witness_store::line_build(
      line, sizeof(line), rec->seq, rec->time_bucket, (uint8_t)rec->type,
      rec->payload_hash, rec->prev_hash, rec->chain_hash, rec->signature);
  if (n == 0) return witness_sd_fail("line build failed");

  // FILE_APPEND + close-per-write: a power cut at most loses the in-flight
  // line, never the file structure (crash model of csi_event_log.cpp and
  // the beacon audit log).
  File f = SD.open("/WITNESS/records.jsonl", FILE_APPEND);
  if (!f) return witness_sd_fail("open failed");
  const size_t wrote = f.write((const uint8_t*)line, n);
  f.close();
  if (wrote != n) return witness_sd_fail("short write (card full?)");
  g_witness_sd_warned = false;  // healthy again — re-arm the warning latch
  return true;
#else
  (void)rec;
  return false;
#endif
}

// Recover the chain head from the SD log of record at boot / hot-mount.
// The NVS cache persists only every SD_PERSIST_INTERVAL records, so after
// a power cut (or an NVS wipe/reflash while the card kept its history) the
// cached head can be BEHIND the last record actually signed — resuming
// from it would fork the supposedly append-only chain. SD wins when its
// tail is strictly ahead AND the tail record's signature verifies under
// THIS device's public key: a foreign card (another device's history) or
// a tampered tail must never move our chain head.
static void witness_recover_from_sd() {
#if FEATURE_SD_STORAGE
  File f = SD.open("/WITNESS/records.jsonl", FILE_READ);
  if (!f) return;
  const size_t size = f.size();
  if (size == 0) {
    f.close();
    return;
  }

  char tail[witness_store::TAIL_READ + 1];
  const size_t want =
      (size < witness_store::TAIL_READ) ? size : witness_store::TAIL_READ;
  if (!f.seek(size - want)) {
    f.close();
    return;
  }
  const size_t got = f.read((uint8_t*)tail, want);
  f.close();
  if (got == 0) return;
  tail[got] = '\0';

  witness_store::TailRecord rec;
  if (!witness_store::tail_parse(tail, &rec)) return;
  if (!witness_store::sd_wins(g_device.seq, rec.seq)) return;

  // Bind seq/tb to the signature: the Ed25519 signature covers only the
  // chain hash, so recompute that hash from the line's own fields and
  // require a match — otherwise a tampered card could keep a genuine
  // ch/sig pair while editing seq to move the device sequence to an
  // arbitrary value (the same check verify_witness_log.py runs offline).
  uint8_t recomputed[32];
  compute_chain_hash(rec.prev, rec.ph, rec.seq, rec.tb, recomputed);
  if (memcmp(recomputed, rec.ch, 32) != 0) {
    log_health(SCV_LOG_WARNING, SCV_CAT_STORAGE,
               "Witness SD tail ignored: chain hash mismatch", nullptr);
    return;
  }
  if (!verify_signature(g_device.pubkey, rec.ch, 32, rec.sig)) {
    log_health(SCV_LOG_WARNING, SCV_CAT_STORAGE,
               "Witness SD tail ignored: signature not ours", nullptr);
    return;
  }

  g_device.seq = rec.seq;
  memcpy(g_device.chain_head, rec.ch, 32);
  persist_chain_state();
  char detail[32];
  snprintf(detail, sizeof(detail), "seq %u", (unsigned)rec.seq);
  log_health(SCV_LOG_NOTICE, SCV_CAT_STORAGE,
             "Witness chain head recovered from SD", detail);
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// CBOR PAYLOAD BUILDING (Simple CBOR encoding)
// ════════════════════════════════════════════════════════════════════════════

class CborWriter {
public:
  CborWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap), pos_(0), error_(false) {}
  
  void write_map(size_t n) {
    if (n <= 23) {
      write_byte(0xA0 + n);
    } else if (n <= 255) {
      write_byte(0xB8);
      write_byte(n);
    } else {
      write_byte(0xB9);
      write_byte((n >> 8) & 0xFF);
      write_byte(n & 0xFF);
    }
  }
  
  void write_text(const char* s) {
    size_t len = strlen(s);
    if (len <= 23) {
      write_byte(0x60 + len);
    } else if (len <= 255) {
      write_byte(0x78);
      write_byte(len);
    } else {
      write_byte(0x79);
      write_byte((len >> 8) & 0xFF);
      write_byte(len & 0xFF);
    }
    for (size_t i = 0; i < len; i++) {
      write_byte(s[i]);
    }
  }
  
  void write_uint(uint64_t v) {
    if (v <= 23) {
      write_byte(v);
    } else if (v <= 255) {
      write_byte(0x18);
      write_byte(v);
    } else if (v <= 65535) {
      write_byte(0x19);
      write_byte((v >> 8) & 0xFF);
      write_byte(v & 0xFF);
    } else if (v <= 0xFFFFFFFF) {
      write_byte(0x1A);
      write_byte((v >> 24) & 0xFF);
      write_byte((v >> 16) & 0xFF);
      write_byte((v >> 8) & 0xFF);
      write_byte(v & 0xFF);
    } else {
      write_byte(0x1B);
      for (int i = 7; i >= 0; i--) {
        write_byte((v >> (i * 8)) & 0xFF);
      }
    }
  }
  
  void write_int(int64_t v) {
    if (v >= 0) {
      write_uint((uint64_t)v);
    } else {
      uint64_t neg = (uint64_t)(-(v + 1));
      if (neg <= 23) {
        write_byte(0x20 + neg);
      } else if (neg <= 255) {
        write_byte(0x38);
        write_byte(neg);
      } else if (neg <= 65535) {
        write_byte(0x39);
        write_byte((neg >> 8) & 0xFF);
        write_byte(neg & 0xFF);
      } else {
        write_byte(0x3A);
        write_byte((neg >> 24) & 0xFF);
        write_byte((neg >> 16) & 0xFF);
        write_byte((neg >> 8) & 0xFF);
        write_byte(neg & 0xFF);
      }
    }
  }
  
  void write_bool(bool v) {
    write_byte(v ? 0xF5 : 0xF4);
  }
  
  void write_null() {
    write_byte(0xF6);
  }
  
  void write_float(double v) {
    write_byte(0xFB);
    union { double d; uint64_t u; } conv;
    conv.d = v;
    for (int i = 7; i >= 0; i--) {
      write_byte((conv.u >> (i * 8)) & 0xFF);
    }
  }
  
  void write_bytes(const uint8_t* data, size_t len) {
    if (len <= 23) {
      write_byte(0x40 + len);
    } else if (len <= 255) {
      write_byte(0x58);
      write_byte(len);
    } else {
      write_byte(0x59);
      write_byte((len >> 8) & 0xFF);
      write_byte(len & 0xFF);
    }
    for (size_t i = 0; i < len; i++) {
      write_byte(data[i]);
    }
  }
  
  size_t size() const { return pos_; }
  bool ok() const { return !error_; }
  
private:
  void write_byte(uint8_t b) {
    if (pos_ < cap_) {
      buf_[pos_++] = b;
    } else {
      error_ = true;
    }
  }
  
  uint8_t* buf_;
  size_t cap_;
  size_t pos_;
  bool error_;
};

// ════════════════════════════════════════════════════════════════════════════
// RECORD BUILDING
// ════════════════════════════════════════════════════════════════════════════

// Spatial coarsening for operator-facing GPS (Invariant III): round latitude/
// longitude before any HTTP/serial emission so the device never publishes
// tracking-grade precision. Internal computation (motion filter, anchoring) keeps
// full precision; only output is coarsened.
//
// Precision is a per-deployment privacy knob (see GPS_COARSEN_DECIMALS in
// build_config.h): the right value depends on the install's circumstances, e.g.
// population density / how identifying a precise fix is. The build-time default
// can be overridden at runtime via the "gps_prec" NVS key (clamped 0..7).
//
// NOTE: defined here (after the GPS type definitions) rather than at the top —
// a free function placed before the .ino's type defs makes it the "first function"
// and Arduino hoists all auto-generated prototypes above those types, which breaks
// the build.
static const uint8_t GPS_COARSEN_DECIMALS_MAX = 7;  // ~1 cm — full GPS precision

// Effective decimals: build-time default, overridable once from NVS, then cached.
static uint8_t gps_coarsen_decimals() {
  static int cached = -1;  // -1 => not yet loaded
  if (cached < 0) {
    uint32_t v = nvs_load_u32(NVS_KEY_GPS_PREC, GPS_COARSEN_DECIMALS);
    if (v > GPS_COARSEN_DECIMALS_MAX) v = GPS_COARSEN_DECIMALS_MAX;
    cached = (int)v;
  }
  return (uint8_t)cached;
}

static inline double gps_coarsen_deg(double v) {
  // Pass non-finite values through unchanged — casting NaN/Inf to integer is UB.
  if (v != v || (v - v) != (v - v)) return v;
  double scale = 1.0;
  for (uint8_t i = 0; i < gps_coarsen_decimals(); i++) scale *= 10.0;
  double scaled = v * scale;
  // round-half-away-from-zero without depending on libm rounding mode
  double r = (scaled >= 0.0) ? (double)(long long)(scaled + 0.5)
                             : (double)(long long)(scaled - 0.5);
  return r / scale;
}

static bool build_witness_event(const GnssFix* fx, FixState st, uint8_t* out, size_t cap, size_t* out_len) {
  CborWriter w(out, cap);
  
  // Build CBOR map with PWK-compatible structure
  w.write_map(8);
  
  // "device_id"
  w.write_text("device_id");
  w.write_text(g_device.device_id);
  
  // "zone_id"
  w.write_text("zone_id");
  w.write_text(ZONE_ID);
  
  // "state"
  w.write_text("state");
  w.write_text(state_name(st));
  
  // "gps" (nested map)
  w.write_text("gps");
  w.write_map(8);
  w.write_text("valid"); w.write_bool(fx->valid);
  w.write_text("lat"); w.write_float(gps_coarsen_deg(fx->lat));
  w.write_text("lon"); w.write_float(gps_coarsen_deg(fx->lon));
  w.write_text("alt"); w.write_float(fx->altitude_m);
  w.write_text("speed"); w.write_float(g_speed_ema);
  w.write_text("hdop"); w.write_float(fx->hdop);
  w.write_text("sats"); w.write_uint(fx->satellites);
  w.write_text("mode"); w.write_uint((int)fx->fix_mode);
  
  // "quality"
  w.write_text("quality");
  w.write_uint(fx->quality);
  
  // "time_bucket"
  w.write_text("time_bucket");
  w.write_uint(time_bucket());
  
  // "uptime_sec"
  w.write_text("uptime_sec");
  w.write_uint(uptime_seconds());
  
  // "firmware"
  w.write_text("firmware");
  w.write_text(FIRMWARE_VERSION);
  
  if (!w.ok()) return false;
  *out_len = w.size();
  return true;
}

static bool build_boot_attestation(uint8_t* out, size_t cap, size_t* out_len) {
  CborWriter w(out, cap);
  
  w.write_map(9);
  
  w.write_text("device_id");
  w.write_text(g_device.device_id);
  
  w.write_text("device_type");
  w.write_text(DEVICE_TYPE);
  
  w.write_text("firmware");
  w.write_text(FIRMWARE_VERSION);
  
  w.write_text("ruleset");
  w.write_text(RULESET_ID);
  
  w.write_text("protocol");
  w.write_text(PROTOCOL_VERSION);
  
  w.write_text("pubkey");
  w.write_bytes(g_device.pubkey, 32);
  
  w.write_text("boot_count");
  w.write_uint(g_device.boot_count);
  
  w.write_text("boot_ms");
  w.write_uint(millis());
  
  w.write_text("chain_algorithm");
  w.write_text(CHAIN_ALGORITHM);
  
  if (!w.ok()) return false;
  *out_len = w.size();
  return true;
}

static bool build_state_change(FixState from, FixState to, const char* reason, 
                               uint8_t* out, size_t cap, size_t* out_len) {
  CborWriter w(out, cap);
  
  w.write_map(5);
  
  w.write_text("device_id");
  w.write_text(g_device.device_id);
  
  w.write_text("from_state");
  w.write_text(state_name(from));
  
  w.write_text("to_state");
  w.write_text(state_name(to));
  
  w.write_text("reason");
  w.write_text(reason);
  
  w.write_text("time_bucket");
  w.write_uint(time_bucket());
  
  if (!w.ok()) return false;
  *out_len = w.size();
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// WITNESS RECORD CREATION
// ════════════════════════════════════════════════════════════════════════════

static bool create_witness_record(const uint8_t* payload, size_t len, RecordType type, WitnessRecord* out) {
  // Hash payload
  uint8_t payload_hash[32];
  sha256_domain("securacv:fw:payload:v1", payload, len, payload_hash);
  
  // Update chain
  uint32_t tb = time_bucket();
  update_chain(payload_hash, tb, out);
  out->type = type;
  out->payload_len = len;
  
  // Sign chain hash
  sign_message(g_device.privkey, g_device.pubkey, out->chain_hash, 32, out->signature);
  
  // Verify immediately
  out->verified = verify_signature(g_device.pubkey, out->chain_hash, 32, out->signature);
  
  if (!out->verified) {
    g_health.verify_failures++;
    return false;
  }
  
  g_health.records_created++;
  g_health.records_verified++;

  // Durable tier FIRST, NVS cache second (codex P1 on #844): if the NVS
  // seq/head advanced before a failed or torn SD append, reboot would see
  // NVS ahead of the card, sd_wins() would keep the NVS head, and the next
  // line appended to the card would chain from a hash the card never got —
  // an unverifiable gap in exactly the window the recovery path exists
  // for. Appending first means a crash between the two steps leaves SD
  // ahead, which is precisely what the SD-wins reconciliation repairs.
  // Best-effort — a missing card keeps chaining in RAM/NVS behind one
  // latched health warning and never blocks the record path.
  if (sd_append_witness_record(out)) {
    g_health.sd_writes++;
  }

  // Persist chain state periodically
  if ((g_device.seq - g_device.seq_persisted) >= SD_PERSIST_INTERVAL) {
    persist_chain_state();
  }

  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Public bridge for csi_integration.cpp's strong override of
// csi_event_commit_witness(). The library declares that hook as a weak
// symbol; the host plugs in this implementation so every committed P0/P1
// CSI event signs into the existing witness chain.
//
// Payload format is intentionally tiny and human-greppable. We don't pull
// CBOR in just for this — the witness chain itself is the canonical
// record, the payload is just the per-event detail that gets hashed and
// signed alongside the chain head.
//
//   csi <module> <type> <category> <state> <conf> m=<n> b=<n> bpm=<n>
//       d=<n> bk=<n> kv=<firmware> rs=<ruleset> zn=<zone>
//
// kv / rs / zn satisfy spec/event_contract.md §2 — every event MUST
// carry the firmware version, ruleset, and zone it was scored under.
// All fields are ASCII; the chokepoint already sanitised the strings to
// printable ASCII before this point.
//
// The string is built by csi_witness_build_payload() (host-buildable,
// dependency-free) so the privacy-invariants fuzzer can assert the
// format on every CI run without dragging Arduino headers in.

// CSI zone id. Defaults to ZONE_ID; overridable via NVS key
// "core.zone_id" so a future dashboard / setup flow can label a
// canary's coverage area without recompiling.
//
// The wire format `kv=<v> rs=<r> zn=<z>` is space-delimited, so the
// override gets sanitised to a tokenizer-safe charset before caching:
// any byte outside [A-Za-z0-9._:-] becomes '_'. This keeps a future
// setup UI that accepts free-text labels (with spaces, accents,
// emoji) from corrupting the wire format and breaking downstream
// log_verify parsers that split on whitespace.
//
// `loaded` is set to true ONLY after `cached` is fully populated, so
// any caller (the chokepoint is single-threaded today, but a future
// task model shouldn't be a footgun) racing on first call sees either
// "buffer not yet populated, run init again" or "buffer fully ready"
// — never a torn read of partially-copied bytes.
static const char* csi_zone_id() {
  static char  cached[32];
  static bool  loaded = false;
  if (!loaded) {
    char  scratch[32];
    bool  found = false;
    Preferences prefs;
    if (prefs.begin("csi", /*readOnly=*/true)) {
      String v = prefs.getString("core.zone_id", "");
      prefs.end();
      if (v.length() > 0 && v.length() < sizeof(scratch)) {
        strncpy(scratch, v.c_str(), sizeof(scratch) - 1);
        scratch[sizeof(scratch) - 1] = '\0';
        found = true;
      }
    }
    if (!found) {
      strncpy(scratch, ZONE_ID, sizeof(scratch) - 1);
      scratch[sizeof(scratch) - 1] = '\0';
    }
    // Sanitize: keep only token-safe chars; replace others with '_'.
    for (size_t i = 0; scratch[i] != '\0' && i < sizeof(scratch); ++i) {
      const unsigned char c = (unsigned char)scratch[i];
      const bool ok = (c >= 'A' && c <= 'Z')
                   || (c >= 'a' && c <= 'z')
                   || (c >= '0' && c <= '9')
                   || c == '.' || c == '_' || c == '-' || c == ':';
      if (!ok) scratch[i] = '_';
    }
    memcpy(cached, scratch, sizeof(cached));
    loaded = true;   // publish only after cached is fully populated
  }
  return cached;
}

// Witness payload buffer size. Worst case for the format
// `csi <m> <t> <c> <s> <conf> m=N b=N bpm=N d=N bk=N kv=<fw> rs=<rs> zn=<zn>`
// runs ~220 bytes when every CSI_EVENT_NAME_MAX field is full plus a
// 31-char zone id; 256 leaves comfortable headroom and the helper's
// buffer-too-small return (-1) safely catches any future field growth
// without truncating mid-token.
static constexpr size_t CSI_WITNESS_PAYLOAD_MAX = 256;

extern "C" bool csi_witness_emit_event(const char* module_id,
                                       const char* type_name,
                                       uint8_t     category,       // 0=ambient 1=event 2=anomaly
                                       const char* state_name,
                                       const char* confidence,
                                       uint8_t     motion_score,
                                       uint8_t     breathing_score,
                                       uint8_t     bpm,
                                       uint16_t    duration_sec,
                                       uint8_t     time_bucket) {
  uint8_t payload[CSI_WITNESS_PAYLOAD_MAX];
  const int len = csi_witness_build_payload(
    (char*)payload, sizeof(payload),
    module_id, type_name, category,
    state_name, confidence,
    motion_score, breathing_score, bpm,
    duration_sec, time_bucket,
    FIRMWARE_VERSION, RULESET_ID, csi_zone_id());
  if (len <= 0) return false;

  WitnessRecord rec;
  // RECORD_WITNESS_EVENT (=1) was reserved in the RecordType enum and
  // unused — adopt it for module-emitted CSI events. RECORD_STATE_CHANGE
  // is reserved for the device-state pipeline already in use elsewhere.
  //
  // NOTE: this bridge fires on BUNDLE COMMIT (the chokepoint buffers
  // same-state events for up to the 10-minute bundle window), so it is
  // the wrong place for anything that must react to an arrival NOW —
  // the vault's motion trigger hooks rf_presence's immediate transition
  // callback instead (codex P1 on #847).
  return create_witness_record(payload, (size_t)len, RECORD_WITNESS_EVENT, &rec);
}

static bool verify_record_signature(const WitnessRecord* rec) {
  return verify_signature(g_device.pubkey, rec->chain_hash, 32, rec->signature);
}

// ════════════════════════════════════════════════════════════════════════════
// HEALTH LOGGING
// ════════════════════════════════════════════════════════════════════════════

void log_health(LogLevel level, LogCategory category, const char* message, const char* detail) {
  // Store threshold is operator-configurable (Device tab), default INFO.
  // g_log_min_level is clamped to <= WARNING, so ERROR/CRITICAL are always
  // stored no matter the setting — a user can quiet noise, not silence faults.
  if (level < g_log_min_level) return;

  if (g_health_log_ring == nullptr) {
    // Ring allocation failed at boot — degrade to Serial-only logging.
    Serial.printf("[%s/%s] %s", log_level_name(level),
                  log_category_name(category), message);
    if (detail && detail[0]) Serial.printf(" | %s", detail);
    Serial.println();
    return;
  }

  HealthLogRingEntry& entry = g_health_log_ring[g_health_log_ring_head];
  entry.seq = ++g_device.log_seq;
  entry.timestamp_ms = millis();
  entry.level = level;
  entry.category = category;
  entry.ack_status = ACK_STATUS_UNREAD;
  
  strncpy(entry.message, message ? message : "", sizeof(entry.message) - 1);
  entry.message[sizeof(entry.message) - 1] = '\0';
  
  if (detail) {
    strncpy(entry.detail, detail, sizeof(entry.detail) - 1);
    entry.detail[sizeof(entry.detail) - 1] = '\0';
  } else {
    entry.detail[0] = '\0';
  }
  
  g_health_log_ring_head = (g_health_log_ring_head + 1) % HEALTH_LOG_RING_SIZE;
  if (g_health_log_ring_count < HEALTH_LOG_RING_SIZE) {
    g_health_log_ring_count++;
  }
  
  g_health.logs_stored++;
  if (log_level_requires_attention(level)) {
    g_health.logs_unacked++;
  }

  // Durable tier: stage a fully-escaped JSON line for the loop-task
  // drainer. Format OUTSIDE the critical section; only the memcpy and
  // ring bookkeeping run under the lock. A full ring drops the OLDEST
  // staged line — the newest entry is the one that explains a burst.
  if (g_health_pending != nullptr) {
    char line[health_store::HS_LINE_MAX];
    const size_t n = health_store::line_build(
        line, sizeof(line), entry.seq, entry.timestamp_ms,
        log_level_name(level), log_category_name(category), message,
        detail ? detail : "");
    if (n > 0) {
      portENTER_CRITICAL(&g_health_pending_mux);
      HealthPendingLine* slot = &g_health_pending[g_health_pending_head];
      memcpy(slot->line, line, n);
      slot->line[n] = '\0';  // n < HS_LINE_MAX (line_build contract)
      slot->len = n;
      g_health_pending_head =
          (g_health_pending_head + 1) % health_store::HS_PENDING_SLOTS;
      if (g_health_pending_count < health_store::HS_PENDING_SLOTS) {
        g_health_pending_count++;
      } else {
        g_health_pending_dropped++;  // overwrote the oldest staged line
      }
      portEXIT_CRITICAL(&g_health_pending_mux);
    }
  }

  // Also print to Serial
  Serial.printf("[%s/%s] %s", log_level_name(level), log_category_name(category), message);
  if (detail && detail[0]) {
    Serial.printf(" | %s", detail);
  }
  Serial.println();
}

// Drain staged health lines to the per-boot /HEALTH file. LOOP TASK
// ONLY (the one SD-writer task). Copies the staged burst out under the
// critical section, then does file I/O with the lock released. One
// latched warning per failure streak (beacon-audit pattern); success
// re-arms it. Health files are regenerable artifacts — datamgmt's
// /HEALTH count rotation bounds the per-boot collection (Invariant IV
// protects only /WITNESS and /CHAIN).
static void health_store_drain() {
#if FEATURE_SD_STORAGE
  if (g_health_pending == nullptr) return;
  if (!g_sd_mounted || sd_mount_in_flight()) return;

  // Back off after a failure: a mounted-but-failing card (write-locked,
  // full, corrupt directory) would otherwise be re-probed on every loop
  // pass for as long as entries stay staged. Wrap-safe uint32 math.
  static uint32_t s_last_fail_ms = 0;
  if (s_last_fail_ms != 0 &&
      (uint32_t)(millis() - s_last_fail_ms) < 5000UL) {
    return;
  }

  // Anything staged? (Cheap peek; the real dequeue happens per line.)
  portENTER_CRITICAL(&g_health_pending_mux);
  size_t staged = g_health_pending_count;
  portEXIT_CRITICAL(&g_health_pending_mux);
  if (staged == 0) return;

  bool ok = SD.exists("/HEALTH") || SD.mkdir("/HEALTH");
  if (ok) {
    char path[health_store::HS_PATH_MAX];
    health_store::boot_filename(g_device.boot_count, path, sizeof(path));
    File f = SD.open(path, FILE_APPEND);
    if (!f) {
      ok = false;
    } else {
      // Dequeue one line at a time: a 512 B stack copy per line keeps
      // the critical section to a memcpy and needs no static burst
      // buffer (which would hand 4 KB back to internal DRAM — the exact
      // budget the PSRAM diet reclaimed).
      while (ok) {
        char line[health_store::HS_LINE_MAX];
        size_t len = 0;
        portENTER_CRITICAL(&g_health_pending_mux);
        if (g_health_pending_count > 0) {
          const size_t idx =
              (g_health_pending_head + health_store::HS_PENDING_SLOTS -
               g_health_pending_count) %
              health_store::HS_PENDING_SLOTS;
          len = g_health_pending[idx].len;
          if (len < health_store::HS_LINE_MAX) {
            memcpy(line, g_health_pending[idx].line, len);
          } else {
            len = 0;  // corrupted slot length — drop, never overflow
          }
          g_health_pending_count--;
        }
        portEXIT_CRITICAL(&g_health_pending_mux);
        if (len == 0) break;
        ok = (f.write((const uint8_t*)line, len) == len);
      }
      if (ok) {
        portENTER_CRITICAL(&g_health_pending_mux);
        const uint32_t dropped = g_health_pending_dropped;
        g_health_pending_dropped = 0;
        portEXIT_CRITICAL(&g_health_pending_mux);
        if (dropped > 0) {
          char note[112];
          const int m = snprintf(
              note, sizeof(note),
              "{\"v\":1,\"lvl\":\"WARNING\",\"cat\":\"STORAGE\","
              "\"msg\":\"health lines dropped\",\"detail\":\"%u\"}\n",
              (unsigned)dropped);
          if (m > 0 && (size_t)m < sizeof(note)) {
            ok = (f.write((const uint8_t*)note, (size_t)m) == (size_t)m);
          }
        }
      }
      f.close();
    }
  }

  if (ok) {
    g_health_sd_warned = false;  // healthy again — re-arm the latch
    s_last_fail_ms = 0;
    return;
  }
  s_last_fail_ms = millis();
  if (!g_health_sd_warned) {
    g_health_sd_warned = true;
    // This warning stages into the pending ring like any other entry;
    // the latch keeps a persistently failing card from looping it.
    log_health(SCV_LOG_WARNING, SCV_CAT_STORAGE,
               "health log: SD tier unavailable — RAM ring only", nullptr);
  }
#endif
}

// Public wrapper for external modules (e.g., chirp_channel.cpp)
// This function is declared in health_log.h
void health_log(LogLevel level, LogCategory category, const char* message) {
  log_health(level, category, message, nullptr);
}

static bool acknowledge_log_entry(uint32_t log_seq, AckStatus new_status, const char* reason) {
  for (size_t i = 0; i < g_health_log_ring_count; i++) {
    HealthLogRingEntry& entry = g_health_log_ring[i];
    if (entry.seq == log_seq) {
      if (entry.ack_status == ACK_STATUS_UNREAD && log_level_requires_attention(entry.level)) {
        if (g_health.logs_unacked > 0) g_health.logs_unacked--;
      }
      entry.ack_status = new_status;
      return true;
    }
  }
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// STATE TRANSITION LOGGING
// ════════════════════════════════════════════════════════════════════════════

static void log_state_transition(FixState from, FixState to, const char* reason) {
  #if FEATURE_STATE_LOG
  g_health.state_changes++;
  
  char msg[64];
  snprintf(msg, sizeof(msg), "%s -> %s", state_name(from), state_name(to));
  log_health(SCV_LOG_NOTICE, SCV_CAT_GPS, msg, reason);
  
  // Create state change witness record
  uint8_t payload[256];
  size_t payload_len = 0;
  if (build_state_change(from, to, reason, payload, sizeof(payload), &payload_len)) {
    create_witness_record(payload, payload_len, RECORD_STATE_CHANGE, &g_last_record);
  }
  #endif
}

// ════════════════════════════════════════════════════════════════════════════
// NMEA PARSING
// ════════════════════════════════════════════════════════════════════════════

static bool read_nmea_line(char* out, size_t cap, size_t* len) {
  while (true) {
    uint8_t b;
    if (!g_gps_rb || !g_gps_rb->pop(b)) {
      return false;
    }
    
    if (b == '\n' || b == '\r') {
      if (g_line_len > 0) {
        size_t copy_len = (g_line_len < cap - 1) ? g_line_len : cap - 1;
        memcpy(out, g_line_buf, copy_len);
        out[copy_len] = '\0';
        *len = copy_len;
        g_line_len = 0;
        return true;
      }
    } else if (g_line_len < sizeof(g_line_buf) - 1) {
      g_line_buf[g_line_len++] = b;
    }
  }
}

static int parse_int(const char* s, int def) {
  if (!s || !*s) return def;
  return atoi(s);
}

static double parse_double(const char* s, double def) {
  if (!s || !*s) return def;
  return atof(s);
}

static char* get_field(char* s, int field) {
  int f = 0;
  char* p = s;
  while (*p && f < field) {
    if (*p == ',') f++;
    p++;
  }
  if (f != field) return nullptr;
  return p;
}

static void parse_nmea(char* line, GnssFix* fx) {
  g_health.gps_sentences++;
  
  #if DEBUG_NMEA
  Serial.println(line);
  #endif
  
  // Verify checksum
  if (line[0] != '$') return;
  char* star = strchr(line, '*');
  if (!star) return;
  
  // Parse sentence type
  char* type = line + 3;  // Skip $XX
  
  if (strncmp(type, "GGA", 3) == 0) {
    g_health.gga_count++;
    // $GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,47.0,M,,*47
    char* lat_str = get_field(line, 2);
    char* lat_dir = get_field(line, 3);
    char* lon_str = get_field(line, 4);
    char* lon_dir = get_field(line, 5);
    char* quality = get_field(line, 6);
    char* sats = get_field(line, 7);
    char* hdop_str = get_field(line, 8);
    char* alt_str = get_field(line, 9);
    char* geoid_str = get_field(line, 11);
    
    fx->quality = parse_int(quality, 0);
    fx->satellites = parse_int(sats, 0);
    fx->hdop = parse_double(hdop_str, 99.9);
    fx->altitude_m = parse_double(alt_str, 0);
    fx->geoid_sep_m = parse_double(geoid_str, 0);
    
    if (lat_str && *lat_str) {
      double lat_raw = parse_double(lat_str, 0);
      int lat_deg = (int)(lat_raw / 100);
      double lat_min = lat_raw - lat_deg * 100;
      fx->lat = lat_deg + lat_min / 60.0;
      if (lat_dir && *lat_dir == 'S') fx->lat = -fx->lat;
    }
    
    if (lon_str && *lon_str) {
      double lon_raw = parse_double(lon_str, 0);
      int lon_deg = (int)(lon_raw / 100);
      double lon_min = lon_raw - lon_deg * 100;
      fx->lon = lon_deg + lon_min / 60.0;
      if (lon_dir && *lon_dir == 'W') fx->lon = -fx->lon;
    }
    
    fx->valid = (fx->quality > 0);
    fx->last_gga_ms = millis();
    fx->last_update_ms = millis();
    
    if (fx->valid && g_health.gps_lock_ms == 0) {
      g_health.gps_lock_ms = millis();
    }
  }
  else if (strncmp(type, "RMC", 3) == 0) {
    g_health.rmc_count++;
    // $GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
    char* time_str = get_field(line, 1);
    char* status = get_field(line, 2);
    char* speed = get_field(line, 7);
    char* course = get_field(line, 8);
    char* date_str = get_field(line, 9);

    // Status 'A' = active/valid fix, 'V' = void (no fix, or a warm-up
    // sentence the receiver emits before it has one). A void RMC can still
    // carry a stale or all-zero time/date/speed/course — those fields must
    // not be trusted, or presented as a fix, when status isn't 'A'.
    bool rmc_active = status && *status == 'A';

    if (rmc_active && speed && *speed) {
      fx->speed_knots = parse_double(speed, 0);
      fx->speed_kmh = knots_to_kmh(fx->speed_knots);
      float mps = knots_to_mps(fx->speed_knots);
      g_speed_ema = g_speed_ema * (1.0f - SPEED_EMA_ALPHA) + mps * SPEED_EMA_ALPHA;
    }

    if (rmc_active && course && *course) {
      fx->course_deg = parse_double(course, 0);
    }

    if (rmc_active && time_str && strlen(time_str) >= 6 &&
        date_str && strlen(date_str) >= 6) {
      int hour = (time_str[0] - '0') * 10 + (time_str[1] - '0');
      int minute = (time_str[2] - '0') * 10 + (time_str[3] - '0');
      int second = (time_str[4] - '0') * 10 + (time_str[5] - '0');
      int centisecond = (strlen(time_str) > 7) ? parse_int(time_str + 7, 0) : 0;
      int day = (date_str[0] - '0') * 10 + (date_str[1] - '0');
      int month = (date_str[2] - '0') * 10 + (date_str[3] - '0');
      int year = 2000 + (date_str[4] - '0') * 10 + (date_str[5] - '0');

      // Guard against a corrupt-but-checksum-valid sentence, or a receiver
      // that hasn't loaded ephemeris yet and emits all-zero fields, before
      // this ever reaches g_gps_utc / a system-clock sync.
      if (securacv::gnss::gnss_calendar_valid(year, month, day, hour, minute, second)) {
        g_gps_utc.hour = hour;
        g_gps_utc.minute = minute;
        g_gps_utc.second = second;
        g_gps_utc.centisecond = centisecond;
        g_gps_utc.day = day;
        g_gps_utc.month = month;
        g_gps_utc.year = year;
        g_gps_utc.valid = true;
        g_gps_utc.last_seen_ms = millis();
      }
    }

    fx->last_rmc_ms = millis();
  }
  else if (strncmp(type, "GSA", 3) == 0) {
    g_health.gsa_count++;
    // $GNGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.0,0.9,0.4*30
    char* mode = get_field(line, 2);
    char* pdop = get_field(line, 15);
    char* hdop = get_field(line, 16);
    char* vdop = get_field(line, 17);
    
    if (mode && *mode) {
      fx->fix_mode = (GpsFixMode)parse_int(mode, 1);
    }
    fx->pdop = parse_double(pdop, 99.9);
    if (hdop && *hdop) fx->hdop = parse_double(hdop, fx->hdop);
    fx->vdop = parse_double(vdop, 99.9);
    fx->last_gsa_ms = millis();
  }
  else if (strncmp(type, "GSV", 3) == 0) {
    g_health.gsv_count++;
    // Satellites in view
    char* siv = get_field(line, 3);
    if (siv && *siv) {
      fx->sats_in_view = parse_int(siv, 0);
    }
  }
  else if (strncmp(type, "VTG", 3) == 0) {
    g_health.vtg_count++;
    // Course and speed
    char* course = get_field(line, 1);
    char* speed_kmh = get_field(line, 7);
    if (course && *course) {
      fx->course_deg = parse_double(course, fx->course_deg);
    }
    if (speed_kmh && *speed_kmh) {
      fx->speed_kmh = parse_double(speed_kmh, fx->speed_kmh);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// STATE MACHINE
// ════════════════════════════════════════════════════════════════════════════

static FixState update_state(const GnssFix* fx, FixState cur) {
  uint32_t now = millis();
  FixState desired = cur;
  const char* reason = nullptr;
  
  bool has_recent_fix = fx->valid && (now - fx->last_update_ms < FIX_LOST_TIMEOUT_MS);
  
  if (!has_recent_fix) {
    if (cur != STATE_NO_FIX && cur != STATE_FIX_LOST) {
      desired = STATE_FIX_LOST;
      reason = "timeout";
    } else if (cur == STATE_FIX_LOST && (now - g_state_entered_ms) > 10000) {
      desired = STATE_NO_FIX;
      reason = "prolonged_loss";
    }
  } else {
    if (cur == STATE_NO_FIX || cur == STATE_FIX_LOST) {
      desired = STATE_FIX_ACQUIRED;
      reason = "fix_obtained";
      g_health.gps_healthy = true;
    } else if (cur == STATE_FIX_ACQUIRED) {
      if (g_speed_ema >= MOVING_THRESHOLD_MPS) {
        desired = STATE_MOVING;
        reason = "speed_high";
      } else if (g_speed_ema <= STATIC_THRESHOLD_MPS) {
        desired = STATE_STATIONARY;
        reason = "speed_low";
      }
    } else if (cur == STATE_STATIONARY && g_speed_ema >= MOVING_THRESHOLD_MPS) {
      desired = STATE_MOVING;
      reason = "started_moving";
    } else if (cur == STATE_MOVING && g_speed_ema <= STATIC_THRESHOLD_MPS) {
      desired = STATE_STATIONARY;
      reason = "stopped";
    }
  }
  
  if (desired != cur) {
    bool needs_hysteresis = 
      (cur == STATE_STATIONARY && desired == STATE_MOVING) ||
      (cur == STATE_MOVING && desired == STATE_STATIONARY);
    
    if (needs_hysteresis) {
      if (g_pending_state != desired) {
        g_pending_state = desired;
        g_pending_state_ms = now;
      }
      
      if ((now - g_pending_state_ms) >= STATE_HYSTERESIS_MS) {
        log_state_transition(cur, desired, reason);
        g_state_entered_ms = now;
        g_pending_state = desired;
        return desired;
      }
      return cur;
    } else {
      log_state_transition(cur, desired, reason);
      g_state_entered_ms = now;
      g_pending_state = desired;
      return desired;
    }
  }
  
  g_pending_state = cur;
  return cur;
}

// ════════════════════════════════════════════════════════════════════════════
// MOTION FILTER
// ════════════════════════════════════════════════════════════════════════════
//
// Why this exists: the L76K — like every consumer GNSS — emits a non-zero
// speed and a few meters of position scatter even when the device is bolted
// to a wall. The witness chain still records the raw fix (we never lie about
// what the receiver said), but the API/UI publishes a filtered view so a
// stationary mounted camera presents as stationary. The filter has two modes:
//
//   LOCKED   (stationary): displayed lat/lon/alt are pinned to a running
//              centroid of recent fixes; displayed speed is forced to 0.
//              We accept new fixes into the centroid only while they stay
//              within STATIONARY_RADIUS_M of the anchor and HDOP is sane.
//
//   UNLOCKED (moving): displayed values track the raw fix verbatim, and
//              speed is the existing EMA. We re-acquire an anchor as soon
//              as the witness state machine settles back to STATIONARY.
//
// Transitions follow the existing FixState hysteresis: the filter only
// unlocks once we've seen sustained motion (raw speed >= MOVING_THRESHOLD_MPS
// or position drift > STATIONARY_RADIUS_M for MOTION_RELEASE_MS). That keeps
// a stolen device honest — real motion shows up — without flickering during
// a brief multipath spike at rest.

static double motion_haversine_m(double lat1, double lon1, double lat2, double lon2) {
  // Spherical Earth approximation; accurate to ~0.5% for short distances,
  // which is well within GNSS noise. We only use this for "is this fix near
  // the anchor?" decisions, not for any logged position.
  const double R = 6371000.0;
  const double d2r = 0.017453292519943295;  // M_PI / 180
  double phi1 = lat1 * d2r;
  double phi2 = lat2 * d2r;
  double dphi = (lat2 - lat1) * d2r;
  double dlam = (lon2 - lon1) * d2r;
  double s1 = sin(dphi * 0.5);
  double s2 = sin(dlam * 0.5);
  double a = s1 * s1 + cos(phi1) * cos(phi2) * s2 * s2;
  if (a < 0) a = 0; if (a > 1) a = 1;
  return 2.0 * R * asin(sqrt(a));
}

static void motion_reset_anchor(double lat, double lon, double alt) {
  g_motion.anchor_lat = lat;
  g_motion.anchor_lon = lon;
  g_motion.anchor_alt_m = alt;
  g_motion.anchor_samples = 1;
  g_motion.has_anchor = true;
  g_motion.motion_candidate_since_ms = 0;
}

static void motion_filter_update(const GnssFix* fx, FixState state) {
  uint32_t now = millis();

  // No fix → nothing meaningful to display. Drop the lock so we don't keep
  // showing a stale anchor; surface zeros and let the UI's "Waiting for fix"
  // copy take over.
  if (!fx->valid) {
    g_motion.is_locked = false;
    g_motion.has_anchor = false;
    g_motion.display_lat = 0.0;
    g_motion.display_lon = 0.0;
    g_motion.display_alt_m = 0.0;
    g_motion.display_speed_mps = 0.0f;
    g_motion.motion_candidate_since_ms = 0;
    return;
  }

  // Untrustworthy fix (deep multipath, urban canyon) — pass it through but
  // don't let it pollute the anchor.
  bool fix_trustworthy = (fx->hdop > 0 && fx->hdop <= HDOP_LOCK_MAX);

  bool want_locked = (state == STATE_STATIONARY) ||
                     (state == STATE_FIX_ACQUIRED && g_speed_ema <= STATIC_THRESHOLD_MPS);

  if (want_locked) {
    if (!g_motion.has_anchor || !g_motion.is_locked) {
      // (Re)entering the locked state — seed the anchor at the current fix.
      motion_reset_anchor(fx->lat, fx->lon, fx->altitude_m);
      g_motion.is_locked = true;
    } else {
      double drift = motion_haversine_m(g_motion.anchor_lat, g_motion.anchor_lon, fx->lat, fx->lon);

      if (drift <= STATIONARY_RADIUS_M && fix_trustworthy) {
        // Fold the new fix into the anchor with an EMA. Using a slow alpha
        // means a single bad fix can't yank the displayed position around,
        // but real settling still shows up over ~10 samples.
        const double a = (double)ANCHOR_EMA_ALPHA;
        g_motion.anchor_lat   = g_motion.anchor_lat   * (1.0 - a) + fx->lat        * a;
        g_motion.anchor_lon   = g_motion.anchor_lon   * (1.0 - a) + fx->lon        * a;
        g_motion.anchor_alt_m = g_motion.anchor_alt_m * (1.0 - a) + fx->altitude_m * a;
        if (g_motion.anchor_samples < UINT32_MAX) g_motion.anchor_samples++;
        g_motion.motion_candidate_since_ms = 0;
      } else if (drift > STATIONARY_RADIUS_M) {
        // Fix is outside the lock radius. Could be real motion the state
        // machine hasn't caught yet, or a one-off multipath outlier. Start
        // the candidate clock; only release the lock if we keep seeing
        // outside-radius fixes for MOTION_RELEASE_MS.
        if (g_motion.motion_candidate_since_ms == 0) {
          g_motion.motion_candidate_since_ms = now;
        }
        if ((now - g_motion.motion_candidate_since_ms) >= MOTION_RELEASE_MS) {
          g_motion.is_locked = false;
          g_motion.has_anchor = false;
        }
      }
    }
  } else {
    // State machine says we're moving — release the lock immediately so the
    // UI tracks the raw fix.
    g_motion.is_locked = false;
    g_motion.has_anchor = false;
    g_motion.motion_candidate_since_ms = 0;
  }

  if (g_motion.is_locked && g_motion.has_anchor) {
    g_motion.display_lat = g_motion.anchor_lat;
    g_motion.display_lon = g_motion.anchor_lon;
    g_motion.display_alt_m = g_motion.anchor_alt_m;
    g_motion.display_speed_mps = 0.0f;
  } else {
    g_motion.display_lat = fx->lat;
    g_motion.display_lon = fx->lon;
    g_motion.display_alt_m = fx->altitude_m;
    // While moving, surface the smoothed speed; deadband sub-threshold so we
    // don't render meaningless 0.05 m/s twitches once we've settled.
    g_motion.display_speed_mps = (g_speed_ema < STATIC_THRESHOLD_MPS) ? 0.0f : g_speed_ema;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// HTTP HANDLERS
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t http_send_json(httpd_req_t* req, const char* json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_send_error(httpd_req_t* req, int status_code, const char* error_code) {
  httpd_resp_set_status(req, status_code == 400 ? "400 Bad Request" :
                              status_code == 404 ? "404 Not Found" :
                              status_code == 500 ? "500 Internal Server Error" : "400 Bad Request");
  char response[128];
  snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", error_code);
  return http_send_json(req, response);
}

// The dashboard/settings/companion pages ship only as gzip (the uncompressed
// copies are compiled out to save flash). Every browser sends
// "Accept-Encoding: gzip", but a client that doesn't (e.g. plain `curl`
// without --compressed) would otherwise receive unreadable bytes. Serve the
// gzip body to capable clients; reply 406 with a hint to everyone else.
static esp_err_t send_gzip_html(httpd_req_t* req, const uint8_t* gz, size_t gz_len) {
  char accept_enc[128] = {0};
  bool accepts_gzip =
      httpd_req_get_hdr_value_str(req, "Accept-Encoding", accept_enc,
                                  sizeof(accept_enc)) == ESP_OK &&
      strstr(accept_enc, "gzip") != nullptr;
  if (!accepts_gzip) {
    httpd_resp_set_status(req, "406 Not Acceptable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(
        req, "This page is gzip-encoded. Use a client that accepts gzip "
             "(browsers do; for curl pass --compressed).");
  }
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, (const char*)gz, gz_len);
}

static esp_err_t handle_ui(httpd_req_t* req) {
  // The default route now lands on the headline Sensing dashboard from
  // csi_dashboard_html.h (Phase 3 of the WiFi CSI Tool plan). The legacy
  // tabbed admin dashboard is reachable at /admin via handle_legacy_ui().
  //
  // Three branches:
  //   1. cv_session cookie present and valid → serve the dashboard HTML
  //      as-is. The page's in-page fetches authenticate via the cookie
  //      (HttpOnly + SameSite=Strict, sent automatically by browsers),
  //      so no token ever appears in HTML source for an on-AP attacker
  //      to view-source and harvest (PR #392 review r3213361582).
  //   2. ?cv_pair=<hex> query param present → consume the one-shot
  //      pair token, mint a fresh session, set the cookie, and 302
  //      redirect to "/" so the URL is clean and the cookie is in
  //      effect for subsequent requests.
  //   3. Neither cookie nor pair param → render the "Welcome / Open
  //      dashboard" landing page (csi_integration::send_pair_landing),
  //      which mints a new pair token and offers a one-tap link.
  g_health.http_requests++;

  // Branch 0: first-boot setup. canary.local (or the 192.168.4.1 fallback link)
  // should land on the setup wizard — but the captive DNS hijack points *every*
  // domain at this IP, so captive-portal assistants probing the root of foreign
  // domains also hit "/". Gate on the Host header: only a request that actually
  // asked for canary.local / 192.168.4.1 is the user's real browser, so mint a
  // token and redirect it to the wizard. Anything else gets the plain static
  // page — redirecting a captive mini-browser to the SPA is the white screen
  // this PR exists to fix.
  if (setup_wizard::is_active()) {
    char host[64] = {0};
    bool direct = false;
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
      direct = (strstr(host, "canary.local") != nullptr) ||
               (strstr(host, "192.168.4.1") != nullptr);
    }
    if (direct) {
      char tok_hex[csi_integration::PAIR_TOKEN_HEX_LEN + 1];
      if (csi_integration::pair_token_issue(tok_hex, sizeof(tok_hex))) {
        char location[128];
        // Defense-in-depth: a truncated Location would redirect to a malformed
        // (and unauthenticated) URI, so bail out rather than emit it.
        if (snprintf(location, sizeof(location), "/companion?token=%s", tok_hex) >= (int)sizeof(location)) {
          httpd_resp_set_status(req, "500 Internal Server Error");
          httpd_resp_set_type(req, "text/plain");
          return httpd_resp_send(req, "Setup link error. Try again.", HTTPD_RESP_USE_STRLEN);
        }
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", location);
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
        return httpd_resp_send(req, nullptr, 0);
      }
      // Token mint failed — fall through to the normal dashboard landing.
    } else {
      // Captive-portal probe (foreign Host via DNS hijack): serve the plain
      // static "open canary.local" instruction, never the SPA.
      httpd_resp_set_status(req, "200 OK");
      httpd_resp_set_type(req, "text/html; charset=utf-8");
      httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
      return httpd_resp_send(req, CAPTIVE_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    }
  }

  // Branch 1: existing valid session.
  if (csi_integration::session_validate_cookie(req)) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return send_gzip_html(req, CSI_DASHBOARD_HTML_GZ, CSI_DASHBOARD_HTML_GZ_LEN);
  }

  // Branch 2: one-shot pair-token consumption.
  char qs[160];
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
    char pair_hex[csi_integration::PAIR_TOKEN_HEX_LEN + 1];
    if (httpd_query_key_value(qs, "cv_pair", pair_hex, sizeof(pair_hex)) == ESP_OK
        && csi_integration::pair_token_consume(pair_hex)) {
      char session_hex[csi_integration::SESSION_COOKIE_HEX_LEN + 1];
      if (csi_integration::session_issue(session_hex, sizeof(session_hex))) {
        // 86400 = 24 h, matching SESSION_TTL_MS in csi_integration.cpp.
        // HttpOnly: no JS can read the cookie (mitigates XSS exfil).
        // SameSite=Strict: cross-origin requests can't carry it (CSRF).
        // Path=/: covers /, /tune, /api/*.
        char cookie_hdr[160];
        snprintf(cookie_hdr, sizeof(cookie_hdr),
          "cv_session=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=86400",
          session_hex);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_hdr);
        httpd_resp_set_hdr(req, "Location", "/");
        return httpd_resp_send(req, nullptr, 0);
      }
    }
  }

  // Branch 3: pair landing.
  return csi_integration::send_pair_landing(req) ? ESP_OK : ESP_FAIL;
}

static esp_err_t handle_legacy_ui(httpd_req_t* req) {
  // The original tabbed administrator dashboard. Reachable at /admin so
  // the canary-wap fleet keeps a familiar surface for power-user tasks
  // (camera peek, witness export, fine-grained tabs), and at /settings
  // for a direct route to device settings from the headline dashboard.
  g_health.http_requests++;
  httpd_resp_set_type(req, "text/html");
  return send_gzip_html(req, CANARY_UI_HTML_GZ, CANARY_UI_HTML_GZ_LEN);
}

// ── Companion PWA (Web Bluetooth) ───────────────────────────────────────────
// Serves the standalone phone-side console at /companion. Pure static asset
// shipping — no auth needed (the page itself just renders; the BLE
// characteristics it talks to enforce READ_ENC + READ_AUTHEN, so a
// drive-by visitor can load the HTML but can't see device state without
// pairing first).

static esp_err_t handle_companion_html(httpd_req_t* req) {
  g_health.http_requests++;
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  // Cache aggressively in the browser; the service worker will revalidate
  // network-first when it can reach us, so updates land within one visit.
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
  return send_gzip_html(req, COMPANION_HTML_GZ, COMPANION_HTML_GZ_LEN);
}

static esp_err_t handle_companion_sw(httpd_req_t* req) {
  g_health.http_requests++;
  httpd_resp_set_type(req, "application/javascript; charset=utf-8");
  // Allow the SW (served from /) to claim the /companion scope. Without
  // this header the browser refuses scope: '/companion' in register().
  httpd_resp_set_hdr(req, "Service-Worker-Allowed", "/companion");
  // SW itself MUST NOT be cached so updates can land — browsers
  // re-fetch on every navigation when no cache directive present.
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, COMPANION_SW_JS, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_companion_manifest(httpd_req_t* req) {
  g_health.http_requests++;
  httpd_resp_set_type(req, "application/manifest+json");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
  return httpd_resp_send(req, COMPANION_MANIFEST, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_status(httpd_req_t* req) {
  g_health.http_requests++;

  JsonDocument doc;
  doc["ok"] = true;
  doc["device_id"] = g_device.device_id;
  doc["device_type"] = DEVICE_TYPE;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["ruleset"] = RULESET_ID;

  char fp_hex[17];
  hex_to_str(fp_hex, g_device.pubkey_fp, 8);
  doc["fingerprint"] = fp_hex;

  char pubkey_hex[65];
  hex_to_str(pubkey_hex, g_device.pubkey, 32);
  doc["pubkey"] = pubkey_hex;

  doc["uptime_sec"] = uptime_seconds();
  doc["boot_count"] = g_device.boot_count;
  doc["chain_seq"] = g_device.seq;
  doc["witness_count"] = g_health.records_created;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["min_heap"] = g_health.min_heap;

  doc["crypto_healthy"] = g_health.crypto_healthy;
  doc["wifi_active"] = g_health.wifi_active;

  // ═══════════════════════════════════════════════════════════════════════════
  // Hardware State — Use hardware_state.h for resilient, non-blocking info
  // ═══════════════════════════════════════════════════════════════════════════

  doc["safe_mode"] = g_hw.safe_mode;
  doc["last_reset"] = reset_reason_name(g_hw.last_reset_reason);
  doc["last_reset_crash"] = g_hw.last_reset_was_crash;

  // GPS status (using hardware state - non-blocking, graceful degradation)
  doc["gps_healthy"] = g_hw.gps_available;
  doc["gps_available"] = g_hw.gps_available;
  doc["gps_state"] = gps_state_name(g_hw.gps_state);

  // SD card status (using CACHED values - non-blocking!)
  // This is critical: SD.totalBytes() and SD.usedBytes() can block if card is removed
  doc["sd_mounted"] = sd_is_available();
  doc["sd_healthy"] = g_hw.sd_state == SD_MOUNTED;
  doc["sd_state"] = sd_state_name(g_hw.sd_state);

  // Use cached SD info (updated periodically in loop(), not on every request)
  if (sd_is_available()) {
    doc["sd_total"] = g_hw.sd_total_bytes;
    doc["sd_used"] = g_hw.sd_total_bytes - g_hw.sd_free_bytes;
    doc["sd_free"] = g_hw.sd_free_bytes;
  } else {
    doc["sd_total"] = 0;
    doc["sd_used"] = 0;
    doc["sd_free"] = 0;
  }

#if FEATURE_CAMERA_PEEK
  doc["camera_ready"] = g_camera_initialized;
  doc["camera_available"] = g_hw.camera_available;
  doc["peek_active"] = g_peek_active;
  doc["peek_resolution"] = (int)g_peek_framesize;
#endif

  doc["logs_stored"] = g_health.logs_stored;
  doc["unacked_count"] = g_health.logs_unacked;

  // GPS position data (safe even if GPS absent - returns zeros/false).
  // We surface the motion-filtered values here so a stationary mounted
  // device shows a stable lat/lon and 0 m/s, instead of the raw L76K jitter.
  // The raw fix is still recorded in the witness chain — see
  // motion_filter_update() for the rationale.
  JsonObject gps = doc["gps"].to<JsonObject>();
  gps["available"] = g_hw.gps_available;
  gps["fix"] = g_fix.valid;
  if (g_hw.gps_available && g_fix.valid) {
    gps["lat"] = gps_coarsen_deg(g_motion.display_lat);
    gps["lon"] = gps_coarsen_deg(g_motion.display_lon);
    gps["alt"] = g_motion.display_alt_m;
    gps["speed"] = g_motion.display_speed_mps;
    gps["hdop"] = g_fix.hdop;
    gps["stationary"] = g_motion.is_locked;
    gps["raw_speed"] = g_speed_ema;  // For diagnostics / "show jitter" toggles
  } else {
    gps["lat"] = 0.0;
    gps["lon"] = 0.0;
    gps["alt"] = 0.0;
    gps["speed"] = 0.0;
    gps["hdop"] = 99.9;
    gps["stationary"] = false;
    gps["raw_speed"] = 0.0;
  }
  gps["quality"] = g_fix.quality;
  gps["satellites"] = g_hw.gps_available ? g_fix.satellites : 0;
  gps["fix_mode"] = (int)g_fix.fix_mode;
  gps["state"] = state_name(g_state);

  // WiFi Presence Detection status
  JsonObject presence = doc["presence"].to<JsonObject>();
  presence["wifi_available"] = (bool)FEATURE_WIFI_PRESENCE;
  presence["wifi_enabled"] = wifi_presence::is_enabled();
  presence["wifi_count"] = wifi_presence::get_current_count();
  presence["wifi_last_count"] = wifi_presence::get_last_count();
  presence["ble_available"] = (bool)FEATURE_BLE;
  #if FEATURE_BLE
  presence["ble_enabled"] = ble_manager::isNearbyActive();
  #else
  presence["ble_enabled"] = false;
  #endif

  // Audible Chirp status
  JsonObject chirp_hw = doc["audible_chirp"].to<JsonObject>();
  chirp_hw["available"] = (bool)FEATURE_AUDIBLE_CHIRP;
  chirp_hw["visual_only"] = audible_chirp::is_visual_only();
  chirp_hw["chirps_played"] = audible_chirp::get_chirps_played();

  // CSI sensing pipeline health. `running` = HAL has registered the
  // ESP-IDF callback; `frames_received` rising = radio is producing data;
  // `windows_emitted` rising = main-loop pump is draining and finalizing
  // 1 Hz windows; `frames_dropped_full` = pump is being starved (should be
  // 0 in normal operation); `snapshot_valid` = a v1 module has committed
  // at least one event since boot.
  JsonObject csi = doc["csi"].to<JsonObject>();
  csi["running"] = csi_integration::csi_running();
  csi["snapshot_valid"] = csi_integration::snapshot_valid();
  csi_stats_t cs = {};
  if (csi_integration::csi_get_stats(&cs)) {
    csi["frames_received"] = (uint32_t)cs.frames_received;
    csi["windows_emitted"] = (uint32_t)cs.windows_emitted;
    csi["frames_dropped_full"] = (uint32_t)cs.frames_dropped_full;
    csi["frames_dropped_rate"] = (uint32_t)cs.frames_dropped_rate;
    csi["frames_dropped_rssi"] = (uint32_t)cs.frames_dropped_rssi;
    csi["windows_degraded"] = (uint32_t)cs.windows_degraded;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

#if FEATURE_SYS_MONITOR
static esp_err_t handle_system_metrics(httpd_req_t* req) {
  g_health.http_requests++;

  // Use the sys_monitor JSON generator (larger buffer for full device info + F/C temps)
  char buf[2048];
  size_t len = sys_monitor::get_json(buf, sizeof(buf));

  if (len == 0) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"Failed to generate metrics\"}");
  }

  return http_send_json(req, buf);
}
#endif

// ────────────────────────────────────────────────────────────────────────────
// GET /api/diagnostics — heap snapshot + SD health + degradation level
// ────────────────────────────────────────────────────────────────────────────

#if FEATURE_SYS_MONITOR
static esp_err_t handle_diagnostics(httpd_req_t* req) {
  g_health.http_requests++;

  sys_monitor::DegradeLevel degrade = sys_monitor::get_degrade_level();
  sys_monitor::SDHealthStats sd_h = sys_monitor::get_sd_health();

  char buf[512];
  int len = snprintf(buf, sizeof(buf),
    "{"
    "\"ok\":true,"
    "\"heap\":{"
      "\"free\":%u,"
      "\"min_free\":%u,"
      "\"largest_block\":%u,"
      "\"total\":%u"
    "},"
    "\"degradation\":{"
      "\"level\":%u,"
      "\"level_name\":\"%s\""
    "},"
    "\"sd_health\":{"
      "\"total_writes\":%u,"
      "\"write_errors\":%u,"
      "\"usage_pct\":%u,"
      "\"space_warning\":%s,"
      "\"space_critical\":%s"
    "},"
    "\"uptime_sec\":%u"
    "}",
    (unsigned)sys_monitor::g_sys_metrics.heap_free,
    (unsigned)sys_monitor::g_sys_metrics.heap_min_free,
    (unsigned)sys_monitor::g_sys_metrics.heap_largest_block,
    (unsigned)sys_monitor::g_sys_metrics.heap_total,
    (unsigned)degrade,
    sys_monitor::degrade_level_name(degrade),
    (unsigned)sd_h.total_writes,
    (unsigned)sd_h.write_errors,
    (unsigned)sd_h.usage_pct,
    sd_h.space_warning  ? "true" : "false",
    sd_h.space_critical ? "true" : "false",
    (unsigned)sys_monitor::g_sys_metrics.uptime_sec);

  if (len <= 0 || len >= (int)sizeof(buf)) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"buffer overflow\"}");
  }

  return http_send_json(req, buf);
}
#endif

// ────────────────────────────────────────────────────────────────────────────
// GET /api/battery/history — battery health stats
// ────────────────────────────────────────────────────────────────────────────

#if FEATURE_POWER_MONITOR
static esp_err_t handle_battery_history(httpd_req_t* req) {
  g_health.http_requests++;

  PowerState pwr;
  if (!power_monitor::get_state(&pwr)) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"power monitor unavailable\"}");
  }
  PowerHistory hist = {};
  power_monitor::get_history(&hist);

  char buf[640];
  int len = snprintf(buf, sizeof(buf),
    "{"
    "\"ok\":true,"
    "\"voltage_mv\":%u,"
    "\"soc_pct\":%u,"
    "\"charge_state\":\"%s\","
    "\"monitor_mode\":\"%s\","
    "\"battery_present\":%s,"
    "\"divider_detected\":%s,"
    "\"samples_taken\":%u,"
    "\"min_voltage_mv\":%u,"
    "\"max_voltage_mv\":%u,"
    "\"trend_mv_per_min\":%d,"
    "\"charge_cycles\":%u,"
    "\"health_pct\":%u,"
    "\"est_runtime_min\":%u,"
    "\"total_runtime_min\":%u,"
    "\"soc_min_pct\":%u,"
    "\"brownout_count\":%u,"
    "\"uptime_sec\":%u"
    "}",
    (unsigned)pwr.voltage_mv,
    (unsigned)pwr.soc_pct,
    power_monitor::charge_state_name(pwr.charge_state),
    power_monitor::mode_name(pwr.monitor_mode),
    pwr.battery_present ? "true" : "false",
    pwr.divider_detected ? "true" : "false",
    (unsigned)pwr.samples_taken,
    (unsigned)pwr.min_voltage_mv,
    (unsigned)pwr.max_voltage_mv,
    (int)pwr.trend_mv_per_min,
    (unsigned)pwr.charge_cycles,
    (unsigned)power_monitor::health_pct(),
    (unsigned)power_monitor::estimate_runtime_min(),
    (unsigned)hist.total_runtime_min,
    (unsigned)hist.soc_min_pct,
    (unsigned)hist.brownout_count,
    (unsigned)(millis() / 1000));

  if (len <= 0 || len >= (int)sizeof(buf)) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"buffer overflow\"}");
  }

  return http_send_json(req, buf);
}
#endif

// ────────────────────────────────────────────────────────────────────────────
// GET /api/audio/status — PDM mic state, mute info, detection counters
// POST /api/audio/mute  — {"muted":bool}; deferred to the main loop's
//                         audio_process() tick (audio_mute is HTTP-task-safe)
// ────────────────────────────────────────────────────────────────────────────

#if FEATURE_ACOUSTIC_EVENTS
static esp_err_t handle_audio_status(httpd_req_t* req) {
  g_health.http_requests++;

  audio_stats_t stats = {};
  audio_get_stats(&stats);
  audio_mute_info_t mute_info = {};
  audio_get_mute_info(&mute_info);
  uint16_t rms = 0;
  uint32_t rms_age_ms = UINT32_MAX;
  audio_get_live_level(&rms, &rms_age_ms);

  const char* mute_source = "never";
  if (mute_info.age_ms != UINT32_MAX) {
    mute_source = (mute_info.source == AUDIO_MUTE_SOURCE_BOOT) ? "boot" : "dashboard";
  }

  char buf[512];
  int len = snprintf(buf, sizeof(buf),
    "{"
    "\"ok\":true,"
    "\"running\":%s,"
    "\"muted\":%s,"
    "\"mute_source\":\"%s\","
    "\"mute_age_ms\":%u,"
    "\"last_rms\":%u,"
    "\"rms_age_ms\":%u,"
    "\"frames_processed\":%u,"
    "\"t3_detected\":%u,"
    "\"t4_detected\":%u,"
    "\"knock_detected\":%u,"
    "\"doorbell_detected\":%u,"
    "\"glass_break_detected\":%u,"
    "\"i2s_read_errors\":%u,"
    "\"mic_silent\":%s"
    "}",
    audio_is_running() ? "true" : "false",
    audio_is_muted() ? "true" : "false",
    mute_source,
    (unsigned)(mute_info.age_ms == UINT32_MAX ? 0 : mute_info.age_ms),
    (unsigned)rms,
    (unsigned)(rms_age_ms == UINT32_MAX ? 0 : rms_age_ms),
    (unsigned)stats.frames_processed,
    (unsigned)stats.t3_detected,
    (unsigned)stats.t4_detected,
    (unsigned)stats.knock_detected,
    (unsigned)stats.doorbell_detected,
    (unsigned)stats.glass_break_detected,
    (unsigned)stats.i2s_read_errors,
    // Flat-signal watchdog: the driver runs but every 20 ms window for
    // 30+ s computed RMS == 0 — a dead data line, not a quiet room. The
    // dashboard turns this into a "no signal from the microphone" warning
    // instead of a lying LISTENING badge.
    (audio_is_running() &&
     stats.zero_rms_streak >= AUDIO_SILENT_STREAK_FRAMES) ? "true" : "false");

  if (len <= 0 || len >= (int)sizeof(buf)) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"buffer overflow\"}");
  }

  return http_send_json(req, buf);
}

static esp_err_t handle_audio_mute(httpd_req_t* req) {
  g_health.http_requests++;

  char buf[64];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) return http_send_error(req, 400, "invalid_body");
  buf[ret] = '\0';

  JsonDocument body;
  if (deserializeJson(body, buf) != DeserializationError::Ok ||
      !body["muted"].is<bool>()) {
    return http_send_error(req, 400, "invalid_json");
  }

  const bool muted = body["muted"];
  if (!audio_mute(muted, AUDIO_MUTE_SOURCE_HTTP)) {
    return http_send_error(req, 500, "audio_not_initialized");
  }
  // Persist the intent so the next boot honors it via
  // audio_mute_sync_at_boot(). The witness-chain audit record is signed
  // from the mute callback once the toggle is actually applied. The
  // runtime mute still applies if NVS fails, but report it honestly —
  // a "persistent" mute that reverts on reboot would mislead the user.
  const bool persisted = audio_save_mute_intent(muted);
  if (!persisted) {
    log_health(SCV_LOG_WARNING, SCV_CAT_STORAGE,
               "Mic mute intent NOT persisted", "NVS write failed");
  }

  char resp[80];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"muted\":%s,\"persisted\":%s}",
           muted ? "true" : "false", persisted ? "true" : "false");
  return http_send_json(req, resp);
}

// GET /api/audio/selftest — self-test progress/result
// POST /api/audio/selftest — {"action":"start","duration_ms":30000} | {"action":"stop"}
//
// Self-test runs the T3/T4 matchers with relaxed timing tolerance so the
// user can press their smoke/CO alarm's TEST button and watch the device
// confirm it heard the cadence. The normal event callback is suppressed
// while active, so a test press never flows into real alarm automations.
static esp_err_t handle_audio_selftest_get(httpd_req_t* req) {
  g_health.http_requests++;

  audio_selftest_status_t st;
  if (!audio_selftest_status(&st)) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"audio not initialized\"}");
  }

  // peak_rms alone doesn't tell the UI whether the test heard "enough" —
  // ship the live ON threshold alongside so the failure copy can say
  // "too quiet" (0 < peak < on) vs "loud but not an alarm" (peak ≥ on).
  audio_config_t cfg = AUDIO_CONFIG_DEFAULT;
  audio_get_config(&cfg);

  char buf[256];
  int len = snprintf(buf, sizeof(buf),
    "{"
    "\"ok\":true,"
    "\"active\":%s,"
    "\"matched_type\":%u,"
    "\"matched_name\":\"%s\","
    "\"matched_conf\":%u,"
    "\"remaining_ms\":%u,"
    "\"transitions_seen\":%u,"
    "\"peak_rms\":%u,"
    "\"rms_on_threshold\":%u"
    "}",
    st.active ? "true" : "false",
    (unsigned)st.matched_type,
    audio_event_name(st.matched_type),
    (unsigned)st.matched_conf,
    (unsigned)st.remaining_ms,
    (unsigned)st.transitions_seen,
    (unsigned)st.peak_rms,
    (unsigned)cfg.rms_on_threshold);

  if (len <= 0 || len >= (int)sizeof(buf)) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"buffer overflow\"}");
  }

  return http_send_json(req, buf);
}

static esp_err_t handle_audio_selftest_post(httpd_req_t* req) {
  g_health.http_requests++;

  char buf[96];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) return http_send_error(req, 400, "invalid_body");
  buf[ret] = '\0';

  JsonDocument body;
  if (deserializeJson(body, buf) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }
  const char* action = body["action"] | (const char*)nullptr;
  if (!action) return http_send_error(req, 400, "missing_action");

  if (strcmp(action, "start") == 0) {
    const uint32_t duration_ms = body["duration_ms"] | 30000u;
    if (!audio_selftest_start(duration_ms)) {
      // start() refuses while muted or before init — tell the user which.
      return http_send_error(req, 409,
          audio_is_muted() ? "mic_muted" : "audio_not_initialized");
    }
    return http_send_json(req, "{\"ok\":true,\"active\":true}");
  }
  if (strcmp(action, "stop") == 0) {
    audio_selftest_stop();
    return http_send_json(req, "{\"ok\":true,\"active\":false}");
  }
  return http_send_error(req, 400, "unknown_action");
}

// GET /api/audio/config — current envelope thresholds
// POST /api/audio/config — {"rms_on":N,"rms_off":N}; rms_on > rms_off > 0.
//                          Applied immediately and persisted to NVS so the
//                          next boot re-applies it (room-noise sensitivity).
static esp_err_t handle_audio_config_get(httpd_req_t* req) {
  g_health.http_requests++;

  audio_config_t cfg;
  if (!audio_get_config(&cfg)) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"audio not initialized\"}");
  }
  char buf[96];
  const int len = snprintf(buf, sizeof(buf),
    "{\"ok\":true,\"rms_on\":%u,\"rms_off\":%u}",
    (unsigned)cfg.rms_on_threshold, (unsigned)cfg.rms_off_threshold);
  if (len <= 0 || len >= (int)sizeof(buf)) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"buffer overflow\"}");
  }
  return http_send_json(req, buf);
}

static esp_err_t handle_audio_config_post(httpd_req_t* req) {
  g_health.http_requests++;

  char buf[96];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) return http_send_error(req, 400, "invalid_body");
  buf[ret] = '\0';

  JsonDocument body;
  if (deserializeJson(body, buf) != DeserializationError::Ok ||
      !body["rms_on"].is<unsigned int>() || !body["rms_off"].is<unsigned int>()) {
    return http_send_error(req, 400, "invalid_json");
  }
  const unsigned rms_on  = body["rms_on"];
  const unsigned rms_off = body["rms_off"];
  if (rms_on > 0xFFFFu || rms_off > 0xFFFFu ||
      !audio_set_thresholds((uint16_t)rms_on, (uint16_t)rms_off)) {
    return http_send_error(req, 400, "invalid_thresholds");
  }

  bool persisted = false;
  {
    Preferences sens_prefs;
    if (sens_prefs.begin("securacv", false)) {
      persisted = sens_prefs.putUShort("mic_rms_on", (uint16_t)rms_on) > 0 &&
                  sens_prefs.putUShort("mic_rms_off", (uint16_t)rms_off) > 0;
      sens_prefs.end();
    }
  }
  if (!persisted) {
    log_health(SCV_LOG_WARNING, SCV_CAT_STORAGE,
               "Mic sensitivity NOT persisted", "NVS write failed");
  }
  log_health(SCV_LOG_NOTICE, SCV_CAT_USER, "Mic sensitivity changed", nullptr);

  char resp[96];
  const int rl = snprintf(resp, sizeof(resp),
    "{\"ok\":true,\"rms_on\":%u,\"rms_off\":%u,\"persisted\":%s}",
    rms_on, rms_off, persisted ? "true" : "false");
  if (rl <= 0 || rl >= (int)sizeof(resp)) {
    return http_send_json(req, "{\"ok\":true}");
  }
  return http_send_json(req, resp);
}

// GET /api/audio/transitions — recent envelope on/off transitions (newest
// first). Diagnostic surface for the dashboard's "show me the cadence"
// view: lets a user see WHY a test press did or didn't match. Carries
// only {on, age_ms, dur_ms, tone} per entry — same privacy-bounded fields
// the internal matcher uses, no audio content. `tone` is the alarm-band
// ratio ×100 the T3/T4 tone gate checks (≥50 = alarm-band dominant).
static esp_err_t handle_audio_transitions(httpd_req_t* req) {
  g_health.http_requests++;

  audio_transition_t trans[16];
  const size_t n = audio_get_recent_transitions(trans, 16, 0);

  // Explicit remaining-space accounting: every snprintf return value is
  // validated against the space that was actually available BEFORE pos
  // advances, so a truncated (or failed) write can never push pos past
  // the buffer and turn `sizeof(buf) - pos` into an underflowed length.
  char buf[1024];
  size_t pos = 0;
  size_t remaining = sizeof(buf);
  int w = snprintf(buf, remaining,
                   "{\"ok\":true,\"running\":%s,\"transitions\":[",
                   audio_is_running() ? "true" : "false");
  bool fits = (w > 0 && (size_t)w < remaining);
  if (fits) { pos += (size_t)w; remaining -= (size_t)w; }
  for (size_t i = 0; fits && i < n && remaining > 80; i++) {
    w = snprintf(buf + pos, remaining,
                 "%s{\"on\":%u,\"age_ms\":%lu,\"dur_ms\":%lu,\"tone\":%u}",
                 i ? "," : "",
                 (unsigned)trans[i].is_on,
                 (unsigned long)trans[i].age_ms,
                 (unsigned long)trans[i].dur_ms,
                 (unsigned)trans[i].tone_x100);
    fits = (w > 0 && (size_t)w < remaining);
    if (fits) { pos += (size_t)w; remaining -= (size_t)w; }
  }
  if (fits) {
    w = snprintf(buf + pos, remaining, "]}");
    fits = (w > 0 && (size_t)w < remaining);
  }
  if (!fits) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"buffer overflow\"}");
  }
  return http_send_json(req, buf);
}
#endif

static esp_err_t handle_chain(httpd_req_t* req) {
  g_health.http_requests++;

  JsonDocument doc;
  doc["ok"] = true;
  
  char chain_hex[65];
  hex_to_str(chain_hex, g_device.chain_head, 32);
  doc["chain_head"] = chain_hex;
  doc["sequence"] = g_device.seq;
  
  JsonArray blocks = doc["blocks"].to<JsonArray>();
  
  // Add last record info
  if (g_last_record.seq > 0) {
    JsonObject block = blocks.add<JsonObject>();
    char hash[65];
    hex_to_str(hash, g_last_record.chain_hash, 32);
    block["seq"] = g_last_record.seq;
    block["hash"] = hash;
    block["type"] = record_type_name(g_last_record.type);
    block["verified"] = g_last_record.verified;
  }
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_logs(httpd_req_t* req) {
  g_health.http_requests++;
  
  // Check for unacked filter
  char query[64];
  bool unacked_only = false;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (strstr(query, "unacked=true")) {
      unacked_only = true;
    }
  }
  
  JsonDocument doc;
  doc["ok"] = true;
  doc["total"] = g_health_log_ring_count;
  
  JsonArray logs = doc["logs"].to<JsonArray>();
  
  // Iterate through ring buffer (most recent first)
  for (size_t i = 0; i < g_health_log_ring_count; i++) {
    size_t idx = (g_health_log_ring_head + HEALTH_LOG_RING_SIZE - 1 - i) % HEALTH_LOG_RING_SIZE;
    HealthLogRingEntry& entry = g_health_log_ring[idx];
    
    if (unacked_only && entry.ack_status != ACK_STATUS_UNREAD) {
      continue;
    }
    
    JsonObject log = logs.add<JsonObject>();
    log["seq"] = entry.seq;
    log["timestamp_ms"] = entry.timestamp_ms;
    log["level"] = (int)entry.level;
    log["level_name"] = log_level_name(entry.level);
    log["category"] = log_category_name(entry.category);
    log["message"] = entry.message;
    if (entry.detail[0]) {
      log["detail"] = entry.detail;
    }
    log["ack_status"] = ack_status_name(entry.ack_status);
  }
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_log_ack(httpd_req_t* req) {
  g_health.http_requests++;
  
  // Extract sequence number from URI
  const char* uri = req->uri;
  const char* seq_start = strstr(uri, "/logs/");
  if (!seq_start) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
  }
  seq_start += 6;
  uint32_t seq = atoi(seq_start);
  
  // Read body for reason
  char content[128] = {0};
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  
  const char* reason = "";
  if (ret > 0) {
    JsonDocument body;
    if (deserializeJson(body, content) == DeserializationError::Ok) {
      reason = body["reason"] | "";
    }
  }
  
  bool success = acknowledge_log_entry(seq, ACK_STATUS_ACKNOWLEDGED, reason);
  
  JsonDocument doc;
  doc["ok"] = success;
  if (!success) {
    doc["error"] = "Log entry not found";
  }
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_ack_all(httpd_req_t* req) {
  g_health.http_requests++;
  
  uint32_t acked = 0;
  for (size_t i = 0; i < g_health_log_ring_count; i++) {
    HealthLogRingEntry& entry = g_health_log_ring[i];
    if (entry.ack_status == ACK_STATUS_UNREAD) {
      entry.ack_status = ACK_STATUS_ACKNOWLEDGED;
      acked++;
    }
  }
  g_health.logs_unacked = 0;
  
  log_health(SCV_LOG_INFO, SCV_CAT_USER, "Bulk acknowledgment", nullptr);
  
  JsonDocument doc;
  doc["ok"] = true;
  doc["acknowledged"] = acked;
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_witness(httpd_req_t* req) {
  g_health.http_requests++;
  
  JsonDocument doc;
  doc["ok"] = true;
  doc["total"] = g_health.records_created;
  
  JsonArray records = doc["records"].to<JsonArray>();
  
  // Just show last record info for now
  if (g_last_record.seq > 0) {
    JsonObject rec = records.add<JsonObject>();
    rec["seq"] = g_last_record.seq;
    rec["time_bucket"] = g_last_record.time_bucket;
    rec["type"] = (int)g_last_record.type;
    rec["type_name"] = record_type_name(g_last_record.type);
    rec["payload_len"] = g_last_record.payload_len;
    rec["verified"] = g_last_record.verified;
    
    char hash[65];
    hex_to_str(hash, g_last_record.chain_hash, 32);
    rec["chain_hash"] = hash;
  }
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_config_get(httpd_req_t* req) {
  g_health.http_requests++;

  JsonDocument doc;
  doc["ok"] = true;
  // Effective runtime values (NVS-persisted, clamped on load).
  doc["record_interval_ms"] = g_record_interval_ms;
  doc["time_bucket_ms"] = g_time_bucket_ms;
  doc["time_bucket_floor_ms"] = TIME_BUCKET_MS;  // can't be set finer than this (Invariant III)
  doc["gps_coarsen_decimals"] = gps_coarsen_decimals();  // privacy coarsening (Invariant III)
  doc["log_level"] = g_log_min_level;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// NVS keys for the runtime device config (Device tab).
static const char* NVS_KEY_REC_INTERVAL = "rec_int_ms";
static const char* NVS_KEY_TIME_BUCKET  = "tbucket_ms";
static const char* NVS_KEY_LOG_LEVEL    = "log_lvl";

// Load persisted runtime config (called once at boot). Every value is passed
// through config_logic's clamps, so even a corrupted/hostile NVS value can't
// push the device out of its safe envelope — most importantly the time bucket
// is raised to the compile-time floor if smaller (Invariant III).
static void config_load_runtime() {
  g_record_interval_ms = config_logic::clamp_record_interval_ms(
      nvs_load_u32(NVS_KEY_REC_INTERVAL, RECORD_INTERVAL_MS),
      RECORD_INTERVAL_MIN_MS, RECORD_INTERVAL_MAX_MS);
  g_time_bucket_ms = config_logic::clamp_time_bucket_ms(
      nvs_load_u32(NVS_KEY_TIME_BUCKET, TIME_BUCKET_MS), TIME_BUCKET_MS);
  g_log_min_level = config_logic::clamp_log_level(
      nvs_load_u32(NVS_KEY_LOG_LEVEL, SCV_LOG_INFO), LOG_LEVEL_STORE_MAX);
}

// POST /api/config — persist the three Device-tab settings. Each field is
// optional; only provided fields change. All values are clamped before use
// AND before persistence, so NVS never holds an out-of-envelope value. The
// time bucket is clamped to at least its compile-time floor — event timing is
// never finer than the Invariant III minimum (5000 ms). The operator owns the
// device and may retune it above that floor in either direction (Invariant
// IV/sovereignty); the floor is the privacy guarantee, not a one-way ratchet.
static esp_err_t handle_config_post(httpd_req_t* req) {
  g_health.http_requests++;

  char content[256] = {0};
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  JsonDocument body;
  if (ret <= 0 || deserializeJson(body, content) != DeserializationError::Ok) {
    httpd_resp_set_status(req, "400 Bad Request");
    return http_send_json(req, "{\"ok\":false,\"error\":\"Invalid JSON\"}");
  }

  // Only persist a field when its clamped value actually differs from the
  // current setting — re-saving an unchanged config shouldn't burn a flash
  // write cycle. (A null/missing field fails is<uint32_t>() and is skipped.)
  bool clamped = false;
  if (body["record_interval_ms"].is<uint32_t>()) {
    uint32_t req_v = body["record_interval_ms"].as<uint32_t>();
    uint32_t v = config_logic::clamp_record_interval_ms(
        req_v, RECORD_INTERVAL_MIN_MS, RECORD_INTERVAL_MAX_MS);
    clamped = clamped || (v != req_v);
    if (v != g_record_interval_ms) {
      g_record_interval_ms = v;
      nvs_store_u32(NVS_KEY_REC_INTERVAL, v);
    }
  }
  if (body["time_bucket_ms"].is<uint32_t>()) {
    uint32_t req_v = body["time_bucket_ms"].as<uint32_t>();
    uint32_t v = config_logic::clamp_time_bucket_ms(req_v, TIME_BUCKET_MS);
    clamped = clamped || (v != req_v);
    if (v != g_time_bucket_ms) {
      g_time_bucket_ms = v;
      nvs_store_u32(NVS_KEY_TIME_BUCKET, v);
    }
  }
  if (body["log_level"].is<uint32_t>()) {
    uint32_t req_v = body["log_level"].as<uint32_t>();
    uint8_t v = config_logic::clamp_log_level(req_v, LOG_LEVEL_STORE_MAX);
    clamped = clamped || (v != req_v);
    if (v != g_log_min_level) {
      g_log_min_level = v;
      nvs_store_u32(NVS_KEY_LOG_LEVEL, v);
    }
  }

  log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM, "Device config updated", nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["clamped"] = clamped;  // true if a value was adjusted to its safe range
  doc["record_interval_ms"] = g_record_interval_ms;
  doc["time_bucket_ms"] = g_time_bucket_ms;
  doc["time_bucket_floor_ms"] = TIME_BUCKET_MS;
  doc["log_level"] = g_log_min_level;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_export(httpd_req_t* req) {
  g_health.http_requests++;

  // Check SD card availability using hardware state (non-blocking)
  if (!sd_is_available()) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "SD card not available";
    doc["sd_state"] = sd_state_name(g_hw.sd_state);
    doc["sd_available"] = false;
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  // Verify SD card is still present before proceeding
  if (!sd_verify_present()) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "SD card was removed during operation";
    doc["sd_state"] = sd_state_name(g_hw.sd_state);
    doc["sd_available"] = false;
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  // Create export bundle
  char export_path[64];
  // Root-level /EXPORT: the SD library already roots paths at the card, so
  // the old "/sd/EXPORT" spelling addressed a literal "sd" subdirectory
  // that nothing ever created — every bundle write failed on a card
  // provisioned with the /EXPORT dir the mount path mkdirs.
  snprintf(export_path, sizeof(export_path), "/EXPORT/bundle_%u.json", (unsigned)millis());

  File file = SD.open(export_path, FILE_WRITE);
  if (!file) {
    // SD operation failed - mark as error
    sd_op_failure();
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "Failed to create export file";
    doc["sd_state"] = sd_state_name(g_hw.sd_state);
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  // Write export header
  JsonDocument header;
  header["version"] = PROTOCOL_VERSION;
  header["device_id"] = g_device.device_id;
  header["firmware"] = FIRMWARE_VERSION;
  header["ruleset"] = RULESET_ID;
  header["export_time_ms"] = millis();
  header["chain_seq"] = g_device.seq;
  header["records_total"] = g_health.records_created;

  char pubkey_hex[65];
  hex_to_str(pubkey_hex, g_device.pubkey, 32);
  header["pubkey"] = pubkey_hex;

  serializeJson(header, file);
  file.close();

  // Mark successful SD operation
  sd_op_success();

  log_health(SCV_LOG_INFO, SCV_CAT_USER, "Export created", export_path);

  JsonDocument doc;
  doc["ok"] = true;
  doc["download_url"] = String("/api/download?path=") + export_path;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_reboot(httpd_req_t* req) {
  g_health.http_requests++;
  
  log_health(SCV_LOG_NOTICE, SCV_CAT_USER, "Reboot requested", nullptr);
  
  // Persist state
  nvs_store_u32(NVS_KEY_SEQ, g_device.seq);
  nvs_store_bytes(NVS_KEY_CHAIN, g_device.chain_head, 32);
  
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Rebooting...";
  
  String response;
  serializeJson(doc, response);
  http_send_json(req, response.c_str());
  
  pre_reboot_fn hook = __atomic_load_n(&g_pre_reboot_hook, __ATOMIC_ACQUIRE);
  if (hook) hook();
  delay(500);
  ESP.restart();
  return ESP_OK;
}

// Manual escape from a maxed-out safe mode: reset the recovery budget and
// reboot into full operation. The dashboard surfaces this once the device
// has exhausted its automatic recovery attempts.
static esp_err_t handle_safe_mode_retry(httpd_req_t* req) {
  g_health.http_requests++;

  log_health(SCV_LOG_NOTICE, SCV_CAT_USER, "Safe mode retry requested", nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Resetting recovery budget and rebooting...";

  String response;
  serializeJson(doc, response);
  http_send_json(req, response.c_str());

  safe_mode_force_retry();  // resets recov_count + clears safe mode, then ESP.restart()
  return ESP_OK;
}

// ════════════════════════════════════════════════════════════════════════════
// CAMERA PEEK (Live Preview for Setup) — FIXED VERSION
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_CAMERA_PEEK

// Apply Canary surveillance-tuned defaults to the OV2640 / OV3660 sensor.
// These bias the sensor for low-light indoor scenes (typical Canary placement)
// rather than the factory defaults which assume bright daylight. All values
// are documented in the ESP32 esp_camera sensor.h header.
//
// Safe to call multiple times — every setter is idempotent on the sensor side.
static void apply_default_sensor_tuning() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;

  // Image tuning (range −2..2 for these three) — neutral defaults.
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);

  // No special effect (0=none, 1=negative, 2=grayscale, 3=red, 4=green, 5=blue, 6=sepia)
  s->set_special_effect(s, 0);

  // White balance: enable AWB + AWB gain, automatic mode.
  s->set_whitebal(s, 1);          // Enable WB
  s->set_awb_gain(s, 1);          // Enable AWB gain
  s->set_wb_mode(s, 0);           // 0=auto, 1=sunny, 2=cloudy, 3=office, 4=home

  // Auto exposure: enable AEC + AEC2 with neutral level. Manual aec_value
  // ignored when set_aec is enabled.
  s->set_exposure_ctrl(s, 1);     // AEC on
  s->set_aec2(s, 1);              // AEC DSP-side on (smoother low-light)
  s->set_ae_level(s, 0);          // Bias 0 (range −2..2)
  s->set_aec_value(s, 300);       // Manual fallback (0..1200), only used if AEC off

  // Auto gain: enabled, ceiling raised to GAINCEILING_4X for better low-light.
  s->set_gain_ctrl(s, 1);         // AGC on
  s->set_agc_gain(s, 0);          // Manual fallback (0..30)
  s->set_gainceiling(s, (gainceiling_t)GAINCEILING_4X);

  // Image cleanup: black/white pixel correction, gamma, lens correction, DCW.
  s->set_bpc(s, 1);               // Black pixel correction
  s->set_wpc(s, 1);               // White pixel correction
  s->set_raw_gma(s, 1);           // Gamma correction
  s->set_lenc(s, 1);              // Lens shading correction
  s->set_dcw(s, 1);               // Downsize EN (better quality scaling)

  // Orientation — Canary's enclosure mounts the sensor right-side up by
  // default. Operators can flip via the sensor API at runtime.
  s->set_hmirror(s, 0);
  s->set_vflip(s, 0);

  // Test pattern off.
  s->set_colorbar(s, 0);

  // Sensor-specific corrections come LAST so they win over the generic
  // defaults above. The OV3660 sits upside-down on the XIAO Sense module
  // and ships oversaturated; the upstream Espressif demo applies these
  // exact tweaks, and operators can still override them at runtime via
  // /api/peek/sensor.
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
}

// Single attempt at esp_camera_init with the supplied config. Returns ESP_OK
// or the error code so the caller can decide whether to retry.
// ── Sensor whoami (bench triage) ─────────────────────────────────────────
// Runs only after EVERY init attempt failed with the driver's anonymous
// "Detected camera not supported" (0x106). That error hides the one datum
// that matters: WHICH ID the probe saw. We re-ask the sensor directly:
// drive XCLK ourselves (a sensor only answers SCCB while clocked), then
// read the product ID over I2C at the two OmniVision addresses. Outcomes:
//   - PID 0x26xx at 0x30  -> OV2640 (supported; failure is elsewhere)
//   - 0x3660 / 0x5640 at 0x3C -> newer Sense camera module batch
//   - garbage / no ACK    -> the snap-on board or ribbon isn't seated
#include <Wire.h>
#include <driver/ledc.h>
static void camera_whoami_after_failure() {
  ledc_timer_config_t t = {};
  t.speed_mode      = LEDC_LOW_SPEED_MODE;
  t.timer_num       = LEDC_TIMER_3;      // top of the range: clear of the
  t.duty_resolution = LEDC_TIMER_1_BIT;  // core's auto-assigned channels
  t.freq_hz         = 20000000;
  t.clk_cfg         = LEDC_AUTO_CLK;
  ledc_channel_config_t c = {};
  c.gpio_num   = CAM_PIN_XCLK;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel    = LEDC_CHANNEL_7;
  c.timer_sel  = LEDC_TIMER_3;
  c.duty       = 1;                      // 50% at 1-bit resolution
  if (ledc_timer_config(&t) != ESP_OK || ledc_channel_config(&c) != ESP_OK) {
    Serial.println("[CAMERA] whoami: couldn't start probe clock — skipping");
    return;
  }
  delay(20);
  Wire1.begin((int)CAM_PIN_SIOD, (int)CAM_PIN_SIOC, 100000);
  auto ack = [](uint8_t a) {
    Wire1.beginTransmission(a);
    return Wire1.endTransmission() == 0;
  };
  auto rd8 = [](uint8_t a, uint8_t reg, uint8_t* out) {
    Wire1.beginTransmission(a);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom((int)a, 1) != 1) return false;
    *out = (uint8_t)Wire1.read();
    return true;
  };
  auto rd16 = [](uint8_t a, uint16_t reg, uint8_t* out) {
    Wire1.beginTransmission(a);
    Wire1.write((uint8_t)(reg >> 8));
    Wire1.write((uint8_t)(reg & 0xFF));
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom((int)a, 1) != 1) return false;
    *out = (uint8_t)Wire1.read();
    return true;
  };

  if (ack(0x30)) {
    // OV2640 family: select the sensor register bank, read PID/VER.
    Wire1.beginTransmission(0x30);
    Wire1.write(0xFF);
    Wire1.write(0x01);
    Wire1.endTransmission();
    uint8_t pid = 0, ver = 0;
    if (rd8(0x30, 0x0A, &pid) && rd8(0x30, 0x0B, &ver)) {
      Serial.printf("[CAMERA] whoami: sensor at 0x30 reports PID 0x%02X%02X\n", pid, ver);
      if (pid == 0x26) {
        Serial.println("[CAMERA] whoami: that IS an OV2640 — the sensor is fine and");
        Serial.println("[CAMERA] whoami: seated; the failure is in the init config path.");
      } else {
        Serial.println("[CAMERA] whoami: unfamiliar ID at the OV2640 address — likely a");
        Serial.println("[CAMERA] whoami: clone module; send this PID upstream.");
      }
    } else {
      Serial.println("[CAMERA] whoami: 0x30 ACKed but register read failed — marginal");
      Serial.println("[CAMERA] whoami: connection; re-seat the camera board/ribbon.");
    }
  } else if (ack(0x3C)) {
    uint8_t idh = 0, idl = 0;
    if (rd16(0x3C, 0x300A, &idh) && rd16(0x3C, 0x300B, &idl)) {
      const uint16_t id = ((uint16_t)idh << 8) | idl;
      Serial.printf("[CAMERA] whoami: sensor at 0x3C reports chip ID 0x%04X\n", id);
      if (id == 0x3660) {
        Serial.println("[CAMERA] whoami: that's an OV3660 — a newer Sense camera batch.");
        Serial.println("[CAMERA] whoami: this build's camera library rejected it; report");
        Serial.println("[CAMERA] whoami: this line so OV3660 support gets enabled.");
      } else if (id == 0x5640) {
        Serial.println("[CAMERA] whoami: that's an OV5640 — report this line so OV5640");
        Serial.println("[CAMERA] whoami: support gets enabled in the build.");
      }
    } else {
      Serial.println("[CAMERA] whoami: 0x3C ACKed but ID read failed — re-seat the");
      Serial.println("[CAMERA] whoami: camera board/ribbon and retry.");
    }
  } else {
    Serial.println("[CAMERA] whoami: NO sensor answered on the camera bus.");
    Serial.println("[CAMERA] whoami: power off, pop the Sense expansion board off and");
    Serial.println("[CAMERA] whoami: back on firmly, and check the ribbon latch.");
  }
  Wire1.end();
  ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 0);
  // Release every pin we borrowed (review catch): /api/peek/init can retry
  // the real camera driver later on these exact pins, and a half-released
  // I2C matrix routing would sabotage that retry.
  gpio_reset_pin((gpio_num_t)CAM_PIN_XCLK);
  gpio_reset_pin((gpio_num_t)CAM_PIN_SIOD);
  gpio_reset_pin((gpio_num_t)CAM_PIN_SIOC);
}

static esp_err_t try_camera_init(const camera_config_t& cfg, const char* label) {
  esp_err_t err = esp_camera_init(&cfg);
  if (err == ESP_OK) {
    Serial.printf("[CAMERA] Init OK (%s) — frame=%d quality=%d fb=%d %s\n",
                  label,
                  (int)cfg.frame_size,
                  cfg.jpeg_quality,
                  cfg.fb_count,
                  cfg.fb_location == CAMERA_FB_IN_PSRAM ? "psram" : "dram");
  } else {
    Serial.printf("[CAMERA] Init FAILED (%s) — err=0x%x frame=%d %s\n",
                  label,
                  err,
                  (int)cfg.frame_size,
                  cfg.fb_location == CAMERA_FB_IN_PSRAM ? "psram" : "dram");
  }
  return err;
}

// Build a camera_config_t with all pin/clock fields populated. Caller fills
// in frame_size / jpeg_quality / fb_count / fb_location / grab_mode for the
// specific attempt. Critically: zero-initialized so newer ESP-IDF fields
// (sccb_i2c_port, etc.) start clean instead of inheriting stack garbage —
// uninitialized sccb_i2c_port was the most common cause of "init returns
// ESP_OK but sensor probes fail / framebuffer count is 0" on XIAO ESP32S3
// Sense boards built against Arduino-ESP32 v3 / IDF v5.
static camera_config_t make_base_camera_config() {
  camera_config_t config = {};       // zero everything (incl. sccb_i2c_port)
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = CAM_PIN_D0;
  config.pin_d1       = CAM_PIN_D1;
  config.pin_d2       = CAM_PIN_D2;
  config.pin_d3       = CAM_PIN_D3;
  config.pin_d4       = CAM_PIN_D4;
  config.pin_d5       = CAM_PIN_D5;
  config.pin_d6       = CAM_PIN_D6;
  config.pin_d7       = CAM_PIN_D7;
  config.pin_xclk     = CAM_PIN_XCLK;
  config.pin_pclk     = CAM_PIN_PCLK;
  config.pin_vsync    = CAM_PIN_VSYNC;
  config.pin_href     = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn     = CAM_PIN_PWDN;
  config.pin_reset    = CAM_PIN_RESET;
  config.sccb_i2c_port = -1;         // -1 = let driver pick a free I2C port
  config.xclk_freq_hz = 20000000;    // 20 MHz — Seeed/Espressif recommended
  config.pixel_format = PIXFORMAT_JPEG;
  return config;
}

static bool init_camera() {
  // Some flashing routes (Arduino IDE FQBN without PSRAM=opi, or stock board
  // variant) leave OPI PSRAM uninitialized. psramInit() is a no-op when the
  // ROM has already enabled PSRAM, but turns it on otherwise so psramFound()
  // reports correctly downstream.
  psramInit();

  g_peek_init_count++;

  // If a previous init attempt got partway through, the SCCB state machine
  // and the LEDC channel may still be claimed. Always start cold so a runtime
  // /api/peek/init retry behaves the same as a fresh boot init.
  esp_camera_deinit();

  bool psram_ok = psramFound();
  size_t psram_total = psram_ok ? ESP.getPsramSize() : 0;
  g_peek_psram_found = psram_ok;
  Serial.printf("[CAMERA] PSRAM: %s (%u bytes), free heap=%u, largest DMA block=%u\n",
                psram_ok ? "found" : "not found",
                (unsigned)psram_total,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT));

  // The XIAO ESP32-S3 *Sense* ships with 8MB OPI PSRAM. If psramFound() comes
  // back false we are almost certainly running an Arduino-IDE FQBN that did
  // not enable PSRAM at compile time — psramInit() cannot rescue this since
  // the SDK's address space layout is decided at boot. Surface the most
  // common operator mistake explicitly so the user can fix it without
  // chasing kernel logs.
  if (!psram_ok) {
    Serial.println("[CAMERA] *** PSRAM not detected. On XIAO ESP32-S3 Sense this");
    Serial.println("[CAMERA] *** almost always means the sketch was flashed without");
    Serial.println("[CAMERA] *** PSRAM enabled. In Arduino IDE: Tools > PSRAM > 'OPI PSRAM'.");
    Serial.println("[CAMERA] *** Falling back to QVGA-in-DRAM (still streams, just lower res).");
  }

  // Attempt order is most-capable → most-conservative. Each attempt that
  // returns non-OK is followed by esp_camera_deinit() to fully reset the
  // sensor before the next try (otherwise OV2640's I²C state machine can
  // wedge after a failed probe).
  camera_config_t attempts[3];
  const char*     labels[3];
  int n_attempts = 0;

  if (psram_ok) {
    // PSRAM fast path: 1024×768 with double-buffered LATEST grab. This is
    // the highest-quality config that still hits the 25 fps target on a
    // XIAO Sense (8MB OPI PSRAM @ 80 MHz).
    camera_config_t cfg = make_base_camera_config();
    cfg.frame_size  = FRAMESIZE_XGA;       // 1024×768
    cfg.jpeg_quality = 10;                 // 0..63 (lower=better)
    cfg.fb_count    = 2;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode   = CAMERA_GRAB_LATEST;
    attempts[n_attempts] = cfg;
    labels[n_attempts]   = "psram-xga";
    n_attempts++;

    // PSRAM safe path: VGA still uses PSRAM but a single buffer (lower
    // bandwidth) — survives if SPIRAM_SPEED_80M was tried but the chip
    // can't sustain it.
    camera_config_t cfg2 = make_base_camera_config();
    cfg2.frame_size  = FRAMESIZE_VGA;      // 640×480
    cfg2.jpeg_quality = 12;
    cfg2.fb_count    = 1;
    cfg2.fb_location = CAMERA_FB_IN_PSRAM;
    cfg2.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
    attempts[n_attempts] = cfg2;
    labels[n_attempts]   = "psram-vga";
    n_attempts++;
  }

  // No-PSRAM / last-resort: QVGA in DRAM. The OV2640 still drives JPEG fine
  // here, just at a lower resolution. ESP32-S3 has enough free DRAM after
  // Wi-Fi to allocate a single 320×240 JPEG framebuffer.
  camera_config_t cfg3 = make_base_camera_config();
  cfg3.frame_size  = FRAMESIZE_QVGA;
  cfg3.jpeg_quality = 12;
  cfg3.fb_count    = 1;
  cfg3.fb_location = CAMERA_FB_IN_DRAM;
  cfg3.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
  attempts[n_attempts] = cfg3;
  labels[n_attempts]   = "dram-qvga";
  n_attempts++;

  esp_err_t last_err = ESP_FAIL;
  int       chosen   = -1;
  for (int i = 0; i < n_attempts; i++) {
    last_err = try_camera_init(attempts[i], labels[i]);
    g_peek_last_init_err = (int)last_err;
    strlcpy(g_peek_last_init_label, labels[i], sizeof(g_peek_last_init_label));
    if (last_err == ESP_OK) { chosen = i; break; }
    // Driver may be half-initialized after a failed probe; reset before retry.
    esp_camera_deinit();
    delay(100);
  }

  if (chosen < 0) {
    Serial.printf("[CAMERA] All init attempts failed — last err=0x%x (%s)\n",
                  last_err, esp_err_to_name(last_err));
    camera_whoami_after_failure();
    return false;
  }
  g_peek_last_init_err = 0;

  const camera_config_t& winning = attempts[chosen];
  g_peek_framesize    = winning.frame_size;
  g_peek_jpeg_quality = winning.jpeg_quality;
  g_peek_xclk_freq_hz = winning.xclk_freq_hz;
  g_peek_psram_used   = (winning.fb_location == CAMERA_FB_IN_PSRAM);
  g_peek_fb_count     = winning.fb_count;

  // Capture real sensor identity (PID identifies OV2640/OV5640/etc.)
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    g_peek_sensor_pid  = s->id.PID;
    g_peek_sensor_ver  = s->id.VER;
    g_peek_sensor_midh = s->id.MIDH;
    g_peek_sensor_midl = s->id.MIDL;
  }

  // Apply surveillance-friendly defaults (AWB/AEC/AGC on, LENC, DCW, etc.)
  apply_default_sensor_tuning();

  Serial.printf("[CAMERA] Initialized — sensor PID=0x%02X quality=%d xclk=%dHz psram=%s fb_count=%u\n",
                g_peek_sensor_pid, g_peek_jpeg_quality, g_peek_xclk_freq_hz,
                g_peek_psram_used ? "yes" : "no",
                (unsigned)g_peek_fb_count);
  return true;
}

// Map sensor PID to a human-readable model name (real values from omnivision sensor.h)
static const char* sensor_model_name(uint16_t pid) {
  switch (pid) {
    case 0x26: return "OV2640";
    case 0x56: return "OV5640";
    case 0x77: return "OV7670";
    case 0x73: return "OV7725";
    case 0x96: return "OV9650";
    case 0x99: return "OV9655";
    case 0x30: return "OV3660";
    case 0x40: return "GC0308";
    case 0x55: return "GC2145";
    case 0x80: return "BF3005";
    case 0xDA: return "NT99141";
    case 0x9B: return "SC101IOT";
    case 0xCC: return "SC030IOT";
    case 0xE0: return "SC031GS";
    default:   return "unknown";
  }
}

// Set camera resolution
static bool set_camera_resolution(framesize_t size) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;
  
  if (s->set_framesize(s, size) != 0) {
    return false;
  }
  
  g_peek_framesize = size;
  return true;
}

// Get resolution name
static const char* framesize_name(framesize_t size) {
  switch (size) {
    case FRAMESIZE_QQVGA:   return "160x120";
    case FRAMESIZE_240X240: return "240x240";
    case FRAMESIZE_QVGA:    return "320x240";
    case FRAMESIZE_CIF:   return "400x296";
    case FRAMESIZE_VGA:   return "640x480";
    case FRAMESIZE_SVGA:  return "800x600";
    case FRAMESIZE_XGA:   return "1024x768";
    case FRAMESIZE_HD:    return "1280x720";
    case FRAMESIZE_SXGA:  return "1280x1024";
    case FRAMESIZE_UXGA:  return "1600x1200";
    default: return "unknown";
  }
}

// ════════════════════════════════════════════════════════════════════════════
// CAMERA POWER MANAGER — idle standby, on-demand wake, peek gating
// ════════════════════════════════════════════════════════════════════════════
//
// The Sense's OV2640 has no wired PWDN pin, so "standby" means
// esp_camera_deinit(): the 20 MHz XCLK stops, the framebuffers are freed,
// idle current and heat drop. "Wake" re-runs the boot init ladder (~1 s).
// Decisions are pure and host-tested (camera_gate_logic.h); this block is
// only the glue. PEEK yields to the battery policy and to thermal
// protection; VAULT captures (life-safety) and QR provisioning (explicit
// user action) always may wake the sensor.

static volatile bool g_cam_standby = false;   // parked by choice, not failure
static uint32_t g_cam_last_use_ms = 0;
static bool g_cam_shed_latch = false;         // one log line per shed streak

// Function-local static: C++11 guards make the first-call init race-free,
// so every task (loop / httpd / seal worker) can share the mutex safely.
static SemaphoreHandle_t cam_lock() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

static void camera_note_use() { g_cam_last_use_ms = millis(); }

// True when the sensor is up OR only down because the manager parked it
// (a wake brings it back). False only after a genuine init failure.
static bool camera_usable() { return g_camera_initialized || g_cam_standby; }

static bool camera_ensure_awake() {
  if (xSemaphoreTake(cam_lock(), pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = g_camera_initialized;
  if (!ok) {
    const bool was_standby = g_cam_standby;
    ok = init_camera();
    g_camera_initialized  = ok;
    g_hw.camera_available = ok;
    if (ok) {
      g_hw.camera_ever_init = true;
      if (was_standby)
        log_health(SCV_LOG_INFO, SCV_CAT_SENSOR, "Camera woke from standby",
                   nullptr);
    }
    g_cam_standby = false;  // either awake now, or genuinely failed
    g_hw.camera_standby = false;
  }
  if (ok) camera_note_use();
  xSemaphoreGive(cam_lock());
  return ok;
}

static void camera_enter_standby(const char* reason) {
  if (xSemaphoreTake(cam_lock(), pdMS_TO_TICKS(1000)) != pdTRUE) return;
  bool busy = g_peek_active;
  #if FEATURE_QR_PROVISION
  busy = busy || g_qr_scan_active;
  #endif
  #if FEATURE_VAULT_SNAPSHOT
  busy = busy || vault_snapshot::worker_busy();
  #endif
  if (g_camera_initialized && !busy) {
    esp_camera_deinit();
    g_camera_initialized  = false;
    g_hw.camera_available = false;
    g_cam_standby = true;
    g_hw.camera_standby = true;
    log_health(SCV_LOG_INFO, SCV_CAT_SENSOR, "Camera standby", reason);
  }
  xSemaphoreGive(cam_lock());
}

// Battery policy verdict for the PEEK surface only (vault/QR are never
// policy-gated). True when no policy is compiled in or none initialized.
static bool camera_policy_allows_peek() {
  #if FEATURE_POWER_POLICY
  const PolicyFeatures* pf = power_policy::get_features();
  if (pf != nullptr && !pf->camera_peek) return false;
  #endif
  return true;
}

static camera_gate::PeekGate peek_gate_now() {
  const bool hot_crit =
      (sys_monitor::get_temp_state() == sys_monitor::TEMP_HOT_CRIT);
  return camera_gate::peek_gate(camera_usable(), camera_policy_allows_peek(),
                                hot_crit);
}

// Send the gate's honest 503. Returns true when the caller must bail
// (a response has been sent).
static bool peek_gate_refuse(httpd_req_t* req, camera_gate::PeekGate g) {
  if (g == camera_gate::PeekGate::ALLOW) return false;
  httpd_resp_set_status(req, "503 Service Unavailable");
  char resp[160];
  snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}",
           camera_gate::peek_gate_reason(g));
  http_send_json(req, resp);
  return true;
}

// Loop-task tick: shed the stream when critically hot or when the battery
// policy turned the peek surface off, then let the idle timer (or the
// policy) park the sensor. Cheap enough to run every loop pass.
static void camera_power_tick() {
  const bool hot_crit =
      (sys_monitor::get_temp_state() == sys_monitor::TEMP_HOT_CRIT);
  const bool policy_ok = camera_policy_allows_peek();

  if (g_peek_active && (hot_crit || !policy_ok)) {
    if (!g_cam_shed_latch) {
      g_cam_shed_latch = true;
      log_health(SCV_LOG_WARNING, SCV_CAT_SENSOR,
                 hot_crit ? "Peek stream stopped: device too hot"
                          : "Peek stream stopped: battery saver",
                 nullptr);
    }
    g_peek_active = false;  // the stream worker exits on its next frame
  }
  if (!hot_crit && policy_ok) g_cam_shed_latch = false;

  bool in_use = g_peek_active;
  #if FEATURE_QR_PROVISION
  in_use = in_use || g_qr_scan_active;
  #endif
  #if FEATURE_VAULT_SNAPSHOT
  in_use = in_use || vault_snapshot::worker_busy();
  #endif

  if (camera_gate::standby_due(millis(), g_cam_last_use_ms,
                               g_camera_initialized, in_use,
                               policy_ok && !hot_crit)) {
    camera_enter_standby(hot_crit ? "too hot"
                                  : (policy_ok ? "idle" : "battery saver"));
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK START — NEW ENDPOINT (POST /api/peek/start)
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_start(httpd_req_t* req) {
  g_health.http_requests++;

  if (peek_gate_refuse(req, peek_gate_now())) return ESP_OK;
  if (!camera_ensure_awake()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    const char* resp = "{\"ok\":false,\"error\":\"Camera not initialized\"}";
    return http_send_json(req, resp);
  }

  // g_peek_active is owned by the stream worker lifecycle now: the stream
  // handler sets it when a client connects and the worker clears it on exit.
  // Flipping it here without a worker would make /api/peek/status report a
  // phantom "active" stream, so this endpoint just confirms readiness.
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "Peek ready", nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Camera ready; open /api/peek/stream to start";
  doc["peek_active"] = g_peek_active;
  doc["resolution"] = framesize_name(g_peek_framesize);
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK STREAM — async worker task
//
// esp_http_server runs a single task, so a long-lived MJPEG loop inside the
// handler used to hold the only worker hostage: /api/peek/status polls, the
// sensor sliders, and every other tab froze for as long as the preview ran.
// The handler now detaches the request with httpd_req_async_handler_begin()
// and hands it to a dedicated FreeRTOS task, freeing the httpd task
// immediately. Rules the worker lives by:
//   - priority 3 with an unconditional vTaskDelay(>=20ms) per loop iteration
//     (every path yields), so the WDT-subscribed IDLE tasks always run
//   - 8 KB internal-RAM stack (task stacks can't live in PSRAM)
//   - it is the only writer that CLEARS g_peek_stream_task_busy, and it does
//     so only after httpd_req_async_handler_complete() releases the socket
//   - exactly one stream at a time; a second client gets 409 Conflict
// ════════════════════════════════════════════════════════════════════════════

static void peek_stream_task(void* arg) {
  httpd_req_t* req = (httpd_req_t*)arg;

  // ── TCP keepalive on the streaming socket ─────────────────────────
  // The MJPEG response is open-ended, so a vanished client (laptop lid
  // closed, browser killed, WiFi roam) is invisible to the application
  // until the next chunk send fails. SO_KEEPALIVE lets LwIP detect a
  // dead peer in ~20 s (5 s idle + 3×5 s probes) and fail the next send
  // with ECONNRESET, breaking us out of the loop cleanly instead of
  // letting the worker spin on a zombie socket.
  int sockfd = httpd_req_to_sockfd(req);
  if (sockfd >= 0) {
    int yes = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    int idle = 5;     // start probing after 5 s of silence
    int intvl = 5;    // 5 s between probes
    int cnt = 3;      // 3 lost probes -> dead
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
  }

  // Set proper MJPEG multipart headers
  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, pre-check=0, post-check=0, max-age=0");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

  // Stream frames while active
  uint32_t consecutive_capture_failures = 0;
  while (g_peek_active) {
    camera_note_use();  // streaming counts as use for the idle timer
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[PEEK] Frame capture failed");
      // A camera that died mid-stream (unseated connector, driver wedge)
      // would otherwise spin this loop forever without ever touching the
      // socket — so a vanished client is never noticed and the busy flag
      // blocks every new stream until reboot. Bail after ~1 s of failures.
      if (peek_stream_logic::capture_should_abort(++consecutive_capture_failures)) {
        Serial.println("[PEEK] Aborting stream after repeated capture failures");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;  // Transient failure: try again instead of breaking
    }
    consecutive_capture_failures = 0;

    // Build multipart boundary + headers
    char part_buf[128];
    int part_len = snprintf(
      part_buf, sizeof(part_buf),
      "--frame\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: %u\r\n"
      "\r\n",
      (unsigned)fb->len
    );

    // Send boundary + headers
    esp_err_t res = httpd_resp_send_chunk(req, part_buf, part_len);
    if (res != ESP_OK) {
      esp_camera_fb_return(fb);
      break;
    }

    // Send JPEG data
    res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    if (res != ESP_OK) {
      esp_camera_fb_return(fb);
      break;
    }

    // Capture real metrics for this delivered frame BEFORE returning the buffer
    uint32_t frame_bytes = (uint32_t)fb->len;

    // Send trailing CRLF
    res = httpd_resp_send_chunk(req, "\r\n", 2);
    esp_camera_fb_return(fb);

    if (res == ESP_OK) {
      uint32_t t_ms = millis();
      portENTER_CRITICAL(&g_peek_metrics_mux);
      g_peek_last_frame_bytes = frame_bytes;
      g_peek_last_frame_ms    = t_ms;
      g_peek_total_bytes     += frame_bytes;
      g_peek_frame_count++;
      g_peek_fps_window_count++;
      // Close the FPS window once it spans >=1000ms. Normalize by the actual
      // elapsed time so jitter (slow capture, congestion) doesn't inflate the
      // reported rate above the true delivery rate.
      uint32_t window_elapsed = t_ms - g_peek_fps_window_start;
      if (window_elapsed >= 1000) {
        g_peek_fps_last         = (g_peek_fps_window_count * 1000U) / window_elapsed;
        g_peek_fps_window_count = 0;
        g_peek_fps_window_start = t_ms;
      }
      portEXIT_CRITICAL(&g_peek_metrics_mux);
    }

    if (res != ESP_OK) {
      break;
    }

    // Yield. Pacing is configurable at runtime via /api/peek/sensor
    // (frame_delay_ms). Default 40 ms ≈ 25 fps target; OV2640 will deliver
    // fewer in low light because AEC stretches the exposure window — that's
    // accurately reflected in the measured FPS. This task is not subscribed
    // to the TWDT (no esp_task_wdt_reset here); the delay is what keeps the
    // WDT-subscribed IDLE tasks fed.
    vTaskDelay(pdMS_TO_TICKS(peek_stream_logic::pace_clamp_ms(g_peek_frame_delay_ms)));
  }

  g_peek_active = false;

  // Freeze the uptime clock so /api/peek/status can keep reporting the real
  // duration of the finished stream ("LAST STREAM" stats) instead of zero.
  portENTER_CRITICAL(&g_peek_metrics_mux);
  g_peek_stream_end_ms = millis();
  portEXIT_CRITICAL(&g_peek_metrics_mux);

  // End chunked response, then release the socket back to the server.
  httpd_resp_send_chunk(req, NULL, 0);
  httpd_req_async_handler_complete(req);

  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "Peek stream ended", nullptr);

  // Clear last: handlers wait on this flag before touching the camera driver,
  // so it must stay set until the request is fully released above.
  __atomic_store_n(&g_peek_stream_task_busy, false, __ATOMIC_RELEASE);
  vTaskDelete(NULL);
}

static esp_err_t handle_peek_stream(httpd_req_t* req) {
  g_health.http_requests++;

  if (peek_gate_refuse(req, peek_gate_now())) return ESP_OK;
  if (!camera_ensure_awake()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "Camera not initialized", HTTPD_RESP_USE_STRLEN);
  }

  // Single stream at a time. Handlers all run on the one httpd task, so this
  // check-then-set cannot race another handler — only the worker's clear.
  if (__atomic_load_n(&g_peek_stream_task_busy, __ATOMIC_ACQUIRE)) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_send(req, "Stream already active", HTTPD_RESP_USE_STRLEN);
  }

  httpd_req_t* async_req = nullptr;
  if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "Failed to detach request", HTTPD_RESP_USE_STRLEN);
  }

  __atomic_store_n(&g_peek_stream_task_busy, true, __ATOMIC_RELEASE);
  g_peek_active = true;

  // Reset per-stream metrics so the UI shows real, fresh values.
  uint32_t now_ms = millis();
  portENTER_CRITICAL(&g_peek_metrics_mux);
  g_peek_frame_count      = 0;
  g_peek_total_bytes      = 0;
  g_peek_last_frame_bytes = 0;
  g_peek_last_frame_ms    = 0;
  g_peek_stream_start_ms  = now_ms;
  g_peek_stream_end_ms    = 0;
  g_peek_fps_window_start = now_ms;
  g_peek_fps_window_count = 0;
  g_peek_fps_last         = 0;
  portEXIT_CRITICAL(&g_peek_metrics_mux);

  // Internal-RAM stack (PSRAM task stacks aren't supported by the prebuilt
  // core); priority 3 is safe because every loop path yields >=20 ms.
  BaseType_t created = xTaskCreate(peek_stream_task, "peek_stream", 8192,
                                   async_req, 3, nullptr);
  if (created != pdPASS) {
    g_peek_active = false;
    httpd_resp_set_status(async_req, "503 Service Unavailable");
    httpd_resp_send(async_req, "Out of memory for stream task", HTTPD_RESP_USE_STRLEN);
    httpd_req_async_handler_complete(async_req);
    __atomic_store_n(&g_peek_stream_task_busy, false, __ATOMIC_RELEASE);
    return ESP_OK;
  }

  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "Peek stream started", nullptr);
  return ESP_OK;
}

static esp_err_t handle_peek_snapshot(httpd_req_t* req) {
  g_health.http_requests++;

  if (peek_gate_refuse(req, peek_gate_now())) return ESP_OK;
  if (!camera_ensure_awake()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "Camera not initialized", HTTPD_RESP_USE_STRLEN);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "Frame capture failed", HTTPD_RESP_USE_STRLEN);
  }
  
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=peek.jpg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  
  return res;
}

// Stop any in-flight stream and wait for the worker task to fully exit (it
// clears the busy flag only after releasing its async request). Returns false
// on timeout — callers must NOT touch the camera driver in that case, because
// the worker may still be holding a framebuffer. A wedged client socket can
// stall the worker in httpd_resp_send_chunk for up to send_wait_timeout (30 s),
// so a bounded wait + refusal is the fail-closed choice.
static bool peek_stream_stop_and_wait(uint32_t timeout_ms) {
  g_peek_active = false;
  uint32_t waited = 0;
  while (__atomic_load_n(&g_peek_stream_task_busy, __ATOMIC_ACQUIRE)) {
    if (waited >= timeout_ms) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    waited += 20;
  }
  return true;
}

static esp_err_t handle_peek_stop(httpd_req_t* req) {
  g_health.http_requests++;

  // Wait (bounded) for the worker to record its end timestamp and exit, so
  // the status poll the UI fires right after this response sees the frozen
  // uptime instead of racing the worker mid-frame-delay (which rendered the
  // just-finished stream as 0 kbps / no uptime). The worker's typical exit
  // latency is one frame pace (<=500 ms); a wedged send can exceed the wait,
  // in which case status falls back to last_frame_ms (see stream_uptime_ms).
  bool worker_exited = peek_stream_stop_and_wait(2000);

  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "Peek stopped", nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Peek stopped";
  doc["worker_exited"] = worker_exited;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_peek_status(httpd_req_t* req) {
  g_health.http_requests++;

  JsonDocument doc;
  doc["ok"] = true;
  doc["camera_initialized"] = g_camera_initialized;
  doc["standby"] = g_cam_standby;
  doc["gate"] = camera_gate::peek_gate_name(peek_gate_now());
  doc["peek_active"] = g_peek_active;
  doc["resolution"] = (int)g_peek_framesize;
  doc["resolution_name"] = framesize_name(g_peek_framesize);

  // Real sensor identity (no placeholders — pulled from sensor_t at init)
  doc["sensor_pid"]    = g_peek_sensor_pid;
  doc["sensor_ver"]    = g_peek_sensor_ver;
  doc["sensor_midh"]   = g_peek_sensor_midh;
  doc["sensor_midl"]   = g_peek_sensor_midl;
  doc["sensor_model"]  = sensor_model_name(g_peek_sensor_pid);

  // Real capture configuration
  doc["pixel_format"]   = "JPEG";  // init_camera() always configures PIXFORMAT_JPEG
  doc["jpeg_quality"]   = g_peek_jpeg_quality;   // 0..63 (lower = better)
  doc["xclk_hz"]        = g_peek_xclk_freq_hz;
  doc["psram"]          = g_peek_psram_used;
  doc["fb_count"]       = g_peek_fb_count;
  doc["frame_delay_ms"] = g_peek_frame_delay_ms; // stream pacing target

  // Init diagnostics — when camera_initialized=false the UI now has enough
  // signal to tell the user *why*: which attempt label was tried last, the
  // raw esp_err_t code, whether PSRAM was visible, and how many init
  // attempts have run since boot (including manual /api/peek/init retries).
  doc["psram_found"]      = g_peek_psram_found;
  doc["last_init_err"]    = g_peek_last_init_err;
  doc["last_init_err_name"] = esp_err_to_name((esp_err_t)g_peek_last_init_err);
  doc["last_init_label"]  = g_peek_last_init_label;
  doc["init_attempts"]    = g_peek_init_count;
  doc["free_heap"]        = (uint32_t)ESP.getFreeHeap();

  // Snapshot all volatile metrics atomically under the shared spinlock. 64-bit
  // reads are not atomic on the ESP32 (32-bit core), and reading the same
  // volatile multiple times can produce inconsistent rows (e.g.
  // avg_frame_bytes not matching total_bytes / frame_count).
  bool     snap_active;
  uint32_t snap_frame_count;
  uint32_t snap_last_frame_bytes;
  uint64_t snap_total_bytes;
  uint32_t snap_fps_last;
  uint32_t snap_stream_start_ms;
  uint32_t snap_stream_end_ms;
  uint32_t snap_last_frame_ms;
  portENTER_CRITICAL(&g_peek_metrics_mux);
  snap_active           = g_peek_active;
  snap_frame_count      = g_peek_frame_count;
  snap_last_frame_bytes = g_peek_last_frame_bytes;
  snap_total_bytes      = g_peek_total_bytes;
  snap_fps_last         = g_peek_fps_last;
  snap_stream_start_ms  = g_peek_stream_start_ms;
  snap_stream_end_ms    = g_peek_stream_end_ms;
  snap_last_frame_ms    = g_peek_last_frame_ms;
  portEXIT_CRITICAL(&g_peek_metrics_mux);

  doc["frame_count"]       = snap_frame_count;
  doc["last_frame_bytes"]  = snap_last_frame_bytes;
  doc["total_bytes"]       = snap_total_bytes;  // ArduinoJson v7 supports uint64_t natively — no 4GB wrap
  doc["fps"]               = snap_fps_last;     // measured over the last full ~1s window, jitter-normalized

  // Live stream: uptime counts from start to now. Finished stream: uptime is
  // frozen at its real duration so the UI's "LAST STREAM" throughput/uptime
  // stay truthful instead of collapsing to zero the moment the stream stops.
  // last_frame_ms covers the stop-vs-worker-exit race (see the logic header).
  uint32_t uptime_ms = peek_stream_logic::stream_uptime_ms(
      snap_active, snap_stream_start_ms, snap_stream_end_ms,
      snap_last_frame_ms, millis());
  doc["stream_uptime_ms"]  = uptime_ms;
  uint32_t avg_bytes = (snap_frame_count > 0)
                         ? (uint32_t)(snap_total_bytes / snap_frame_count)
                         : 0;
  doc["avg_frame_bytes"]   = avg_bytes;
  // Average throughput in kbps over the entire stream so far (real, computed from totals)
  doc["avg_kbps"]          = peek_stream_logic::avg_kbps(snap_total_bytes, uptime_ms);

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK INIT / RE-INIT — POST /api/peek/init
//
// When the camera fails at boot (loose Sense connector, transient PSRAM
// glitch, heap pressure during WiFi+TLS bring-up), the user previously had
// no way to recover except a full reboot. This endpoint runs the same
// multi-stage init the boot flow uses, after deinit-ing whatever half-
// initialized state is currently present. Returns the resulting status so
// the UI can show success/failure inline.
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_init(httpd_req_t* req) {
  g_health.http_requests++;

  // Stop any in-flight stream first so the streaming task isn't holding a
  // framebuffer while we tear the driver down.
  if (!peek_stream_stop_and_wait(3000)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    const char* resp = "{\"ok\":false,\"error\":\"Stream is still shutting down; retry in a few seconds\"}";
    return http_send_json(req, resp);
  }

  Serial.println("[CAMERA] /api/peek/init — runtime re-init requested");
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "Camera re-init requested", nullptr);

  bool ok = init_camera();
  g_camera_initialized = ok;
  g_hw.camera_available = ok;
  if (ok) g_hw.camera_ever_init = true;

  JsonDocument doc;
  doc["ok"] = ok;
  doc["camera_initialized"]  = g_camera_initialized;
  doc["psram_found"]         = g_peek_psram_found;
  doc["last_init_err"]       = g_peek_last_init_err;
  doc["last_init_err_name"]  = esp_err_to_name((esp_err_t)g_peek_last_init_err);
  doc["last_init_label"]     = g_peek_last_init_label;
  doc["init_attempts"]       = g_peek_init_count;
  doc["sensor_pid"]          = g_peek_sensor_pid;
  doc["sensor_model"]        = sensor_model_name(g_peek_sensor_pid);
  doc["resolution_name"]     = framesize_name(g_peek_framesize);
  doc["fb_count"]            = g_peek_fb_count;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK RESOLUTION — NEW ENDPOINT (POST /api/peek/resolution)
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_resolution(httpd_req_t* req) {
  g_health.http_requests++;
  
  if (!g_camera_initialized) {
    const char* resp = "{\"ok\":false,\"error\":\"Camera not initialized\"}";
    return http_send_json(req, resp);
  }
  
  // Read body
  char content[64] = {0};
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  
  if (ret <= 0) {
    const char* resp = "{\"ok\":false,\"error\":\"No body\"}";
    return http_send_json(req, resp);
  }
  
  JsonDocument body;
  if (deserializeJson(body, content) != DeserializationError::Ok) {
    const char* resp = "{\"ok\":false,\"error\":\"Invalid JSON\"}";
    return http_send_json(req, resp);
  }
  
  int size = body["size"] | -1;
  if (size < 0 || size > FRAMESIZE_UXGA) {
    const char* resp = "{\"ok\":false,\"error\":\"Invalid resolution. Use 0-13 (QQVGA to UXGA)\"}";
    return http_send_json(req, resp);
  }
  
  // Stop the stream (if any) and wait for the worker to release its request.
  // The old code restored g_peek_active = true here, but the flag alone can't
  // resurrect a finished HTTP response — the client must reconnect. We report
  // stream_stopped so the UI knows to restart its <img> source.
  bool was_active = g_peek_active;
  if (!peek_stream_stop_and_wait(3000)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    const char* resp = "{\"ok\":false,\"error\":\"Stream is still shutting down; retry in a few seconds\"}";
    return http_send_json(req, resp);
  }

  bool success = set_camera_resolution((framesize_t)size);

  JsonDocument doc;
  doc["ok"] = success;
  doc["stream_stopped"] = was_active;
  if (success) {
    doc["resolution"] = size;
    doc["resolution_name"] = framesize_name((framesize_t)size);
    log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "Resolution changed", framesize_name((framesize_t)size));
  } else {
    doc["error"] = "Failed to set resolution";
  }
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK SENSOR PARAMETERS — GET / POST /api/peek/sensor
//
// Lets the operator tune the OV2640/OV3660 in real time without rebooting:
// JPEG quality, brightness/contrast/saturation, white balance, exposure,
// gain, lens correction, mirror/flip, etc. All values come straight from
// the on-chip sensor_t — never fabricated.
// ════════════════════════════════════════════════════════════════════════════

// Helpers — apply one named field from a JsonObject if present.
// Returns true if the field was present and successfully applied.
static bool apply_int_setting(sensor_t* s, const JsonObject& body, const char* key,
                              int (*setter)(sensor_t*, int), int min_val, int max_val) {
  if (!body[key].is<int>()) return false;
  int v = body[key].as<int>();
  if (v < min_val) v = min_val;
  if (v > max_val) v = max_val;
  setter(s, v);
  return true;
}

static esp_err_t handle_peek_sensor_get(httpd_req_t* req) {
  g_health.http_requests++;

  if (!g_camera_initialized) {
    const char* resp = "{\"ok\":false,\"error\":\"Camera not initialized\"}";
    return http_send_json(req, resp);
  }

  sensor_t* s = esp_camera_sensor_get();
  if (!s) {
    const char* resp = "{\"ok\":false,\"error\":\"sensor_get failed\"}";
    return http_send_json(req, resp);
  }

  // sensor->status holds the current values for every adjustable setting.
  // This is the on-chip ground truth — what the OV2640 is actually doing
  // right now.
  JsonDocument doc;
  doc["ok"]              = true;
  doc["sensor_pid"]      = s->id.PID;
  doc["sensor_model"]    = sensor_model_name(s->id.PID);
  doc["framesize"]       = s->status.framesize;
  doc["quality"]         = s->status.quality;        // 0..63 (lower=better)
  doc["brightness"]      = s->status.brightness;     // -2..2
  doc["contrast"]        = s->status.contrast;       // -2..2
  doc["saturation"]      = s->status.saturation;     // -2..2
  doc["sharpness"]       = s->status.sharpness;      // -2..2 (OV3660+)
  doc["denoise"]         = s->status.denoise;        // 0..255 (OV3660+)
  doc["special_effect"]  = s->status.special_effect; // 0..6
  doc["wb_mode"]         = s->status.wb_mode;        // 0..4
  doc["awb"]             = s->status.awb;            // 0/1
  doc["awb_gain"]        = s->status.awb_gain;       // 0/1
  doc["aec"]             = s->status.aec;            // 0/1
  doc["aec2"]            = s->status.aec2;           // 0/1
  doc["ae_level"]        = s->status.ae_level;       // -2..2
  doc["aec_value"]       = s->status.aec_value;      // 0..1200 (manual)
  doc["agc"]             = s->status.agc;            // 0/1
  doc["agc_gain"]        = s->status.agc_gain;       // 0..30 (manual)
  doc["gainceiling"]     = s->status.gainceiling;    // 0..6 (2x..128x)
  doc["bpc"]             = s->status.bpc;            // 0/1
  doc["wpc"]             = s->status.wpc;            // 0/1
  doc["raw_gma"]         = s->status.raw_gma;        // 0/1
  doc["lenc"]            = s->status.lenc;           // 0/1
  doc["hmirror"]         = s->status.hmirror;        // 0/1
  doc["vflip"]           = s->status.vflip;          // 0/1
  doc["dcw"]             = s->status.dcw;            // 0/1
  doc["colorbar"]        = s->status.colorbar;       // 0/1 (test pattern)
  doc["frame_delay_ms"]  = g_peek_frame_delay_ms;    // stream pacing

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_peek_sensor_set(httpd_req_t* req) {
  g_health.http_requests++;

  if (!g_camera_initialized) {
    const char* resp = "{\"ok\":false,\"error\":\"Camera not initialized\"}";
    return http_send_json(req, resp);
  }

  sensor_t* s = esp_camera_sensor_get();
  if (!s) {
    const char* resp = "{\"ok\":false,\"error\":\"sensor_get failed\"}";
    return http_send_json(req, resp);
  }

  // Read body — generous limit to allow tuning multiple fields atomically.
  char content[512] = {0};
  int total = 0;
  while (total < (int)sizeof(content) - 1) {
    int r = httpd_req_recv(req, content + total, sizeof(content) - 1 - total);
    if (r <= 0) break;
    total += r;
  }
  if (total <= 0) {
    const char* resp = "{\"ok\":false,\"error\":\"No body\"}";
    return http_send_json(req, resp);
  }

  JsonDocument body;
  if (deserializeJson(body, content) != DeserializationError::Ok) {
    const char* resp = "{\"ok\":false,\"error\":\"Invalid JSON\"}";
    return http_send_json(req, resp);
  }
  if (!body.is<JsonObject>()) {
    const char* resp = "{\"ok\":false,\"error\":\"Body must be a JSON object\"}";
    return http_send_json(req, resp);
  }
  JsonObject obj = body.as<JsonObject>();

  // JPEG quality is special — also sync our cached metric so /status reflects it.
  if (obj["quality"].is<int>()) {
    int q = obj["quality"].as<int>();
    if (q < 0) q = 0; if (q > 63) q = 63;
    s->set_quality(s, q);
    g_peek_jpeg_quality = q;
  }

  // Image tuning (range −2..2)
  apply_int_setting(s, obj, "brightness",     s->set_brightness,     -2,  2);
  apply_int_setting(s, obj, "contrast",       s->set_contrast,       -2,  2);
  apply_int_setting(s, obj, "saturation",     s->set_saturation,     -2,  2);
  apply_int_setting(s, obj, "sharpness",      s->set_sharpness,      -2,  2);
  apply_int_setting(s, obj, "denoise",        s->set_denoise,         0, 255);
  apply_int_setting(s, obj, "special_effect", s->set_special_effect,  0,  6);
  apply_int_setting(s, obj, "ae_level",       s->set_ae_level,       -2,  2);

  // White balance
  apply_int_setting(s, obj, "wb_mode",        s->set_wb_mode,         0,  4);
  apply_int_setting(s, obj, "awb",            s->set_whitebal,        0,  1);
  apply_int_setting(s, obj, "awb_gain",       s->set_awb_gain,        0,  1);

  // Exposure
  apply_int_setting(s, obj, "aec",            s->set_exposure_ctrl,   0,  1);
  apply_int_setting(s, obj, "aec2",           s->set_aec2,            0,  1);
  apply_int_setting(s, obj, "aec_value",      s->set_aec_value,       0, 1200);

  // Gain
  apply_int_setting(s, obj, "agc",            s->set_gain_ctrl,       0,  1);
  apply_int_setting(s, obj, "agc_gain",       s->set_agc_gain,        0, 30);
  if (obj["gainceiling"].is<int>()) {
    int g = obj["gainceiling"].as<int>();
    if (g < 0) g = 0; if (g > 6) g = 6;
    s->set_gainceiling(s, (gainceiling_t)g);
  }

  // Image cleanup
  apply_int_setting(s, obj, "bpc",            s->set_bpc,             0,  1);
  apply_int_setting(s, obj, "wpc",            s->set_wpc,             0,  1);
  apply_int_setting(s, obj, "raw_gma",        s->set_raw_gma,         0,  1);
  apply_int_setting(s, obj, "lenc",           s->set_lenc,            0,  1);
  apply_int_setting(s, obj, "dcw",            s->set_dcw,             0,  1);

  // Orientation + diagnostics
  apply_int_setting(s, obj, "hmirror",        s->set_hmirror,         0,  1);
  apply_int_setting(s, obj, "vflip",          s->set_vflip,           0,  1);
  apply_int_setting(s, obj, "colorbar",       s->set_colorbar,        0,  1);

  // Stream pacing (NOT a sensor setting, but lives here so the UI can tune
  // FPS in one place). Range is clamped to keep the streaming task from
  // either spinning the CPU or appearing frozen.
  if (obj["frame_delay_ms"].is<int>()) {
    int d = obj["frame_delay_ms"].as<int>();
    if (d < 20)  d = 20;     // 50 fps cap (board can't actually deliver this)
    if (d > 500) d = 500;    // 2 fps floor
    g_peek_frame_delay_ms = (uint32_t)d;
  }

  // Reset to surveillance defaults if requested.
  if (obj["reset_defaults"].is<bool>() && obj["reset_defaults"].as<bool>()) {
    apply_default_sensor_tuning();
  }

  // Echo back the (now-current) sensor state so the UI doesn't need a second round-trip.
  return handle_peek_sensor_get(req);
}

#endif // FEATURE_CAMERA_PEEK

// ════════════════════════════════════════════════════════════════════════════
// MESH NETWORK (OPERA) API HANDLERS
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_MESH_NETWORK

static esp_err_t handle_mesh_status(httpd_req_t* req) {
  g_health.http_requests++;

  mesh_network::MeshStatus status = mesh_network::get_status();
  const mesh_network::OperaConfig* config = mesh_network::get_opera_config();
  const mesh_network::PairingSession* pairing = mesh_network::get_pairing_session();

  JsonDocument doc;
  doc["ok"] = true;
  doc["state"] = mesh_network::state_name(status.state);
  doc["enabled"] = mesh_network::is_enabled();
  doc["has_opera"] = mesh_network::has_opera();
  doc["opera_id"] = status.opera_id_hex;
  doc["opera_name"] = config->opera_name;
  doc["peers_total"] = status.peers_total;
  doc["peers_online"] = status.peers_online;
  doc["peers_offline"] = status.peers_offline;
  doc["peers_stale"] = status.peers_stale;
  doc["messages_sent"] = status.messages_sent;
  doc["messages_received"] = status.messages_received;
  doc["alerts_sent"] = status.alerts_sent;
  doc["alerts_received"] = status.alerts_received;
  doc["auth_failures"] = status.auth_failures;
  doc["uptime_ms"] = status.uptime_ms;

  // Include pairing code if in pairing confirm state
  if (status.state == mesh_network::MESH_PAIRING_CONFIRM && pairing->code_displayed) {
    doc["pairing_code"] = pairing->confirmation_code;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_mesh_peers(httpd_req_t* req) {
  g_health.http_requests++;

  uint8_t count = mesh_network::get_peer_count();
  JsonDocument doc;
  doc["ok"] = true;
  doc["count"] = count;

  JsonArray peers = doc["peers"].to<JsonArray>();
  for (uint8_t i = 0; i < count; i++) {
    const mesh_network::OperaPeer* peer = mesh_network::get_peer(i);
    if (!peer) continue;

    JsonObject p = peers.add<JsonObject>();
    p["name"] = peer->name;

    char fp_hex[17];
    for (int j = 0; j < 8; j++) {
      snprintf(fp_hex + j * 2, sizeof(fp_hex) - j * 2, "%02X", peer->fingerprint[j]);
    }
    p["fingerprint"] = fp_hex;

    p["state"] = mesh_network::peer_state_name(peer->state);
    p["rssi"] = peer->rssi;
    p["alerts_received"] = peer->alerts_received;

    if (peer->last_seen_ms > 0) {
      p["last_seen_sec"] = (millis() - peer->last_seen_ms) / 1000;
    }
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_mesh_alerts(httpd_req_t* req) {
  g_health.http_requests++;

  size_t count = 0;
  const mesh_network::MeshAlert* alerts = mesh_network::get_alerts(&count);

  JsonDocument doc;
  doc["ok"] = true;
  doc["count"] = count;

  JsonArray arr = doc["alerts"].to<JsonArray>();
  for (size_t i = 0; i < count; i++) {
    const mesh_network::MeshAlert* alert = &alerts[i];
    JsonObject a = arr.add<JsonObject>();

    a["timestamp_ms"] = alert->timestamp_ms;
    a["type"] = mesh_network::alert_type_name(alert->type);
    a["severity"] = (int)alert->severity;
    a["sender_name"] = alert->sender_name;
    a["detail"] = alert->detail;
    a["witness_seq"] = alert->witness_seq;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_mesh_alerts_clear(httpd_req_t* req) {
  g_health.http_requests++;
  mesh_network::clear_alerts();
  return http_send_json(req, "{\"ok\":true}");
}

static esp_err_t handle_mesh_enable(httpd_req_t* req) {
  g_health.http_requests++;

  char buf[64];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) return http_send_error(req, 400, "invalid_body");
  buf[ret] = '\0';

  JsonDocument body;
  if (deserializeJson(body, buf) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  bool enabled = body["enabled"] | false;
  mesh_network::set_enabled(enabled);
  log_health(SCV_LOG_INFO, SCV_CAT_MESH, enabled ? "Mesh enabled" : "Mesh disabled", nullptr);

  return http_send_json(req, "{\"ok\":true}");
}

static esp_err_t handle_mesh_pair_start(httpd_req_t* req) {
  g_health.http_requests++;

  char buf[128];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  buf[ret > 0 ? ret : 0] = '\0';

  const char* opera_name = nullptr;
  JsonDocument body;
  if (ret > 0 && deserializeJson(body, buf) == DeserializationError::Ok) {
    opera_name = body["name"] | (const char*)nullptr;
  }

  if (mesh_network::start_pairing_initiator(opera_name)) {
    log_health(SCV_LOG_INFO, SCV_CAT_MESH, "Pairing started (initiator)", nullptr);
    return http_send_json(req, "{\"ok\":true}");
  }
  return http_send_error(req, 400, "pairing_failed");
}

static esp_err_t handle_mesh_pair_join(httpd_req_t* req) {
  g_health.http_requests++;

  if (mesh_network::start_pairing_joiner()) {
    log_health(SCV_LOG_INFO, SCV_CAT_MESH, "Pairing started (joiner)", nullptr);
    return http_send_json(req, "{\"ok\":true}");
  }
  return http_send_error(req, 400, "pairing_failed");
}

static esp_err_t handle_mesh_pair_confirm(httpd_req_t* req) {
  g_health.http_requests++;

  if (mesh_network::confirm_pairing()) {
    log_health(SCV_LOG_INFO, SCV_CAT_MESH, "Pairing confirmed", nullptr);
    return http_send_json(req, "{\"ok\":true}");
  }
  return http_send_error(req, 400, "confirm_failed");
}

static esp_err_t handle_mesh_pair_cancel(httpd_req_t* req) {
  g_health.http_requests++;
  mesh_network::cancel_pairing();
  log_health(SCV_LOG_INFO, SCV_CAT_MESH, "Pairing canceled", nullptr);
  return http_send_json(req, "{\"ok\":true}");
}

static esp_err_t handle_mesh_leave(httpd_req_t* req) {
  g_health.http_requests++;

  if (mesh_network::leave_opera()) {
    log_health(SCV_LOG_WARNING, SCV_CAT_MESH, "Left opera", nullptr);
    return http_send_json(req, "{\"ok\":true}");
  }
  return http_send_error(req, 400, "leave_failed");
}

static esp_err_t handle_mesh_remove(httpd_req_t* req) {
  g_health.http_requests++;

  char buf[64];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) return http_send_error(req, 400, "invalid_body");
  buf[ret] = '\0';

  JsonDocument body;
  if (deserializeJson(body, buf) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  const char* fp_hex = body["fingerprint"] | "";
  if (strlen(fp_hex) != 16) {
    return http_send_error(req, 400, "invalid_fingerprint");
  }

  // Parse hex fingerprint
  uint8_t fp[8];
  for (int i = 0; i < 8; i++) {
    char byte_hex[3] = { fp_hex[i*2], fp_hex[i*2+1], 0 };
    fp[i] = (uint8_t)strtol(byte_hex, nullptr, 16);
  }

  if (mesh_network::remove_peer(fp)) {
    log_health(SCV_LOG_WARNING, SCV_CAT_MESH, "Peer removed", fp_hex);
    return http_send_json(req, "{\"ok\":true}");
  }
  return http_send_error(req, 400, "remove_failed");
}

static esp_err_t handle_mesh_name(httpd_req_t* req) {
  g_health.http_requests++;

  char buf[128];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) return http_send_error(req, 400, "invalid_body");
  buf[ret] = '\0';

  JsonDocument body;
  if (deserializeJson(body, buf) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  const char* name = body["name"] | "";
  if (strlen(name) == 0 || strlen(name) > mesh_network::MAX_OPERA_NAME_LEN) {
    return http_send_error(req, 400, "invalid_name");
  }

  if (mesh_network::set_opera_name(name)) {
    log_health(SCV_LOG_INFO, SCV_CAT_MESH, "Opera name changed", name);
    return http_send_json(req, "{\"ok\":true}");
  }
  return http_send_error(req, 400, "rename_failed");
}

#endif // FEATURE_MESH_NETWORK

// ════════════════════════════════════════════════════════════════════════════
// WIFI PROVISIONING API HANDLERS
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_wifi_status(httpd_req_t* req) {
  g_health.http_requests++;

  wifi_update_status();

  JsonDocument doc;
  doc["ok"] = true;
  doc["state"] = wifi_state_name(g_wifi_status.state);
  doc["ap_active"] = g_wifi_status.ap_active;
  doc["ap_ssid"] = g_device.ap_ssid;
  doc["ap_ip"] = g_wifi_status.ap_ip;
  doc["ap_clients"] = g_wifi_status.ap_clients;
  doc["sta_connected"] = g_wifi_status.sta_connected;
  doc["sta_ssid"] = g_wifi_creds.configured ? g_wifi_creds.ssid : "";
  doc["sta_ip"] = g_wifi_status.sta_ip;
  doc["rssi"] = g_wifi_status.rssi;
  doc["configured"] = g_wifi_creds.configured;
  doc["enabled"] = g_wifi_creds.enabled;
  doc["ap_only"] = g_wifi_ap_only;
  doc["connect_attempts"] = g_wifi_status.connect_attempts;
  doc["fail_reason"] = g_wifi_status.last_fail_reason;

  if (g_wifi_status.sta_connected && g_wifi_status.connected_since_ms > 0) {
    doc["connected_sec"] = (millis() - g_wifi_status.connected_since_ms) / 1000;
  }

#if FEATURE_QR_PROVISION
  doc["qr_provision"] = true;
#endif

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ── WiFi scan cache ─────────────────────────────────────────────────────
// A scan hops the single radio across every channel, stalling the SoftAP a
// provisioning phone is attached to — the classic way the wizard's scan
// fetch died mid-flight. Results are therefore cached and served for
// SCAN_CACHE_TTL_MS (a boot-time pre-scan fills the cache before any phone
// joins); only an explicit ?force=1 ("Scan again") sweeps the radio while a
// client is on the AP. Freshness decision: provisioning_logic::scan_cache_fresh.
static const uint32_t SCAN_CACHE_TTL_MS = 5UL * 60UL * 1000UL;
static const int SCAN_CACHE_MAX = 20;
struct ScanCacheEntry {
  char ssid[33];
  int32_t rssi;
  int32_t channel;
  char security[12];
};
static ScanCacheEntry g_scan_cache[SCAN_CACHE_MAX];
static int g_scan_cache_count = 0;
static uint32_t g_scan_cache_at_ms = 0;

// Non-zero = first-boot setup finished with a live WiFi join; reboot into
// steady state once this deadline passes (see the deferred-reboot block in
// loop() and provisioning_logic::deferred_reboot_due). Written by loop()
// AND by the /api/selftest handler (HTTP task) to hold the reboot off while
// the user is on the wizard's final step, so all accesses are atomic.
static uint32_t g_setup_grace_reboot_at_ms = 0;

// Minimum runway to keep after a step-5 self-test poll: enough for the user
// to read every row and tap "Run again" once before the steady-state reboot.
static const uint32_t SELFTEST_REBOOT_MIN_MS = 90000;

static const char* wifi_auth_mode_name(wifi_auth_mode_t authMode) {
  if (authMode == WIFI_AUTH_OPEN) return "open";
  if (authMode == WIFI_AUTH_WPA_PSK) return "wpa";
  if (authMode == WIFI_AUTH_WPA2_PSK) return "wpa2";
  if (authMode == WIFI_AUTH_WPA_WPA2_PSK) return "wpa/wpa2";
  if (authMode == WIFI_AUTH_WPA3_PSK) return "wpa3";
  if (authMode == WIFI_AUTH_WPA2_WPA3_PSK) return "wpa2/wpa3";
  return "other";
}

static esp_err_t send_scan_cache_json(httpd_req_t* req, bool from_cache, uint32_t now) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["scanning"] = false;
  doc["count"] = g_scan_cache_count;
  doc["cached"] = from_cache;
  if (from_cache) {
    doc["age_s"] = (uint32_t)(now - g_scan_cache_at_ms) / 1000;
  }
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (int i = 0; i < g_scan_cache_count; i++) {
    JsonObject net = networks.add<JsonObject>();
    net["ssid"] = g_scan_cache[i].ssid;
    net["rssi"] = g_scan_cache[i].rssi;
    net["channel"] = g_scan_cache[i].channel;
    net["security"] = g_scan_cache[i].security;
  }
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// A request that asked for the device by its real name (vs a captive-portal
// assistant probing a hijacked foreign domain). Same gate the "/" redirect
// uses before minting a pairing token into the wizard URL.
static bool request_host_is_direct(httpd_req_t* req) {
  char host[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
    return false;
  }
  return (strstr(host, "canary.local") != nullptr) ||
         (strstr(host, "192.168.4.1") != nullptr);
}

// Re-issue a pairing token to the live wizard. The URL token is RAM-backed
// (10-min TTL, wiped on reboot, 4-slot eviction); without this, a stale
// token dead-ends the user at "setup link expired" even with a correct
// password. Answers ONLY while the first-boot wizard is active and only to
// a direct-Host browser — the same posture as the "/" redirect that minted
// the original token. The AP itself remains the security boundary
// (companion_pwa.h wizard doc block); the token stays a UX gate.
static esp_err_t handle_wifi_pair_token(httpd_req_t* req) {
  g_health.http_requests++;
  if (!setup_wizard::is_active() || !request_host_is_direct(req)) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
  }
  setup_wizard::touch();
  char tok_hex[csi_integration::PAIR_TOKEN_HEX_LEN + 1];
  if (!csi_integration::pair_token_issue(tok_hex, sizeof(tok_hex))) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["token"] = tok_hex;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_scan(httpd_req_t* req) {
  g_health.http_requests++;
  // A human is actively driving the wizard — don't reboot the portal
  // out from under them (the 15-min window is for abandonment only).
  setup_wizard::touch();

  // Serve the cache when it's fresh — unless the client explicitly asked
  // for a live sweep (?force=1, the wizard's "Scan again"). See the cache
  // doc block above for why sweeping under an attached phone is harmful.
  uint32_t now = millis();
  bool force = false;
  {
    char qs[32] = {0};
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
      char val[4] = {0};
      force = (httpd_query_key_value(qs, "force", val, sizeof(val)) == ESP_OK &&
               val[0] == '1');
    }
  }
  // Never serve the cache while a sweep is running: a forced rescan's
  // follow-up polls must keep reporting {scanning:true} until the NEW
  // results are harvested, not short-circuit back to the stale list.
  if (!force && !g_wifi_scan_in_progress &&
      provisioning_logic::scan_cache_fresh(now, g_scan_cache_at_ms,
                                           g_scan_cache_count > 0,
                                           SCAN_CACHE_TTL_MS)) {
    return send_scan_cache_json(req, true, now);
  }

  // Check if async scan is complete
  int16_t scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_RUNNING) {
    // Scan still in progress - tell client to poll again
    JsonDocument doc;
    doc["ok"] = true;
    doc["scanning"] = true;
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  if (scanResult == WIFI_SCAN_FAILED || (!g_wifi_scan_in_progress && scanResult < 0)) {
    // No scan running - start async scan (non-blocking)
    g_wifi_scan_in_progress = true;
    g_wifi_status.state = WIFI_PROV_SCANNING;
    WiFi.scanNetworks(true, false, false, 300);  // async=true

    JsonDocument doc;
    doc["ok"] = true;
    doc["scanning"] = true;
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  // Scan complete — refill the cache and serve from it.
  g_wifi_scan_in_progress = false;
  if (g_wifi_status.state == WIFI_PROV_SCANNING) {
    g_wifi_status.state = g_wifi_creds.configured ? WIFI_PROV_IDLE : WIFI_PROV_AP_ONLY;
  }

  int n = scanResult;
  g_scan_cache_count = 0;
  for (int i = 0; i < n && i < SCAN_CACHE_MAX; i++) {
    ScanCacheEntry& e = g_scan_cache[g_scan_cache_count++];
    strncpy(e.ssid, WiFi.SSID(i).c_str(), sizeof(e.ssid) - 1);
    e.ssid[sizeof(e.ssid) - 1] = '\0';
    e.rssi = WiFi.RSSI(i);
    e.channel = WiFi.channel(i);
    strncpy(e.security, wifi_auth_mode_name(WiFi.encryptionType(i)),
            sizeof(e.security) - 1);
    e.security[sizeof(e.security) - 1] = '\0';
  }
  g_scan_cache_at_ms = now;

  WiFi.scanDelete();

  return send_scan_cache_json(req, false, now);
}

// Credential gate shared by /api/wifi/connect and /api/wifi/ap-only. The
// wizard carries a live pair token; the settings panel carries an admin
// credential (session cookie / Bearer) instead. A *presented* admin
// credential is validated through the lockout-aware `api_auth_check` so
// Bearer guessing is throttled exactly like every other admin route —
// `api_auth_check_optional` (used before) skipped the 429 backoff and never
// recorded a failure, turning this fallback into an unthrottled token
// oracle: an on-AP/LAN client could spray Authorization guesses and tell a
// valid token from the handler proceeding. A bare pair-token miss (no
// admin credential presented at all) stays a friendly `invalid_token` so
// the wizard can silently re-issue its RAM-backed token and retry.
// (WifiChangeAuth is declared in wifi_provisioning_auth.h — included at the
// top — so arduino-cli's hoisted prototype for this function sees the type.)
static WifiChangeAuth wifi_change_authorize(httpd_req_t* req,
                                            const char* pair_token) {
  if (csi_integration::pair_token_valid(pair_token)) return WifiChangeAuth::PROCEED;
  const bool has_bearer = httpd_req_get_hdr_value_len(req, "Authorization") > 0;
  const bool has_cookie = (cv_session_validate && cv_session_validate(req));
  if (has_bearer || has_cookie) {
    // api_auth_check enforces the lockout, records failures for
    // presented-but-wrong tokens, and sends its own 401/403/429 response,
    // so on failure the caller just returns ESP_OK.
    return api_auth_check(req, g_device.api_token_str)
               ? WifiChangeAuth::PROCEED
               : WifiChangeAuth::RESPONDED;
  }
  return WifiChangeAuth::INVALID_TOKEN;  // wizard self-heal path
}

static esp_err_t wifi_change_send_invalid_token(httpd_req_t* req) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["code"] = "invalid_token";
  doc["error"] = "This setup link has expired. Reconnect to the SecuraCV network to start over.";
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// "Use without home WiFi": the explicit standalone exit from the setup
// wizard. Same credential gate as /api/wifi/connect (the choice changes
// how the device runs, so it deserves the same posture as a credential
// save). Persists the AP-only preference, completes first-boot setup — no
// more 15-minute reboot loop — and leaves the SoftAP up as the product.
static esp_err_t handle_wifi_ap_only(httpd_req_t* req) {
  g_health.http_requests++;
  setup_wizard::touch();

  char content[256] = {0};
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  JsonDocument body;
  if (ret <= 0 || deserializeJson(body, content) != DeserializationError::Ok) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"Invalid JSON\"}");
  }
  // Pair token (wizard) OR a lockout-throttled admin credential (settings
  // panel). See wifi_change_authorize — the admin fallback must go through
  // the throttled path so it can't be used as a token-guessing oracle.
  const char* token = body["token"] | "";
  switch (wifi_change_authorize(req, token)) {
    case WifiChangeAuth::PROCEED: break;
    case WifiChangeAuth::RESPONDED: return ESP_OK;  // auth helper already replied
    case WifiChangeAuth::INVALID_TOKEN: return wifi_change_send_invalid_token(req);
  }

  NvsManager& nvs = NvsManager::instance();
  if (nvs.beginReadWrite()) {
    nvs.putBool(NVS_KEY_WIFI_AP_ONLY, true);
    nvs.end();
  }
  g_wifi_ap_only = true;
  g_wifi_creds.enabled = false;
  // Kill any in-flight or auto-reconnect STA attempt (the user may have
  // tried a network, backed out, then chosen standalone) — the radio is
  // the SoftAP's alone from here. false = leave WiFi (and the AP) up.
  WiFi.disconnect(false);
  g_wifi_status.state = WIFI_PROV_AP_ONLY;
  setup_wizard::mark_complete();
  // mark_complete() stops the captive DNS as part of closing the first-boot
  // wizard — but in standalone mode the AP (and canary.local on it) IS the
  // product, so bring the resolver right back up. dns_process() in the main
  // loop services it whenever it's running.
  setup_wizard::start_captive_portal();
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK,
             "Standalone (AP-only) mode chosen in setup wizard", nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["ap_only"] = true;
  doc["message"] = "Running standalone on the SecuraCV network";
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_connect(httpd_req_t* req) {
  g_health.http_requests++;
  setup_wizard::touch();

  // Read body (sized for ssid + password + token + optional device_name)
  char content[384] = {0};
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);

  if (ret <= 0) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "No body";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  JsonDocument body;
  if (deserializeJson(body, content) != DeserializationError::Ok) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "Invalid JSON";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  // Pairing-token gate. The captive-portal QR + manual fallback link both
  // bake a fresh pairing token into /companion?token=<hex>, and the wizard
  // JS forwards that token in the body of every POST here. pair_token_valid
  // rejects empty/short/wrong-hex too, and validates WITHOUT consuming so
  // the wizard can retry within the TTL (e.g. mistyped password).
  //
  // Settings-panel path: no pair token, but an admin credential (session
  // cookie / Bearer). wifi_change_authorize routes that through the
  // lockout-aware api_auth_check so it can't become a token-guessing oracle;
  // a bare pair-token miss still returns invalid_token so the wizard
  // self-heals. (Without any admin fallback the panel's Connect could never
  // succeed post-setup — it holds no pair token.)
  const char* token = body["token"] | "";
  switch (wifi_change_authorize(req, token)) {
    case WifiChangeAuth::PROCEED: break;
    case WifiChangeAuth::RESPONDED: return ESP_OK;  // auth helper already replied
    case WifiChangeAuth::INVALID_TOKEN: return wifi_change_send_invalid_token(req);
  }

  const char* ssid = body["ssid"] | "";
  const char* password = body["password"] | "";

  if (strlen(ssid) == 0 || strlen(ssid) > 32) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "Invalid SSID (1-32 chars required)";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  if (strlen(password) > 64) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "Password too long (max 64 chars)";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  // Optional friendly device name (e.g. "kitchen"). When present it both
  // persists to NVS ("dev_name") and becomes this device's unique mDNS
  // hostname canary-<name>.local, so multiple Canaries are easy to tell apart.
  // We only accept a name that survives RFC-1123 sanitization (otherwise it
  // would yield an empty/degenerate label); silently ignore an unusable name
  // rather than failing the whole WiFi save.
  const char* device_name = body["device_name"] | "";
  if (strlen(device_name) > 0 && strlen(device_name) <= setup_wizard::DEVICE_NAME_MAX) {
    char probe[setup_wizard::DEVICE_NAME_MAX + 1];
    sanitize_mdns_label(device_name, probe, sizeof(probe));
    if (probe[0] && setup_wizard::set_device_name(device_name)) {
      generate_mdns_hostname(g_device.mdns_hostname, sizeof(g_device.mdns_hostname));
    }
  }

  // Save credentials
  strncpy(g_wifi_creds.ssid, ssid, sizeof(g_wifi_creds.ssid) - 1);
  g_wifi_creds.ssid[sizeof(g_wifi_creds.ssid) - 1] = '\0';
  strncpy(g_wifi_creds.password, password, sizeof(g_wifi_creds.password) - 1);
  g_wifi_creds.password[sizeof(g_wifi_creds.password) - 1] = '\0';
  g_wifi_creds.enabled = true;
  g_wifi_creds.configured = true;

  wifi_save_credentials();

  // Clear stale failure context from any previous attempt before retrying.
  g_wifi_status.last_fail_reason[0] = '\0';

  // Attempt connection
  wifi_connect_to_home();

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Credentials saved, attempting connection";
  doc["ssid"] = g_wifi_creds.ssid;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// External-linkage bridge for the BLE log-export module. ble_log_export.cpp
// reads through these helpers rather than poking g_health_log_ring directly,
// so the storage layout stays internal to this TU and the BLE module doesn't
// need to know about HEALTH_LOG_RING_SIZE / head-index arithmetic.
//
// BleLogSnapshot is defined in ble_log_export.h (included above) so the
// type is visible to Arduino IDE's auto-prototype generator — it lifts
// .ino function signatures to the top of the translation unit before
// walking the body, and would otherwise fail to find BleLogSnapshot.

bool ble_log_get_head(uint32_t* count, uint32_t* oldest_seq, uint32_t* newest_seq,
                      uint32_t* ring_size) {
  if (!count || !oldest_seq || !newest_seq || !ring_size) return false;
  *count     = (uint32_t)g_health_log_ring_count;
  *ring_size = (uint32_t)HEALTH_LOG_RING_SIZE;
  if (g_health_log_ring_count == 0) {
    *oldest_seq = 0;
    *newest_seq = 0;
    return true;
  }
  // The ring stores entries in chronological order (oldest at head when
  // wrapped, newest at head-1). Walk both ends to read the seq numbers
  // without tying the BLE module to ring-buffer mechanics.
  size_t newest_idx = (g_health_log_ring_head + HEALTH_LOG_RING_SIZE - 1)
                       % HEALTH_LOG_RING_SIZE;
  size_t oldest_idx = (g_health_log_ring_count == HEALTH_LOG_RING_SIZE)
                       ? g_health_log_ring_head
                       : 0;
  *newest_seq = g_health_log_ring[newest_idx].seq;
  *oldest_seq = g_health_log_ring[oldest_idx].seq;
  return true;
}

bool ble_log_get_by_index(size_t newest_first_index, BleLogSnapshot* out) {
  if (!out) return false;
  if (newest_first_index >= g_health_log_ring_count) return false;
  // newest-first: index 0 = most recent; matches handle_logs() iteration
  // order so a phone scrolling through "0..count-1" sees the same order
  // as the SPA's logs panel.
  size_t idx = (g_health_log_ring_head + HEALTH_LOG_RING_SIZE - 1
                - newest_first_index) % HEALTH_LOG_RING_SIZE;
  const HealthLogRingEntry& entry = g_health_log_ring[idx];
  out->seq          = entry.seq;
  out->timestamp_ms = entry.timestamp_ms;
  out->level        = (uint8_t)entry.level;
  out->category     = (uint8_t)entry.category;
  out->ack_status   = (uint8_t)entry.ack_status;
  // strncpy + explicit NUL: messages are bounded to the struct sizes and
  // we want to make sure short messages don't read past their end.
  strncpy(out->message, entry.message, sizeof(out->message) - 1);
  out->message[sizeof(out->message) - 1] = '\0';
  strncpy(out->detail, entry.detail, sizeof(out->detail) - 1);
  out->detail[sizeof(out->detail) - 1] = '\0';
  return true;
}

// External-linkage bridges for the BLE witness-export module
// (ble_witness_export.cpp). The chain head + last record live behind
// internal linkage in this TU; the bridges marshal the relevant fields
// into compact JSON the BLE module just streams as a characteristic
// value. Bonded peers can independently verify the device's chain
// state without WiFi by reading these.

// Cheap getter so the BLE module can detect chain advancement without
// rebuilding HEAD just to throw it away. Mirrors `g_health.records_created`
// 1:1; the value monotonically increases for the device's lifetime.
uint32_t ble_witness_get_total_records() {
  return (uint32_t)g_health.records_created;
}

bool ble_witness_get_head_json(char* out, size_t out_len) {
  if (!out || out_len < 200) return false;
  // Compact field names because the whole payload has to fit in MTU 247
  // (244-byte ATT data) for a single read without long-read fallback.
  //   s  = current chain seq
  //   t  = total records ever created
  //   h  = chain head (32-byte hash, hex)
  //   pk = device pubkey (32 bytes, hex) — verifier needs this to check
  //        the signature in the RECORD characteristic
  char head_hex[65], pub_hex[65];
  hex_to_str(head_hex, g_device.chain_head, 32);
  hex_to_str(pub_hex,  g_device.pubkey, 32);
  int n = snprintf(out, out_len,
                   "{\"s\":%u,\"t\":%u,\"h\":\"%s\",\"pk\":\"%s\"}",
                   (unsigned)g_device.seq,
                   (unsigned)g_health.records_created,
                   head_hex, pub_hex);
  return n > 0 && (size_t)n < out_len;
}

bool ble_witness_get_record_json(char* out, size_t out_len) {
  if (!out || out_len < 512) return false;
  // No record yet — return false so the BLE module can short-circuit
  // and surface a "no chain history yet" placeholder rather than
  // emit a half-formed JSON.
  if (g_last_record.seq == 0) return false;

  char ph[65], prev[65], ch[65], sig[129];
  hex_to_str(ph,   g_last_record.payload_hash, 32);
  hex_to_str(prev, g_last_record.prev_hash,    32);
  hex_to_str(ch,   g_last_record.chain_hash,   32);
  hex_to_str(sig,  g_last_record.signature,    64);

  // Field names again kept short to fit ~440 bytes inside the 512-byte
  // characteristic value cap (BLE 4.2 long-read limit). Verifier needs
  // payload_hash + prev_hash to recompute chain_hash, then the signature
  // verifies chain_hash with the pubkey from HEAD.
  int n = snprintf(out, out_len,
                   "{\"seq\":%u,\"tb\":%u,\"t\":\"%s\",\"plen\":%u,"
                   "\"ph\":\"%s\",\"prev\":\"%s\",\"ch\":\"%s\","
                   "\"sig\":\"%s\",\"v\":%s}",
                   (unsigned)g_last_record.seq,
                   (unsigned)g_last_record.time_bucket,
                   record_type_name(g_last_record.type),
                   (unsigned)g_last_record.payload_len,
                   ph, prev, ch, sig,
                   g_last_record.verified ? "true" : "false");
  return n > 0 && (size_t)n < out_len;
}

// External-linkage bridge for the BLE provisioning module. ble_provision.cpp
// hands a paired phone's chosen credentials in here; we run the same input
// validation as handle_wifi_connect, persist to NVS, and kick the existing
// connect state machine. Returns false on validation failure (caller surfaces
// that to the BLE peer via the STATE characteristic).
//
// NOT static — needs external linkage so ble_provision.cpp's `extern bool
// ble_request_wifi_provisioning(...)` declaration links against this body.
bool ble_request_wifi_provisioning(const char* ssid, const char* password) {
  if (!ssid || !password) return false;
  const size_t ssid_len = strlen(ssid);
  const size_t pw_len   = strlen(password);
  if (ssid_len == 0 || ssid_len > 32) return false;   // WPA2 SSID bound
  if (pw_len > 64)                    return false;   // WPA2 PSK bound
  // Empty password = open network. Allowed (matches handle_wifi_connect).

  strncpy(g_wifi_creds.ssid, ssid, sizeof(g_wifi_creds.ssid) - 1);
  g_wifi_creds.ssid[sizeof(g_wifi_creds.ssid) - 1] = '\0';
  strncpy(g_wifi_creds.password, password, sizeof(g_wifi_creds.password) - 1);
  g_wifi_creds.password[sizeof(g_wifi_creds.password) - 1] = '\0';
  g_wifi_creds.enabled    = true;
  g_wifi_creds.configured = true;

  wifi_save_credentials();
  g_wifi_status.last_fail_reason[0] = '\0';
  wifi_connect_to_home();

  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK,
             "WiFi credentials applied via BLE provisioning",
             g_wifi_creds.ssid);
  return true;
}

static esp_err_t handle_wifi_disconnect(httpd_req_t* req) {
  g_health.http_requests++;

  WiFi.disconnect(false);
  g_wifi_creds.enabled = false;
  g_wifi_status.state = WIFI_PROV_AP_ONLY;
  // The management AP may have been dropped after the STA link went healthy
  // (see wifi_drop_ap). Bring it back before tearing down STA so the device
  // stays reachable for re-provisioning instead of going dark until a reboot.
  wifi_raise_ap();

  // Update NVS
  NvsManager& nvs = NvsManager::instance();
  if (nvs.beginReadWrite()) {
    nvs.putBool(NVS_KEY_WIFI_EN, false);
    nvs.end();
  }

  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "WiFi disconnected", nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Disconnected from home WiFi";

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_forget(httpd_req_t* req) {
  g_health.http_requests++;

  WiFi.disconnect(true);
  wifi_clear_credentials();

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "WiFi credentials forgotten";

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_reconnect(httpd_req_t* req) {
  g_health.http_requests++;

  if (!g_wifi_creds.configured) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "No WiFi credentials configured";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  g_wifi_creds.enabled = true;

  // Update NVS
  {
    NvsManager& nvs = NvsManager::instance();
    if (nvs.beginReadWrite()) {
      nvs.putBool(NVS_KEY_WIFI_EN, true);
      nvs.end();
    }
  }

  wifi_connect_to_home();

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Attempting to reconnect";
  doc["ssid"] = g_wifi_creds.ssid;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// FLEET QR — server-side SVG QR code for provisioning new Canaries
// ════════════════════════════════════════════════════════════════════════════

// ESP-IDF's httpd_query_key_value does not percent-decode values, so a
// password like "p@ss word" arrives as "p%40ss+word". Decode in place:
// '+' → space, "%XX" → byte. Safe to run on the same buffer since the
// decoded form is never longer than the encoded form.
static void url_decode_inplace(char* s) {
  char* w = s;
  for (const char* r = s; *r; ) {
    if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
      };
      *w++ = (char)((hex(r[1]) << 4) | hex(r[2]));
      r += 3;
    } else if (*r == '+') {
      *w++ = ' ';
      r++;
    } else {
      *w++ = *r++;
    }
  }
  *w = '\0';
}

// Render a text payload as an SVG QR code response. Shared by the fleet
// WiFi-credentials QR and the pairing-receipt QR.
static esp_err_t send_qr_svg(httpd_req_t* req, const char* payload) {
  // Use version 1-10 range (enough for short payloads, small QR)
  static constexpr int QR_MAX_VER = 10;
  uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VER)];
  uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VER)];

  if (!qrcodegen_encodeText(payload, tmp, qr,
      qrcodegen_Ecc_MEDIUM, 1, QR_MAX_VER,
      qrcodegen_Mask_AUTO, true)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "QR encode failed", -1);
  }

  int size = qrcodegen_getSize(qr);
  int border = 2;
  int total = size + border * 2;

  httpd_resp_set_type(req, "image/svg+xml");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  // Buffer SVG markup and flush in ~1KB chunks rather than one TCP write
  // per dark module — a QR can have hundreds of modules, and per-module
  // chunked writes overwhelm the ESP32 httpd. Each appended fragment is
  // at most ~48 bytes, so flush whenever fewer than 64 bytes remain.
  char buf[1024];
  int len = 0;
  auto flush = [&]() {
    if (len > 0) { httpd_resp_send_chunk(req, buf, len); len = 0; }
  };

  int n = snprintf(buf + len, sizeof(buf) - len,
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 %d %d'"
    " shape-rendering='crispEdges'><rect width='%d' height='%d' fill='#fff'/>",
    total, total, total, total);
  if (n > 0 && n < (int)(sizeof(buf) - len)) len += n;

  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (qrcodegen_getModule(qr, x, y)) {
        if ((int)sizeof(buf) - len < 64) flush();
        n = snprintf(buf + len, sizeof(buf) - len,
          "<rect x='%d' y='%d' width='1' height='1'/>", x + border, y + border);
        if (n > 0 && n < (int)(sizeof(buf) - len)) len += n;
      }
    }
  }

  if ((int)sizeof(buf) - len < 16) flush();
  n = snprintf(buf + len, sizeof(buf) - len, "</svg>");
  if (n > 0 && n < (int)(sizeof(buf) - len)) len += n;
  flush();
  return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t handle_fleet_qr(httpd_req_t* req) {
  g_health.http_requests++;

  char qs[256] = {};
  char ssid[33] = {};
  char pass[65] = {};

  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Missing query parameters", -1);
  }

  if (httpd_query_key_value(qs, "ssid", ssid, sizeof(ssid)) != ESP_OK
      || ssid[0] == '\0') {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ssid is required", -1);
  }
  httpd_query_key_value(qs, "pass", pass, sizeof(pass));

  url_decode_inplace(ssid);
  url_decode_inplace(pass);

  // The SECURACV: payload is ';'-delimited and the scanner's parser has no
  // escape sequence — credentials containing ';' would encode a QR that
  // silently truncates at scan time. Refuse with a real message instead.
  if (strchr(ssid, ';') || strchr(pass, ';')) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req,
        "Network names/passwords containing ';' can't be QR-provisioned yet",
        -1);
  }

  // Build SECURACV: payload
  char payload[256];
  snprintf(payload, sizeof(payload), "SECURACV:S:%s;P:%s;;", ssid, pass);

  return send_qr_svg(req, payload);
}

static esp_err_t handle_fleet_qr_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_fleet_qr(req);
}

// ════════════════════════════════════════════════════════════════════════════
// FLEET LAN DISCOVERY — GET /api/fleet/scan
//
// Every Canary already advertises a _securacv._tcp mDNS service with TXT
// records (device_id, name, host, fw, model) — but nothing consumed them:
// the Fleet sheet listed only ESP-NOW opera-mesh members, so two Canaries
// sharing home WiFi showed each other as "No other Canaries found yet."
// This endpoint browses that service and returns every SecuraCV device on
// the LAN, plus this device's own identity (an mDNS querier does not
// reliably see its own responder), so the UI can render the whole fleet.
//
// The browse blocks ~2 s, which must never stall the single httpd task —
// same trap as the MJPEG stream. It runs on a short-lived worker task; the
// handler serves the latest cached result immediately and kicks a refresh
// when the cache is stale. The Fleet sheet's poll picks up fresh results a
// couple seconds later.
// ════════════════════════════════════════════════════════════════════════════

static volatile bool     g_fleet_scan_busy    = false;
static uint32_t          g_fleet_scan_done_ms = 0;      // guarded by mux
static bool              g_fleet_scan_have    = false;  // guarded by mux
// Sized for the 8-device browse cap below: each entry serializes to ~200 B
// worst-case (32-char name, 30-char hostname, TXT fields), so 8 × ~200 B +
// wrapper ≈ 1.7 KB; 2560 leaves honest slack instead of truncating the whole
// result at exactly the advertised capacity.
//
// Cache + handler snapshot live in PSRAM (csi_mem.h), allocated in setup():
// 2 x 2.5 KB of internal DRAM back for the BLE budget. Both are only
// touched from task context (scan worker + the single httpd task). The
// memcpys under the mux get a little slower through the PSRAM cache
// (~2.5 KB, tens of microseconds, once per 10 s scan) — acceptable for a
// spinlock that only guards tearing. NULL (allocation failed) makes the
// endpoint answer "out of memory" instead of crashing.
static const size_t      FLEET_SCAN_CACHE_SIZE = 2560;
static char*             g_fleet_scan_cache = nullptr;  // guarded by mux
static char*             g_fleet_scan_snap  = nullptr;  // httpd task only
static portMUX_TYPE      g_fleet_scan_mux = portMUX_INITIALIZER_UNLOCKED;
static const uint32_t    FLEET_SCAN_TTL_MS = 10000;

static void fleet_scan_task(void*) {
  /* Nested scope: vTaskDelete(NULL) never returns, so JsonDocument's
   * destructor (and its heap pool) only runs if the scope closes first —
   * without it every scan leaked the doc's pool. */
  {
  JsonDocument doc;
  JsonArray arr = doc["canaries"].to<JsonArray>();

  // Blocking browse (~2 s). The mDNS component is internally thread-safe.
  int n = MDNS.queryService("securacv", "tcp");
  for (int i = 0; i < n && i < 8; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["device_id"] = MDNS.txt(i, "device_id");
    o["name"]      = MDNS.txt(i, "name");
    o["mdns_host"] = MDNS.txt(i, "host");
    o["fw"]        = MDNS.txt(i, "fw");
    o["model"]     = MDNS.txt(i, "model");
    // Device type + role (canonical TXT schema) — the SPA branches its
    // per-type wizard steps and badges off dt ("canary-vision",
    // "canary-sense", "canary-wap"); older firmware adverts return "".
    o["dt"]        = MDNS.txt(i, "dt");
    o["role"]      = MDNS.txt(i, "role");
    o["ip"]        = MDNS.address(i).toString();
    o["port"]      = MDNS.port(i);
  }

  // Heap staging (not a function-local static): keeps the one-shot task's
  // stack small and leaves nothing shared between task instances. calloc,
  // not malloc: the full buffer is memcpy'd into the cache below, and the
  // bytes past serializeJson's NUL must be zeros, not heap garbage.
  char* staging = (char*)calloc(1, FLEET_SCAN_CACHE_SIZE);
  if (staging && g_fleet_scan_cache) {
    size_t written = serializeJson(doc, staging, FLEET_SCAN_CACHE_SIZE);
    if (written >= FLEET_SCAN_CACHE_SIZE) {
      staging[FLEET_SCAN_CACHE_SIZE - 1] = '\0';
    }

    portENTER_CRITICAL(&g_fleet_scan_mux);
    memcpy(g_fleet_scan_cache, staging, FLEET_SCAN_CACHE_SIZE);
    g_fleet_scan_done_ms = millis();
    g_fleet_scan_have    = true;
    portEXIT_CRITICAL(&g_fleet_scan_mux);
  }
  free(staging);
  }  /* scope closes: doc's destructor runs BEFORE the task dies */

  __atomic_store_n(&g_fleet_scan_busy, false, __ATOMIC_RELEASE);
  vTaskDelete(NULL);
}

static esp_err_t handle_fleet_scan(httpd_req_t* req) {
  g_health.http_requests++;

  if (!g_fleet_scan_cache || !g_fleet_scan_snap) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"out of memory\"}");
  }

  // Snapshot the cache torn-free (writer runs on the scan worker task).
  // g_fleet_scan_snap is httpd-task-only: esp_http_server runs handlers
  // sequentially on one task, same reasoning as the old function-static.
  char* snap = g_fleet_scan_snap;
  bool     have;
  uint32_t done_ms;
  portENTER_CRITICAL(&g_fleet_scan_mux);
  memcpy(snap, g_fleet_scan_cache, FLEET_SCAN_CACHE_SIZE);
  have    = g_fleet_scan_have;
  done_ms = g_fleet_scan_done_ms;
  portEXIT_CRITICAL(&g_fleet_scan_mux);

  const uint32_t now = millis();
  const bool stale = !have || (uint32_t)(now - done_ms) >= FLEET_SCAN_TTL_MS;
  bool busy = __atomic_load_n(&g_fleet_scan_busy, __ATOMIC_ACQUIRE);
  if (stale && !busy) {
    __atomic_store_n(&g_fleet_scan_busy, true, __ATOMIC_RELEASE);
    // Internal-RAM stack; the task builds a small JSON doc + mDNS browse.
    if (xTaskCreate(fleet_scan_task, "fleet_scan", 6144, nullptr, 1, nullptr)
        != pdPASS) {
      __atomic_store_n(&g_fleet_scan_busy, false, __ATOMIC_RELEASE);
    } else {
      busy = true;
    }
  }

  // Wrap the cached peer list with live self-identity + scan state. The
  // self entry lets the UI always render this device even before the first
  // browse completes (and dedupe it out of the browse results).
  JsonDocument doc;
  doc["ok"] = true;
  doc["scanning"] = busy;
  doc["age_ms"] = have ? (uint32_t)(now - done_ms) : 0;
  JsonObject self = doc["self"].to<JsonObject>();
  self["device_id"] = (const char*)g_device.device_id;
  self["name"]      = setup_wizard::get_device_name() ? setup_wizard::get_device_name() : "";
  self["mdns_host"] = (const char*)g_device.mdns_hostname;
  self["fw"]        = FIRMWARE_VERSION;
  IPAddress sta_ip = WiFi.localIP();
  IPAddress ap_ip  = WiFi.softAPIP();
  self["ip"] = ((uint32_t)sta_ip != 0) ? sta_ip.toString() : ap_ip.toString();
  if (have && snap[0]) {
    JsonDocument cached;
    if (deserializeJson(cached, snap) == DeserializationError::Ok) {
      doc["canaries"] = cached["canaries"];
    }
  }
  if (!doc["canaries"].is<JsonArray>()) {
    doc["canaries"].to<JsonArray>();
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_fleet_scan_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_fleet_scan(req);
}

/* GET /api/fleet — the coarse, UNAUTHENTICATED fleet presence/health contract
 * (tvos/discovery/DISCOVERY.md). This is what the public Witness Wall emulator's
 * "Connect your fleet" reads and what the Flasher's post-flash LAN discovery
 * (witness_discover) hits to make a just-flashed Canary appear on the wall — no
 * hub required, this single AP-fronting device answers for itself.
 *
 * The wire shape is built by the ONE shared builder (fleet_selfreport.h, in
 * firmware/common, host-tested), so EVERY networked board answers byte-for-byte
 * identically — a change to the shape is a change to that one header, never a
 * per-board edit. Distinct from /api/fleet-scan above, which is the rich,
 * Bearer-gated mDNS browser for the operator UI; this one is presence-only and
 * carries no secrets and no media, safe to read anonymously.
 */
static esp_err_t handle_fleet(httpd_req_t* req) {
  g_health.http_requests++;
  FleetSelfDevice self{};
  const char* nm = setup_wizard::get_device_name();
  self.name    = (nm && nm[0]) ? nm : (const char*)g_device.device_id;
  self.product = "canary-wap";
  self.online  = 1;   // we are answering this request, so we are up
  // Honest coarse chain state: OK unless tamper is latched or a witness record
  // failed verification. No hashes, no seq internals leaked beyond the height.
  self.chain_ok     = (!g_device.tamper_active && g_health.verify_failures == 0) ? 1 : 0;
  self.chain_height = (int)(g_device.seq & 0x7fffffff);
  // When this device's key was born. Left at 0 — and so omitted entirely —
  // until this Canary has met a believable clock; see birth_day.h.
  self.born_day   = g_device.born_day;
  self.born_exact = g_device.born_exact ? 1 : 0;
  // Sized by the shared macro for the WORST case: a stored 32-byte name of
  // all-escaping bytes (the rename path bounds length, not content) expands
  // 6x and is written twice — a smaller fixed buffer would truncate that
  // accepted name into invalid JSON served with a 200 (Codex P2 on #1226).
  char body[FLEET_SELFREPORT_BODY_CAP(setup_wizard::DEVICE_NAME_MAX, 16)];
  fleet_selfreport_build(body, sizeof(body), &self);
  return http_send_json(req, body);
}

/* OPTIONS /api/fleet — CORS preflight (DISCOVERY.md). A same-origin or simple
 * cross-origin GET doesn't trigger a preflight, but answering it keeps stricter
 * clients happy. http_send_json already sends Access-Control-Allow-Origin: *. */
static esp_err_t handle_fleet_options(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, "", 0);
}

/* Pairing QR: SVG QR of the compact provisioning receipt, scanned by the
 * Canary Vision companion app's "Scan pairing QR" fallback.
 *
 * The payload carries the API token, so this endpoint demands the same
 * auth as the receipt's Bearer path — the QR is something an already-
 * authenticated operator deliberately shows to their own phone, never an
 * anonymous read. Only the three fields the app's scanner consumes are
 * encoded ({device_id, base_url, token}); the slim payload keeps the QR
 * at a low version so phone cameras lock on quickly.
 */
static esp_err_t handle_pairing_qr(httpd_req_t* req) {
  g_health.http_requests++;
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;

  // Prefer the home-LAN mDNS name (stable across DHCP leases); fall back
  // to the AP address while the device is still in setup mode.
  char base_host[56];
  if (WiFi.isConnected() && g_device.mdns_hostname[0] != '\0') {
    snprintf(base_host, sizeof(base_host), "%s.local", g_device.mdns_hostname);
  } else {
    snprintf(base_host, sizeof(base_host), "%s",
             WiFi.softAPIP().toString().c_str());
  }

  char payload[224];
  int n = snprintf(payload, sizeof(payload),
    "{\"device_id\":\"%s\",\"base_url\":\"%s://%s\",\"token\":\"%s\"}",
    g_device.device_id,
    g_tls_enabled ? "https" : "http",
    base_host,
    g_device.api_token_str);
  if (n < 0 || (size_t)n >= sizeof(payload)) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Failed to build pairing payload");
  }

  return send_qr_svg(req, payload);
}

/* OS connectivity-probe handler (Apple / Android / Windows captive checks).
 *
 * These probes decide whether a phone *stays* on our setup AP, and the right
 * answer is platform-specific. The full rationale and the per-platform policy
 * (which path → which kind/body) live in captive_probe.h and are exercised by
 * the host tests in tests_host/test_captive_probe.cpp. This handler is just
 * the httpd glue that turns captive_probe::respond()'s descriptor into an HTTP
 * response:
 *
 *   • Apple   → 200 + the instruction HTML (pops the Captive Network Assistant
 *     sheet; never Apple's "<TITLE>Success</TITLE>" token, and never the
 *     wizard SPA — captive mini-browsers can't run it and show a blank screen).
 *   • Android → 204 No Content (marks the AP validated; no sheet, no cellular
 *     fallback, no disconnect).
 *   • Windows → the exact NCSI success body.
 *
 * Privacy: no outbound bytes — every response is served from the device.
 */
static esp_err_t handle_captive_probe(httpd_req_t* req) {
  g_health.http_requests++;
  const captive_probe::ProbeResponse r = captive_probe::respond(req->uri);
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

  if (r.kind == captive_probe::ProbeKind::AndroidNoContent) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
  }
  if (r.kind == captive_probe::ProbeKind::WindowsNcsiBody) {
    httpd_resp_set_type(req, r.content_type);
    return httpd_resp_send(req, r.body, HTTPD_RESP_USE_STRLEN);
  }
  // Apple instruction page (also the safe default for any unmatched probe).
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, r.content_type);
  return httpd_resp_send(req, CAPTIVE_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

// ════════════════════════════════════════════════════════════════════════════
// QR-CODE WIFI PROVISIONING (camera scans QR from phone)
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_QR_PROVISION

static volatile bool     g_qr_scan_success = false;
static char              g_qr_scanned_ssid[33] = {};
static char              g_qr_scan_error[64] = {};
static char              g_qr_scan_token[csi_integration::PAIR_TOKEN_HEX_LEN + 1] = {};
static TaskHandle_t      g_qr_scan_task = nullptr;
static constexpr uint32_t QR_SCAN_TIMEOUT_MS = 60000;

static void qr_scan_task_fn(void* param) {
  (void)param;

  sensor_t* sensor = esp_camera_sensor_get();
  int orig_framesize = sensor ? sensor->status.framesize : -1;
  if (sensor) sensor->set_framesize(sensor, FRAMESIZE_QVGA);

  bool scanner_ready = qr_scanner::init();
  if (!scanner_ready) {
    strncpy(g_qr_scan_error, "Scanner init failed", sizeof(g_qr_scan_error));
    g_qr_scan_active = false;
    if (g_qr_auto_scan) g_qr_auto_next_ms = millis() + 5000;
    g_qr_auto_scan = false;
    if (sensor && orig_framesize >= 0)
      sensor->set_framesize(sensor, (framesize_t)orig_framesize);
    __atomic_store_n(&g_qr_scan_task, (TaskHandle_t) nullptr, __ATOMIC_RELEASE);
    vTaskDelete(nullptr);
    return;
  }

  uint32_t start = millis();
  char payload[512];

  while (g_qr_scan_active) {
    if (millis() - start > QR_SCAN_TIMEOUT_MS) {
      // The boot scan-to-join loop restarts quietly (scanning is the safe
      // idle); only a phone-session scan reports its window timing out.
      if (!g_qr_auto_scan)
        strncpy(g_qr_scan_error, "timeout", sizeof(g_qr_scan_error));
      break;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    int result = qr_scanner::scan_frame(fb, payload, sizeof(payload));
    esp_camera_fb_return(fb);

    if (result == 1) {
      char ssid[33] = {};
      char pass[65] = {};
      char qr_token[csi_integration::PAIR_TOKEN_HEX_LEN + 1] = {};
      bool parsed = false;
      bool fatal = false;   // stop this scan window vs keep watching
      bool hub_saved = false;

      // Shared grammar first (provision_qr.h): SCV1 is what the display
      // mints; it also owns the modern WIFI: dialects. Legacy SECURACV:
      // stays below for old wizards.
      securacv::qr::Provision prov;
      const securacv::qr::Parse pr =
          securacv::qr::parse(payload, strlen(payload), prov);
      if (pr == securacv::qr::Parse::Ok) {
        const time_t now_epoch = time(nullptr);
        if (!prov.wifi_only && prov.expires_at > 0 &&
            now_epoch > 1700000000 && (int64_t)now_epoch > prov.expires_at) {
          // Fail fast on a stale code (checked before any join attempt);
          // the boot scan keeps watching for a fresh one.
          strncpy(g_qr_scan_error, "Code expired - make a new one",
                  sizeof(g_qr_scan_error));
          audible_chirp::play_pattern(audible_chirp::PATTERN_ERROR);
          fatal = !g_qr_auto_scan;
        } else if (prov.wifi_only && !g_qr_auto_scan &&
                   !csi_integration::pair_token_valid(g_qr_scan_token)) {
          // A phone-session scan keeps its session gate for plain wifi
          // codes; the boot scan lets them in — trust still lands on the
          // display's one-tap blessing, never here.
          strncpy(g_qr_scan_error, "Session token expired",
                  sizeof(g_qr_scan_error));
          fatal = true;
        } else {
          parsed = true;
          strlcpy(ssid, prov.ssid, sizeof(ssid));
          strlcpy(pass, prov.pass, sizeof(pass));
          if (!prov.wifi_only && prov.host[0]) {
            // The display told us where the hub lives: point the MQTT
            // bridge there and re-init (idempotent) so the fleet sees
            // this canary the moment WiFi comes up — the display's
            // "it's in the fleet" celebration keys on that.
            csi_mqtt::Config mc;
            if (csi_mqtt::config_load(&mc)) {
              strlcpy(mc.host, prov.host, sizeof(mc.host));
              mc.port = prov.port;
              mc.enabled = true;
              if (csi_mqtt::config_save(mc)) {
                char pubkey_hex[65];
                hex_to_str(pubkey_hex, g_device.pubkey, 32);
                csi_mqtt::init(g_device.device_id, FIRMWARE_VERSION,
                               pubkey_hex);
                hub_saved = true;
              }
            }
          }
        }
        memset(&prov, 0, sizeof(prov));  // held wifi credentials
      } else if (pr == securacv::qr::Parse::Malformed) {
        // Ours but broken (or a hostile over-cap field): say so and keep
        // watching — a garbled code must never end the scan (the Wyze
        // silent-forever-loop lesson, inverted honestly).
        strncpy(g_qr_scan_error, "Code hard to read - hold steady",
                sizeof(g_qr_scan_error));
      } else if (qr_scanner::parse_securacv(payload, ssid, sizeof(ssid),
                                            pass, sizeof(pass),
                                            qr_token, sizeof(qr_token))) {
        parsed = true;
        const char* tok = qr_token[0] ? qr_token : g_qr_scan_token;
        if (!csi_integration::pair_token_valid(tok)) {
          strncpy(g_qr_scan_error, "Invalid token in QR", sizeof(g_qr_scan_error));
          memset(pass, 0, sizeof(pass));
          parsed = false;
          fatal = true;
        }
      }
      // (Foreign codes — someone's wallpaper — fall through unparsed and
      // the scan keeps watching.)

      if (parsed && ssid[0]) {
        strncpy(g_wifi_creds.ssid, ssid, sizeof(g_wifi_creds.ssid) - 1);
        strncpy(g_wifi_creds.password, pass, sizeof(g_wifi_creds.password) - 1);
        g_wifi_creds.configured = true;
        g_wifi_creds.enabled = true;
        wifi_save_credentials();
        g_wifi_status.last_fail_reason[0] = '\0';
        wifi_connect_to_home();

        strncpy(g_qr_scanned_ssid, ssid, sizeof(g_qr_scanned_ssid) - 1);
        g_qr_scan_success = true;
        // The "for sure it saw it" answer: an ascending chirp (or LED
        // blink on silent hardware) the moment credentials are accepted.
        audible_chirp::play_pattern(audible_chirp::PATTERN_SUCCESS);
        log_health(SCV_LOG_INFO, SCV_CAT_NETWORK,
                   hub_saved ? "WiFi + hub applied via QR scan"
                             : "WiFi credentials applied via QR scan",
                   ssid);
      }

      memset(pass, 0, sizeof(pass));
      memset(payload, 0, sizeof(payload));
      if (parsed || fatal) break;
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }

  qr_scanner::deinit();
  if (sensor && orig_framesize >= 0)
    sensor->set_framesize(sensor, (framesize_t)orig_framesize);

  g_qr_scan_active = false;
  // The breather between windows starts when the window ENDS, not when it
  // began (review catch: a start-anchored cooldown is long expired after a
  // 60 s window, and the camera would scan back-to-back forever).
  if (g_qr_auto_scan) g_qr_auto_next_ms = millis() + 5000;
  g_qr_auto_scan = false;
  // Release-store: the start handler and the auto tick poll this handle
  // from other tasks/cores (review catch — repo atomic convention).
  __atomic_store_n(&g_qr_scan_task, (TaskHandle_t) nullptr, __ATOMIC_RELEASE);
  vTaskDelete(nullptr);
}

// Boot scan-to-join tick (called every loop pass while FEATURE_QR_PROVISION):
// an unprovisioned canary with a usable camera runs 60 s scan windows with a
// short breather, forever, until WiFi is configured — power it on, point it
// at the display's add-a-canary code, done. A phone captive-portal session
// can still run the classic wizard at any time (its start handler simply
// takes over; this tick stands down while any scan or peek is live).
static void qr_auto_scan_tick(uint32_t now) {
  if (g_wifi_creds.configured) return;
  if (g_qr_scan_active ||
      __atomic_load_n(&g_qr_scan_task, __ATOMIC_ACQUIRE) != nullptr)
    return;
#if FEATURE_CAMERA_PEEK
  if (g_peek_active) return;
#endif
  if (!camera_usable()) return;
  if ((int32_t)(now - g_qr_auto_next_ms) < 0) return;
  g_qr_auto_next_ms = now + 5000;  // retry gap if the task fails to start;
                                   // the real between-window breather is
                                   // re-armed at task exit

  // Same claim-before-wake ordering as the session start handler: the
  // busy flag must be up before a standby camera is woken, or the power
  // tick can park the sensor mid-wake.
  g_qr_scan_active = true;
#if FEATURE_CAMERA_PEEK
  if (!camera_ensure_awake()) {
    g_qr_scan_active = false;
    return;
  }
#endif
  g_qr_auto_scan = true;
  g_qr_scan_success = false;
  g_qr_scan_error[0] = '\0';
  g_qr_scanned_ssid[0] = '\0';
  g_qr_scan_token[0] = '\0';
  if (xTaskCreatePinnedToCore(qr_scan_task_fn, "qr_scan", 16384, nullptr, 1,
                              &g_qr_scan_task, 0) != pdPASS) {
    g_qr_scan_active = false;
    g_qr_auto_scan = false;
  }
}

static esp_err_t handle_qr_scan_start(httpd_req_t* req) {
  g_health.http_requests++;

  if (g_qr_scan_active) {
    if (!g_qr_auto_scan) {
      return http_send_json(req, "{\"ok\":false,\"error\":\"Scan already active\"}");
    }
    // The boot scan-to-join yields to an explicit phone session: push the
    // auto tick's cooldown out first (so it can't reclaim the camera in
    // the gap), stop the window, and wait for the task to unwind.
    g_qr_auto_next_ms = millis() + 15000;
    g_qr_scan_active = false;
    // Atomic loads: the handle is nulled by the scan task on core 0 while
    // this handler polls from the httpd task (repo convention — same as
    // the provisioning gate's cross-core reads).
    for (int i = 0;
         i < 40 && __atomic_load_n(&g_qr_scan_task, __ATOMIC_ACQUIRE) != nullptr;
         i++) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (__atomic_load_n(&g_qr_scan_task, __ATOMIC_ACQUIRE) != nullptr) {
      return http_send_json(req, "{\"ok\":false,\"error\":\"Scan already active\"}");
    }
  }

#if FEATURE_CAMERA_PEEK
  if (g_peek_active) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"Camera is busy with peek stream\"}");
  }
#endif

  char body[256];
  int len = httpd_req_recv(req, body, sizeof(body) - 1);
  if (len <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    return http_send_json(req, "{\"ok\":false,\"error\":\"Missing request body\"}");
  }
  body[len] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return http_send_json(req, "{\"ok\":false,\"error\":\"Invalid JSON\"}");
  }

  const char* token = doc["token"] | "";
  if (!csi_integration::pair_token_valid(token)) {
    return http_send_json(req, "{\"ok\":false,\"code\":\"invalid_token\","
      "\"error\":\"This setup link has expired. Reconnect to the SecuraCV network to start over.\"}");
  }

  // Claim the scan BEFORE waking the camera: the loop-side power tick
  // parks an unused sensor immediately on battery, and the busy flag is
  // what marks it as in use — waking first would race the park (codex
  // P2 on #847). Every failure path below must clear the claim.
  g_qr_scan_active = true;
  g_qr_auto_scan = false;  // a phone session takes over from the boot scan

  #if FEATURE_CAMERA_PEEK
  // Explicit user action: wake a standby camera before the scan task
  // starts (a ~1 s re-init on the httpd task is fine here).
  if (!camera_ensure_awake()) {
    g_qr_scan_active = false;
    return http_send_json(req,
        "{\"ok\":false,\"error\":\"Camera did not start. Try Reinit on the Camera panel.\"}");
  }
  #endif

  g_qr_scan_success = false;
  g_qr_scan_error[0] = '\0';
  g_qr_scanned_ssid[0] = '\0';
  strncpy(g_qr_scan_token, token, sizeof(g_qr_scan_token) - 1);

  BaseType_t created = xTaskCreatePinnedToCore(
    qr_scan_task_fn, "qr_scan", 16384, nullptr, 1, &g_qr_scan_task, 0);

  if (created != pdPASS) {
    g_qr_scan_active = false;
    return http_send_json(req, "{\"ok\":false,\"error\":\"Could not start scan task\"}");
  }

  return http_send_json(req, "{\"ok\":true,\"scanning\":true}");
}

// Status/stop share the start handler's credential model: a live pair
// token (the wizard sends ?token=<hex>) or an authenticated admin
// credential. Without a gate, any on-AP peer could read the scanned SSID
// or cancel a user's in-flight QR scan — POST already required the token,
// so the read/cancel sides matching it is just closing the same door.
static bool qr_scan_request_allowed(httpd_req_t* req) {
  char qs[192];
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
    char tok[csi_integration::PAIR_TOKEN_HEX_LEN + 2];
    if (httpd_query_key_value(qs, "token", tok, sizeof(tok)) == ESP_OK &&
        csi_integration::pair_token_valid(tok)) {
      return true;
    }
  }
  return api_auth_check_optional(req, g_device.api_token_str);
}

static esp_err_t handle_qr_scan_status(httpd_req_t* req) {
  g_health.http_requests++;

  if (!qr_scan_request_allowed(req)) {
    httpd_resp_set_status(req, "403 Forbidden");
    return http_send_json(req, "{\"ok\":false,\"error\":\"forbidden\"}");
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["scanning"] = (bool)g_qr_scan_active;
  doc["success"] = (bool)g_qr_scan_success;
  if (g_qr_scanned_ssid[0])
    doc["ssid"] = g_qr_scanned_ssid;
  if (g_qr_scan_error[0])
    doc["error"] = g_qr_scan_error;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_qr_scan_stop(httpd_req_t* req) {
  g_health.http_requests++;
  if (!qr_scan_request_allowed(req)) {
    httpd_resp_set_status(req, "403 Forbidden");
    return http_send_json(req, "{\"ok\":false,\"error\":\"forbidden\"}");
  }
  g_qr_scan_active = false;
  return http_send_json(req, "{\"ok\":true}");
}

#endif // FEATURE_QR_PROVISION

// ════════════════════════════════════════════════════════════════════════════
// BLE DISCOVERY API HANDLERS (Opera/Chirp/Nearby)
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_BLE

// All three BLE Discovery handlers are Bearer/session-gated like the rest
// of the admin API. They historically shipped without a check and were only
// unreachable because they overflowed the handler table — the capacity fix
// resurrects them, so an unauthenticated LAN peer must not be able to read
// nearby-device inventory or trigger chirp broadcasts.

// GET /api/ble/status — BLE subsystem status
static esp_err_t handle_ble_status(httpd_req_t* req) {
  g_health.http_requests++;
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  String json = ble_manager::statusJson();
  return http_send_json(req, json.c_str());
}

// GET /api/nearby — Nearby Canary devices
static esp_err_t handle_ble_nearby(httpd_req_t* req) {
  g_health.http_requests++;
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  String json = ble_manager::nearbyJson();
  return http_send_json(req, json.c_str());
}

// POST /api/chirp/send — Trigger a manual chirp alert
static esp_err_t handle_ble_chirp_send(httpd_req_t* req) {
  g_health.http_requests++;
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;

  if (!ble_manager::isAvailable()) {
    return http_send_error(req, 503, "ble_unavailable");
  }

  // Parse request body for chirp type
  char content[64];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  ChirpType chirpType = CHIRP_ALERT;  // Default

  if (content_len > 0) {
    content[content_len] = '\0';
    JsonDocument doc;
    if (deserializeJson(doc, content) == DeserializationError::Ok) {
      const char* typeStr = doc["type"] | "";
      if (strcmp(typeStr, "heartbeat") == 0) chirpType = CHIRP_HEARTBEAT;
      else if (strcmp(typeStr, "tamper") == 0) chirpType = CHIRP_TAMPER;
      else if (strcmp(typeStr, "witness") == 0) chirpType = CHIRP_WITNESS;
      else if (strcmp(typeStr, "boot") == 0) chirpType = CHIRP_BOOT;
    }
  }

  // Rate limit check
  if (ble_chirp::isRateLimited()) {
    httpd_resp_set_status(req, "429 Too Many Requests");
    return http_send_json(req, "{\"ok\":false,\"error\":\"rate_limited\"}");
  }

  if (ble_manager::sendChirp(chirpType)) {
    char response[128];
    snprintf(response, sizeof(response),
      "{\"ok\":true,\"chirp_type\":\"%s\",\"chirps_sent\":%u}",
      chirpTypeName(chirpType), ble_chirp::getChirpsSent());
    return http_send_json(req, response);
  }

  return http_send_error(req, 500, "chirp_failed");
}

#endif // FEATURE_BLE

// ════════════════════════════════════════════════════════════════════════════
// API: DEVICE INFO (public, no auth required)
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_device_info(httpd_req_t* req) {
  g_health.http_requests++;

  const char* dev_name = setup_wizard::get_device_name();
  char json[768];
  // Privacy (Invariant III): salted pseudonym, never the raw MAC.
  char hw_token[device_pseudonym::HEX_LEN + 1];
  if (!device_pseudonym::device_id_hex(hw_token, sizeof(hw_token))) hw_token[0] = '\0';
  snprintf(json, sizeof(json),
    "{"
    "\"device_id\":\"%s\","
    "\"device_name\":\"%s\","
    "\"mdns_host\":\"%s\","
    "\"firmware\":\"%s\","
    "\"pubkey_fp\":\"%s\","
    "\"hw_token\":\"%s\","
    "\"uptime_ms\":%lu,"
    "\"chain_length\":%lu,"
    // When this device's KEY was born, in days since the Unix epoch — a fact
    // about the Canary, not about whoever paired it. `born_day` is 0 until the
    // device has met a believable clock, and `born_exact` false means the day
    // is when it was first DATED rather than born, so a reader must not call
    // it a birthday. A day carries no time of day, on purpose (birth_day.h).
    "\"born_day\":%lu,"
    "\"born_exact\":%s,"
    "\"auth_required\":true,"
    "\"tls_enabled\":%s,"
    "\"provisioning_gate\":\"physical_button\""
    "}",
    g_device.device_id,
    dev_name ? dev_name : "",
    g_device.mdns_hostname,
    FIRMWARE_VERSION,
    g_device.fingerprint_hex,
    hw_token,
    (unsigned long)millis(),
    (unsigned long)g_device.seq,
    (unsigned long)g_device.born_day,
    g_device.born_exact ? "true" : "false",
    g_tls_enabled ? "true" : "false"
  );

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, json);
}

// ════════════════════════════════════════════════════════════════════════════
// API: PROVISIONING RECEIPT (physical gate or Bearer auth)
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t send_provisioning_receipt(httpd_req_t* req) {
  char json[1024];
  // Privacy (Invariant III): salted pseudonym, never the raw MAC.
  char hw_token[device_pseudonym::HEX_LEN + 1];
  if (!device_pseudonym::device_id_hex(hw_token, sizeof(hw_token))) hw_token[0] = '\0';
  snprintf(json, sizeof(json),
    "{\n"
    "  \"device_id\": \"%s\",\n"
    "  \"base_url\": \"%s://%s\",\n"
    "  \"token\": \"%s\",\n"
    "  \"pubkey_fp\": \"%s\",\n"
    "  \"firmware\": \"%s\",\n"
    "  \"hw_token\": \"%s\",\n"
    "  \"ap_ssid\": \"%s\",\n"
    "  \"ap_password\": \"%s\",\n"
    "  \"tls_cert_fp\": \"%s\",\n"
    "  \"provisioned_at\": \"boot:%lu\"\n"
    "}",
    g_device.device_id,
    g_tls_enabled ? "https" : "http",
    WiFi.softAPIP().toString().c_str(),
    g_device.api_token_str,
    g_device.fingerprint_hex,
    FIRMWARE_VERSION,
    hw_token,
    g_device.ap_ssid,
    g_device.ap_password,
    g_tls_cert_fp_hex,
    (unsigned long)g_device.boot_count
  );

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, json);
}

static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {
  g_health.http_requests++;

  // If authenticated with valid Bearer token, always serve (for SAP re-sync)
  if (api_auth_check_optional(req, g_device.api_token_str)) {
    return send_provisioning_receipt(req);
  }

  // No valid token — check physical gate
  if (!provisioning_gate_is_open()) {
    // Derive the advertised TTL from the constant so the 403 contract
    // and the gate behavior can never drift apart.
    const unsigned long ttl_s = (unsigned long)(PROVISIONING_GATE_TTL_MS / 1000);
    char body[256];
    int n = snprintf(body, sizeof(body),
      "{\"error\":\"physical_confirmation_required\","
      "\"hint\":\"Press the BOOT button on the device to reveal the provisioning receipt.\","
      "\"button\":\"BOOT (short tap, then poll within %lu seconds)\","
      "\"gate_ttl_seconds\":%lu}",
      ttl_s, ttl_s);
    if (n < 0 || (size_t)n >= sizeof(body)) {
      // Truncation guard: a future expansion of the body would otherwise
      // ship malformed JSON. Fall back to a 500 rather than lie.
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "Failed to build provisioning gate response");
    }
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
  }

  // Gate is open — serve receipt and close gate
  char session_hex[csi_integration::SESSION_COOKIE_HEX_LEN + 1];
  if (csi_integration::session_issue(session_hex, sizeof(session_hex))) {
    char cookie_hdr[160];
    int cookie_len = snprintf(cookie_hdr, sizeof(cookie_hdr),
      "cv_session=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=86400",
      session_hex);
    if (cookie_len > 0 && (size_t)cookie_len < sizeof(cookie_hdr)) {
      httpd_resp_set_hdr(req, "Set-Cookie", cookie_hdr);
    }
  }
  esp_err_t result = send_provisioning_receipt(req);
  __atomic_store_n(&g_provisioning_gate_opened_at, 0, __ATOMIC_RELAXED);
  Serial.println("[AUTH] Provisioning receipt served. Gate closed.");
  log_health(SCV_LOG_INFO, SCV_CAT_AUTH, "Provisioning receipt served via HTTPS", nullptr);
  return result;
}

// ════════════════════════════════════════════════════════════════════════════
// HTTP → HTTPS REDIRECT (port 80)
// ════════════════════════════════════════════════════════════════════════════

#if SECURACV_HAS_HTTPS_SERVER
static esp_err_t handle_https_redirect(httpd_req_t* req) {
  char location[128];
  snprintf(location, sizeof(location), "https://%s%s",
           WiFi.softAPIP().toString().c_str(),
           req->uri);
  httpd_resp_set_status(req, "301 Moved Permanently");
  httpd_resp_set_hdr(req, "Location", location);
  return httpd_resp_sendstr(req, "Redirecting to HTTPS...");
}
#endif // SECURACV_HAS_HTTPS_SERVER

// ════════════════════════════════════════════════════════════════════════════
// HTTP SERVER SETUP (with optional TLS)
// ════════════════════════════════════════════════════════════════════════════

// Register all route handlers on a given server handle
static void register_api_routes(httpd_handle_t server);

// ════════════════════════════════════════════════════════════════════════════
// API: IDENTIFY — "locate this device" (Philips Hue identify analogue)
//
// Flashes LED_BUILTIN in a distinct triple-blink + plays the "I'm here" chirp
// for a bounded window so a user onboarding several Canaries can physically
// pick out exactly which box is responding. Fully non-blocking: a millis-based
// scheduler re-arms PATTERN_IDENTIFY from loop() until the window expires, so
// the HTTP server and witness loop stay responsive the whole time.
// ════════════════════════════════════════════════════════════════════════════

static uint32_t g_identify_until_ms    = 0;   // 0 = inactive
static uint32_t g_identify_next_rep_ms = 0;

static void start_identify(uint32_t duration_ms) {
  if (duration_ms < 1000)  duration_ms = 1000;
  if (duration_ms > 60000) duration_ms = 60000;
  g_identify_until_ms    = millis() + duration_ms;
  g_identify_next_rep_ms = millis();   // fire the first cycle immediately
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "Identify requested", g_device.mdns_hostname);
}

// Drive from loop(). Re-arms the identify pattern every ~1.5s while active.
static void identify_tick() {
  if (g_identify_until_ms == 0) return;
  uint32_t now = millis();
  if ((int32_t)(now - g_identify_until_ms) >= 0) {  // window elapsed
    g_identify_until_ms = 0;
    return;
  }
#if FEATURE_AUDIBLE_CHIRP
  if ((int32_t)(now - g_identify_next_rep_ms) >= 0 && !audible_chirp::is_playing()) {
    audible_chirp::play_pattern(audible_chirp::PATTERN_IDENTIFY);
    g_identify_next_rep_ms = now + 1500;
  }
#endif
}

static esp_err_t handle_identify(httpd_req_t* req) {
  if (!api_auth_check_or_query(req, g_device.api_token_str)) return ESP_OK;
  g_health.http_requests++;

  uint32_t duration_ms = 15000;  // default ~15s, like Hue's identify
  char content[128] = {0};
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret > 0) {
    JsonDocument body;
    if (deserializeJson(body, content) == DeserializationError::Ok && body["duration_ms"].is<uint32_t>()) {
      duration_ms = body["duration_ms"].as<uint32_t>();
    }
  }

  start_identify(duration_ms);

  JsonDocument doc;
  doc["ok"] = true;
  doc["duration_ms"] = (g_identify_until_ms > millis()) ? (g_identify_until_ms - millis()) : 0;
#if FEATURE_AUDIBLE_CHIRP
  doc["visual_only"] = audible_chirp::is_visual_only();
#else
  doc["visual_only"] = true;
#endif
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// mDNS broker gossip (fleet self-discovery). Keep the _securacv._tcp
// "broker"/"bport" TXT records in lockstep with our ACTUAL MQTT link, so a
// freshly-plugged SecuraCV display self-configures from a broker that is
// provably reachable — and never chases a dead one. Ground truth in both
// directions: advertise the host we are connected to while the link is up;
// retract it (empty-string tombstone — the only "remove" ESPmDNS offers)
// the instant we drop. Mirrors the display's own discovery.cpp gossip
// semantics so the two interoperate byte-for-byte: the display reads
// "broker" verbatim (IP, DNS name, or resolvable *.local) and parses
// "bport" as a plain decimal, defaulting to 1883 when absent/empty.
static void mdns_sync_broker_txt() {
#if defined(FEATURE_MDNS_BROKER_GOSSIP) && FEATURE_MDNS_BROKER_GOSSIP
  csi_mqtt::Config cfg;
  const bool up = csi_mqtt::connected() && csi_mqtt::config_load(&cfg) &&
                  cfg.enabled && cfg.host[0];
  // The display rejects any broker string >= 64 bytes (a stack guard against
  // an unauthenticated LAN TXT record), so a pathological host is treated as
  // "nothing to advertise" rather than silently truncated to a wrong address.
  if (up && strlen(cfg.host) < 64) {
    MDNS.addServiceTxt("securacv", "tcp", "broker", (const char*)cfg.host);
    char p[8];
    snprintf(p, sizeof(p), "%u", (unsigned)cfg.port);
    // Cast the char[] to const char* so the three addServiceTxt overloads
    // (char*, const char*, String) don't make the call ambiguous — same
    // reason the device_id/host lines above cast.
    MDNS.addServiceTxt("securacv", "tcp", "bport", (const char*)p);
  } else {
    MDNS.addServiceTxt("securacv", "tcp", "broker", "");  // tombstone
    MDNS.addServiceTxt("securacv", "tcp", "bport", "");
  }
#endif
}

// Re-announce mDNS with the current unique hostname + TXT records and re-assert
// the canary.local catch-all. Used after a rename so the new name takes effect
// without a reboot. Mirrors the announce block in wifi_init_provisioning().
static void mdns_reannounce() {
  MDNS.end();
  if (!MDNS.begin(g_device.mdns_hostname)) {
    log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK, "mDNS re-announce failed", g_device.mdns_hostname);
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("securacv", "tcp", 80);
  MDNS.addServiceTxt("securacv", "tcp", "device_id", (const char*)g_device.device_id);
  MDNS.addServiceTxt("securacv", "tcp", "fw",        FIRMWARE_VERSION);
  MDNS.addServiceTxt("securacv", "tcp", "host",      (const char*)g_device.mdns_hostname);
  MDNS.addServiceTxt("securacv", "tcp", "name",
                     setup_wizard::get_device_name() ? setup_wizard::get_device_name() : "");
  #if defined(HARDWARE_XIAO_ESP32C3)
  MDNS.addServiceTxt("securacv", "tcp", "model", "XIAO ESP32C3");
  #else
  MDNS.addServiceTxt("securacv", "tcp", "model", "XIAO ESP32S3");
  #endif
  // Canonical fleet TXT identity (see docs/onboarding_unified_wizard.md):
  // dt is the canonical hyphenated device type the HA component and the
  // companion app key modality/wizard branches off; role separates
  // witnesses from glance surfaces (canary-display advertises "display").
  MDNS.addServiceTxt("securacv", "tcp", "dt",   "canary-wap");
  MDNS.addServiceTxt("securacv", "tcp", "role", "witness");
  mdns_sync_broker_txt();      // re-add broker/bport if the link is up (else tombstone)
  schedule_catch_all_claim();  // staggered; performed by catch_all_tick()
}

// API: rename this device after onboarding. Sets the friendly name (NVS
// "dev_name"), regenerates the unique mDNS hostname canary-<name>.local, and
// re-announces mDNS so the new name resolves without a reboot.
static esp_err_t handle_device_name(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  g_health.http_requests++;

  char content[128] = {0};
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  JsonDocument doc;
  if (ret <= 0) { doc["ok"] = false; doc["error"] = "No body"; }
  else {
    JsonDocument body;
    const char* name = nullptr;
    if (deserializeJson(body, content) == DeserializationError::Ok) {
      name = body["name"] | "";
    }
    char probe[setup_wizard::DEVICE_NAME_MAX + 1] = {0};
    if (name) sanitize_mdns_label(name, probe, sizeof(probe));
    if (!name || strlen(name) == 0 || strlen(name) > setup_wizard::DEVICE_NAME_MAX || !probe[0]) {
      doc["ok"] = false;
      doc["error"] = "Invalid name (1-32 chars, letters/digits/hyphen)";
    } else if (!setup_wizard::set_device_name(name)) {
      doc["ok"] = false;
      doc["error"] = "Could not save name";
    } else {
      generate_mdns_hostname(g_device.mdns_hostname, sizeof(g_device.mdns_hostname));
      mdns_reannounce();
      doc["ok"] = true;
      doc["device_name"] = setup_wizard::get_device_name();
      doc["mdns_host"] = g_device.mdns_hostname;
    }
  }
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ── Auth-wrapped handler helpers ──
// These wrappers add Bearer token authentication to existing handlers.

static esp_err_t handle_status_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_status(req);
}
static esp_err_t handle_chain_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_chain(req);
}
static esp_err_t handle_logs_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_logs(req);
}
static esp_err_t handle_log_ack_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_log_ack(req);
}
static esp_err_t handle_ack_all_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_ack_all(req);
}

// POST /api/logs/rotate — storage housekeeping. Witness export bundles
// accumulate at /EXPORT/bundle_<ms>.json every time the operator exports,
// and datamgmt's periodic sweep only bounds /HEALTH — so EXPORT grows
// unbounded and is what actually fills the card. This trims it to the
// newest EXPORT_KEEP_FILES via the tested count-based
// datamgmt::rotate_dir. It NEVER touches /WITNESS or /CHAIN — export
// bundles are regenerable disclosure artifacts, the sealed evidence is not
// (Invariant IV).
static esp_err_t handle_logs_rotate(httpd_req_t* req) {
  g_health.http_requests++;
  JsonDocument doc;
#if FEATURE_DATA_MGMT && FEATURE_SD_STORAGE
  if (!sd_is_available()) {
    doc["ok"] = false;
    doc["error"] = "SD card not available";
  } else {
    static const uint32_t EXPORT_KEEP_FILES = 20;  // newest N export bundles kept
    uint32_t deleted = datamgmt::rotate_dir("/EXPORT", EXPORT_KEEP_FILES);
    doc["ok"] = true;
    doc["deleted_count"] = deleted;
    doc["kept"] = EXPORT_KEEP_FILES;
    log_health(SCV_LOG_INFO, SCV_CAT_STORAGE, "Export bundles rotated", nullptr);
  }
#else
  doc["ok"] = false;
  doc["error"] = "Storage management not built into this firmware";
#endif
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}
static esp_err_t handle_logs_rotate_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_logs_rotate(req);
}

static esp_err_t handle_witness_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_witness(req);
}
static esp_err_t handle_config_get_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_config_get(req);
}
static esp_err_t handle_config_post_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_config_post(req);
}
static esp_err_t handle_export_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_export(req);
}
static esp_err_t handle_reboot_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_reboot(req);
}

static esp_err_t handle_safe_mode_retry_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_safe_mode_retry(req);
}

// WiFi-management auth wrappers. These three endpoints mutate provisioning
// state (disable WiFi, wipe NVS-stored credentials, re-enable) and are only
// meaningful AFTER first-setup. The dashboard's secureFetch already sends
// `Authorization: Bearer <api_token>` on all three, but the handlers
// themselves were originally registered without an _auth wrapper, so a
// LAN-local curl could hit /api/wifi/forget and force a re-provisioning
// without authenticating. (/api/wifi and /api/wifi/scan stay
// unauthenticated by design — both are read-only and need to work during
// captive-portal setup before any API token exists. /api/wifi/connect
// gets its own pre-credential gate from pair_token_valid as of #434.)
static esp_err_t handle_wifi_disconnect_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_wifi_disconnect(req);
}
static esp_err_t handle_wifi_forget_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_wifi_forget(req);
}
static esp_err_t handle_wifi_reconnect_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_wifi_reconnect(req);
}

// Opera-mesh auth wrappers. The /api/mesh/* family covers everything from
// status readbacks (state, peer table, recent alerts) through state-
// mutating control (enable/disable, pair init/join/confirm/cancel,
// leave, remove peer, rename). All 12 endpoints are post-setup-only —
// the mesh service doesn't start until WiFi is up — so none of them
// have a "must work pre-token" exception like /api/wifi/scan does, and
// none of them have a pre-credential gate like /api/wifi/connect's
// pair_token_valid. The dashboard's secureFetch already supplies the
// Bearer token on every call (web_ui.h:3063-3170), so wrapping mirrors
// the same plumbing-only fix as #435 for the WiFi-mgmt handlers.
static esp_err_t handle_mesh_status_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_status(req);
}
static esp_err_t handle_mesh_peers_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_peers(req);
}
static esp_err_t handle_mesh_alerts_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_alerts(req);
}
static esp_err_t handle_mesh_alerts_clear_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_alerts_clear(req);
}
static esp_err_t handle_mesh_enable_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_enable(req);
}
static esp_err_t handle_mesh_pair_start_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_pair_start(req);
}
static esp_err_t handle_mesh_pair_join_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_pair_join(req);
}
static esp_err_t handle_mesh_pair_confirm_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_pair_confirm(req);
}
static esp_err_t handle_mesh_pair_cancel_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_pair_cancel(req);
}
static esp_err_t handle_mesh_leave_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_leave(req);
}
static esp_err_t handle_mesh_remove_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_remove(req);
}
static esp_err_t handle_mesh_name_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_mesh_name(req);
}

#if FEATURE_CAMERA_PEEK
static esp_err_t handle_peek_stream_auth(httpd_req_t* req) {
  if (!api_auth_check_or_query(req, g_device.api_token_str)) return ESP_OK;
  return handle_peek_stream(req);
}
static esp_err_t handle_peek_snapshot_auth(httpd_req_t* req) {
  if (!api_auth_check_or_query(req, g_device.api_token_str)) return ESP_OK;
  return handle_peek_snapshot(req);
}
static esp_err_t handle_peek_status_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_peek_status(req);
}
static esp_err_t handle_peek_start_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_peek_start(req);
}
static esp_err_t handle_peek_stop_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_peek_stop(req);
}
static esp_err_t handle_peek_resolution_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_peek_resolution(req);
}
static esp_err_t handle_peek_sensor_get_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_peek_sensor_get(req);
}
static esp_err_t handle_peek_sensor_set_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_peek_sensor_set(req);
}
static esp_err_t handle_peek_init_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_peek_init(req);
}
#endif

#if FEATURE_VAULT_SNAPSHOT
// ════════════════════════════════════════════════════════════════════════════
// SEALED-SNAPSHOT VAULT — /api/vault/* (vault_snapshot.h)
// All state mutation goes through vault_snapshot's fail-closed setters; the
// only capture these routes can start is the explicit TEST one, and even
// that is deferred to loop() (request_capture is loop-task-only).
// ════════════════════════════════════════════════════════════════════════════

// vault_snapshot.cpp cannot include data_mgmt_api.h (it transitively pulls in
// the single-TU hardware_state.h, which defines g_hw). This sketch is the one
// TU that owns both, so it supplies the ring-rotation hook.
// Camera-manager bridges for vault_snapshot.cpp (the .ino owns both
// subsystems). Wake runs on the seal worker task where a ~1 s re-init is
// harmless; "usable" feeds the loop-side capture decision so a camera
// that is merely PARKED (standby) does not read as absent.
bool vault_camera_wake_hook(void) {
#if FEATURE_CAMERA_PEEK
  return camera_ensure_awake();
#else
  return g_camera_initialized;
#endif
}

static bool vault_camera_usable(void) {
#if FEATURE_CAMERA_PEEK
  return camera_usable();
#else
  return g_camera_initialized;
#endif
}

uint32_t vault_rotate_dir_hook(const char* dir, uint32_t keep) {
  return datamgmt::rotate_dir(dir, keep);
}

static esp_err_t handle_vault_status(httpd_req_t* req) {
  g_health.http_requests++;
  const vault_logic::VaultConfig cfg = vault_snapshot::get_config();
  char key_id[17];
  vault_snapshot::key_id_hex(key_id);

  JsonDocument doc;
  doc["ok"]         = true;
  doc["has_key"]    = vault_snapshot::has_pubkey();
  doc["key_id"]     = key_id;
  doc["t3_smoke"]   = cfg.t3_enabled;
  doc["t4_co"]      = cfg.t4_enabled;
  doc["glass"]      = cfg.glass_enabled;
  doc["motion"]     = cfg.motion_enabled;
  doc["mesh"]       = cfg.mesh_enabled;
  doc["cooldown_s"] = cfg.cooldown_s;
  doc["sealing"]    = vault_snapshot::worker_busy();
  doc["sd_ok"]      = sd_is_available();
  doc["camera_ok"]  = vault_camera_usable();
  doc["keep_files"] = (uint32_t)vault_logic::KEEP_FILES;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_vault_config(httpd_req_t* req) {
  g_health.http_requests++;
  char content[192] = {0};
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"No body\"}");
  }
  JsonDocument body;
  if (deserializeJson(body, content) != DeserializationError::Ok) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"Invalid JSON\"}");
  }

  vault_logic::VaultConfig cfg = vault_snapshot::get_config();
  if (body["t3_smoke"].is<bool>()) cfg.t3_enabled    = body["t3_smoke"].as<bool>();
  if (body["t4_co"].is<bool>())    cfg.t4_enabled    = body["t4_co"].as<bool>();
  if (body["glass"].is<bool>())    cfg.glass_enabled  = body["glass"].as<bool>();
  if (body["motion"].is<bool>())   cfg.motion_enabled = body["motion"].as<bool>();
  if (body["mesh"].is<bool>())     cfg.mesh_enabled   = body["mesh"].as<bool>();
  if (body["cooldown_s"].is<uint32_t>()) {
    // Clamp BEFORE the uint16 narrowing so an oversized value saturates
    // instead of wrapping (set_config clamps again to [10, 3600]).
    uint32_t cool = body["cooldown_s"].as<uint32_t>();
    if (cool > 3600) cool = 3600;
    cfg.cooldown_s = (uint16_t)cool;
  }

  const bool wanted_arm = cfg.t3_enabled || cfg.t4_enabled ||
                          cfg.glass_enabled || cfg.motion_enabled ||
                          cfg.mesh_enabled;
  vault_snapshot::set_config(cfg);  // clamps cooldown; forces triggers off keyless
  const vault_logic::VaultConfig applied = vault_snapshot::get_config();

  JsonDocument doc;
  doc["ok"]         = true;
  doc["t3_smoke"]   = applied.t3_enabled;
  doc["t4_co"]      = applied.t4_enabled;
  doc["glass"]      = applied.glass_enabled;
  doc["motion"]     = applied.motion_enabled;
  doc["mesh"]       = applied.mesh_enabled;
  doc["cooldown_s"] = applied.cooldown_s;
  if (wanted_arm && !vault_snapshot::has_pubkey()) {
    doc["warning"] = "Register an unlock key first — triggers stay off without one";
  }
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_vault_key_set(httpd_req_t* req) {
  g_health.http_requests++;
  char content[160] = {0};
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"No body\"}");
  }
  JsonDocument body;
  if (deserializeJson(body, content) != DeserializationError::Ok) {
    return http_send_json(req, "{\"ok\":false,\"error\":\"Invalid JSON\"}");
  }
  const char* pub = body["pubkey"];
  if (!pub || !vault_snapshot::set_pubkey_hex(pub)) {
    return http_send_json(req,
        "{\"ok\":false,\"error\":\"pubkey must be the 64-hex X25519 public key "
        "printed by unseal_snapshot.py gen-key\"}");
  }
  char key_id[17];
  vault_snapshot::key_id_hex(key_id);
  JsonDocument doc;
  doc["ok"]     = true;
  doc["key_id"] = key_id;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_vault_key_clear(httpd_req_t* req) {
  g_health.http_requests++;
  vault_snapshot::clear_pubkey();  // also forces every trigger off
  return http_send_json(req, "{\"ok\":true,\"has_key\":false}");
}

static esp_err_t handle_vault_list(httpd_req_t* req) {
  g_health.http_requests++;
  JsonDocument doc;
  doc["ok"] = true;
  doc["sd_ok"] = sd_is_available();
  JsonArray items = doc["items"].to<JsonArray>();
  if (sd_is_available()) {
    vault_snapshot::ItemInfo infos[vault_logic::KEEP_FILES + 4];
    const int n = vault_snapshot::list_items(
        infos, (int)(sizeof(infos) / sizeof(infos[0])));
    for (int i = 0; i < n; i++) {
      JsonObject it = items.add<JsonObject>();
      it["name"]    = infos[i].name;
      it["trigger"] = vault_logic::trigger_tag(
          (vault_logic::Trigger)infos[i].trigger);
      it["time_bucket"] = infos[i].time_bucket;
      it["size"]        = infos[i].size;
    }
  }
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// Shared by download + delete: pulls ?name= and rejects anything that isn't
// a well-formed vault filename (filename_parse is the traversal gate).
static bool vault_query_name(httpd_req_t* req, char* name, size_t name_len) {
  char qs[96] = {0};
  return httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK &&
         httpd_query_key_value(qs, "name", name, name_len) == ESP_OK &&
         vault_snapshot::validate_name(name);
}

static esp_err_t handle_vault_download(httpd_req_t* req) {
  g_health.http_requests++;
  char name[40] = {0};
  if (!vault_query_name(req, name, sizeof(name))) {
    httpd_resp_set_status(req, "400 Bad Request");
    return http_send_json(req, "{\"ok\":false,\"error\":\"name must be a vault filename\"}");
  }
  char path[56];
  snprintf(path, sizeof(path), "/VAULT/%s", name);
  File f = SD.open(path, FILE_READ);
  if (!f) {
    httpd_resp_set_status(req, "404 Not Found");
    return http_send_json(req, "{\"ok\":false,\"error\":\"No such sealed file\"}");
  }
  httpd_resp_set_type(req, "application/octet-stream");
  char disp[72];
  snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
  httpd_resp_set_hdr(req, "Content-Disposition", disp);
  uint8_t buf[1024];
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    if (httpd_resp_send_chunk(req, (const char*)buf, (ssize_t)n) != ESP_OK) {
      f.close();
      return ESP_FAIL;
    }
  }
  f.close();
  return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t handle_vault_item_delete(httpd_req_t* req) {
  g_health.http_requests++;
  char name[40] = {0};
  if (!vault_query_name(req, name, sizeof(name))) {
    httpd_resp_set_status(req, "400 Bad Request");
    return http_send_json(req, "{\"ok\":false,\"error\":\"name must be a vault filename\"}");
  }
  if (!vault_snapshot::delete_item(name)) {
    httpd_resp_set_status(req, "404 Not Found");
    return http_send_json(req, "{\"ok\":false,\"error\":\"No such sealed file\"}");
  }
  log_health(SCV_LOG_NOTICE, SCV_CAT_USER, "Sealed snapshot deleted", name);
  return http_send_json(req, "{\"ok\":true}");
}

static esp_err_t handle_vault_test(httpd_req_t* req) {
  g_health.http_requests++;
  // Fail the obvious gates here for an immediate answer; the authoritative
  // fail-closed decision still runs on the loop when the flag drains.
  if (!vault_snapshot::has_pubkey()) {
    return http_send_json(req,
        "{\"ok\":false,\"error\":\"No unlock key registered\"}");
  }
  if (vault_snapshot::worker_busy()) {
    return http_send_json(req,
        "{\"ok\":false,\"error\":\"A seal is already in flight\"}");
  }
  g_vault_test_pending = true;
  return http_send_json(req, "{\"ok\":true,\"queued\":true}");
}

static esp_err_t handle_vault_status_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_vault_status(req);
}
static esp_err_t handle_vault_config_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_vault_config(req);
}
static esp_err_t handle_vault_key_set_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_vault_key_set(req);
}
static esp_err_t handle_vault_key_clear_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_vault_key_clear(req);
}
static esp_err_t handle_vault_list_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_vault_list(req);
}
// Browser downloads can't set an Authorization header — accept ?token= too,
// same as the peek stream/snapshot routes.
static esp_err_t handle_vault_download_auth(httpd_req_t* req) {
  if (!api_auth_check_or_query(req, g_device.api_token_str)) return ESP_OK;
  return handle_vault_download(req);
}
static esp_err_t handle_vault_item_delete_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_vault_item_delete(req);
}
static esp_err_t handle_vault_test_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_vault_test(req);
}
#endif  /* FEATURE_VAULT_SNAPSHOT */

#if FEATURE_SYS_MONITOR
static esp_err_t handle_system_metrics_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_system_metrics(req);
}
static esp_err_t handle_diagnostics_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_diagnostics(req);
}
#endif

#if FEATURE_POWER_MONITOR
static esp_err_t handle_battery_history_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_battery_history(req);
}
#endif

#if FEATURE_ACOUSTIC_EVENTS
static esp_err_t handle_audio_status_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_audio_status(req);
}
static esp_err_t handle_audio_mute_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_audio_mute(req);
}
static esp_err_t handle_audio_selftest_get_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_audio_selftest_get(req);
}
static esp_err_t handle_audio_selftest_post_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_audio_selftest_post(req);
}
static esp_err_t handle_audio_config_get_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_audio_config_get(req);
}
static esp_err_t handle_audio_config_post_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_audio_config_post(req);
}
static esp_err_t handle_audio_transitions_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_audio_transitions(req);
}
#endif

// Register all route handlers on a given httpd server handle
static void register_api_routes(httpd_handle_t server) {
  // UI — no auth required. / serves the headline Sensing dashboard;
  // /admin keeps the legacy tabbed dashboard reachable for power-user tasks,
  // and /settings deep-links to its Settings tab.
  httpd_uri_t ui = { .uri = "/", .method = HTTP_GET, .handler = handle_ui };
  httpd_register_uri_handler(server, &ui);
  httpd_uri_t legacy_ui = { .uri = "/admin", .method = HTTP_GET, .handler = handle_legacy_ui };
  httpd_register_uri_handler(server, &legacy_ui);
  httpd_uri_t settings_ui = { .uri = "/settings", .method = HTTP_GET, .handler = handle_legacy_ui };
  httpd_register_uri_handler(server, &settings_ui);

  // Device enrollment endpoints — unauthenticated by design (pubkey +
  // fingerprint are PUBLIC data). HA's config flow pulls /api/device/enroll
  // to TOFU-pin the device's pubkey; the /enroll HTML page renders the
  // fingerprint in big monospace text for an installer to read off the
  // captive-portal page and type into HA when they want to pin manually.
  httpd_uri_t enroll_json_uri = { .uri = "/api/device/enroll", .method = HTTP_GET,
                                  .handler = device_identity_api::handle_enroll_json };
  httpd_register_uri_handler(server, &enroll_json_uri);
  httpd_uri_t enroll_html_uri = { .uri = "/enroll", .method = HTTP_GET,
                                  .handler = device_identity_api::handle_enroll_html };
  httpd_register_uri_handler(server, &enroll_html_uri);

  // Companion PWA (Web Bluetooth phone-side console). Three static assets,
  // no auth — the page itself is just HTML/JS/manifest; the BLE
  // characteristics it talks to enforce READ_ENC + READ_AUTHEN, so a
  // drive-by visitor without a bonded phone sees nothing useful.
  httpd_uri_t comp_html = { .uri = "/companion", .method = HTTP_GET, .handler = handle_companion_html };
  httpd_register_uri_handler(server, &comp_html);
  httpd_uri_t comp_sw = { .uri = "/companion-sw.js", .method = HTTP_GET, .handler = handle_companion_sw };
  httpd_register_uri_handler(server, &comp_sw);
  httpd_uri_t comp_man = { .uri = "/companion-manifest.webmanifest", .method = HTTP_GET, .handler = handle_companion_manifest };
  httpd_register_uri_handler(server, &comp_man);

  // Device info — no auth required (non-sensitive metadata)
  httpd_uri_t devinfo = { .uri = "/api/device-info", .method = HTTP_GET, .handler = handle_device_info };
  httpd_register_uri_handler(server, &devinfo);

  // Provisioning receipt — physical gate or Bearer auth
  httpd_uri_t prov = { .uri = "/api/provisioning-receipt", .method = HTTP_GET, .handler = handle_provisioning_receipt };
  httpd_register_uri_handler(server, &prov);

  // Authenticated API endpoints (Bearer token required)
  httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status_auth };
  httpd_register_uri_handler(server, &status);

#if FEATURE_SYS_MONITOR
  httpd_uri_t sys_metrics = { .uri = "/api/system", .method = HTTP_GET, .handler = handle_system_metrics_auth };
  httpd_register_uri_handler(server, &sys_metrics);

  httpd_uri_t diagnostics = { .uri = "/api/diagnostics", .method = HTTP_GET, .handler = handle_diagnostics_auth };
  httpd_register_uri_handler(server, &diagnostics);
#endif

#if FEATURE_POWER_MONITOR
  httpd_uri_t battery_history = { .uri = "/api/battery/history", .method = HTTP_GET, .handler = handle_battery_history_auth };
  httpd_register_uri_handler(server, &battery_history);
#endif

#if FEATURE_ACOUSTIC_EVENTS
  httpd_uri_t audio_status = { .uri = "/api/audio/status", .method = HTTP_GET, .handler = handle_audio_status_auth };
  httpd_register_uri_handler(server, &audio_status);

  httpd_uri_t audio_mute_uri = { .uri = "/api/audio/mute", .method = HTTP_POST, .handler = handle_audio_mute_auth };
  httpd_register_uri_handler(server, &audio_mute_uri);

  httpd_uri_t audio_st_get = { .uri = "/api/audio/selftest", .method = HTTP_GET, .handler = handle_audio_selftest_get_auth };
  httpd_register_uri_handler(server, &audio_st_get);

  httpd_uri_t audio_st_post = { .uri = "/api/audio/selftest", .method = HTTP_POST, .handler = handle_audio_selftest_post_auth };
  httpd_register_uri_handler(server, &audio_st_post);

  httpd_uri_t audio_cfg_get = { .uri = "/api/audio/config", .method = HTTP_GET, .handler = handle_audio_config_get_auth };
  httpd_register_uri_handler(server, &audio_cfg_get);

  httpd_uri_t audio_cfg_post = { .uri = "/api/audio/config", .method = HTTP_POST, .handler = handle_audio_config_post_auth };
  httpd_register_uri_handler(server, &audio_cfg_post);

  httpd_uri_t audio_trans = { .uri = "/api/audio/transitions", .method = HTTP_GET, .handler = handle_audio_transitions_auth };
  httpd_register_uri_handler(server, &audio_trans);
#endif

  httpd_uri_t chain = { .uri = "/api/chain", .method = HTTP_GET, .handler = handle_chain_auth };
  httpd_register_uri_handler(server, &chain);

  httpd_uri_t logs = { .uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs_auth };
  httpd_register_uri_handler(server, &logs);

  httpd_uri_t log_ack = { .uri = "/api/logs/*/ack", .method = HTTP_POST, .handler = handle_log_ack_auth };
  httpd_register_uri_handler(server, &log_ack);

  httpd_uri_t ack_all = { .uri = "/api/logs/ack-all", .method = HTTP_POST, .handler = handle_ack_all_auth };
  httpd_register_uri_handler(server, &ack_all);

  httpd_uri_t logs_rotate = { .uri = "/api/logs/rotate", .method = HTTP_POST, .handler = handle_logs_rotate_auth };
  httpd_register_uri_handler(server, &logs_rotate);

  httpd_uri_t witness = { .uri = "/api/witness", .method = HTTP_GET, .handler = handle_witness_auth };
  httpd_register_uri_handler(server, &witness);

  httpd_uri_t config_get = { .uri = "/api/config", .method = HTTP_GET, .handler = handle_config_get_auth };
  httpd_register_uri_handler(server, &config_get);

  httpd_uri_t config_post = { .uri = "/api/config", .method = HTTP_POST, .handler = handle_config_post_auth };
  httpd_register_uri_handler(server, &config_post);

  httpd_uri_t export_bundle = { .uri = "/api/export", .method = HTTP_POST, .handler = handle_export_auth };
  httpd_register_uri_handler(server, &export_bundle);

  httpd_uri_t reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = handle_reboot_auth };
  httpd_register_uri_handler(server, &reboot);

  httpd_uri_t safe_retry = { .uri = "/api/safe-mode/retry", .method = HTTP_POST, .handler = handle_safe_mode_retry_auth };
  httpd_register_uri_handler(server, &safe_retry);

  // CSI library integration: registers /api/csi/stream, /api/csi/window,
  // /api/events/today, /api/events/dismiss, registers the four v1 sensing
  // modules, and brings up csi_hal on this WiFi context. The api_token
  // is the Bearer token every CSI handler verifies via api_auth_check;
  // the dashboard at / bootstraps it through window.__CV_TOKEN injected
  // by handle_ui below.
  csi_integration::init(server, g_device.api_token_str);

  // Fuse CSI into RF presence: every finalized feature window feeds the
  // rf_presence FSM's motion/breathing scalars, so WiFi sensing and the
  // BLE/RF presence pipeline corroborate each other instead of running
  // as two blind silos. (This hook existed since Phase 2 but was never
  // registered — rf_presence::feed_csi_window was dead code and the RF
  // FSM never saw a single CSI window.) feed_csi_window guards its own
  // initialized/enabled state, so unconditional registration is safe.
  csi_integration::set_legacy_features_hook(
      [](const csi_features_t* f) { rf_presence::feed_csi_window(f); });

  // Optional MQTT bridge (publishes CSI events / health / chain / counts
  // to a user-supplied broker so custom_components/securacv/ in HA sees
  // live data). init() is a no-op if disabled in NVS, and idempotent —
  // re-runs whenever /api/mqtt/config POST changes the broker. We pass
  // the device id, firmware version, and pubkey hex up front so the
  // health payload is self-contained.
  char pubkey_hex[65];
  hex_to_str(pubkey_hex, g_device.pubkey, 32);
  csi_mqtt::init(g_device.device_id, FIRMWARE_VERSION, pubkey_hex);

  // Per-device Ed25519 signature service. Mounts the keypair into a
  // dedicated module so csi_mqtt (and any future signed-egress path
  // like SD-resync) can stamp chain/event/counts publishes with a
  // signature HA verifies against its pinned-pubkey trust store.
  // Must come after generate_keypair has populated g_device.{priv,pub,fp}.
  device_signature::init(g_device.privkey,
                         g_device.pubkey,
                         g_device.device_id,
                         g_device.fingerprint_hex);
}

// GET /api/selftest wrapper: while the user is on the wizard's final step
// polling the health check, hold off the deferred post-provisioning reboot
// so "Run again" keeps working. The deadline is only ARMED after setup
// completed with a live join, so this only ever widens an already-pending
// window — it never schedules a reboot that wasn't due, and never touches a
// disarmed (0) deadline (reboot_deadline_extend guards both). The selftest
// endpoint itself stays unauthenticated (the AP is its boundary, like
// /api/wifi/scan) — this wrapper adds no data path.
static esp_err_t handle_selftest_wrap(httpd_req_t* req) {
  // Compare-and-swap so this read-modify-write can't clobber a concurrent
  // update from loop() — critically, if loop() disarms the deadline (stores
  // 0) between our load and store, the CAS fails, reloads dl == 0, and the
  // guard drops out WITHOUT re-arming a reboot loop() just canceled. Only
  // extends an already-armed deadline; reboot_deadline_extend never pulls it
  // sooner, so a lost race can at worst leave a slightly longer window.
  uint32_t dl = __atomic_load_n(&g_setup_grace_reboot_at_ms, __ATOMIC_ACQUIRE);
  while (dl != 0) {
    uint32_t extended = provisioning_logic::reboot_deadline_extend(
        dl, millis(), SELFTEST_REBOOT_MIN_MS);
    if (extended == dl) break;  // already far enough out
    // desired is passed BY VALUE; on failure dl is reloaded to the current
    // value and we recompute against it.
    if (__atomic_compare_exchange_n(&g_setup_grace_reboot_at_ms, &dl, extended,
                                    /*weak=*/false, __ATOMIC_RELEASE,
                                    __ATOMIC_ACQUIRE)) {
      break;
    }
  }
  return selftest::handle_selftest(req);
}

static void start_http_server() {
  // Max URI handlers, itemized to match what the active server actually
  // registers. esp_http_server SILENTLY drops every registration past this
  // budget (ESP_ERR_HTTPD_HANDLERS_FULL, and none of the register call
  // sites check the return), so an under-count 404s whole API families —
  // which is exactly how the Presence, Household, Bluetooth-clear, Chirp
  // and BLE routes disappeared: 154 registrations against an old 123-slot
  // budget dropped the last 31. Each category is gated on the SAME feature
  // flag its registrations are, so the budget tracks the build.
  //
  // The exact per-config counts are enforced by
  //   tests_host/check_route_budget.py  (CI: firmware.yml)
  // which emulates the preprocessor for FULL/S3, DEV/S3 and FULL/C3 and
  // asserts >= 8 free slots. If it fails, RAISE a number here — never lower.
  const int base_handlers = 48;       // register_api_routes core (incl. /api/config
                                       // GET+POST) + the always-on
                                       // register_extra_routes singles (WiFi
                                       // provisioning, OTA x4, identify,
                                       // device-name, selftest, fleet/pairing QR,
                                       // fleet/scan, sys-monitor, battery)
                                       // + captive probes
  const int csi_handlers = 23;        // csi_integration::init (stream/window/events/
                                       // calibrate/settings/mqtt/tune/pair-token/…)
  const int wifi_presence_handlers = 4;   // /api/presence/{combined,wifi,wifi/start,wifi/stop}
  const int rf_presence_handlers = 7;     // /api/rf/{status,settings GET/POST,conformance,enable,disable,rotate}
  const int datamgmt_handlers = 1;        // /api/logs/rotate (registered unconditionally)
  const int household_handlers = 6;       // /api/household* + /api/presence override
  const int audible_chirp_handlers = 4;   // /api/audible-chirp{,/play,/test,/config}
#if FEATURE_ACOUSTIC_EVENTS
  const int audio_handlers = 7;       // /api/audio/{status,mute,transitions,selftest,config x2}
#else
  const int audio_handlers = 0;
#endif
#if FEATURE_CAMERA_PEEK
  const int camera_handlers = 9;      // Camera peek endpoints
#else
  const int camera_handlers = 0;
#endif
#if FEATURE_QR_PROVISION
  const int qr_handlers = 3;          // /api/wifi/qr-scan POST/GET/DELETE
#else
  const int qr_handlers = 0;
#endif
#if FEATURE_VAULT_SNAPSHOT
  const int vault_handlers = 8;       // /api/vault/{status,config,key POST+DELETE,
                                       // list,download,item,test}
#else
  const int vault_handlers = 0;
#endif
#if FEATURE_MESH_NETWORK
  const int mesh_handlers = 12;       // Mesh network (opera) endpoints
  const int chirp_handlers = 13;      // chirp_api::register_routes (/api/chirp/*)
#else
  const int mesh_handlers = 0;
  const int chirp_handlers = 0;
#endif
#if FEATURE_BLUETOOTH
  const int bluetooth_handlers = 24;  // bluetooth_api::register_routes
#else
  const int bluetooth_handlers = 0;
#endif
#if FEATURE_BLE
  const int ble_discovery_handlers = 3; // /api/ble/status, /api/nearby, /api/ble/chirp/send
#else
  const int ble_discovery_handlers = 0;
#endif
  const int fleet_handlers = 2;       // /api/fleet GET + OPTIONS (DISCOVERY.md)
  const int handler_headroom = 24;    // Reserve for future additions
  const int total_handlers = base_handlers + csi_handlers + wifi_presence_handlers
      + rf_presence_handlers + datamgmt_handlers + household_handlers
      + audible_chirp_handlers + audio_handlers + camera_handlers + qr_handlers
      + vault_handlers + mesh_handlers + chirp_handlers + bluetooth_handlers
      + ble_discovery_handlers + fleet_handlers + handler_headroom;

  // ── Start HTTPS server (port 443) if TLS cert is available ──
#if SECURACV_HAS_HTTPS_SERVER
  if (g_tls_enabled && g_tls_cert_der && g_tls_key_der) {
    httpd_ssl_config_t ssl_config = HTTPD_SSL_CONFIG_DEFAULT();
    ssl_config.servercert = g_tls_cert_der;
    ssl_config.servercert_len = g_tls_cert_der_len;
    ssl_config.prvtkey_pem = g_tls_key_der;
    ssl_config.prvtkey_len = g_tls_key_der_len;
    ssl_config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    ssl_config.httpd.stack_size = 10240;  // Larger stack for TLS + camera
    ssl_config.httpd.max_uri_handlers = total_handlers;
    ssl_config.httpd.server_port = 443;
    // Long-lived MJPEG peek streams routinely sit between sends for
    // hundreds of ms while the camera captures the next JPEG. With the
    // 5 s default a single WiFi retransmit storm or AP scan sweep was
    // enough to trip send_wait_timeout and tear the socket down ~10–15 s
    // in. The streaming loop self-throttles with vTaskDelay so a longer
    // ceiling can't cause runaway hangs — it only lets transient hiccups
    // recover instead of dropping the client.
    ssl_config.httpd.recv_wait_timeout = 30;
    ssl_config.httpd.send_wait_timeout = 30;
    ssl_config.httpd.lru_purge_enable  = true;

    if (httpd_ssl_start(&g_https_server, &ssl_config) == ESP_OK) {
      Serial.println("[HTTPS] Server started on port 443");
      log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "HTTPS server started", "port 443");

      // Register all routes on HTTPS server
      register_api_routes(g_https_server);

      // ── Start HTTP redirect server (port 80 → HTTPS) ──
      httpd_config_t redirect_config = HTTPD_DEFAULT_CONFIG();
      redirect_config.server_port = 80;
      redirect_config.uri_match_fn = httpd_uri_match_wildcard;
      // 6 captive probes + 2 fleet-discovery routes + 2 wildcard redirects,
      // plus headroom (check_route_budget.py documents this server's budget).
      redirect_config.max_uri_handlers = 12;

      if (httpd_start(&g_http_server, &redirect_config) == ESP_OK) {
        // Connectivity probes must stay on plain HTTP — the OS never sends
        // them over TLS, and a TLS redirect breaks detection. Each platform
        // gets the answer that keeps it connected (see captive_probe.h):
        // Apple → instruction page (pops the CNA sheet, never disconnects);
        // Android → 204; Windows → NCSI success strings.
        httpd_uri_t cp_apple   = { .uri = "/hotspot-detect.html",        .method = HTTP_GET, .handler = handle_captive_probe };
        httpd_register_uri_handler(g_http_server, &cp_apple);
        httpd_uri_t cp_apple2  = { .uri = "/library/test/success.html",  .method = HTTP_GET, .handler = handle_captive_probe };
        httpd_register_uri_handler(g_http_server, &cp_apple2);
        httpd_uri_t cp_android = { .uri = "/generate_204",              .method = HTTP_GET, .handler = handle_captive_probe };
        httpd_register_uri_handler(g_http_server, &cp_android);
        httpd_uri_t cp_android2= { .uri = "/gen_204",                   .method = HTTP_GET, .handler = handle_captive_probe };
        httpd_register_uri_handler(g_http_server, &cp_android2);
        httpd_uri_t cp_win     = { .uri = "/connecttest.txt",           .method = HTTP_GET, .handler = handle_captive_probe };
        httpd_register_uri_handler(g_http_server, &cp_win);
        httpd_uri_t cp_win2    = { .uri = "/ncsi.txt",                  .method = HTTP_GET, .handler = handle_captive_probe };
        httpd_register_uri_handler(g_http_server, &cp_win2);
        // Fleet discovery must stay on plain HTTP too (Codex P2 on #1226):
        // the documented path is http://canary.local/api/fleet — exactly what
        // the Flasher's witness_discover and a LAN browser GET — and a redirect
        // to self-signed HTTPS fails those clients on every TLS-enabled device.
        // esp_http_server matches in registration order, so these two must be
        // registered BEFORE the /* wildcard redirects below. Public, coarse
        // presence-only (documented in check_route_security.py's allowlist).
        httpd_uri_t http_fleet_get = { .uri = "/api/fleet", .method = HTTP_GET, .handler = handle_fleet };
        httpd_register_uri_handler(g_http_server, &http_fleet_get);
        httpd_uri_t http_fleet_opt = { .uri = "/api/fleet", .method = HTTP_OPTIONS, .handler = handle_fleet_options };
        httpd_register_uri_handler(g_http_server, &http_fleet_opt);
        // Everything else redirects to HTTPS
        httpd_uri_t redirect_all = { .uri = "/*", .method = HTTP_GET, .handler = handle_https_redirect };
        httpd_register_uri_handler(g_http_server, &redirect_all);
        httpd_uri_t redirect_post = { .uri = "/*", .method = HTTP_POST, .handler = handle_https_redirect };
        httpd_register_uri_handler(g_http_server, &redirect_post);
        Serial.println("[HTTP]  Redirect server on port 80 -> HTTPS (captive portal kept on HTTP)");
      }

      goto register_extra_routes;
    } else {
      Serial.println("[HTTPS] Server start FAILED — falling back to HTTP");
      log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK, "HTTPS start failed, using HTTP", nullptr);
      g_tls_enabled = false;
    }
  }
#endif // SECURACV_HAS_HTTPS_SERVER

  // ── Fallback: HTTP-only mode (port 80) ──
  {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.max_uri_handlers = total_handlers;
    // See HTTPS-config sibling above — same rationale. The MJPEG stream
    // handler dominates this httpd's traffic in HTTP-only deployments,
    // so the 5 s default was the single biggest cause of the "drops at
    // 10-15 s" symptom users saw on the fleet manager peek tab.
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.lru_purge_enable  = true;

    if (httpd_start(&g_http_server, &config) != ESP_OK) {
      log_health(SCV_LOG_ERROR, SCV_CAT_NETWORK, "HTTP server start failed", nullptr);
      return;
    }

    Serial.println("[HTTP]  Server started on port 80 (no TLS)");
    Serial.println("[WARN] API traffic is NOT encrypted. Use only in trusted environments.");
    log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK, "HTTP-only mode (no TLS)", nullptr);

    // Register all routes on HTTP server
    register_api_routes(g_http_server);
  }

register_extra_routes:
  // Get the active server handle for additional route registration
  httpd_handle_t active_server = g_https_server ? g_https_server : g_http_server;

  // WiFi provisioning endpoints (auth required)
  httpd_uri_t wifi_status = { .uri = "/api/wifi", .method = HTTP_GET, .handler = handle_wifi_status };
  httpd_register_uri_handler(active_server, &wifi_status);

  // Signed pull-OTA (status poll + check / install / settings)
  httpd_uri_t ota_status = { .uri = "/api/ota/status", .method = HTTP_GET, .handler = handle_ota_status };
  httpd_register_uri_handler(active_server, &ota_status);

  httpd_uri_t ota_check = { .uri = "/api/ota/check", .method = HTTP_POST, .handler = handle_ota_check };
  httpd_register_uri_handler(active_server, &ota_check);

  httpd_uri_t ota_install = { .uri = "/api/ota/install", .method = HTTP_POST, .handler = handle_ota_install };
  httpd_register_uri_handler(active_server, &ota_install);

  httpd_uri_t ota_cfg = { .uri = "/api/ota/config", .method = HTTP_POST, .handler = handle_ota_config };
  httpd_register_uri_handler(active_server, &ota_cfg);

  httpd_uri_t wifi_scan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan };
  httpd_register_uri_handler(active_server, &wifi_scan);

  // Setup-wizard-only token re-issue (404s outside first-boot setup; see
  // handler doc block for the security posture).
  httpd_uri_t wifi_pair_token = { .uri = "/api/wifi/pair-token", .method = HTTP_GET, .handler = handle_wifi_pair_token };
  httpd_register_uri_handler(active_server, &wifi_pair_token);

  // Standalone mode: pairing-token-gated like /api/wifi/connect.
  httpd_uri_t wifi_ap_only = { .uri = "/api/wifi/ap-only", .method = HTTP_POST, .handler = handle_wifi_ap_only };
  httpd_register_uri_handler(active_server, &wifi_ap_only);

  httpd_uri_t wifi_connect = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = handle_wifi_connect };
  httpd_register_uri_handler(active_server, &wifi_connect);

  httpd_uri_t wifi_disconnect = { .uri = "/api/wifi/disconnect", .method = HTTP_POST, .handler = handle_wifi_disconnect_auth };
  httpd_register_uri_handler(active_server, &wifi_disconnect);

  httpd_uri_t wifi_forget = { .uri = "/api/wifi/forget", .method = HTTP_POST, .handler = handle_wifi_forget_auth };
  httpd_register_uri_handler(active_server, &wifi_forget);

  httpd_uri_t wifi_reconnect = { .uri = "/api/wifi/reconnect", .method = HTTP_POST, .handler = handle_wifi_reconnect_auth };
  httpd_register_uri_handler(active_server, &wifi_reconnect);

  // Identify ("blink + chirp this device"). Auth via Bearer header OR ?token=
  // so the fleet manager can locate a device without custom headers.
  httpd_uri_t identify = { .uri = "/api/identify", .method = HTTP_POST, .handler = handle_identify };
  httpd_register_uri_handler(active_server, &identify);

  // Rename this device → canary-<name>.local (Bearer-gated).
  httpd_uri_t device_name = { .uri = "/api/device-name", .method = HTTP_POST, .handler = handle_device_name };
  httpd_register_uri_handler(active_server, &device_name);

  // Wizard pre-flight self-test (no auth — must be reachable on AP
  // before any post-pair token exists, identical to /api/wifi/scan).
  httpd_uri_t selftest = { .uri = "/api/selftest", .method = HTTP_GET, .handler = handle_selftest_wrap };
  httpd_register_uri_handler(active_server, &selftest);

#if FEATURE_QR_PROVISION
  httpd_uri_t qr_start  = { .uri = "/api/wifi/qr-scan", .method = HTTP_POST,   .handler = handle_qr_scan_start };
  httpd_uri_t qr_status = { .uri = "/api/wifi/qr-scan", .method = HTTP_GET,    .handler = handle_qr_scan_status };
  httpd_uri_t qr_stop   = { .uri = "/api/wifi/qr-scan", .method = HTTP_DELETE, .handler = handle_qr_scan_stop };
  httpd_register_uri_handler(active_server, &qr_start);
  httpd_register_uri_handler(active_server, &qr_status);
  httpd_register_uri_handler(active_server, &qr_stop);
#endif

  // Connectivity-probe URLs, answered per-platform to keep the phone on the
  // AP (see captive_probe.h): Apple → instruction page (pops the CNA sheet,
  // never disconnects); Android → 204; Windows → NCSI success strings.
  // (In HTTPS mode these also live on the port-80 redirect server above; the
  // OS only ever probes over plain HTTP.)
  httpd_uri_t captive_apple   = { .uri = "/hotspot-detect.html",       .method = HTTP_GET, .handler = handle_captive_probe };
  httpd_register_uri_handler(active_server, &captive_apple);

  httpd_uri_t captive_apple2  = { .uri = "/library/test/success.html", .method = HTTP_GET, .handler = handle_captive_probe };
  httpd_register_uri_handler(active_server, &captive_apple2);

  httpd_uri_t captive_android = { .uri = "/generate_204",             .method = HTTP_GET, .handler = handle_captive_probe };
  httpd_register_uri_handler(active_server, &captive_android);

  httpd_uri_t captive_android2= { .uri = "/gen_204",                  .method = HTTP_GET, .handler = handle_captive_probe };
  httpd_register_uri_handler(active_server, &captive_android2);

  httpd_uri_t captive_win     = { .uri = "/connecttest.txt",          .method = HTTP_GET, .handler = handle_captive_probe };
  httpd_register_uri_handler(active_server, &captive_win);

  httpd_uri_t captive_win2    = { .uri = "/ncsi.txt",                 .method = HTTP_GET, .handler = handle_captive_probe };
  httpd_register_uri_handler(active_server, &captive_win2);

#if FEATURE_CAMERA_PEEK
  // Camera peek endpoints (auth required for all peek operations)
  httpd_uri_t peek_start = { .uri = "/api/peek/start", .method = HTTP_POST, .handler = handle_peek_start_auth };
  httpd_register_uri_handler(active_server, &peek_start);

  httpd_uri_t peek_stream = { .uri = "/api/peek/stream", .method = HTTP_GET, .handler = handle_peek_stream_auth };
  httpd_register_uri_handler(active_server, &peek_stream);

  httpd_uri_t peek_snapshot = { .uri = "/api/peek/snapshot", .method = HTTP_GET, .handler = handle_peek_snapshot_auth };
  httpd_register_uri_handler(active_server, &peek_snapshot);

  httpd_uri_t peek_stop = { .uri = "/api/peek/stop", .method = HTTP_POST, .handler = handle_peek_stop_auth };
  httpd_register_uri_handler(active_server, &peek_stop);

  httpd_uri_t peek_status = { .uri = "/api/peek/status", .method = HTTP_GET, .handler = handle_peek_status_auth };
  httpd_register_uri_handler(active_server, &peek_status);

  httpd_uri_t peek_resolution = { .uri = "/api/peek/resolution", .method = HTTP_POST, .handler = handle_peek_resolution_auth };
  httpd_register_uri_handler(active_server, &peek_resolution);

  httpd_uri_t peek_sensor_get = { .uri = "/api/peek/sensor", .method = HTTP_GET, .handler = handle_peek_sensor_get_auth };
  httpd_register_uri_handler(active_server, &peek_sensor_get);

  httpd_uri_t peek_sensor_set = { .uri = "/api/peek/sensor", .method = HTTP_POST, .handler = handle_peek_sensor_set_auth };
  httpd_register_uri_handler(active_server, &peek_sensor_set);

  httpd_uri_t peek_init = { .uri = "/api/peek/init", .method = HTTP_POST, .handler = handle_peek_init_auth };
  httpd_register_uri_handler(active_server, &peek_init);
#endif

#if FEATURE_VAULT_SNAPSHOT
  // Sealed-snapshot vault (auth required; download also accepts ?token=)
  httpd_uri_t vault_status = { .uri = "/api/vault/status", .method = HTTP_GET, .handler = handle_vault_status_auth };
  httpd_register_uri_handler(active_server, &vault_status);

  httpd_uri_t vault_config = { .uri = "/api/vault/config", .method = HTTP_POST, .handler = handle_vault_config_auth };
  httpd_register_uri_handler(active_server, &vault_config);

  httpd_uri_t vault_key_set = { .uri = "/api/vault/key", .method = HTTP_POST, .handler = handle_vault_key_set_auth };
  httpd_register_uri_handler(active_server, &vault_key_set);

  httpd_uri_t vault_key_clear = { .uri = "/api/vault/key", .method = HTTP_DELETE, .handler = handle_vault_key_clear_auth };
  httpd_register_uri_handler(active_server, &vault_key_clear);

  httpd_uri_t vault_list = { .uri = "/api/vault/list", .method = HTTP_GET, .handler = handle_vault_list_auth };
  httpd_register_uri_handler(active_server, &vault_list);

  httpd_uri_t vault_download = { .uri = "/api/vault/download", .method = HTTP_GET, .handler = handle_vault_download_auth };
  httpd_register_uri_handler(active_server, &vault_download);

  httpd_uri_t vault_item_delete = { .uri = "/api/vault/item", .method = HTTP_DELETE, .handler = handle_vault_item_delete_auth };
  httpd_register_uri_handler(active_server, &vault_item_delete);

  httpd_uri_t vault_test = { .uri = "/api/vault/test", .method = HTTP_POST, .handler = handle_vault_test_auth };
  httpd_register_uri_handler(active_server, &vault_test);
#endif

  // Fleet provisioning QR code
  httpd_uri_t fleet_qr = { .uri = "/api/fleet/qr", .method = HTTP_GET, .handler = handle_fleet_qr_auth };
  httpd_register_uri_handler(active_server, &fleet_qr);

  // Fleet LAN discovery (mDNS _securacv._tcp browse, async + cached)
  httpd_uri_t fleet_scan = { .uri = "/api/fleet/scan", .method = HTTP_GET, .handler = handle_fleet_scan_auth };
  httpd_register_uri_handler(active_server, &fleet_scan);

  httpd_uri_t pairing_qr = { .uri = "/api/pairing-qr", .method = HTTP_GET, .handler = handle_pairing_qr };
  httpd_register_uri_handler(active_server, &pairing_qr);

#if FEATURE_MESH_NETWORK
  // Mesh network (opera) endpoints
  httpd_uri_t mesh_status = { .uri = "/api/mesh", .method = HTTP_GET, .handler = handle_mesh_status_auth };
  httpd_register_uri_handler(active_server, &mesh_status);

  httpd_uri_t mesh_peers = { .uri = "/api/mesh/peers", .method = HTTP_GET, .handler = handle_mesh_peers_auth };
  httpd_register_uri_handler(active_server, &mesh_peers);

  httpd_uri_t mesh_alerts = { .uri = "/api/mesh/alerts", .method = HTTP_GET, .handler = handle_mesh_alerts_auth };
  httpd_register_uri_handler(active_server, &mesh_alerts);

  httpd_uri_t mesh_alerts_clear = { .uri = "/api/mesh/alerts", .method = HTTP_DELETE, .handler = handle_mesh_alerts_clear_auth };
  httpd_register_uri_handler(active_server, &mesh_alerts_clear);

  httpd_uri_t mesh_enable = { .uri = "/api/mesh/enable", .method = HTTP_POST, .handler = handle_mesh_enable_auth };
  httpd_register_uri_handler(active_server, &mesh_enable);

  httpd_uri_t mesh_pair_start = { .uri = "/api/mesh/pair/start", .method = HTTP_POST, .handler = handle_mesh_pair_start_auth };
  httpd_register_uri_handler(active_server, &mesh_pair_start);

  httpd_uri_t mesh_pair_join = { .uri = "/api/mesh/pair/join", .method = HTTP_POST, .handler = handle_mesh_pair_join_auth };
  httpd_register_uri_handler(active_server, &mesh_pair_join);

  httpd_uri_t mesh_pair_confirm = { .uri = "/api/mesh/pair/confirm", .method = HTTP_POST, .handler = handle_mesh_pair_confirm_auth };
  httpd_register_uri_handler(active_server, &mesh_pair_confirm);

  httpd_uri_t mesh_pair_cancel = { .uri = "/api/mesh/pair/cancel", .method = HTTP_POST, .handler = handle_mesh_pair_cancel_auth };
  httpd_register_uri_handler(active_server, &mesh_pair_cancel);

  httpd_uri_t mesh_leave = { .uri = "/api/mesh/leave", .method = HTTP_POST, .handler = handle_mesh_leave_auth };
  httpd_register_uri_handler(active_server, &mesh_leave);

  httpd_uri_t mesh_remove = { .uri = "/api/mesh/remove", .method = HTTP_POST, .handler = handle_mesh_remove_auth };
  httpd_register_uri_handler(active_server, &mesh_remove);

  httpd_uri_t mesh_name = { .uri = "/api/mesh/name", .method = HTTP_POST, .handler = handle_mesh_name_auth };
  httpd_register_uri_handler(active_server, &mesh_name);
#endif

#if FEATURE_BLUETOOTH
  // Bluetooth endpoints
  bluetooth_api::register_routes(active_server, g_device.api_token_str);
#endif

#if FEATURE_BLE
  // BLE Discovery endpoints (Opera/Chirp/Nearby)
  {
    httpd_uri_t ble_status = { .uri = "/api/ble/status", .method = HTTP_GET, .handler = handle_ble_status, .user_ctx = nullptr };
    httpd_register_uri_handler(active_server, &ble_status);

    httpd_uri_t ble_nearby = { .uri = "/api/nearby", .method = HTTP_GET, .handler = handle_ble_nearby, .user_ctx = nullptr };
    httpd_register_uri_handler(active_server, &ble_nearby);

    // Legacy BLE-mediated chirp send. Moved off /api/chirp/send to make room
    // for the v0.2 chirp_api::register_routes() which owns /api/chirp/*.
    // The BLE relay path remains available at /api/ble/chirp/send for
    // backwards compatibility with the old BLE companion app.
    httpd_uri_t ble_chirp_send = { .uri = "/api/ble/chirp/send", .method = HTTP_POST, .handler = handle_ble_chirp_send, .user_ctx = nullptr };
    httpd_register_uri_handler(active_server, &ble_chirp_send);
  }
#endif

  // WiFi Presence Detection endpoints
  wifi_presence_api::register_routes(active_server, g_device.api_token_str);

  // RF Presence fusion endpoints (/api/rf/*). Every route Bearer/session
  // gated via rf_presence_api's auth_gated trampoline.
  rf_presence_api::register_routes(active_server, g_device.api_token_str);

  // Household roles + auto-context (Owner/Family/Guest tagging, presence)
  household_api::register_routes(active_server, g_device.api_token_str);

  // Audible Chirp endpoints
  audible_chirp_api::register_routes(active_server, g_device.api_token_str);

#if FEATURE_MESH_NETWORK
  // Chirp Channel v0.2 — anonymous community witness mesh (audit C12).
  // Every endpoint Bearer-token-gated via the chirp_api template trampoline.
  chirp_api::register_routes(active_server, g_device.api_token_str);
#endif

#if FEATURE_BEACON_CHANNEL
  // Beacon Channel — harm-reduction broadcast with two-pubkey co-signing.
  // Bearer-gated from day one (see spec/beacon_channel_v0.md §10).
  beacon_api::register_routes(active_server, g_device.api_token_str);
#endif

  // Coarse, unauthenticated fleet presence — the DISCOVERY.md /api/fleet
  // contract the Witness Wall + Flasher read. Discoverable at
  // canary.local/api/fleet (this device already advertises canary.local).
  httpd_uri_t fleet_get = { .uri = "/api/fleet", .method = HTTP_GET, .handler = handle_fleet };
  httpd_register_uri_handler(active_server, &fleet_get);
  httpd_uri_t fleet_opt = { .uri = "/api/fleet", .method = HTTP_OPTIONS, .handler = handle_fleet_options };
  httpd_register_uri_handler(active_server, &fleet_opt);

  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "API server started",
             g_tls_enabled ? "HTTPS port 443" : "HTTP port 80");
}

// ════════════════════════════════════════════════════════════════════════════
// WIFI PROVISIONING
// ════════════════════════════════════════════════════════════════════════════

static const char* wifi_state_name(WiFiProvState s) {
  switch (s) {
    case WIFI_PROV_IDLE:       return "idle";
    case WIFI_PROV_SCANNING:   return "scanning";
    case WIFI_PROV_CONNECTING: return "connecting";
    case WIFI_PROV_CONNECTED:  return "connected";
    case WIFI_PROV_FAILED:     return "failed";
    case WIFI_PROV_AP_ONLY:    return "ap_only";
    default:                   return "unknown";
  }
}

static bool wifi_load_credentials() {
  memset(&g_wifi_creds, 0, sizeof(g_wifi_creds));

  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;

  // Standalone preference loads regardless of whether creds exist.
  g_wifi_ap_only = nvs.getBool(NVS_KEY_WIFI_AP_ONLY, false);

  size_t ssid_len = nvs.getBytesLength(NVS_KEY_WIFI_SSID);
  if (ssid_len > 0 && ssid_len <= 32) {
    nvs.getBytes(NVS_KEY_WIFI_SSID, g_wifi_creds.ssid, ssid_len);
    g_wifi_creds.ssid[ssid_len] = '\0';

    size_t pass_len = nvs.getBytesLength(NVS_KEY_WIFI_PASS);
    if (pass_len > 0 && pass_len <= 64) {
      nvs.getBytes(NVS_KEY_WIFI_PASS, g_wifi_creds.password, pass_len);
      g_wifi_creds.password[pass_len] = '\0';
    }

    g_wifi_creds.enabled = nvs.getBool(NVS_KEY_WIFI_EN, true);
    g_wifi_creds.configured = (strlen(g_wifi_creds.ssid) > 0);
  }

  nvs.end();
  return g_wifi_creds.configured;
}

static bool wifi_save_credentials() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;

  nvs.putBytes(NVS_KEY_WIFI_SSID, g_wifi_creds.ssid, strlen(g_wifi_creds.ssid));
  nvs.putBytes(NVS_KEY_WIFI_PASS, g_wifi_creds.password, strlen(g_wifi_creds.password));
  nvs.putBool(NVS_KEY_WIFI_EN, g_wifi_creds.enabled);
  // Saving real credentials is an explicit exit from standalone mode.
  nvs.putBool(NVS_KEY_WIFI_AP_ONLY, false);

  nvs.end();
  g_wifi_ap_only = false;
  g_wifi_creds.configured = true;

  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "WiFi credentials saved", g_wifi_creds.ssid);
  return true;
}

static bool wifi_clear_credentials() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;

  nvs.remove(NVS_KEY_WIFI_SSID);
  nvs.remove(NVS_KEY_WIFI_PASS);
  nvs.remove(NVS_KEY_WIFI_EN);

  nvs.end();

  memset(&g_wifi_creds, 0, sizeof(g_wifi_creds));
  g_wifi_status.state = WIFI_PROV_AP_ONLY;
  g_wifi_status.last_fail_reason[0] = '\0';
  // Forgetting creds drops back to AP-only provisioning; ensure the management
  // AP is up (it may have been torn down once the STA link was healthy) so the
  // device remains reachable after the home network is forgotten.
  wifi_raise_ap();

  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "WiFi credentials cleared", nullptr);
  return true;
}

static void wifi_update_status() {
  g_wifi_status.ap_active = (WiFi.getMode() & WIFI_AP) != 0;
  g_wifi_status.sta_connected = WiFi.isConnected();
  g_wifi_status.ap_clients = WiFi.softAPgetStationNum();

  if (g_wifi_status.sta_connected) {
    g_wifi_status.rssi = WiFi.RSSI();
    IPAddress ip = WiFi.localIP();
    snprintf(g_wifi_status.sta_ip, sizeof(g_wifi_status.sta_ip),
             "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  } else {
    g_wifi_status.rssi = 0;
    g_wifi_status.sta_ip[0] = '\0';
  }

  IPAddress apip = WiFi.softAPIP();
  snprintf(g_wifi_status.ap_ip, sizeof(g_wifi_status.ap_ip),
           "%d.%d.%d.%d", apip[0], apip[1], apip[2], apip[3]);
}

static void wifi_connect_to_home() {
  if (!provisioning_logic::sta_join_allowed(g_wifi_ap_only,
                                            g_wifi_creds.configured,
                                            g_wifi_creds.enabled)) {
    g_wifi_status.state = WIFI_PROV_AP_ONLY;
    return;
  }

  if (strlen(g_wifi_creds.ssid) == 0) {
    g_wifi_status.state = WIFI_PROV_AP_ONLY;
    return;
  }

  // Drop any leftover async scan results before attempting STA association.
  // A pending scan handle keeps the radio busy and causes WiFi.begin() to
  // silently fail to associate on some core versions.
  if (g_wifi_scan_in_progress || WiFi.scanComplete() >= 0) {
    WiFi.scanDelete();
    g_wifi_scan_in_progress = false;
  }

  // Ensure AP+STA mode is active (required to keep the captive-portal AP up
  // while the STA tries to associate to the home network).
  if ((WiFi.getMode() & WIFI_MODE_APSTA) != WIFI_MODE_APSTA) {
    WiFi.mode(WIFI_AP_STA);
  }

  // We persist credentials in our own NVS namespace, so disable the Arduino
  // core's auto-persist to avoid double-writes that can corrupt the wpa NVS
  // partition and silently fail subsequent WiFi.begin() calls.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  // Clear any stale STA state (previous failed attempt, prior association)
  // before starting a fresh association. Without this, WiFi.begin() can
  // immediately return WL_DISCONNECTED on retries. Pass eraseap=false: the
  // periodic retry path also calls this function, and erasing the SDK's
  // STA NVS config on every failed attempt would needlessly wear flash.
  // WiFi.begin() below installs a fresh config anyway.
  WiFi.disconnect(false, false);

  g_wifi_status.state = WIFI_PROV_CONNECTING;
  g_wifi_status.connect_attempts++;
  g_wifi_status.last_connect_ms = millis();

  char msg[64];
  snprintf(msg, sizeof(msg), "Connecting to: %s", g_wifi_creds.ssid);
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, msg, nullptr);

  // Start connection (non-blocking)
  WiFi.begin(g_wifi_creds.ssid, g_wifi_creds.password);
}

// F4 (coexistence): tear down the management SoftAP once the STA link is
// healthy so the single 2.4 GHz radio runs STA + BLE (Espressif's stable Y
// combo) instead of AP + STA + BLE (rated C1/unstable with a client joined).
// Idempotent — a no-op if the AP is already down.
static void wifi_drop_ap() {
  if (!(WiFi.getMode() & WIFI_MODE_AP)) return;  // already STA-only
  WiFi.softAPdisconnect(true);   // stop the SoftAP and release its netif
  WiFi.mode(WIFI_STA);
  g_wifi_status.ap_active = false;
  setup_wizard::stop_captive_portal();  // AP is gone — stop the captive DNS poller (frees CPU)
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK,
             "AP dropped — STA up, BLE-stable mode", g_wifi_status.sta_ip);
}

// F4: bring the management SoftAP back up (e.g. the home WiFi link was lost)
// without re-running the whole provisioning path, so the device stays reachable
// at canary.local for reconfiguration. Idempotent — a no-op if the AP is up.
static void wifi_raise_ap() {
  if (WiFi.getMode() & WIFI_MODE_AP) return;  // already up
  char ap_pass[32] = {0};
  if (!resolve_ap_password(ap_pass, sizeof(ap_pass))) return;
  WiFi.mode(WIFI_AP_STA);
  bool ap_ok = WiFi.softAP(g_device.ap_ssid, ap_pass, AP_CHANNEL, false, AP_MAX_CLIENTS);
  secure_zero(ap_pass, sizeof(ap_pass));  // wipe the AP password from the stack (DCE-safe)
  if (!ap_ok) {
    log_health(SCV_LOG_ERROR, SCV_CAT_NETWORK, "AP re-raise failed", nullptr);
    WiFi.mode(WIFI_STA);  // don't leave the radio half-configured in AP_STA with no AP up
    return;
  }
  g_wifi_status.ap_active = true;
  g_health.wifi_active = true;
  IPAddress ip = WiFi.softAPIP();
  snprintf(g_wifi_status.ap_ip, sizeof(g_wifi_status.ap_ip),
           "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  setup_wizard::start_captive_portal();  // restart captive DNS for the re-raised AP
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "AP re-raised — STA link down", g_wifi_status.ap_ip);
}

static void wifi_check_connection() {
  uint32_t now = millis();

  // Update status
  wifi_update_status();

  // Handle state transitions
  switch (g_wifi_status.state) {
    case WIFI_PROV_CONNECTING: {
      if (WiFi.isConnected()) {
        g_wifi_status.state = WIFI_PROV_CONNECTED;
        g_wifi_status.connected_since_ms = now;
        g_wifi_status.last_fail_reason[0] = '\0';

        char msg[80];
        snprintf(msg, sizeof(msg), "Connected to %s", g_wifi_creds.ssid);
        log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, msg, g_wifi_status.sta_ip);
        break;
      }

      // Detect specific failure modes early so the UI can show a useful
      // reason instead of waiting for the full timeout. WiFi.status() returns
      // a uint8_t; compare directly against the well-known constants rather
      // than narrowing to wl_status_t (which would invoke implementation-
      // defined behavior for any future status value the enum doesn't list).
      const uint8_t wl = WiFi.status();
      const char* fail_reason = nullptr;
      if (wl == WL_NO_SSID_AVAIL) {
        fail_reason = "Network not found";
      } else if (wl == WL_CONNECT_FAILED) {
        fail_reason = "Wrong password or auth rejected";
      }

      if (fail_reason) {
        snprintf(g_wifi_status.last_fail_reason, sizeof(g_wifi_status.last_fail_reason),
                 "%s", fail_reason);
        g_wifi_status.state = WIFI_PROV_FAILED;
        log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK, fail_reason, g_wifi_creds.ssid);
      } else if (now - g_wifi_status.last_connect_ms > WIFI_CONNECT_TIMEOUT_MS) {
        snprintf(g_wifi_status.last_fail_reason, sizeof(g_wifi_status.last_fail_reason),
                 "%s", "Connection timeout");
        g_wifi_status.state = WIFI_PROV_FAILED;
        log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK, "WiFi connection timeout", g_wifi_creds.ssid);
      }
      break;
    }

    case WIFI_PROV_CONNECTED:
      if (!WiFi.isConnected()) {
        g_wifi_status.state = WIFI_PROV_FAILED;
        snprintf(g_wifi_status.last_fail_reason, sizeof(g_wifi_status.last_fail_reason),
                 "%s", "Connection lost");
        log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK, "WiFi connection lost", nullptr);
        // F4: STA link gone — re-raise the management AP so the device stays
        // reachable for reconfiguration while it retries the home network.
        wifi_raise_ap();
      } else if ((WiFi.getMode() & WIFI_MODE_AP) &&
                 provisioning_logic::ap_teardown_due(
                     g_wifi_ap_only, true, now,
                     g_wifi_status.connected_since_ms, AP_DROP_GRACE_MS)) {
        // F4: STA has held the home link past the grace window — drop the AP so
        // the radio runs the stable STA+BLE coexistence combo. The grace window
        // lets the just-provisioned phone see the "connected" result before its
        // AP association is dropped. Never fires in standalone (AP-only) mode,
        // where the AP is the product.
        wifi_drop_ap();
      }
      break;

    case WIFI_PROV_FAILED:
      // Attempt reconnection periodically
      if (g_wifi_creds.configured && g_wifi_creds.enabled &&
          now - g_wifi_status.last_connect_ms > WIFI_RECONNECT_INTERVAL_MS) {
        wifi_connect_to_home();
      }
      break;

    case WIFI_PROV_AP_ONLY:
    case WIFI_PROV_IDLE:
    case WIFI_PROV_SCANNING:
      // No action needed
      break;
  }
}

static bool fingerprint_is_nonzero(const uint8_t fingerprint[8]) {
  for (size_t i = 0; i < 8; i++) {
    if (fingerprint[i] != 0) return true;
  }
  return false;
}

static bool resolve_ap_password(char* out_password, size_t out_len) {
  if (!out_password || out_len == 0) return false;

  // Fail closed in release if provisioning/identity is incomplete.
  const bool fingerprint_ready = fingerprint_is_nonzero(g_device.pubkey_fp);
  if (!g_device.initialized || !fingerprint_ready) {
#if SECURACV_RELEASE_BUILD
    log_health(SCV_LOG_ERROR, SCV_CAT_NETWORK,
               "AP password unavailable", "Release build requires successful provisioning");
    return false;
#else
    uint32_t entropy = esp_random();
    snprintf(out_password, out_len, "dev-%08lx", (unsigned long)entropy);
    log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK,
               "AP debug password generated", "Provisioning incomplete; non-release fallback in use");
    return true;
#endif
  }

  // Derive only when no valid password is currently present.
  if (strlen(g_device.ap_password) < 8) {
    derive_ap_password(g_device.privkey, g_device.ap_password, sizeof(g_device.ap_password));
  }

  // Use either pre-existing or freshly derived device-unique password.
  if (strlen(g_device.ap_password) >= 8) {
    snprintf(out_password, out_len, "%s", g_device.ap_password);
    return true;
  }

#if SECURACV_RELEASE_BUILD
  log_health(SCV_LOG_ERROR, SCV_CAT_NETWORK,
             "AP password unavailable", "Release build requires device-unique credential");
  return false;
#else
  uint32_t entropy = esp_random();
  snprintf(out_password, out_len, "dev-%08lx", (unsigned long)entropy);
  log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK,
             "AP debug password generated", "Non-release fallback in use");
  return true;
#endif
}

// Claim the bare "canary.local" catch-all in ADDITION to our unique
// canary-<name>.local, so single-device homes keep the easy-to-type URL while
// multi-device homes never collide. Strategy: first-wins, made race-safe by
// two additions (decision table host-tested in catchall_logic.h):
//   1. The claim is SCHEDULED with a fingerprint-derived stagger instead of
//      firing at the STA-join instant — two Canaries powering up together
//      (power restored) used to probe simultaneously, find silence, and BOTH
//      claim canary.local. Which device the browser then reached was
//      arbitrary and could flip mid-session, killing the session cookie.
//   2. A periodic conflict check while claimed: if another responder shows
//      up anyway, the deterministic IP tie-break picks exactly one keeper
//      and the loser withdraws (both sides evaluate the same pair from
//      opposite ends, so no negotiation is needed).
// The unique hostname set via MDNS.begin() is unaffected either way, so this
// can only ever ADD reachability.
static bool     g_catch_all_claimed        = false;
static bool     g_catch_all_claim_pending  = false;
static uint32_t g_catch_all_claim_due_ms   = 0;
static uint32_t g_catch_all_next_check_ms  = 0;
static const uint32_t CATCH_ALL_RECHECK_MS = 120000;  // conflict re-probe cadence

// Ask for the catch-all claim to run soon (from the loop tick), after this
// device's stagger window. Callable from WiFi-event/loop contexts; never
// blocks the caller.
static void schedule_catch_all_claim() {
  g_catch_all_claim_due_ms = millis() +
      catchall_logic::claim_stagger_ms(g_device.pubkey_fp[0], g_device.pubkey_fp[1]);
  g_catch_all_claim_pending = true;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
// (Re)register the "canary" delegate for our live interface addresses and
// schedule the next conflict check. quiet=true for the recurring re-adds so
// the health log isn't spammed every cadence.
static void catch_all_add_delegate(bool quiet) {
  // Address list lives in static storage; the mDNS component copies it, but
  // keeping it static is harmless and avoids any lifetime ambiguity.
  static mdns_ip_addr_t ap_node;
  static mdns_ip_addr_t sta_node;
  mdns_ip_addr_t* head = nullptr;

  IPAddress ap_ip = WiFi.softAPIP();
  if ((uint32_t)ap_ip != 0) {
    memset(&ap_node, 0, sizeof(ap_node));
    ap_node.addr.type = ESP_IPADDR_TYPE_V4;
    ap_node.addr.u_addr.ip4.addr = (uint32_t)ap_ip;
    ap_node.next = head;
    head = &ap_node;
  }
  IPAddress sta_ip = WiFi.localIP();
  if ((uint32_t)sta_ip != 0) {
    memset(&sta_node, 0, sizeof(sta_node));
    sta_node.addr.type = ESP_IPADDR_TYPE_V4;
    sta_node.addr.u_addr.ip4.addr = (uint32_t)sta_ip;
    sta_node.next = head;
    head = &sta_node;
  }
  if (!head) { g_catch_all_claimed = false; return; }

  mdns_delegate_hostname_remove("canary");  // idempotent across re-announce
  if (mdns_delegate_hostname_add("canary", head) == ESP_OK) {
    g_catch_all_claimed = true;
    g_catch_all_next_check_ms = millis() + CATCH_ALL_RECHECK_MS +
        catchall_logic::recheck_jitter_ms(g_device.pubkey_fp[0],
                                          g_device.pubkey_fp[1]);
    if (!quiet) {
      log_health(SCV_LOG_INFO, SCV_CAT_NETWORK,
                 "canary.local catch-all claimed", g_device.mdns_hostname);
    }
  } else {
    g_catch_all_claimed = false;
    if (!quiet) {
      log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK,
                 "canary.local catch-all add failed", nullptr);
    }
  }
}
#endif

static void claim_catch_all_hostname() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
  IPAddress existing = MDNS.queryHost("canary", 600);
  if (catchall_logic::probe_is_conflict((uint32_t)existing,
                                        (uint32_t)WiFi.softAPIP(),
                                        (uint32_t)WiFi.localIP())) {
    log_health(SCV_LOG_INFO, SCV_CAT_NETWORK,
               "canary.local catch-all already claimed by a peer",
               "serving unique hostname only");
    g_catch_all_claimed = false;
    return;
  }

  catch_all_add_delegate(false);
#else
  // mdns_delegate_hostname_add requires ESP-IDF >= 4.4. On older cores the
  // device is still reachable at its unique canary-<name>.local; the bare
  // canary.local catch-all is simply not advertised.
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK,
             "canary.local catch-all unavailable on this core", g_device.mdns_hostname);
#endif
}

// Loop-side steward for the catch-all: performs the staggered initial claim,
// then re-probes every CATCH_ALL_RECHECK_MS while we hold it. On a detected
// double-claim, the IP tie-break decides who withdraws. The 600 ms blocking
// probe runs on the loop task at most once per cadence — well inside the
// watchdog budget.
static void catch_all_tick() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
  // Never queue the loop task's blocking mDNS operations (600 ms probe,
  // delegate remove/add) behind an in-flight fleet browse: the mDNS
  // component serializes API calls internally, so stacking them behind the
  // worker's ~3 s _securacv._tcp search parks the loop for the sum — real
  // watchdog budget, gone. Just retry on a later pass; nothing here is
  // urgent on a seconds timescale.
  if (__atomic_load_n(&g_fleet_scan_busy, __ATOMIC_ACQUIRE)) return;

  const uint32_t now = millis();

  if (g_catch_all_claim_pending && catchall_logic::due(now, g_catch_all_claim_due_ms)) {
    g_catch_all_claim_pending = false;
    claim_catch_all_hostname();
    return;  // one blocking probe per tick
  }

  if (g_catch_all_claimed && catchall_logic::due(now, g_catch_all_next_check_ms)) {
    g_catch_all_next_check_ms = now + CATCH_ALL_RECHECK_MS;
    // Withdraw our own delegate for the probe's duration: queryHost returns
    // a single address, so while we're answering, a double-claiming peer can
    // hide behind our own echo and the conflict is never detected. With the
    // delegate down, any answer is genuinely someone else. The ≤600 ms gap
    // in our canary.local answer is invisible next to the re-check cadence,
    // and the fingerprint-jittered schedule keeps two devices' probe windows
    // from overlapping (synchronized withdrawn probes would both see
    // silence and both re-add).
    mdns_delegate_hostname_remove("canary");
    IPAddress answer = MDNS.queryHost("canary", 600);
    const uint32_t my_ap  = (uint32_t)WiFi.softAPIP();
    const uint32_t my_sta = (uint32_t)WiFi.localIP();
    if (catchall_logic::probe_is_conflict((uint32_t)answer, my_ap, my_sta)) {
      const uint32_t my_ip = my_sta ? my_sta : my_ap;
      if (!catchall_logic::keep_claim_on_conflict(my_ip, (uint32_t)answer)) {
        g_catch_all_claimed = false;  // stay withdrawn — the peer keeps it
        log_health(SCV_LOG_INFO, SCV_CAT_NETWORK,
                   "canary.local catch-all withdrawn (peer holds it)",
                   g_device.mdns_hostname);
        return;
      }
      log_health(SCV_LOG_INFO, SCV_CAT_NETWORK,
                 "canary.local double-claim detected — keeping (tie-break)",
                 nullptr);
    }
    catch_all_add_delegate(true);  // still ours — quietly re-register
  }
#endif
}

static void wifi_init_provisioning() {
  memset(&g_wifi_status, 0, sizeof(g_wifi_status));

  // Note on Wi-Fi/BLE coexistence: SW coexistence is enabled by the
  // arduino-esp32 core build and the single 2.4 GHz radio is already
  // time-sliced with a BALANCE preference by default (the legacy
  // esp_coex_preference_set() knob is deprecated in IDF5 and would only
  // re-assert that default). The meaningful stability lever here is the radio
  // *mode*: we leave AP+STA+BLE (rated C1/unstable) by dropping the AP once the
  // STA link is healthy — see wifi_check_connection() / wifi_drop_ap().

  // Load saved credentials
  bool has_creds = wifi_load_credentials();

  // Bring up AP+STA for provisioning: the SoftAP serves the captive portal /
  // local management UI while the STA associates to the home network. Once the
  // STA link is healthy the AP is dropped (wifi_check_connection → wifi_drop_ap)
  // so the radio runs the stable STA+BLE coexistence combo.
  WiFi.mode(WIFI_AP_STA);

  // Set the WiFi STA hostname BEFORE softAP() / begin() so DHCP also
  // propagates the device's UNIQUE name (e.g. "canary-kitchen") to the home
  // router — some routers/clients resolve via DHCP hostname rather than mDNS,
  // and a unique name there is what stops two Canaries showing up identically
  // in the router's client list. The bare "canary.local" catch-all is added
  // separately as a delegated mDNS hostname (see claim_catch_all_hostname()).
  WiFi.setHostname(g_device.mdns_hostname);

  // Register the STA_GOT_IP handler ONCE per boot. ESP-IDF mDNS binds
  // to whichever netifs are up at MDNS.begin() time and does not auto-
  // re-announce when a new netif gains IP later. Without this, the
  // hostname is reachable only on the AP interface — phones on the
  // home WiFi can't resolve it. The handler tears down and restarts
  // mDNS so the STA interface is announced too.
  static bool s_wifi_event_registered = false;
  if (!s_wifi_event_registered) {
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t /*info*/) {
      if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        MDNS.end();
        if (MDNS.begin(g_device.mdns_hostname)) {
          MDNS.addService("http", "tcp", 80);
          MDNS.addService("securacv", "tcp", 80);
          MDNS.addServiceTxt("securacv", "tcp", "device_id",
                             (const char*)g_device.device_id);
          MDNS.addServiceTxt("securacv", "tcp", "fw", FIRMWARE_VERSION);
          // Advertise the unique host label and the friendly name so the SPA
          // and fleet manager can show a human name and resolve the exact
          // <host>.local even when several Canaries are on the LAN.
          MDNS.addServiceTxt("securacv", "tcp", "host", (const char*)g_device.mdns_hostname);
          MDNS.addServiceTxt("securacv", "tcp", "name",
                             setup_wizard::get_device_name() ? setup_wizard::get_device_name() : "");
          #if defined(HARDWARE_XIAO_ESP32C3)
          MDNS.addServiceTxt("securacv", "tcp", "model", "XIAO ESP32C3");
          #else
          MDNS.addServiceTxt("securacv", "tcp", "model", "XIAO ESP32S3");
          #endif
          // Canonical fleet TXT identity (see docs/onboarding_unified_wizard.md):
          // dt is the canonical hyphenated device type the HA component and the
          // companion app key modality/wizard branches off; role separates
          // witnesses from glance surfaces (canary-display advertises "display").
          MDNS.addServiceTxt("securacv", "tcp", "dt",   "canary-wap");
          MDNS.addServiceTxt("securacv", "tcp", "role", "witness");
          mdns_sync_broker_txt();      // re-add broker/bport if the link is up
          schedule_catch_all_claim();  // staggered re-assert of canary.local
          log_health(SCV_LOG_INFO, SCV_CAT_NETWORK,
                     "mDNS re-announced on STA", g_device.mdns_hostname);
        } else {
          log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK,
                     "mDNS STA re-announce failed", nullptr);
        }
      }
    });
    s_wifi_event_registered = true;
  }

  // Modem-sleep is on by default and adds 50–200 ms of variable latency
  // to every TCP send. That's invisible for short JSON requests but it's
  // exactly what made the long-lived MJPEG peek stream drop after a
  // dozen seconds: a single sleep window stretches a chunk send past the
  // httpd send_wait_timeout. Streaming is the headline use case here, so
  // we keep the radio awake.
  WiFi.setSleep(false);

  // Start Access Point with device-unique password.
  char ap_pass[32] = {0};
  if (!resolve_ap_password(ap_pass, sizeof(ap_pass))) {
    return;
  }
  // One stable SSID for the lifetime of the device — the same name during
  // first-boot setup and in steady state. The SoftAP is broadcast while the
  // device is unprovisioned or its STA link is down; once the STA holds the
  // home network past AP_DROP_GRACE_MS the AP is torn down for radio stability
  // and automatically re-raised on STA loss (wifi_raise_ap / wifi_drop_ap).
  const char* ap_ssid = g_device.ap_ssid;

  bool ap_ok = WiFi.softAP(ap_ssid, ap_pass, AP_CHANNEL, false, AP_MAX_CLIENTS);
  secure_zero(ap_pass, sizeof(ap_pass));  // wipe the AP password from the stack (DCE-safe)

  if (!ap_ok) {
    log_health(SCV_LOG_ERROR, SCV_CAT_NETWORK, "WiFi AP start failed", nullptr);
    return;
  }

  g_wifi_status.ap_active = true;
  g_health.wifi_active = true;

  IPAddress ip = WiFi.softAPIP();
  snprintf(g_wifi_status.ap_ip, sizeof(g_wifi_status.ap_ip),
           "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  // Bring up the captive DNS redirector for the whole life of the AP, not
  // just first-boot setup, so a phone joining the management AP (during setup,
  // or after home WiFi dropped and the AP was re-raised) resolves canary.local
  // to the device instead of being flagged "no internet" and disconnected. The
  // captive portal is restarted by wifi_raise_ap() whenever the AP comes back.
  // The setup wizard's landing/timeout logic stays separately gated on
  // is_active().
  if (setup_wizard::start_captive_portal()) {
    Serial.println(setup_wizard::is_active()
                     ? "[OK] Captive DNS active (first-boot setup)"
                     : "[OK] Captive DNS active (AP management)");
  }

  // First-boot pre-scan: sweep for home networks NOW, before any phone has
  // joined the SoftAP. A scan hops the single radio across channels and
  // stalls the AP, so doing it later — under the provisioning phone — is
  // exactly what dropped the wizard's first /api/wifi/scan fetch ("Scan
  // failed: Load failed"). The results land in the scan cache on the first
  // endpoint hit and the phone gets an instant list instead of a radio sweep.
  if (setup_wizard::is_active()) {
    g_wifi_scan_in_progress = true;
    WiFi.scanNetworks(true, false, false, 300);  // async
    Serial.println("[OK] Pre-scanning WiFi networks for the setup wizard");
  }

  char msg[64];
  snprintf(msg, sizeof(msg), "AP: %s", ap_ssid);
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, msg, g_wifi_status.ap_ip);

  // Start mDNS with this device's UNIQUE hostname (e.g. canary-kitchen or
  // canary-aabb). Each Canary owns a distinct <host>.local, so a second
  // device never collides with the first — the previous behavior hardcoded
  // "canary" for every unit and relied on RFC 6762 §9 conflict renaming,
  // which the ESPmDNS wrapper does not perform reliably. The bare
  // "canary.local" catch-all is then claimed separately (first-wins) by
  // claim_catch_all_hostname() so single-device homes keep the easy URL. The
  // SPA wizard's `_securacv._tcp` browse uses the device_id/host/name TXT
  // records to distinguish individual Canaries. Mirrors the hostname grammar
  // in firmware/canary/lib/securacv_network and canary-vision/docs/discovery.md
  // so one SPA build talks to both the WAP and modular `canary/` builds.
  if (MDNS.begin(g_device.mdns_hostname)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("securacv", "tcp", 80);
    // ESPmDNS::addServiceTxt has three overloads (char*, const char*,
    // String). g_device.device_id is char[32], which is implicitly
    // convertible to all three — making the call ambiguous. Cast the
    // non-const-array argument to const char* to pick a single overload.
    MDNS.addServiceTxt("securacv", "tcp", "device_id", (const char*)g_device.device_id);
    MDNS.addServiceTxt("securacv", "tcp", "fw",        FIRMWARE_VERSION);
    // Unique host label + optional friendly name (see STA re-announce above).
    MDNS.addServiceTxt("securacv", "tcp", "host",      (const char*)g_device.mdns_hostname);
    MDNS.addServiceTxt("securacv", "tcp", "name",
                       setup_wizard::get_device_name() ? setup_wizard::get_device_name() : "");
    // The same firmware compiles for both XIAO ESP32S3 and XIAO ESP32C3
    // (see DEVICE_ID_PREFIX selection at lines 201-205). Advertise the
    // actual hardware so the fleet manager and the SPA can pick the
    // right capability set (e.g., the C3 has no camera).
    #if defined(HARDWARE_XIAO_ESP32C3)
    MDNS.addServiceTxt("securacv", "tcp", "model",     "XIAO ESP32C3");
    #else
    MDNS.addServiceTxt("securacv", "tcp", "model",     "XIAO ESP32S3");
    #endif
    // Canonical fleet TXT identity (see docs/onboarding_unified_wizard.md):
    // dt is the canonical hyphenated device type the HA component and the
    // companion app key modality/wizard branches off; role separates
    // witnesses from glance surfaces (canary-display advertises "display").
    MDNS.addServiceTxt("securacv", "tcp", "dt",   "canary-wap");
    MDNS.addServiceTxt("securacv", "tcp", "role", "witness");
    mdns_sync_broker_txt();      // broker/bport if already connected (else tombstone)
    schedule_catch_all_claim();  // staggered; answers bare canary.local if free
    log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "mDNS started", g_device.mdns_hostname);
  } else {
    log_health(SCV_LOG_WARNING, SCV_CAT_NETWORK, "mDNS begin failed", g_device.mdns_hostname);
  }

  // Attempt to connect to home WiFi if configured
  if (has_creds && g_wifi_creds.enabled) {
    wifi_connect_to_home();
  } else {
    g_wifi_status.state = WIFI_PROV_AP_ONLY;
    log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "AP-only mode", "No home WiFi configured");
  }
}

// Legacy function for compatibility
static bool start_wifi_ap() {
  wifi_init_provisioning();
  return g_wifi_status.ap_active;
}

// ════════════════════════════════════════════════════════════════════════════
// SD CARD INITIALIZATION (Legacy wrapper - now uses hardware_state.h)
// ════════════════════════════════════════════════════════════════════════════
// NOTE: SD initialization is now handled by sd_mount_safe() in hardware_state.h
// which provides timeout protection and graceful failure handling.
// The legacy sd_init() has been removed to prevent accidental use of blocking SD.begin().

// ════════════════════════════════════════════════════════════════════════════
// DEVICE PROVISIONING
// ════════════════════════════════════════════════════════════════════════════

static bool provision_device() {
  Serial.println("[PROV] Provisioning device identity...");

  g_device.first_boot = false;

  // ── Load or generate the Ed25519 keypair FIRST ──
  // Device identity (device_id / AP SSID / mDNS hostname) is derived from the
  // pubkey fingerprint, not the MAC (event_contract §10), so the keypair and its
  // fingerprint must exist before we name anything.
  if (nvs_load_key(g_device.privkey)) {
    Serial.println("[PROV] Loaded existing keypair from NVS");
  } else {
    Serial.println("[PROV] Generating Ed25519 keypair from hardware RNG...");
    if (!generate_keypair(g_device.privkey, g_device.pubkey)) {
      Serial.println("[!!] Keypair generation failed");
      return false;
    }
    if (!nvs_store_key(g_device.privkey)) {
      Serial.println("[!!] Failed to store keypair");
      return false;
    }
    Serial.println("[PROV] Keypair stored in NVS");
    g_device.first_boot = true;
    // This boot made the key, so this boot is the only one that can honestly
    // say how old it is when a clock finally arrives.
    g_device.key_is_new = true;
    g_device.key_born_ms = millis();
  }

  // What we already know about when this key was born. Loaded before anything
  // can offer a clock, so the "already recorded" rule is in force from the
  // first opportunity to stamp.
  g_device.born_day = nvs_load_u32(NVS_KEY_BORN, 0);
  g_device.born_exact = nvs_load_u32(NVS_KEY_BORN_EX, 0) != 0;

  // Derive public key and fingerprint
  Ed25519::derivePublicKey(g_device.pubkey, g_device.privkey);
  compute_fingerprint(g_device.pubkey, g_device.pubkey_fp);
  hex_to_str(g_device.fingerprint_hex, g_device.pubkey_fp, 8);
  Serial.printf("[PROV] Public key fingerprint: %s\n", g_device.fingerprint_hex);

  // ── Derive device identity from the pubkey fingerprint (never the MAC) ──
  generate_device_id(g_device.device_id, sizeof(g_device.device_id));
  generate_ap_ssid(g_device.ap_ssid, sizeof(g_device.ap_ssid));
  generate_mdns_hostname(g_device.mdns_hostname, sizeof(g_device.mdns_hostname));

  // ── Derive API token (HKDF-style, 2-step) ──
  Serial.println("[PROV] Deriving API token (HKDF-style, 2-step)...");
  if (nvs_load_token(g_device.api_token_str, sizeof(g_device.api_token_str))) {
    Serial.println("[PROV] Loaded existing API token from NVS");
  } else {
    if (!derive_api_token(g_device.privkey, g_device.api_token_str, sizeof(g_device.api_token_str))) {
      Serial.println("[!!] API token derivation failed");
      return false;
    }
    if (!nvs_store_token(g_device.api_token_str)) {
      Serial.println("[!!] Failed to store API token");
      return false;
    }
    Serial.println("[PROV] API token stored in NVS");
  }

  // ── Derive device-unique AP password ──
  Serial.println("[PROV] Deriving device-unique AP password...");
  derive_ap_password(g_device.privkey, g_device.ap_password, sizeof(g_device.ap_password));

  // Load operator-configurable runtime settings (Device tab). Clamped on
  // load, so the record loop, time-coarsening bucket, and log threshold are
  // in-envelope before they are first read.
  config_load_runtime();

  // Load chain state
  g_device.seq = nvs_load_u32(NVS_KEY_SEQ, 0);
  g_device.seq_persisted = g_device.seq;
  g_device.boot_count = nvs_load_u32(NVS_KEY_BOOTS, 0) + 1;
  nvs_store_u32(NVS_KEY_BOOTS, g_device.boot_count);
  g_device.log_seq = nvs_load_u32(NVS_KEY_LOGSEQ, 0);

  if (!nvs_load_bytes(NVS_KEY_CHAIN, g_device.chain_head, 32)) {
    // Initialize genesis chain hash
    sha256_domain("securacv:genesis:v1", (const uint8_t*)g_device.device_id, strlen(g_device.device_id), g_device.chain_head);
    nvs_store_bytes(NVS_KEY_CHAIN, g_device.chain_head, 32);
  }

  g_device.boot_ms = millis();
  g_device.initialized = true;
  g_health.crypto_healthy = true;
  g_health.min_heap = ESP.getFreeHeap();

  Serial.printf("[PROV] Device ID: %s\n", g_device.device_id);
  Serial.printf("[PROV] Boot count: %u\n", g_device.boot_count);
  Serial.printf("[PROV] Chain seq: %u\n", g_device.seq);

  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// OUTPUT FORMATTING
// ════════════════════════════════════════════════════════════════════════════

static void print_quick_connect_details(const char* title) {
  #if FEATURE_WIFI_AP
  if (!g_device.initialized) return;

  Serial.println(title);
  Serial.printf("[PROV]   WiFi SSID : %s\n", g_device.ap_ssid);
  Serial.printf("[PROV]   WiFi PASS : %s\n", g_device.ap_password);
  Serial.printf("[PROV]   URL       : %s://canary.local  (or %s://%s)\n",
                g_tls_enabled ? "https" : "http",
                g_tls_enabled ? "https" : "http",
                WiFi.softAPIP().toString().c_str());
  Serial.printf("[PROV]   API TOKEN : %s\n", g_device.api_token_str);
  #else
  (void)title;
  #endif
}

static void print_table_header() {
  Serial.println("+------+------+-------+----+-------------+-------------+--------+---+----+-----+-----+------+-------+------------------+");
  Serial.println("|  seq | type | state | ok |     lat     |     lon     | alt(m) | Q |sat |hdop |vdop | m/s  | course|     chain        |");
  Serial.println("+------+------+-------+----+-------------+-------------+--------+---+----+-----+-----+------+-------+------------------+");
}

static void print_table_row(WitnessRecord* r, GnssFix* fx, FixState st) {
  char chain_hex[17];
  hex_to_str(chain_hex, r->chain_hash, 8);
  
  // Privacy (Invariant III): coarsen lat/lon in operator-facing serial logs (3 dp ≈ 110 m).
  Serial.printf("| %4u | %4s | %5s | %s | %11.3f | %12.3f | %6.1f | %d | %2d | %4.1f| %4.1f| %5.2f| %5.1f | %s... |\n",
    r->seq,
    record_type_name(r->type),
    state_name_short(st),
    r->verified ? "OK" : "!!",
    gps_coarsen_deg(fx->lat),
    gps_coarsen_deg(fx->lon),
    fx->altitude_m,
    fx->quality,
    fx->satellites,
    fx->hdop,
    fx->vdop,
    g_speed_ema,
    fx->course_deg,
    chain_hex
  );
}

static void print_status_bar() {
  char uptime_str[16];
  format_uptime(uptime_str, sizeof(uptime_str), uptime_seconds());

  Serial.println();
  if (g_hw.safe_mode) {
    Serial.printf("╔═══ STATUS ══╦══════════════════════════════════════════════════════════════════╗\n");
    Serial.printf("║ ⚠️ SAFE MODE ║  Records: %-6u |  GPS: %-8s  |  SD: %-8s  |  WiFi: %d\n",
      g_health.records_created,
      gps_state_name(g_hw.gps_state),
      sd_state_name(g_hw.sd_state),
      WiFi.softAPgetStationNum()
    );
  } else {
    Serial.printf("╔═══ STATUS ══╦══════════════════════════════════════════════════════════════════╗\n");
    Serial.printf("║ Uptime: %-8s ║  Records: %-6u |  GPS: %-8s  |  SD: %-8s  |  WiFi: %d\n",
      uptime_str,
      g_health.records_created,
      gps_state_name(g_hw.gps_state),
      sd_state_name(g_hw.sd_state),
      WiFi.softAPgetStationNum()
    );
  }
  Serial.printf("╚═════════════╩══════════════════════════════════════════════════════════════════╝\n");
}

static void print_identity_block() {
  char fp_hex[17];
  hex_to_str(fp_hex, g_device.pubkey_fp, 8);
  
  char pubkey_hex[65];
  hex_to_str(pubkey_hex, g_device.pubkey, 32);
  
  Serial.println();
  Serial.println("┌─────────────────────────────────────┐");
  Serial.println("│          DEVICE IDENTITY            │");
  Serial.println("├─────────────────────────────────────┤");
  Serial.printf("│ Device ID  : %s\n", g_device.device_id);
  Serial.printf("│ FP8        : %s\n", fp_hex);
  Serial.printf("│ Pubkey     : %s...\n", pubkey_hex);
  Serial.printf("│ Firmware   : %s\n", FIRMWARE_VERSION);
  Serial.printf("│ Ruleset    : %s\n", RULESET_ID);
  Serial.println("└─────────────────────────────────────┘");
}

static void print_time_block() {
  char uptime_str[16];
  format_uptime(uptime_str, sizeof(uptime_str), uptime_seconds());
  
  Serial.println();
  Serial.println("┌─────────────────────────────────────┐");
  Serial.println("│              TIME                   │");
  Serial.println("├─────────────────────────────────────┤");
  Serial.print("│ Uptime     : "); Serial.print(uptime_str);
  Serial.printf(" (%us)\n", uptime_seconds());
  Serial.print("│ TimeBucket : "); Serial.print(time_bucket());
  Serial.printf(" (%ums buckets)\n", TIME_BUCKET_MS);
  
  Serial.print("│ GPS UTC    : ");
  if (g_gps_utc.valid) {
    Serial.printf("%04d-%02d-%02d %02d:%02d:%02d.%02dZ\n",
                  g_gps_utc.year, g_gps_utc.month, g_gps_utc.day,
                  g_gps_utc.hour, g_gps_utc.minute, g_gps_utc.second, g_gps_utc.centisecond);
  } else {
    Serial.println("waiting for fix");
  }
  Serial.println("└─────────────────────────────────────┘");
}

static void print_gps_block() {
  Serial.println();
  Serial.println("┌─────────────────────────────────────┐");
  Serial.println("│          GPS DETAILS                │");
  Serial.println("├─────────────────────────────────────┤");
  Serial.printf("│ Fix Mode   : %s\n", fix_mode_name(g_fix.fix_mode));
  Serial.printf("│ Quality    : %d (%s)\n", g_fix.quality, quality_name(g_fix.quality));
  Serial.printf("│ Valid      : %s\n", g_fix.valid ? "YES" : "NO");
  // Privacy (Invariant III): coarsen lat/lon in operator-facing serial output (3 dp ≈ 110 m).
  Serial.printf("│ Latitude   : %.3f°\n", gps_coarsen_deg(g_fix.lat));
  Serial.printf("│ Longitude  : %.3f°\n", gps_coarsen_deg(g_fix.lon));
  Serial.printf("│ Altitude   : %.2f m\n", g_fix.altitude_m);
  Serial.printf("│ Speed      : %.2f m/s (EMA)\n", g_speed_ema);
  Serial.printf("│ Satellites : %d used / %d visible\n", g_fix.satellites, g_fix.sats_in_view);
  Serial.printf("│ HDOP       : %.2f\n", g_fix.hdop);
  Serial.println("└─────────────────────────────────────┘");
}

static void print_help() {
  Serial.println();
  Serial.println("┌─────────────────────────────────────┐");
  Serial.println("│           SERIAL COMMANDS           │");
  Serial.println("├─────────────────────────────────────┤");
  Serial.println("│ h / ? : Show this help              │");
  Serial.println("│ i     : Show device identity        │");
  Serial.println("│ s     : Show status bar             │");
  Serial.println("│ t     : Show time info              │");
  Serial.println("│ g     : Show GPS details            │");
  Serial.println("│ w     : Show WiFi AP info           │");
  Serial.println("│ c     : Show camera status          │");
  Serial.println("│ m     : Show system monitor         │");
  Serial.println("│         (temp, heap, PSRAM)         │");
  Serial.println("│ r     : Show data management stats  │");
  Serial.println("│ b     : Show battery status         │");
  Serial.println("│ p     : Show power policy           │");
  Serial.println("│ u     : USB drive (evidence/update)  │");
  Serial.println("└─────────────────────────────────────┘");
}

static void handle_serial_commands() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) return;
    
    switch ((char)c) {
      case 'h':
      case '?':
        print_help();
        break;
      case 'i':
        if (g_device.initialized) print_identity_block();
        break;
      case 's':
        print_status_bar();
        print_table_header();
        break;
      case 't':
        print_time_block();
        break;
      case 'g':
        print_gps_block();
        break;
      case 'w':
        Serial.printf("WiFi AP: %s\n", g_device.ap_ssid);
        Serial.printf("Password: %s\n", g_device.ap_password);
        Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("TLS: %s\n", g_tls_enabled ? "YES (port 443)" : "NO (port 80)");
        Serial.printf("Clients: %d\n", WiFi.softAPgetStationNum());
        {
          char redacted[16];
          auth_redact_token(g_device.api_token_str, redacted, sizeof(redacted));
          Serial.printf("API Token: %s\n", redacted);
        }
        break;
      case 'c':
        #if FEATURE_CAMERA_PEEK
        Serial.printf("Camera initialized: %s\n", g_camera_initialized ? "YES" : "NO");
        Serial.printf("Peek active: %s\n", g_peek_active ? "YES" : "NO");
        Serial.printf("Resolution: %s (%d)\n", framesize_name(g_peek_framesize), (int)g_peek_framesize);
        #else
        Serial.println("Camera not enabled");
        #endif
        break;
      case 'm':
        #if FEATURE_SYS_MONITOR
        sys_monitor::print_status();
        #else
        Serial.println("System monitor not enabled");
        #endif
        break;
      case 'r':
        #if FEATURE_DATA_MGMT && FEATURE_SD_STORAGE
        datamgmt::print_status();
        #else
        Serial.println("Data management not enabled");
        #endif
        break;
      case 'b':
        #if FEATURE_POWER_MONITOR
        power_monitor::print_status();
        #else
        Serial.println("Power monitor not enabled");
        #endif
        break;
      case 'p':
        #if FEATURE_POWER_POLICY
        power_policy::print_status();
        #else
        Serial.println("Power policy not enabled");
        #endif
        break;
      case 'u': {
        // USB drive modes: OFF -> EVIDENCE (SD read-only) -> UPDATE
        // (signed drop-zone) -> OFF. Honest no-op on non-OTG builds.
        static bool usb_drive_init = false;
        if (!usb_drive_init) {
          usb_evidence_drive::Config c;
          c.product = OTA_PRODUCT;
          c.running_version = FIRMWARE_VERSION;
          c.sd_quiesce = nullptr;  // Phase 2: wire real SD flush/close hooks
          c.sd_resume = nullptr;
          usb_evidence_drive::begin(c);
          usb_drive_init = true;
        }
        usb_evidence_drive::cycle_mode();
        Serial.println(usb_evidence_drive::status_line());
        break;
      }
      case '\r':
      case '\n':
      case ' ':
        break;
      default:
        Serial.println("[!] Unknown command. Press 'h' for help.");
        break;
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════════════
// PULL-OTA INTEGRATION (signed firmware updates)
// ════════════════════════════════════════════════════════════════════════════

/* Sign an update lifecycle event into the witness chain — the audit trail
 * proves when the firmware changed and whether a rollback happened. */
static void ota_witness_event(const char* type, const char* version) {
  uint8_t payload[160];
  CborWriter w(payload, sizeof(payload));
  w.write_map(3);
  w.write_text("device_id"); w.write_text(g_device.device_id);
  w.write_text("type");      w.write_text(type);
  w.write_text("ver");       w.write_text(version);
  if (w.ok()) {
    create_witness_record(payload, w.size(), RECORD_STATE_CHANGE, &g_last_record);
  }
}

/* Runs on the OTA task — only flips a flag; the main loop publishes. */
static void ota_on_progress(securacv_ota_state_t state, uint8_t percent,
                            securacv_ota_error_t error, void* user_data) {
  (void)state; (void)percent; (void)error; (void)user_data;
  g_ota_publish_pending = true;
}

/* Install-permission gate. The BLE OTA channel and the pull engine write
 * to the same inactive partition, so they strictly exclude each other
 * (ble_ota.cpp holds the mirror-image check). */
static bool ota_can_install(char* reason, size_t reason_len) {
#if FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)
  const ble_ota::OtaState b = ble_ota::get_state();
  if (b == ble_ota::OTA_RECEIVING || b == ble_ota::OTA_VERIFYING ||
      b == ble_ota::OTA_REBOOTING) {
    snprintf(reason, reason_len, "a Bluetooth update is in progress");
    return false;
  }
#else
  (void)reason; (void)reason_len;
#endif
  return true;
}

/* Pre-reboot hook. The engine records the install target itself the
 * moment the boot partition flips (covering deferred and indirect
 * reboots), and witness records flush per-write — nothing extra to
 * persist here. Kept as the seam for future flush needs. */
static void ota_before_reboot() {
}

static void ota_schedule_next_check(uint32_t delay_ms, uint32_t jitter_ms) {
  uint32_t jitter = (jitter_ms > 0) ? (esp_random() % jitter_ms) : 0;
  g_ota_next_check_ms = millis() + delay_ms + jitter;
}

/* Daily jittered update check — the jitter spreads a fleet's checks over
 * an hour so a release never sees a thundering herd; the first check lands
 * a couple of minutes after boot. */
static void ota_scheduler_process(uint32_t now) {
  if (g_ota_next_check_ms == 0) {
    ota_schedule_next_check(120000UL, 60000UL);
    return;
  }
  if ((int32_t)(now - g_ota_next_check_ms) < 0) return;

  if (securacv_ota_get_state() != SECURACV_OTA_IDLE ||
      WiFi.status() != WL_CONNECTED) {
    // Busy or no route to the update server yet — try again in 15 min.
    ota_schedule_next_check(15UL * 60 * 1000, 60000UL);
    return;
  }

  ota_schedule_next_check(24UL * 60 * 60 * 1000, 3600000UL);
  if (securacv_ota_get_auto_update()) {
    securacv_ota_check_and_install();
  } else {
    securacv_ota_check();
  }
}

/* State payload for the HA MQTT `update` entity (installed_version /
 * latest_version / in_progress / update_percentage / release_summary /
 * release_url). csi_mqtt caches + retains it so HA survives broker
 * restarts. */
static void ota_publish_update_state() {
  JsonDocument doc;
  doc["installed_version"] = FIRMWARE_VERSION;

  const securacv_ota_manifest_t* m = securacv_ota_get_manifest();
  if (m != NULL && securacv_ota_update_available()) {
    doc["latest_version"] = m->version;
    if (m->release_url[0] != '\0') {
      doc["release_url"] = m->release_url;
    }
    if (m->release_notes[0] != '\0') {
      char summary[161];  // keep the whole payload inside csi_mqtt's 512-byte cache
      strncpy(summary, m->release_notes, sizeof(summary) - 1);
      summary[sizeof(summary) - 1] = '\0';
      doc["release_summary"] = summary;
    }
  } else {
    doc["latest_version"] = FIRMWARE_VERSION;
  }

  const securacv_ota_state_t st = securacv_ota_get_state();
  const bool in_progress = (st == SECURACV_OTA_DOWNLOADING ||
                            st == SECURACV_OTA_VERIFYING ||
                            st == SECURACV_OTA_FLASHING ||
                            st == SECURACV_OTA_REBOOTING);
  doc["in_progress"] = in_progress;
  if (in_progress) {
    doc["update_percentage"] = securacv_ota_get_progress();
  } else {
    doc["update_percentage"] = nullptr;  // resets HA's progress bar
  }

  String payload;
  serializeJson(doc, payload);
  csi_mqtt::publish_update_state(payload.c_str());
}

// GET /api/ota/status — everything the Settings UI needs in one call.
// `state_text` / `error_text` are the plain-language strings shown to the
// user; the technical `state` / `error` fields feed diagnostics.
static esp_err_t handle_ota_status(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  g_health.http_requests++;

  const securacv_ota_state_t state = securacv_ota_get_state();
  const securacv_ota_error_t error = securacv_ota_get_last_error();

  JsonDocument doc;
  doc["ok"] = true;
  doc["installed_version"] = securacv_ota_get_version();
  doc["state"] = securacv_ota_state_str(state);
  doc["state_text"] = securacv_ota_friendly_state(state);
  doc["progress"] = securacv_ota_get_progress();
  doc["error"] = securacv_ota_error_str(error);
  doc["error_text"] = securacv_ota_friendly_error(error);
  doc["update_available"] = securacv_ota_update_available();
  doc["auto_update"] = securacv_ota_get_auto_update();
  doc["local_http_allowed"] = securacv_ota_get_local_http_allowed();
  doc["last_check"] = securacv_ota_get_last_check_time();

  char url[256];
  if (securacv_ota_get_manifest_url(url, sizeof(url)) == ESP_OK) {
    doc["manifest_url"] = url;
  }
  doc["manifest_url_is_override"] = securacv_ota_manifest_url_is_override();

  const securacv_ota_manifest_t* m = securacv_ota_get_manifest();
  if (m != NULL) {
    doc["latest_version"] = m->version;
    if (m->release_notes[0] != '\0') doc["release_notes"] = m->release_notes;
    if (m->release_url[0] != '\0') doc["release_url"] = m->release_url;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// POST /api/ota/check — fetch the manifest and compare versions, without
// installing. Results land in /api/ota/status (poll while state=Checking).
static esp_err_t handle_ota_check(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  g_health.http_requests++;

  esp_err_t err = securacv_ota_check();
  if (err == ESP_ERR_INVALID_STATE) {
    return http_send_error(req, 400, "ota_busy");
  }
  if (err != ESP_OK) {
    return http_send_error(req, 500, "ota_check_failed");
  }
  return http_send_json(req, "{\"ok\":true,\"message\":\"Checking for updates…\"}");
}

// POST /api/ota/install — full signed install pipeline (download, verify
// SHA-256 + Ed25519, flash inactive slot, reboot, self-test or roll back).
static esp_err_t handle_ota_install(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  g_health.http_requests++;

  log_health(SCV_LOG_NOTICE, SCV_CAT_SYSTEM,
             "Firmware install requested from dashboard", nullptr);

  esp_err_t err = securacv_ota_check_and_install();
  if (err == ESP_ERR_INVALID_STATE) {
    return http_send_error(req, 400, "ota_busy");
  }
  if (err != ESP_OK) {
    return http_send_error(req, 500, "ota_install_failed");
  }
  return http_send_json(req,
      "{\"ok\":true,\"message\":\"Installing the update. The device will restart on its own.\"}");
}

// POST /api/ota/config — persist update settings (NVS). Body (all fields
// optional): {"manifest_url": "...", "auto_update": bool,
// "local_http_allowed": bool}. An empty manifest_url clears the override.
static esp_err_t handle_ota_config(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  g_health.http_requests++;

  char body[512];
  int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) {
    return http_send_error(req, 400, "empty_body");
  }
  body[recv] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  // Order matters: apply the local-http opt-in first so a manifest_url
  // pointing at a LAN server in the same request validates correctly.
  if (input["local_http_allowed"].is<bool>()) {
    securacv_ota_set_local_http_allowed(input["local_http_allowed"].as<bool>());
  }

  if (input["manifest_url"].is<const char*>()) {
    const char* url = input["manifest_url"].as<const char*>();
    if (securacv_ota_set_manifest_url(url) != ESP_OK) {
      return http_send_error(req, 400, "url_rejected");
    }
  }

  if (input["auto_update"].is<bool>()) {
    const bool enabled = input["auto_update"].as<bool>();
    securacv_ota_set_auto_update(enabled);
    csi_mqtt::publish_update_auto_state(enabled);
  }

  return http_send_json(req, "{\"ok\":true,\"message\":\"Update settings saved.\"}");
}

void setup() {
  Serial.begin(115200);
  serial_wait_for_cdc(SERIAL_CDC_WAIT_MS);

  // ── PSRAM static diet (BEFORE anything can call log_health) ─────────────
  // These used to be internal-DRAM statics: 14 KB health ring + 2 x 2.5 KB
  // fleet buffers, part of the ~41 KB reclaimed for the BLE budget (see the
  // RAM Audit workflow / docs). csi_large_calloc prefers PSRAM and falls
  // back to internal heap; a NULL just disables the owning feature.
  g_health_log_ring = (HealthLogRingEntry*)csi_large_calloc(
      HEALTH_LOG_RING_SIZE * sizeof(HealthLogRingEntry));
  g_health_pending = (HealthPendingLine*)csi_large_calloc(
      health_store::HS_PENDING_SLOTS * sizeof(HealthPendingLine));
  g_fleet_scan_cache = (char*)csi_large_calloc(FLEET_SCAN_CACHE_SIZE);
  g_fleet_scan_snap  = (char*)csi_large_calloc(FLEET_SCAN_CACHE_SIZE);
  {
    // Sizing: 2048-byte byte-ring absorbs one loop pass of NMEA at 9600
    // baud (~960 B/s) with generous slack; the pump also caps reads at
    // 256 B per cycle, so the ring cannot be outrun in steady state.
    void* gps_mem = csi_large_calloc(sizeof(RingBuffer<2048>));
    if (gps_mem) {
      g_gps_rb = new (gps_mem) RingBuffer<2048>();
    } else {
      Serial.println("[--] GPS ring alloc failed — GPS buffering disabled");
    }
  }
  if (!g_health_log_ring) {
    Serial.println("[--] health-log ring alloc failed — Serial-only logging");
  }

  // ── Canary boot banner ──────────────────────────────────────────────────
  {
    boot_info_t bi = {};
    bi.product_name  = "SecuraCV Canary WAP";
    bi.fw_version    = FIRMWARE_VERSION;
    bi.build_date    = __DATE__;
    bi.build_time    = __TIME__;
    bi.device_type   = DEVICE_TYPE;
    bi.model         = "XIAO ESP32S3 Sense";
    // Privacy (Invariant III): do not surface the raw MAC. The device's stable
    // identity is shown separately as "Device ID" (g_device.device_id). mac_address
    // is left null, so boot_banner skips the line.
    bi.board_name    = "XIAO ESP32S3";
    bi.chip_model    = ESP.getChipModel();
    bi.chip_revision = (uint8_t)ESP.getChipRevision();
    bi.cpu_freq_mhz  = (uint16_t)ESP.getCpuFreqMHz();
    bi.cpu_cores     = (uint8_t)ESP.getChipCores();
    bi.flash_mb      = (uint32_t)(ESP.getFlashChipSize() / (1024 * 1024));
    bi.psram_found   = psramFound();
    bi.psram_total_kb = (uint32_t)(ESP.getPsramSize() / 1024);
    bi.psram_free_kb  = (uint32_t)(ESP.getFreePsram() / 1024);
    bi.heap_free_kb   = (uint32_t)(ESP.getFreeHeap() / 1024);
    bi.sdk_version    = ESP.getSdkVersion();

    boot_scene_banner(&bi);
    boot_scene_hardware(&bi);
  }

  // ════════════════════════════════════════════════════════════════════════════
  // PHASE 0: Hardware State & Safe Mode Check (CRITICAL - do this first)
  // ════════════════════════════════════════════════════════════════════════════
  hw_state_init();

  // Check for rapid reboot condition - may enter safe mode
  bool in_safe_mode = safe_mode_check();
  if (in_safe_mode) {
    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════════════════╗");
    Serial.println("║  ⚠️  SAFE MODE ACTIVE — Optional peripherals disabled         ║");
    Serial.println("║  Repeated crashes detected. Core witness functions only.     ║");
    Serial.println("║  Auto-reboots after 60s stable; if it keeps crashing back    ║");
    Serial.println("║  it stays here — use 'Retry full boot' on the dashboard.     ║");
    Serial.println("╚══════════════════════════════════════════════════════════════╝");
    Serial.println();
  }

  // Surface the cause of the last reset so power faults stop masquerading as
  // peripheral failures. A brownout (3.3V rail sag under the WiFi+BLE+camera
  // current spike) typically presents as "BLE/WiFi init failed" or a boot loop
  // into safe mode — logging it explicitly points field triage at the power
  // supply / bulk decoupling rather than the firmware. The full reset reason is
  // also surfaced in sys_monitor's status JSON. See docs/esp32s3_power_resilience.md.
  switch (esp_reset_reason()) {
    case ESP_RST_BROWNOUT:
      log_health(SCV_LOG_ERROR, SCV_CAT_SYSTEM,
                 "Last reset: brownout (supply voltage sag)",
                 "Check 3.3V rail capacity + bulk decoupling — not a firmware fault");
      break;
    case ESP_RST_PANIC:
      log_health(SCV_LOG_WARNING, SCV_CAT_SYSTEM, "Last reset: panic (firmware crash)", nullptr);
      break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      log_health(SCV_LOG_WARNING, SCV_CAT_SYSTEM, "Last reset: watchdog timeout", nullptr);
      break;
    default:
      break;
  }

  setup_wizard::init();
  {
    const char* dn = setup_wizard::get_device_name();
    if (dn) Serial.printf("[OK] Device name: %s\n", dn);
  }

  fix_init(&g_fix);
  utc_init(&g_gps_utc);
  memset(&g_last_record, 0, sizeof(g_last_record));
  g_state_entered_ms = millis();
  g_pending_state = STATE_NO_FIX;

  // ════════════════════════════════════════════════════════════════════════════
  // PHASE 1: Core Systems (crypto, identity) — MUST succeed
  // ════════════════════════════════════════════════════════════════════════════

  // Provision device identity and crypto
  if (!provision_device()) {
    Serial.println();
    Serial.println("[!!] PROVISIONING FAILED - HALTING");
    // Don't infinite loop - just log and continue with limited function
    // The device should still serve HTTP so user can diagnose
    log_health(SCV_LOG_CRITICAL, SCV_CAT_CRYPTO, "Provisioning failed", nullptr);
  }

  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);

  // Flush the witness chain (and mesh replay counters) before any safe-mode
  // recovery/retry reboot, mirroring the /api/reboot path. Without this a
  // recovery reboot rolls the chain back to the last throttled persist and
  // reuses sequence numbers. Read at reboot time, so the mesh hook set later
  // in setup() is picked up too.
  g_safe_mode_pre_reboot = []() {
    persist_chain_state();
    pre_reboot_fn hook = __atomic_load_n(&g_pre_reboot_hook, __ATOMIC_ACQUIRE);
    if (hook) hook();
  };

  // ════════════════════════════════════════════════════════════════════════════
  // PHASE 2: Watchdog — Configure but don't panic on peripheral failures
  // ════════════════════════════════════════════════════════════════════════════

  #if FEATURE_WATCHDOG
  Serial.printf("[..] Watchdog timer: %us timeout\n", WATCHDOG_TIMEOUT_SEC);
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WATCHDOG_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << 0) | (1 << 1),
    .trigger_panic = true
  };
  esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_config);
  if (wdt_err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_init(&wdt_config);
  }
  esp_task_wdt_add(NULL);
  Serial.println("[OK] Watchdog configured");
  #endif

  // ════════════════════════════════════════════════════════════════════════════
  // PHASE 3: Optional Peripherals — Skip if in safe mode, fail gracefully
  // ════════════════════════════════════════════════════════════════════════════

  // Camera comes FIRST among the optional peripherals. esp_camera_init() needs
  // a contiguous DMA-capable internal SRAM block (and, for the PSRAM paths, a
  // sizeable PSRAM block). Once WiFi+HTTP are up, the LWIP/Wi-Fi pools have
  // grabbed and fragmented internal heap; on a cold boot that often pushes
  // OV2640 init into the DRAM-QVGA fallback, or fails it outright. Running
  // the camera init before WiFi reliably gives the driver a clean heap.
  #if FEATURE_CAMERA_PEEK
  if (!in_safe_mode) {
    boot_stage("camera-init");
    Serial.println("[..] Initializing camera for peek/preview...");
    g_camera_initialized = init_camera();
    g_hw.camera_available = g_camera_initialized;
    g_hw.camera_ever_init = g_camera_initialized;
    if (g_camera_initialized) {
      Serial.println("[OK] Camera ready for peek");
    } else {
      Serial.println("[--] Camera init failed - peek disabled (use /api/peek/init to retry)");
    }
  } else {
    Serial.println("[--] Camera init skipped (safe mode)");
  }
  #endif

  // The watchdog was armed above and its next feed is otherwise the first
  // loop() pass — ALL of the Phase-3 init (camera, SD, audio, WiFi, HTTP,
  // BLE) used to share one unfed 8 s budget, so a slow step (camera probe
  // ladder + a slow SD card) panicked the watchdog mid-boot. Feed at each
  // heavy step boundary so no single step inherits the others' spend.
  #if FEATURE_WATCHDOG
  esp_task_wdt_reset();
  #endif

  // Per-phase internal-RAM ledger. The whole multi-radio budget question
  // ("can Bluetooth fit on this build?") turns on who spends what, and the
  // field debugging so far reconstructed it from crash forensics. One line
  // per heavy step makes the budget readable off any boot log.
  auto log_phase_heap = [](const char* phase) {
    Serial.printf("[HEAP] after %-7s: internal free=%u largest=%u\n", phase,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  };
  log_phase_heap("camera");

  // Sealed-snapshot vault: load the persisted operator key + trigger config
  // from NVS (pure state load, no hardware). Skipped in safe mode alongside
  // the camera — without a camera the vault can never arm a capture.
  #if FEATURE_VAULT_SNAPSHOT
  if (!in_safe_mode) {
    vault_snapshot::init();
    if (vault_snapshot::has_pubkey()) {
      Serial.println("[OK] Sealed-snapshot vault key loaded");
    }
  }
  #endif

  // NOTE: Bluetooth/BLE is NOT initialized here. The full stack (controller +
  // NimBLE host + GATT services + discovery subsystems) costs ~55-65 KB of
  // INTERNAL RAM that WiFi, lwIP and the HTTP server need first — a
  // BLE-before-network boot starved the network stack on the FULL build:
  // httpd couldn't create its socket (ENOBUFS), the SoftAP's WPA2 handshake
  // failed (phones looped on the password prompt), and the heap monitor sat
  // in EMERGENCY. The whole BLE bring-up runs from the loop's
  // ble_discovery_start_if_due() once the provisioning join window clears
  // and the setup AP is torn down — the point of MAXIMUM free internal
  // memory (the AP interface's buffers are back), and the heap guard's
  // total-free margin check decides honestly whether BLE fits at all.

  // Initialize SD card storage (with timeout, non-blocking)
  #if FEATURE_SD_STORAGE
  if (!in_safe_mode) {
    boot_stage("sd-init");
    Serial.println("[..] Initializing SD card storage (with timeout)...");
    g_sd_spi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

    if (sd_mount_safe(g_sd_spi, SD_CS_PIN, SD_SPI_FAST)) {
      g_sd_mounted = true;
      g_health.sd_healthy = true;

      // Create directories if needed (non-critical)
      if (!SD.exists("/WITNESS")) SD.mkdir("/WITNESS");
      if (!SD.exists("/HEALTH")) SD.mkdir("/HEALTH");
      if (!SD.exists("/CHAIN")) SD.mkdir("/CHAIN");
      if (!SD.exists("/EXPORT")) SD.mkdir("/EXPORT");

      // CSI event persistence: open /EVENTS/today.ndjson so the MQTT
      // bridge's reconnect path has something to backfill from. The
      // boot-time ring rehydration (re-populating /api/events/today
      // with yesterday's tail) is deferred — it needs a csi_event_inject
      // helper in the canonical CSI library that would touch the
      // firmware/common/csi sources + the staged copy in lockstep, and
      // that's a separate scope.
      csi_event_log::init();

      // Reconcile the chain head against the SD log of record — the NVS
      // cache lags by up to SD_PERSIST_INTERVAL records after a power cut.
      witness_recover_from_sd();

      Serial.println("[OK] SD card ready for witness records");
      log_health(SCV_LOG_INFO, SCV_CAT_STORAGE, "SD card mounted", nullptr);
    } else {
      g_sd_mounted = false;
      g_health.sd_healthy = false;
      Serial.println("[--] SD card not present — witness records buffered in RAM");
      log_health(SCV_LOG_WARNING, SCV_CAT_STORAGE, "SD card not available", nullptr);
    }
  } else {
    Serial.println("[--] SD card init skipped (safe mode)");
  }
  #endif

  log_phase_heap("sd");

  // Fresh feed after the SD phase (bounded at SD_MOUNT_TIMEOUT_MS, but the
  // budget shouldn't come out of the audio/WiFi phases below).
  #if FEATURE_WATCHDOG
  esp_task_wdt_reset();
  #endif

  // Initialize PDM acoustic event detection (T3 smoke / T4 CO cadences).
  // Must run BEFORE the HTTP server starts: audio_mute_sync_at_boot() is
  // single-task-only (it touches the I2S driver synchronously), so it can
  // only be called while setup() is provably the sole task in play.
  #if FEATURE_ACOUSTIC_EVENTS
  // Set when the device boots into the persisted-muted state; the witness
  // record documenting it is deferred until AFTER the boot attestation is
  // signed, so the attestation stays the first record of every boot.
  bool mic_boot_muted = false;
  if (!in_safe_mode) {
    boot_stage("audio-init");
    Serial.println("[..] Initializing PDM acoustic event detection...");
    audio_config_t audio_cfg = AUDIO_CONFIG_DEFAULT;
    // Apply the user's persisted room-noise sensitivity (set via
    // POST /api/audio/config). Invalid/absent pairs fall back to the
    // compiled defaults (ON=800 / OFF=400).
    {
      Preferences sens_prefs;
      if (sens_prefs.begin("securacv", true /* read-only */)) {
        const uint16_t on  = sens_prefs.getUShort("mic_rms_on", 0);
        const uint16_t off = sens_prefs.getUShort("mic_rms_off", 0);
        sens_prefs.end();
        if (off > 0 && on > off) {
          audio_cfg.rms_on_threshold  = on;
          audio_cfg.rms_off_threshold = off;
        }
      }
    }
    if (audio_init(&audio_cfg)) {
      audio_set_event_callback([](const audio_event_t* evt) {
        uint8_t payload[160];
        CborWriter cb(payload, sizeof(payload));
        cb.write_map(5);
        cb.write_text("event_type"); cb.write_text("acoustic");
        cb.write_text("name"); cb.write_text(audio_event_name(evt->event_type));
        cb.write_text("confidence"); cb.write_uint(evt->confidence);
        cb.write_text("cycle_count"); cb.write_uint(evt->cycle_count);
        cb.write_text("time_bucket"); cb.write_uint(evt->time_bucket);
        if (cb.ok()) {
          // Update g_last_record so /api/chain and /api/status report the
          // event as the latest block, consistent with the chain head.
          create_witness_record(payload, cb.size(), RECORD_WITNESS_EVENT, &g_last_record);
        }
        const bool life_safety =
            evt->event_type == AUDIO_EVENT_T3_SMOKE_ALARM ||
            evt->event_type == AUDIO_EVENT_T4_CO_ALARM;
        log_health(life_safety ? SCV_LOG_ALERT : SCV_LOG_NOTICE, SCV_CAT_SENSOR,
                   "Acoustic event detected", audio_event_name(evt->event_type));
        #if FEATURE_AUDIBLE_CHIRP
        if (life_safety) audible_chirp::chirp_alert();
        #endif
        // Hand the event to the MQTT cadence block in loop(), which
        // publishes the /sensing snapshot HA's binary sensors watch.
        snprintf(g_audio_mqtt_event, sizeof(g_audio_mqtt_event), "%s",
                 audio_event_name(evt->event_type));
        g_audio_mqtt_event_ms = millis();
        g_audio_mqtt_dirty = true;
        // And into the csi_event chokepoint, which lands it in the Today
        // timeline, the SD event log, and the MQTT /events stream.
        acoustic_events_emit_detection(evt->event_type, evt->confidence);
        // Sealed-snapshot vault: life-safety triggers may capture ONE frame,
        // sealed to the operator's key. This callback runs synchronously from
        // audio_process() on the main loop, satisfying request_capture()'s
        // loop-task-only contract. Everything is opt-in and fail-closed —
        // the decision table in vault_logic.h skips silently when the
        // trigger isn't enabled (the default).
        #if FEATURE_VAULT_SNAPSHOT
        {
          vault_logic::Trigger vt = vault_logic::Trigger::NONE;
          if      (evt->event_type == AUDIO_EVENT_T3_SMOKE_ALARM) vt = vault_logic::Trigger::T3_SMOKE;
          else if (evt->event_type == AUDIO_EVENT_T4_CO_ALARM)    vt = vault_logic::Trigger::T4_CO;
          else if (evt->event_type == AUDIO_EVENT_GLASS_BREAK)    vt = vault_logic::Trigger::GLASS;
          if (vt != vault_logic::Trigger::NONE) {
            #if FEATURE_QR_PROVISION
            const bool vault_qr_busy = g_qr_scan_active;
            #else
            const bool vault_qr_busy = false;
            #endif
            (void)vault_snapshot::request_capture(vt, vault_camera_usable(),
                                                  vault_qr_busy, sd_is_available());
          }
        }
        #endif
      });
      // Sign every mute / unmute toggle into the witness chain so a later
      // operator can verify when the mic was off and which source flipped
      // it (boot / dashboard) — "was the device listening at the time?"
      audio_set_mute_callback([](bool muted, uint8_t source) {
        uint8_t payload[96];
        CborWriter cb(payload, sizeof(payload));
        cb.write_map(3);
        cb.write_text("event_type"); cb.write_text("mic_mute");
        cb.write_text("muted"); cb.write_uint(muted ? 1 : 0);
        cb.write_text("source"); cb.write_uint(source);
        if (cb.ok()) {
          create_witness_record(payload, cb.size(), RECORD_WITNESS_EVENT, &g_last_record);
        }
        const char* source_name =
            source == AUDIO_MUTE_SOURCE_BOOT ? "boot" :
            source == AUDIO_MUTE_SOURCE_MQTT ? "home_assistant" : "dashboard";
        log_health(SCV_LOG_NOTICE, SCV_CAT_USER,
                   muted ? "Microphone muted" : "Microphone unmuted",
                   source_name);
        // Mirror the state to HA's mic-mute switch entity (retained;
        // cached internally for the reconnect republish), and into the
        // Today timeline so "was it listening?" is visible where users
        // actually look.
        csi_mqtt::publish_mic_state(muted);
        acoustic_events_emit_mute(muted);
      });
      // Honor the user's persisted mute intent. Still single-task here —
      // the HTTP server has not started yet — so the synchronous boot
      // helper may open / skip I2S directly. Runtime toggles after this
      // point go through the deferred audio_mute() path instead.
      Preferences mic_prefs;
      bool persisted_mute = false;
      if (mic_prefs.begin("securacv", true /* read-only */)) {
        persisted_mute = mic_prefs.getBool("mic_muted", false);
        mic_prefs.end();
      }
      const bool boot_ok = audio_mute_sync_at_boot(persisted_mute);
      if (persisted_mute) {
        Serial.println("[OK] Acoustic detector held MUTED by user (NVS)");
        // The witness record documenting the muted boot is emitted after
        // the boot attestation below — see mic_boot_muted.
        mic_boot_muted = true;
      } else if (boot_ok && audio_is_running()) {
        Serial.println("[OK] Acoustic detector armed (T3 smoke / T4 CO)");
      } else {
        Serial.println("[WARN] Acoustic detector start failed");
      }
      // Seed HA's mic-mute switch with the boot state. The broker isn't
      // connected yet; csi_mqtt caches the value and republishes it
      // retained on MQTT_EVENT_CONNECTED.
      csi_mqtt::publish_mic_state(audio_is_muted());
    } else {
      Serial.println("[WARN] Acoustic detector init failed");
    }
  } else {
    Serial.println("[--] Acoustic detector init skipped (safe mode)");
  }
  #endif

  log_phase_heap("audio");

  // Fresh feed before the network phase (TLS keygen, AP bring-up, mDNS
  // probe, HTTP server, BLE) — see the Phase-3 watchdog note above.
  #if FEATURE_WATCHDOG
  esp_task_wdt_reset();
  #endif

  // ── TLS Certificate Initialization ──
  // Skip TLS during first-boot setup: the captive-portal flow runs over plain
  // HTTP on the AP, and a self-signed cert makes captive-portal mini-browsers
  // (iOS CNA, Android) render a blank white screen. There's no sensitive data
  // before WiFi is configured and the AP is the security boundary, so HTTP-only
  // is safe here. Setup completes → reboot → this runs with setup inactive and
  // HTTPS comes up normally for the dashboard and WiFi provisioning.
  #if FEATURE_WIFI_AP && FEATURE_HTTP_SERVER
  if (g_device.initialized && !setup_wizard::is_active()) {
    if (init_tls_cert()) {
      g_tls_enabled = true;
    } else {
      Serial.println("[WARN] TLS unavailable — running in HTTP-ONLY mode");
      Serial.println("[WARN] API traffic is NOT encrypted.");
      g_tls_enabled = false;
    }
  } else if (setup_wizard::is_active()) {
    Serial.println("[..] SETUP MODE: HTTP-only so the captive portal renders");
  }
  #endif

  // Start WiFi Access Point
  #if FEATURE_WIFI_AP
  boot_stage("wifi-ap-start");
  Serial.println("[..] Starting WiFi Access Point...");
  if (start_wifi_ap()) {
    Serial.printf("[WIFI] AP started: %s (password: %s)\n", g_device.ap_ssid, g_device.ap_password);

    #if FEATURE_HTTP_SERVER
    boot_stage("api-start");
    Serial.println("[..] Starting API server...");
    start_http_server();
    #endif
  } else {
    Serial.println("[WARN] WiFi AP failed to start");
  }
  #endif
  // Print quick-connect details early so users do not need to wait for later init phases.
  print_quick_connect_details("[PROV] Quick connect (early):");

  // Initialize mesh network (opera)
  #if FEATURE_MESH_NETWORK
  if (!in_safe_mode) {
    boot_stage("mesh-init");
    Serial.println("[..] Initializing mesh network (opera)...");
    if (mesh_network::init(g_device.privkey, g_device.pubkey, g_device.device_id)) {
      Serial.println("[OK] Mesh network initialized");
      log_health(SCV_LOG_INFO, SCV_CAT_MESH, "Mesh network initialized", nullptr);

      // Set up mesh callbacks
      mesh_network::set_alert_callback([](const mesh_network::MeshAlert* alert) {
        // Log received alerts from other canaries
        char detail[80];
        snprintf(detail, sizeof(detail), "From %s: %s",
                 alert->sender_name, alert->detail);
        log_health((LogLevel)alert->severity, SCV_CAT_MESH, "Opera alert received", detail);

        #if FEATURE_VAULT_SNAPSHOT
        // A peer's SECURITY alert (tamper / motion / breach — not battery
        // housekeeping) can seal one local frame: the opt-in "mesh"
        // trigger. Mesh frames are processed by update() on the loop
        // task, satisfying request_capture()'s loop-only contract.
        const bool security =
            alert->type == mesh_network::ALERT_TAMPER ||
            alert->type == mesh_network::ALERT_MOTION ||
            alert->type == mesh_network::ALERT_BREACH ||
            alert->type == mesh_network::ALERT_OFFLINE_TAMPER;
        if (security) {
          #if FEATURE_QR_PROVISION
          const bool vault_qr_busy = g_qr_scan_active;
          #else
          const bool vault_qr_busy = false;
          #endif
          (void)vault_snapshot::request_capture(vault_logic::Trigger::MESH,
                                                vault_camera_usable(),
                                                vault_qr_busy,
                                                sd_is_available());
        }
        #endif
      });

      mesh_network::load_replay_counters();
      pre_reboot_fn hook = []() { mesh_network::save_replay_counters(); };
      __atomic_store_n(&g_pre_reboot_hook, hook, __ATOMIC_RELEASE);

      mesh_network::set_peer_state_callback([](const mesh_network::OperaPeer* peer,
                                               mesh_network::PeerState old_state,
                                               mesh_network::PeerState new_state) {
        char detail[80];
        snprintf(detail, sizeof(detail), "%s: %s -> %s",
                 peer->name,
                 mesh_network::peer_state_name(old_state),
                 mesh_network::peer_state_name(new_state));
        log_health(SCV_LOG_INFO, SCV_CAT_MESH, "Peer state changed", detail);
      });
    } else {
      Serial.println("[--] Mesh network init failed");
    }

    // Community Chirp channel (v0.2, ESP-NOW). Its /api/chirp/* routes are
    // registered under FEATURE_MESH_NETWORK, but init() was never called, so
    // enable() short-circuited on !g_initialized and the Community > Chirp
    // toggle could never turn on. init() is pure state setup (it does NOT
    // start any radio); the ESP-NOW transport only comes up when the user
    // enables the channel, so this just makes the opt-in reachable.
    if (chirp_channel::init()) {
      Serial.println("[OK] Community chirp channel ready (disabled until enabled)");
      log_phase_heap("mesh");
    } else {
      Serial.println("[--] Community chirp channel init failed");
    }
  } else {
    Serial.println("[--] Mesh network init skipped (safe mode)");
  }
  #endif

  // Arm the deferred BLE bring-up (the init itself runs from the loop's
  // ble_discovery_start_if_due() once the provisioning join window clears —
  // see the internal-RAM budgeting note at the top of Phase 3). _ready_ms is
  // the reference for the AP-only settle / max-hold windows.
  #if FEATURE_BLE || FEATURE_BLUETOOTH || FEATURE_BLE_SCAN
  if (!in_safe_mode) {
    g_ble_discovery_ready    = true;
    g_ble_discovery_ready_ms = millis();
    Serial.println("[..] Bluetooth/BLE bring-up deferred until the join window clears");
    log_phase_heap("network");
  } else {
    Serial.println("[--] Bluetooth/BLE init skipped (safe mode)");
  }
  #endif

  // Initialize WiFi Presence Detection
  #if FEATURE_WIFI_PRESENCE
  if (!in_safe_mode) {
    boot_stage("wifi-presence-init");
    Serial.println("[..] Initializing WiFi presence detection...");
    if (wifi_presence::start()) {
      Serial.println("[OK] WiFi presence monitoring active (probe request counting)");
      log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM, "WiFi presence monitoring started", nullptr);
    } else {
      Serial.println("[--] WiFi presence init failed");
      log_health(SCV_LOG_WARNING, SCV_CAT_SYSTEM, "WiFi presence init failed", nullptr);
    }
  } else {
    Serial.println("[--] WiFi presence skipped (safe mode)");
  }
  #endif

  // RF Presence fusion (BLE + WiFi-probe + CSI scoring FSM). init() only
  // sets up the privacy-preserving session/token state and loads persisted
  // settings — it does NOT start any radio; enable() (user-driven via the
  // RF tab, persisted in NVS) is what makes it score. ble_presence already
  // feeds feed_ble_scan(), which is a no-op until this init runs. Skipped in
  // safe mode like every other optional subsystem.
  if (!in_safe_mode) {
    if (rf_presence::init()) {
      #if FEATURE_VAULT_SNAPSHOT
      // The vault's opt-in "motion" trigger rides the presence engine's
      // IMMEDIATE transition callback — "rf_presence_started" is the
      // fused CSI+RF arrival, emitted the moment presence is confirmed.
      // (The CSI witness bridge would be minutes late: the chokepoint
      // bundles same-state events before committing.) Fires inside
      // rf_presence::update() on the loop task, satisfying
      // request_capture()'s loop-only contract.
      rf_presence::set_event_callback([](const rf_presence::RfEvent* ev) {
        if (ev == nullptr || ev->event_name == nullptr) return;
        if (strcmp(ev->event_name, "rf_presence_started") != 0) return;
        #if FEATURE_QR_PROVISION
        const bool vault_qr_busy = g_qr_scan_active;
        #else
        const bool vault_qr_busy = false;
        #endif
        (void)vault_snapshot::request_capture(vault_logic::Trigger::MOTION,
                                              vault_camera_usable(),
                                              vault_qr_busy,
                                              sd_is_available());
      });
      #endif
      Serial.println(rf_presence::is_enabled()
          ? "[OK] RF presence ready (enabled)"
          : "[OK] RF presence ready (disabled — enable from the RF tab)");
    } else {
      Serial.println("[--] RF presence init failed");
      log_health(SCV_LOG_WARNING, SCV_CAT_SYSTEM, "RF presence init failed", nullptr);
    }
  } else {
    Serial.println("[--] RF presence skipped (safe mode)");
  }

  // Initialize Audible Chirp
  #if FEATURE_AUDIBLE_CHIRP
  if (!in_safe_mode) {
    Serial.println("[..] Initializing audible chirp system...");
    audible_chirp::init();
    // Play boot confirmation chirp
    audible_chirp::chirp_confirm();
    Serial.println("[OK] Audible chirp ready");
    log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM, "Audible chirp initialized", nullptr);
  }
  #endif

  // Initialize System Monitor (always - it's core monitoring)
  #if FEATURE_SYS_MONITOR
  Serial.println("[..] Initializing system monitor...");
  sys_monitor::init();
  {
    char psram_str[16];
    if (sys_monitor::g_sys_metrics.psram_available) {
      sys_monitor::format_bytes(sys_monitor::g_sys_metrics.psram_total, psram_str, sizeof(psram_str));
    } else {
      snprintf(psram_str, sizeof(psram_str), "N/A");
    }
    Serial.printf("[OK] System monitor: %.1fC, Heap: %uKB, PSRAM: %s\n",
                  sys_monitor::g_sys_metrics.temp_celsius,
                  sys_monitor::g_sys_metrics.heap_free / 1024,
                  psram_str);
  }
  log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM, "System monitor initialized", nullptr);
  #endif

  // Initialize Data Management (SD rotation, chain backup, integrity verify)
  #if FEATURE_DATA_MGMT && FEATURE_SD_STORAGE
  Serial.println("[..] Initializing data management...");
  datamgmt::init();
  {
    datamgmt::datamgmt_stats_t dm_stats;
    if (datamgmt::get_stats(&dm_stats)) {
      Serial.printf("[OK] Data mgmt: %u witness, %u health files, backup=%s\n",
                    (unsigned)dm_stats.witness_files,
                    (unsigned)dm_stats.health_files,
                    dm_stats.backup_exists ? "yes" : "no");
    }
  }
  log_health(SCV_LOG_INFO, SCV_CAT_STORAGE, "Data management initialized", nullptr);
  #endif

  // Initialize Power Monitor (battery voltage, SoC, charge state)
  #if FEATURE_POWER_MONITOR
  Serial.println("[..] Initializing power monitor...");
  power_monitor::init(log_health);
  {
    PowerState pwr;
    if (power_monitor::get_state(&pwr)) {
      Serial.printf("[OK] Power monitor: %s, %umV, %u%% SoC, %s\n",
                    power_monitor::mode_name(pwr.monitor_mode),
                    pwr.voltage_mv,
                    pwr.soc_pct,
                    power_monitor::charge_state_name(pwr.charge_state));
    } else {
      Serial.println("[OK] Power monitor initialized");
    }
  }
  log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM, "Power monitor initialized", nullptr);
  #endif

  // Initialize Power Policy Engine (battery-aware feature gating)
  #if FEATURE_POWER_POLICY
  Serial.println("[..] Initializing power policy engine...");
  power_policy::init(log_health);
  Serial.printf("[OK] Power policy: %s\n", power_policy::mode_name(power_policy::get_mode()));
  log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM, "Power policy initialized", nullptr);
  #endif

  // ════════════════════════════════════════════════════════════════════════════
  // PHASE 4: GNSS — Initialize serial, probe only if not in safe mode
  // In safe mode, GPS probing is skipped to avoid potential hangs from
  // misbehaving peripherals that may have caused the reboot loop.
  // ════════════════════════════════════════════════════════════════════════════

  Serial.println();
  Serial.printf("[..] GNSS: %u baud, RX=GPIO%d, TX=GPIO%d\n", GPS_BAUD, GPS_RX_GPIO, GPS_TX_GPIO);
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_GPIO, GPS_TX_GPIO);

  // Probe for GPS module with timeout (skipped in safe mode)
  if (!in_safe_mode) {
    if (gps_probe(Serial1, hw_config::GPS_DETECT_TIMEOUT_MS)) {
      Serial.println("[OK] GPS module detected — waiting for fix");
      g_health.gps_healthy = true;
    } else {
      Serial.println("[--] GPS module not detected — operating without GPS");
      g_health.gps_healthy = false;
    }
  } else {
    Serial.println("[--] GPS probe skipped (safe mode) — GPS state tracking disabled");
  }
  
  // Create boot attestation record
  boot_stage("boot-attestation");
  Serial.println("[..] Creating boot attestation record...");
  uint8_t boot_payload[256];
  size_t boot_len = 0;
  if (build_boot_attestation(boot_payload, sizeof(boot_payload), &boot_len)) {
    create_witness_record(boot_payload, boot_len, RECORD_BOOT_ATTESTATION, &g_last_record);
    Serial.printf("[OK] Boot attestation: seq=%u chain=", g_last_record.seq);
    hex_print(g_last_record.chain_hash, 8);
    Serial.println("...");
  }

  #if FEATURE_ACOUSTIC_EVENTS
  // Sign the boot-state mute record now that the boot attestation anchors
  // the chain. It shows the device booted muted — lets investigators tell
  // "muted before the incident" from "muted in response to it". Only on
  // the muted path; the unmuted default would add one record per reboot.
  if (mic_boot_muted) {
    uint8_t mute_payload[96];
    CborWriter cb(mute_payload, sizeof(mute_payload));
    cb.write_map(3);
    cb.write_text("event_type"); cb.write_text("mic_mute");
    cb.write_text("muted"); cb.write_uint(1);
    cb.write_text("source"); cb.write_uint(AUDIO_MUTE_SOURCE_BOOT);
    if (cb.ok()) {
      create_witness_record(mute_payload, cb.size(), RECORD_WITNESS_EVENT, &g_last_record);
    }
  }
  #endif
  
  // Log boot event
  log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM, "Device boot complete", FIRMWARE_VERSION);

  // ── Signed pull-OTA: confirm or roll back a fresh image, then arm the
  // engine. Reaching this line means identity, storage, WiFi, and the HTTP
  // server all survived the new firmware; the required probe asserts the
  // identity that the witness chain depends on. A failed required probe
  // reboots into the previous firmware (does not return).
  {
    static const securacv_selftest_t k_ota_selftests[] = {
      { "device identity", [](const char*) -> bool {
          return g_device.initialized && g_device.device_id[0] != '\0';
        }, true },
    };
    securacv_ota_register_selftest(&k_ota_selftests[0]);
    securacv_ota_boot_self_test();

    securacv_ota_config_t ota_cfg = SECURACV_OTA_CONFIG_DEFAULT;
    ota_cfg.product          = OTA_PRODUCT;
    ota_cfg.current_version  = FIRMWARE_VERSION;
    ota_cfg.manifest_url     = SECURACV_OTA_MANIFEST_URL;
    ota_cfg.release_pubkey   = SECURACV_OTA_RELEASE_PUBKEY;
    ota_cfg.on_progress      = ota_on_progress;
    ota_cfg.can_install      = ota_can_install;
    ota_cfg.on_before_reboot = ota_before_reboot;
    if (securacv_ota_init(&ota_cfg) == ESP_OK) {
      Serial.printf("[OK] Pull-OTA engine ready (product=%s, version=%s)\n",
                    OTA_PRODUCT, FIRMWARE_VERSION);
    } else {
      Serial.println("[WARN] Pull-OTA engine init failed");
    }

    // Seed the HA auto-update switch state (csi_mqtt caches + retains it).
    csi_mqtt::publish_update_auto_state(securacv_ota_get_auto_update());

    // Witness the outcome of an install reboot. The engine (and the BLE
    // OTA path) recorded the install target the moment the boot partition
    // flipped; running the old version again means rollback.
    char ota_target[SECURACV_OTA_VERSION_MAX];
    if (securacv_ota_take_pending_version(ota_target, sizeof(ota_target))) {
      if (strcmp(ota_target, FIRMWARE_VERSION) == 0) {
        ota_witness_event("fw_update_applied", FIRMWARE_VERSION);
        log_health(SCV_LOG_NOTICE, SCV_CAT_SYSTEM,
                   "Firmware update applied", FIRMWARE_VERSION);
      } else {
        ota_witness_event("fw_update_rolled_back", ota_target);
        log_health(SCV_LOG_WARNING, SCV_CAT_SYSTEM,
                   "Firmware update rolled back", ota_target);
      }
    }
  }

  // Print full provisioning receipt on every boot.
  // Serial output requires local physical access and removes first-boot friction.
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════════════════════╗");
  Serial.println("║            PROVISIONING RECEIPT                               ║");
  Serial.println("║  Save this JSON for the Secure Admin Panel (SAP)              ║");
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.println("║                                                              ║");
  Serial.printf( "║  {                                                           ║\n");
  Serial.printf( "║    \"device_id\": \"%s\",\n", g_device.device_id);
  #if FEATURE_WIFI_AP
  Serial.printf( "║    \"base_url\": \"%s://%s\",\n",
                 g_tls_enabled ? "https" : "http", WiFi.softAPIP().toString().c_str());
  #endif
  Serial.printf( "║    \"token\": \"%s\",\n", g_device.api_token_str);
  Serial.printf( "║    \"pubkey_fp\": \"%s\",\n", g_device.fingerprint_hex);
  Serial.printf( "║    \"firmware\": \"%s\",\n", FIRMWARE_VERSION);
  {
    char hw_token[device_pseudonym::HEX_LEN + 1];
    if (!device_pseudonym::device_id_hex(hw_token, sizeof(hw_token))) hw_token[0] = '\0';
    Serial.printf( "║    \"hw_token\": \"%s\",\n", hw_token);
  }
  Serial.printf( "║    \"ap_ssid\": \"%s\",\n", g_device.ap_ssid);
  Serial.printf( "║    \"ap_password\": \"%s\",\n", g_device.ap_password);
  if (g_tls_enabled) {
    Serial.printf("║    \"tls_cert_fp\": \"%s\",\n", g_tls_cert_fp_hex);
  }
  Serial.printf( "║    \"provisioned_at\": \"boot:%lu\"\n", (unsigned long)g_device.boot_count);
  Serial.println("║  }                                                           ║");
  Serial.println("║                                                              ║");
  Serial.println("╚══════════════════════════════════════════════════════════════╝");
  print_quick_connect_details("[PROV] Quick connect:");

  boot_scene_ready(
      "It will now create a signed witness record",
      "every second and store it to the SD card.",
      "Nobody can alter these records after the fact."
  );
  boot_separator();
  boot_kv("Device ID", g_device.device_id);
  boot_kv("WiFi AP",   g_device.ap_ssid);
  boot_kv("Password",  g_device.ap_password);
  #if FEATURE_WIFI_AP
  boot_kvf("Dashboard", "%s://%s",
           g_tls_enabled ? "https" : "http",
           WiFi.softAPIP().toString().c_str());
  #endif
  boot_blank();
  boot_line("    Commands: h=help i=id s=stat t=time g=gps c=cam m=sys r=data b=bat");
  boot_line("    Token + WiFi credentials are printed above on every boot");
  boot_line("    Hold  BOOT (>3s)   = factory reset");
  boot_separator();
  Serial.println();
  print_table_header();
}

// One-shot Bluetooth/BLE bring-up, deferred out of setup() — both the stack
// INIT (its ~55-65 KB internal-RAM cost must come after WiFi/lwIP/httpd have
// taken theirs, and after the setup AP is torn down that memory is back) and
// the radio ACTIVITY (any BLE duty during the join window starves a phone's
// WPA2 handshake to the SoftAP).
//
// The blocking INIT half of the bring-up runs on a ONE-SHOT WORKER TASK,
// never on the loop task: NimBLE controller/host init synchronizes with the
// WiFi coexistence layer and can block its caller well past the loop's 8 s
// watchdog budget (field crash: "task_wdt: loopTask" ~21 s after boot, both
// cores idle — the loop was parked inside the bring-up while the gate ran it
// inline). Same worker pattern as the SD mount and the MJPEG stream.
// Priority 1, internal-RAM stack; not watchdog-subscribed; deletes itself.
//
// The worker does NOT emit CSI witness events and does NOT start the
// discovery radio: csi_event_emit's bundler/ceiling state is documented
// single-threaded on the main loop (only the ring is mutex-protected), so
// the worker only records outcomes; the loop's finalize stage (below)
// performs every csi-emitting follow-up and the quick radio starts.
#if FEATURE_BLE || FEATURE_BLUETOOTH || FEATURE_BLE_SCAN
static volatile bool   g_ble_bringup_done      = false;  // worker -> loop handoff
static bool            g_ble_bringup_finalized = false;  // loop-only
static volatile int8_t g_ble_mgr_result        = 0;      // 0=not attempted, 1=ok, -1=failed

static void ble_bringup_task(void*) {
  Serial.printf("[HEAP] before BLE bring-up: internal free=%u largest=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

  // Pairing channel first — it owns NimBLEDevice::init() (GAP name, TX
  // power, MTU, security) when both features are compiled in.
  #if FEATURE_BLUETOOTH
  {
    g_ble_init_attempted = true;  // self-test: distinguishes SKIP from FAIL
    Serial.println("[..] Initializing Bluetooth Low Energy (post-join-window)...");
    // Push device metadata into bluetooth_channel BEFORE init(). The DIS
    // characteristics get baked during service creation; setting these
    // afterwards has no effect until the next reinit.
    bluetooth_channel::set_device_metadata(FIRMWARE_VERSION, g_device.fingerprint_hex);
    if (bluetooth_channel::init()) {
      Serial.println("[OK] Bluetooth initialized");
      log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Bluetooth initialized", nullptr);
      // Hand the offline-console module the device's short fingerprint
      // and firmware version so its snapshot JSON identifies us. Both
      // are owned by the .ino — the module copies into its own buffers.
      ble_console::set_device_metadata(g_device.fingerprint_hex, FIRMWARE_VERSION);

      // BLE GATT Status Service — register on the shared NimBLE server
      // created by bluetooth_channel. Exposes battery, health, chain,
      // and SD status to companion phones over standard GATT.
      #if FEATURE_BLE_STATUS
      {
        NimBLEServer* ble_server = NimBLEDevice::getServer();
        if (ble_server) {
          if (ble_status::init(ble_server, g_device.device_id, FIRMWARE_VERSION,
                               &g_device.seq)) {
            Serial.println("[OK] BLE GATT status service registered");
            log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE status service started", nullptr);
          } else {
            Serial.println("[--] BLE GATT status service init failed");
          }
        }
      }
      #endif
    } else {
      Serial.println("[--] Bluetooth init failed (reason recorded for self-test)");
    }
  }
  #endif

  // BLE Discovery (Opera/Chirp/Nearby) — piggybacks on the stack above, or
  // brings it up itself on builds without FEATURE_BLUETOOTH. The CSI module
  // registry is up by now (web server started before the window cleared), so
  // the lifecycle witness events route through the chokepoint directly.
  #if FEATURE_BLE
  {
    g_ble_init_attempted = true;  // self-test: distinguishes SKIP from FAIL
    Serial.println("[..] Initializing BLE Discovery subsystem...");

    // Build device ID hash hex string from pubkey fingerprint
    char ble_device_id_hex[20];
    hex_to_str(ble_device_id_hex, g_device.pubkey_fp, 8);

    if (ble_manager::init(ble_device_id_hex, FIRMWARE_VERSION,
                          &g_device.seq, g_device.chain_head)) {
      Serial.println("[OK] BLE Discovery initialized");
      log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE Discovery initialized", nullptr);
      g_ble_mgr_result = 1;   // lifecycle emit + radio start happen on the loop
    } else {
      Serial.println("[--] BLE Discovery initialization failed — operating without BLE discovery");
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH, "BLE Discovery init failed", nullptr);
      g_ble_mgr_result = -1;
    }
  }
  #endif

  Serial.printf("[HEAP] after BLE bring-up: internal free=%u largest=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  __atomic_store_n(&g_ble_bringup_done, true, __ATOMIC_RELEASE);
  vTaskDelete(NULL);
}

// Loop-side finalize stage: everything that must stay on the main loop —
// CSI witness emits (single-threaded chokepoint contract) and the quick
// radio starts (opera/nearby advertising, boot chirp, Scout scan; these are
// short host-task handoffs, unlike the controller init the worker owns).
static void ble_bringup_finalize_if_done() {
  if (g_ble_bringup_finalized ||
      !__atomic_load_n(&g_ble_bringup_done, __ATOMIC_ACQUIRE)) {
    return;
  }
  g_ble_bringup_finalized = true;

  #if FEATURE_BLE
  if (g_ble_mgr_result == 1) {
    // spec/event_contract.md §10: route the lifecycle event through the
    // CSI chokepoint so the witness-chain row's allow-list is enforced.
    ble_events_emit_initialized();
    ble_manager::operaStart();
    ble_manager::nearbyStart();
    // Boot chirp. Witness-chain side: chirp_sent through the chokepoint so
    // the wire format respects spec/event_contract.md §10's allow-list.
    ble_manager::sendChirp(CHIRP_BOOT);
    ble_events_emit_chirp_sent("boot");
  } else if (g_ble_mgr_result == -1) {
    ble_events_emit_init_failed("ble_manager_init_returned_false");
  }
  #endif

  // BLE Scout (CSI room attribution): csi_integration ran its state-only
  // init at web-server start; permit the radio and complete the deferred
  // NimBLE scan now. Ordered after the worker so bluetooth_channel's heap
  // guard ran against a clean NimBLEDevice::isInitialized()==false state —
  // the Scout starting the stack first is exactly how the guard used to be
  // bypassed on FULL builds (Codex P1 on #824). Runs on the loop because
  // ble_scout_init emits its lifecycle event through the CSI chokepoint;
  // the scan attach itself is a short call once the stack is already up.
  #if FEATURE_BLE_SCAN
  ble_scout::ble_scout_allow_radio();
  ble_scout::ble_scout_init();
  #endif

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
             "BLE bring-up finalized (post-provisioning window)", nullptr);
}
#endif  // FEATURE_BLE || FEATURE_BLUETOOTH || FEATURE_BLE_SCAN

static void ble_discovery_start_if_due() {
#if FEATURE_BLE || FEATURE_BLUETOOTH || FEATURE_BLE_SCAN
  // Stage 2: adopt a finished worker's results (CSI emits + radio starts).
  ble_bringup_finalize_if_done();

  if (!g_ble_discovery_ready || g_ble_discovery_started) return;
  // Gate on the AP being DOWN, not merely WL_CONNECTED: the SoftAP is held up
  // for AP_DROP_GRACE_MS after the STA gets an IP so the provisioning phone can
  // re-associate and read the success card, and starting the 99%-duty scan
  // during that grace would starve the very handoff it protects. Treat a
  // runtime AP-only/no-STA state (e.g. after /api/wifi/disconnect) as AP-only
  // too, so it starts on the settle path rather than the long fallback.
  const bool ap_active = g_wifi_status.ap_active;
  const bool ap_only_mode =
      g_wifi_ap_only || (g_wifi_status.state == WIFI_PROV_AP_ONLY);
  if (!provisioning_logic::ble_discovery_start_due(
          ap_only_mode, ap_active, millis(), g_ble_discovery_ready_ms,
          BLE_DISCOVERY_AP_ONLY_SETTLE_MS, BLE_DISCOVERY_MAX_HOLD_MS)) {
    return;
  }
  g_ble_discovery_started = true;  // one attempt, whatever the outcome

  // Internal-RAM stack (no PSRAM task stacks with the prebuilt core). If the
  // task can't even be created, record the attempt so the self-test reports
  // FAIL rather than sitting on "Starting up…" forever.
  if (xTaskCreate(ble_bringup_task, "ble_bringup", 8192, nullptr, 1, nullptr)
      != pdPASS) {
    g_ble_init_attempted = true;
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "BLE bring-up task create failed (out of memory)", nullptr);
  }
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// LOOP
// ════════════════════════════════════════════════════════════════════════════

void loop() {
  #if FEATURE_WATCHDOG
  esp_task_wdt_reset();
  #endif

  // Service the captive DNS redirector whenever it's up (it self-guards on
  // s_dns_running), so it answers AP clients in steady state too — not only
  // while the first-boot wizard is active.
  setup_wizard::dns_process();
  if (setup_wizard::is_active()) {
    setup_wizard::check_timeout();
    if (WiFi.status() == WL_CONNECTED &&
        __atomic_load_n(&g_setup_grace_reboot_at_ms, __ATOMIC_ACQUIRE) == 0) {
      // The join succeeded, but the provisioning phone is mid-handoff: the
      // single radio just dragged the SoftAP to the STA's channel, and the
      // phone needs to re-associate + poll /api/wifi to render the success
      // card. Rebooting NOW (the old behavior) killed the AP ~1 s after the
      // join and made every provisioning attempt look failed. Complete setup
      // immediately, but defer the steady-state reboot past the same grace
      // window the AP teardown honors.
      setup_wizard::mark_complete();
      // mark_complete() stops the captive DNS; the phone still needs
      // canary.local through the grace window.
      setup_wizard::start_captive_portal();
      __atomic_store_n(&g_setup_grace_reboot_at_ms, millis() + AP_DROP_GRACE_MS,
                       __ATOMIC_RELEASE);
      Serial.println("[OK] Setup complete — WiFi connected; rebooting after the provisioning grace window...");
    }
  } else if (provisioning_logic::deferred_reboot_due(
                 millis(),
                 __atomic_load_n(&g_setup_grace_reboot_at_ms, __ATOMIC_ACQUIRE))) {
    __atomic_store_n(&g_setup_grace_reboot_at_ms, 0u, __ATOMIC_RELEASE);
    Serial.println("[OK] Provisioning grace elapsed — rebooting into steady state...");
    delay(500);
    ESP.restart();
  }

  // Start BLE Discovery's radio activity once the SoftAP join window is clear
  // (STA joined home WiFi, or AP-only settle elapsed) — deferred from setup()
  // so its active scan can't starve a provisioning phone's WPA2 handshake.
  boot_stage("loop:ble-finalize");   // prime wdt-starvation suspect (~17 s
  ble_discovery_start_if_due();      // post-boot); the breadcrumb convicts
  boot_stage("loop:steady");         // or clears it on the next crash

  #if FEATURE_QR_PROVISION
  // Onboarding wave: unprovisioned + camera up = scanning for a code is
  // the safe idle. Stands down the moment WiFi is configured or a phone
  // session / peek owns the camera.
  qr_auto_scan_tick(millis());
  #endif

  #if FEATURE_VAULT_SNAPSHOT
  // Adopt a finished seal worker (emits the frame_sealed witness event and
  // rotates the /VAULT ring — both loop-owned operations), and drain a
  // pending dashboard Test capture latched by the HTTP task.
  vault_snapshot::poll_completion();
  if (g_vault_test_pending) {
    g_vault_test_pending = false;
    #if FEATURE_QR_PROVISION
    const bool vault_qr_busy = g_qr_scan_active;
    #else
    const bool vault_qr_busy = false;
    #endif
    const vault_logic::Decision d = vault_snapshot::request_capture(
        vault_logic::Trigger::TEST, vault_camera_usable(), vault_qr_busy,
        sd_is_available());
    log_health(SCV_LOG_NOTICE, SCV_CAT_USER, "Vault test capture",
               vault_logic::decision_name(d));
  }
  #endif

  // ════════════════════════════════════════════════════════════════════════════
  // HARDWARE STATE MANAGEMENT — Update safe mode, track stability
  // ════════════════════════════════════════════════════════════════════════════
  safe_mode_update();

  // ── Signed pull-OTA: daily jittered check, HA command drain, state publish ──
  {
    const uint32_t ota_now = millis();
    ota_scheduler_process(ota_now);

    // HA pressed Install on the update entity (latched on the esp_mqtt
    // task, acted on here so flash decisions stay on the main loop).
    if (csi_mqtt::take_pending_install()) {
      log_health(SCV_LOG_NOTICE, SCV_CAT_SYSTEM,
                 "Firmware install requested from Home Assistant", nullptr);
      securacv_ota_check_and_install();
    }
    const int ota_auto_cmd = csi_mqtt::take_pending_auto();
    if (ota_auto_cmd >= 0) {
      securacv_ota_set_auto_update(ota_auto_cmd == 1);
      csi_mqtt::publish_update_auto_state(ota_auto_cmd == 1);
      log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM,
                 ota_auto_cmd == 1 ? "Auto-update turned on"
                                   : "Auto-update turned off", nullptr);
    }

    // Witness the moment a download starts — the chain should show every
    // install attempt, not just outcomes.
    const securacv_ota_state_t ota_st = securacv_ota_get_state();
    if (ota_st == SECURACV_OTA_DOWNLOADING &&
        g_ota_last_seen_state != SECURACV_OTA_DOWNLOADING) {
      const securacv_ota_manifest_t* m = securacv_ota_get_manifest();
      ota_witness_event("fw_update_started", (m != NULL) ? m->version : "?");
    }
    g_ota_last_seen_state = ota_st;

    // Alert once per discovered version: the daily check used to complete
    // silently, so an available update was only visible if the operator
    // happened to open Settings -> Device. One NOTICE per version lands it
    // in the Records feed (and the dashboard banner reads the same status).
    if (securacv_ota_update_available()) {
      static char s_ota_alerted_version[SECURACV_OTA_VERSION_MAX] = "";
      const securacv_ota_manifest_t* am = securacv_ota_get_manifest();
      if (am != NULL &&
          strncmp(s_ota_alerted_version, am->version,
                  sizeof(s_ota_alerted_version)) != 0) {
        snprintf(s_ota_alerted_version, sizeof(s_ota_alerted_version), "%s",
                 am->version);
        log_health(SCV_LOG_NOTICE, SCV_CAT_SYSTEM,
                   "Firmware update available", am->version);
      }
    }

    // Push entity changes promptly (progress %, transitions) and refresh
    // the retained snapshot every 30 s.
    static uint32_t s_last_ota_pub_ms = 0;
    const bool periodic = (int32_t)(ota_now - s_last_ota_pub_ms) >= 30000;
    if (csi_mqtt::connected() && (g_ota_publish_pending || periodic)) {
      g_ota_publish_pending = false;
      s_last_ota_pub_ms = ota_now;
      ota_publish_update_state();
    }
  }

  // Handle serial commands
  handle_serial_commands();

  // USB evidence drive: apply deferred host events (eject -> verify/install)
  usb_evidence_drive::poll();

  // Check boot button:
  //   Short press (<2s) = open provisioning gate
  //   Medium hold (1.2-3s) = print device info
  //   Long hold (>3s)  = factory reset (handled by existing code)
  static uint32_t boot_btn_start = 0;
  static bool boot_btn_was_pressed = false;
  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);

  if (pressed && !boot_btn_was_pressed) {
    boot_btn_start = millis();
    boot_btn_was_pressed = true;
  }

  if (!pressed && boot_btn_was_pressed) {
    uint32_t duration = millis() - boot_btn_start;
    boot_btn_was_pressed = false;
    boot_btn_start = 0;

    if (duration >= BOOT_LONG_PRESS_MS) {
      // Factory reset — handled by existing code path below
      // (already exists in the codebase)
    } else if (duration >= BOOT_BUTTON_HOLD_MS) {
      // Medium hold: print device info (existing behavior)
      if (g_device.initialized) {
        print_identity_block();
        print_time_block();
        print_gps_block();
        hw_state_print();
        print_status_bar();
        print_table_header();
      }
    } else if (duration >= BOOT_SHORT_PRESS_MS) {
      // Short press: open provisioning gate. Stamp the press time so the
      // gate can self-close after PROVISIONING_GATE_TTL_MS. Sentinel-1
      // guard handles the exceedingly rare case where millis() is 0.
      uint32_t now_ms = millis();
      __atomic_store_n(&g_provisioning_gate_opened_at,
                       (now_ms == 0) ? 1 : now_ms,
                       __ATOMIC_RELAXED);
      Serial.printf("[AUTH] Provisioning gate OPENED (receipt available for %lu seconds)\n",
                    (unsigned long)(PROVISIONING_GATE_TTL_MS / 1000));
      log_health(SCV_LOG_INFO, SCV_CAT_AUTH, "Provisioning gate opened", "BOOT button");
      // Blink LED 3x to confirm. Skipped while an SD mount attempt is in
      // flight: on the XIAO ESP32-S3 the user LED shares GPIO21 with SD
      // chip-select, and driving it mid-transaction on the mount worker
      // would glitch CS and corrupt the mount.
      #ifdef LED_BUILTIN
      if (!sd_mount_in_flight()) {
        for (int i = 0; i < 3; i++) {
          digitalWrite(LED_BUILTIN, HIGH);
          delay(100);
          digitalWrite(LED_BUILTIN, LOW);
          delay(100);
        }
      }
      #endif
    }
  }

  // ════════════════════════════════════════════════════════════════════════════
  // GPS DATA HANDLING — Non-blocking with state tracking
  // ════════════════════════════════════════════════════════════════════════════

  // Read GPS data (non-blocking - only reads what's available)
  bool gps_data_received = false;
  int gps_bytes_read = 0;
  while (Serial1.available() && gps_bytes_read < 256) {  // Limit per-cycle reads
    uint8_t gps_b = (uint8_t)Serial1.read();  // always drain the UART
    if (g_gps_rb) g_gps_rb->push(gps_b);
    gps_data_received = true;
    gps_bytes_read++;
  }

  // Parse NMEA lines
  static char line[256];
  size_t len;
  bool got_valid_sentence = false;
  while (read_nmea_line(line, sizeof(line), &len)) {
    parse_nmea(line, &g_fix);
    got_valid_sentence = true;
  }

  // No SNTP path exists in this sketch — GPS is the only wall-clock source
  // available. Seed/correct the system clock from it once RMC has a
  // validated date/time (cheap: bails out immediately once synced and not
  // due for a drift-correction check).
  sync_clock_from_gps();

  // Update GPS hardware state
  gps_update_state(gps_data_received, g_fix.valid);

  // Sync hardware state to legacy health tracking
  g_health.gps_healthy = g_hw.gps_available && (g_hw.gps_state >= GPS_DETECTED);

  // Update state machine
  g_state = update_state(&g_fix, g_state);

  // Update motion filter (publishes display_lat/lon/speed to API/UI). The raw
  // g_fix is preserved for the witness chain — only the presentation layer
  // sees the anchor-locked values.
  motion_filter_update(&g_fix, g_state);

  // ════════════════════════════════════════════════════════════════════════════
  // PERIODIC HARDWARE CHECKS — SD card hot-plug, etc.
  // ════════════════════════════════════════════════════════════════════════════

  // canary.local catch-all steward: staggered initial claim + double-claim
  // conflict resolution (see claim_catch_all_hostname / catchall_logic.h).
  catch_all_tick();

  #if FEATURE_SD_STORAGE
  // Periodic SD card check (handles hot-plug/unplug). Mount attempts run on
  // the background mount worker; results are adopted here. Safe mode never
  // attempts SD work (boot skipped it; the loop path honors the same
  // contract — the old unconditional remount is what crash-looped safe mode).
  sd_periodic_check(g_sd_spi, SD_CS_PIN, SD_SPI_FAST);

  // Sync SD hardware state to legacy flags — and provision the card layout
  // on a fresh mount transition. This covers mounts that concluded AFTER the
  // boot-time wait budget (slow card) and cards hot-plugged later: without
  // it, a late-mounting card would be missing /WITNESS etc. and every
  // witness write would fail.
  {
    const bool sd_now = sd_is_available();
    if (sd_now && !g_sd_mounted) {
      if (!SD.exists("/WITNESS")) SD.mkdir("/WITNESS");
      if (!SD.exists("/HEALTH")) SD.mkdir("/HEALTH");
      if (!SD.exists("/CHAIN")) SD.mkdir("/CHAIN");
      if (!SD.exists("/EXPORT")) SD.mkdir("/EXPORT");
      csi_event_log::init();  // idempotent; self-defers if the card vanished
      witness_recover_from_sd();  // NVS may lag the card's chain tail
      log_health(SCV_LOG_INFO, SCV_CAT_STORAGE, "SD card mounted", nullptr);
    }
    g_sd_mounted = sd_now;
    g_health.sd_healthy = sd_now;
  }
  #endif

  // Advance the RF-presence fusion FSM (session/token rotation, decay,
  // baseline bookkeeping). Self-guards on init state and is cheap when the
  // feature is disabled — the time-based housekeeping still needs to run.
  rf_presence::update();

  // Yield to prevent watchdog issues
  yield();

  // ════════════════════════════════════════════════════════════════════════════
  // HEALTH & STATUS UPDATES
  // ════════════════════════════════════════════════════════════════════════════

  uint32_t now = millis();
  g_health.uptime_sec = now / 1000;
  g_health.free_heap = ESP.getFreeHeap();
  if (g_health.free_heap < g_health.min_heap) {
    g_health.min_heap = g_health.free_heap;
  }

  // Check WiFi connection periodically
  wifi_check_connection();

  // Yield after WiFi check
  yield();

  // Update mesh network
  #if FEATURE_MESH_NETWORK
  mesh_network::update();
  // Service the community chirp channel too (no-op until the user enables
  // it; needed so cooldowns, bloom-filter resets and relay TTLs advance).
  chirp_channel::update();
  {
    static uint32_t s_last_replay_save_ms = 0;
    uint32_t now = millis();
    constexpr uint32_t REPLAY_SAVE_INTERVAL_MS = 300000;
    if ((int32_t)(now - s_last_replay_save_ms) >= (int32_t)REPLAY_SAVE_INTERVAL_MS) {
      s_last_replay_save_ms = now;
      mesh_network::save_replay_counters();
    }
  }
  #endif

  // Update Bluetooth (legacy channel)
  #if FEATURE_BLUETOOTH
  bluetooth_channel::update();
  #endif

  // Update BLE GATT status characteristics (rate-limited internally)
  #if FEATURE_BLE_STATUS
  ble_status::update();
  #endif

  // Feed live status into the fleet-link presence beacon (ble_opera applies it
  // on its periodic refresh, and the chirp-restore path re-applies it too).
  // Fail-safe: sources without a value keep their unknown sentinels (battery
  // -1 -> 0xFF, flags default 0). Health has no 0..100 source on the WAP yet
  // (ble_status omits it likewise), so it rides as unknown (0xFF). tamper /
  // alert_active have no cheap WAP-side getter today — left 0 (follow-up).
  #if FEATURE_BLE && FEATURE_BLE_OPERA
  {
    uint8_t beacon_flags = 0;
    int beacon_battery = -1;
    int beacon_health  = -1;
    #if FEATURE_POWER_MONITOR
    {
      PowerState pwr;
      if (power_monitor::get_state(&pwr)) {
        beacon_battery = (pwr.soc_pct > 100) ? 100 : (int)pwr.soc_pct;
      }
    }
    #endif
    #if FEATURE_ACOUSTIC_EVENTS
    if (audio_is_muted()) beacon_flags |= FLEET_BEACON_FLAG_MIC_MUTED;
    #endif
    #if FEATURE_SYS_MONITOR
    if (sys_monitor::get_degrade_level() != sys_monitor::DEGRADE_NONE)
      beacon_flags |= FLEET_BEACON_FLAG_DEGRADED;
    #endif
    if (WiFi.isConnected()) beacon_flags |= FLEET_BEACON_FLAG_ON_WIFI_STA;
    ble_opera::setBeaconStatus(beacon_flags, beacon_battery, beacon_health);
  }
  #endif

  // Update BLE Discovery (Opera/Chirp/Nearby)
  #if FEATURE_BLE
  if (ble_manager::isAvailable()) {
    ble_manager::update();
    ble_manager::chirpHeartbeatCheck();
  }
  #endif

  // Process WiFi presence probe queue (drains ISR queue, hashes, dedup)
  #if FEATURE_WIFI_PRESENCE
  wifi_presence::process_queue();
  #endif

  // Advance the identify "locate me" scheduler (re-arms the blink+chirp
  // pattern for its window; cheap no-op when inactive). The LED/buzzer output
  // is produced by audible_chirp::update() below, so identify is effective on
  // builds with FEATURE_AUDIBLE_CHIRP enabled (the default dev/release set).
  identify_tick();

  // Advance audible chirp state machine (non-blocking playback)
  #if FEATURE_AUDIBLE_CHIRP
  audible_chirp::update();

  // v0.5: monthly NFPA-72 §14 supervised-circuit self-test chirp.
  // Plays PATTERN_SELFTEST_OK at most once per 30 days, only during
  // waking hours (06:00–22:00 local time, when SNTP is synced),
  // only when the device is operationally healthy (no active alarm or
  // trouble condition on the Beacon channel — see codex P1 and gemini
  // high-P review on #461), and only when no other chirp is playing
  // (start_pattern would preempt an active alert otherwise).
  //
  // Persistence: the last-played timestamp is persisted to NVS as a
  // unix wall-clock seconds value so reboots don't perpetually skip the
  // 30-day cadence (codex P2 review on #461). If wall clock isn't
  // synced yet, we don't persist anything (the value is meaningless
  // without time-of-day).
  {
    static uint32_t s_last_selftest_unix = 0;          // cached from NVS
    static bool     s_selftest_nvs_loaded = false;
    static const uint32_t SELFTEST_INTERVAL_S = 30UL * 24UL * 60UL * 60UL;

    if (!s_selftest_nvs_loaded) {
      uint32_t persisted = 0;
      if (nvs_get_u32("st_chirp_at", &persisted)) {
        s_last_selftest_unix = persisted;
      }
      s_selftest_nvs_loaded = true;
    }

    time_t t = time(nullptr);
    if (t >= 1700000000) {  // SNTP synced
      const uint32_t now_unix = (uint32_t)t;
      // On first run (no NVS value): seed the 30-day clock at now so
      // we don't fire immediately after a fresh provisioning.
      if (s_last_selftest_unix == 0) {
        s_last_selftest_unix = now_unix;
        nvs_set_u32("st_chirp_at", s_last_selftest_unix);
      } else if (now_unix - s_last_selftest_unix >= SELFTEST_INTERVAL_S) {
        struct tm* tm_info = localtime(&t);
        if (tm_info) {
          const int hour = tm_info->tm_hour;
          const bool waking = (hour >= 6 && hour < 22);
          if (waking) {
            // Suppress when:
            //   - another chirp is currently playing (don't preempt an
            //     alarm tone with the quiet self-test tone)
            //   - Beacon is in ALARM or TROUBLE state (self-test would
            //     mask an active life-safety alert — codex P1 + gemini)
            bool ok_to_play = !audible_chirp::is_playing();
            #if FEATURE_BEACON_CHANNEL
            const beacon_channel::BeaconStatus bs = beacon_channel::get_status();
            if (bs.state == beacon_channel::BEACON_STATE_ALARM ||
                bs.state == beacon_channel::BEACON_STATE_TROUBLE) {
              ok_to_play = false;
            }
            #endif
            if (ok_to_play) {
              audible_chirp::play_pattern(audible_chirp::PATTERN_SELFTEST_OK);
              s_last_selftest_unix = now_unix;
              nvs_set_u32("st_chirp_at", s_last_selftest_unix);
              log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM,
                         "self-test chirp played (NFPA-72 supervised)", nullptr);
            }
            // If !ok_to_play, leave s_last_selftest_unix unchanged and
            // retry on the next loop tick.
          }
        }
      }
    }
  }
  #endif

  // Update system monitor (temp, heap, alerts, degradation, SD health)
  #if FEATURE_SYS_MONITOR
  sys_monitor::update(log_health);
  #endif

  // Data management auto-processing (rate-limited: rotation every 5 min, backup every hour)
  #if FEATURE_DATA_MGMT && FEATURE_SD_STORAGE
  datamgmt::process(g_device.chain_head, g_device.seq, g_device.privkey);
  #endif

  // Update power monitor (ADC sample, SoC, charge state)
  #if FEATURE_POWER_MONITOR
  power_monitor::process();

  // Persist battery health history (runtime minutes, SoC minimum,
  // brownout count, last-full-charge) every 10 minutes so the stats
  // survive power loss. Cheap: four NVS writes.
  {
    static uint32_t s_last_batt_hist_ms = 0;
    if (now - s_last_batt_hist_ms >= 600000UL) {
      s_last_batt_hist_ms = now;
      power_monitor::persist_history();
    }
  }
  #endif

  // Update power policy (mode transitions, feature gating)
  #if FEATURE_POWER_POLICY
  power_policy::process();

  // Consume the LOW_POWER deep-sleep request (55 s sleep / 5 s wake duty
  // cycle below 5% SoC). Without this the policy sets the pending flag
  // and nothing ever sleeps. Close out persistent state first; deep
  // sleep reboots the chip, so setup() re-evaluates power mode on wake.
  if (power_policy::should_deep_sleep() && !power_monitor::is_charging()) {
    uint32_t sleep_sec = power_policy::get_sleep_duration_sec();
    log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM,
               "low battery: entering timed deep sleep", nullptr);
    persist_chain_state();
    power_monitor::persist_history();
    power_policy::ack_deep_sleep();
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * 1000000ULL);
    esp_deep_sleep_start();
    // Does not return
  } else if (power_policy::should_deep_sleep()) {
    // Charger appeared between policy evaluation and here -- cancel.
    power_policy::ack_deep_sleep();
  }
  #endif

  // Camera power manager: shed the peek stream on thermal-critical or
  // battery policy, park the idle sensor, re-arm when conditions clear.
  #if FEATURE_CAMERA_PEEK
  camera_power_tick();
  #endif

  // Flush staged health-log lines to the per-boot /HEALTH file (the SD
  // tier that makes crash forensics survive the reboot).
  health_store_drain();

  // Pump the acoustic pipeline. Drains up to 8×20 ms PDM frames per call
  // against an 8-deep DMA ring, so the loop must come back within ~160 ms —
  // holds here because loop() ends in delay(1) and HTTP runs in the httpd
  // task. Envelope timing rides the module's sample-stream clock, so even
  // a burst-drain after a rare longer stall keeps beep/gap durations
  // intact. Event/mute callbacks fire synchronously from this call, in
  // the same task context as the witness-record code above.
  #if FEATURE_ACOUSTIC_EVENTS
  audio_process();
  #endif

  // Drain CSI ring, finalize 1-Hz feature windows, dispatch to v1 modules.
  // The WiFi task fills the ring at up to 20 Hz; a stall longer than ~800 ms
  // drops frames at RING_CAP=16. Without this call the entire CSI pipeline
  // is dead and /api/csi/stream returns the boot-fallback "sensing" state
  // forever (see csi_hal.h:39 and firmware/common/csi/README.md:61).
  //
  // Round-two power gate: when the policy turns CSI off (battery saver and
  // below) we skip the CSI-specific work to stop the pipeline's per-loop
  // cost. CSI is pure environmental sensing — no life-safety — so honoring
  // the bit is exactly the profiles' intent; the ring fills and drops
  // (bounded, harmless) and draining resumes on re-enable with no re-init.
  // The gate is passed INTO csi_integration::loop() rather than wrapping
  // it, because that function ALSO services mesh (outbound beacon drain +
  // coordinator/channel maintenance) which must keep running on battery to
  // carry inter-canary security alerts (codex #855 P1).
  {
    bool csi_gate_on = true;
    #if FEATURE_POWER_POLICY
    const PolicyFeatures* pf_csi = power_policy::get_features();
    csi_gate_on = power_gate::feature_runs(pf_csi != nullptr,
                                           pf_csi != nullptr && pf_csi->csi);
    #endif
    csi_integration::loop(csi_gate_on);
  }

  // Optional MQTT bridge — pump (no-op when disabled or unconfigured),
  // plus three cadence-gated publishers for the topics HA expects.
  // Schemas locked against custom_components/securacv/sensor.py.
  csi_mqtt::loop();
#if defined(FEATURE_MDNS_BROKER_GOSSIP) && FEATURE_MDNS_BROKER_GOSSIP
  // Keep the fleet's broker referral honest: re-sync the _securacv._tcp
  // broker/bport TXT on every MQTT link transition — connect advertises the
  // live broker, disconnect tombstones it. Edge-triggered, so the TXT write
  // only fires on the transition, never every loop.
  {
    static bool s_gossip_prev = false;
    const bool up = csi_mqtt::connected();
    if (up != s_gossip_prev) {
      s_gossip_prev = up;
      mdns_sync_broker_txt();
    }
  }
#endif
#if FEATURE_ACOUSTIC_EVENTS
  // Track the broker-connection edge OUTSIDE the connected gate so a
  // reconnect (or first boot connect) forces an immediate /sensing
  // publish below. The topic is retained: without this, a smoke/CO
  // event published just before a reboot/disconnect would linger on
  // the broker and HA could briefly automate on a phantom alarm when
  // the device comes back. The forced publish reflects live state —
  // "none" after a reboot — and overwrites the stale payload.
  static bool s_mqtt_prev_connected = false;
  const bool mqtt_just_connected =
      csi_mqtt::connected() && !s_mqtt_prev_connected;
  s_mqtt_prev_connected = csi_mqtt::connected();
#endif
  if (csi_mqtt::connected()) {
    static uint32_t s_mqtt_status_ms = 0;
    static uint32_t s_mqtt_health_ms = 0;
    static uint32_t s_mqtt_counts_last = 0;
    // Round-two power gate: routine heartbeats (status/health, and the
    // mesh/beacon snapshots below) stretch their cadence as the battery
    // drains — the mqtt bit is TRUE in every mode (the link stays up for
    // panic events), so lengthening ROUTINE traffic is the honest lever.
    // Life-safety (acoustic /sensing) and event-driven (counts/chain)
    // publishes further down are NOT stretched.
    uint8_t pmode = power_gate::MODE_PLUGGED_IN;
    #if FEATURE_POWER_POLICY
    pmode = (uint8_t)power_policy::get_mode();
    #endif
    if (now - s_mqtt_status_ms >= power_gate::routine_interval_ms(30000UL, pmode)) {
      s_mqtt_status_ms = now;
      csi_mqtt::publish_status(
          csi_integration::csi_running(),
          /*wifi_connected=*/(WiFi.status() == WL_CONNECTED || WiFi.softAPgetStationNum() > 0),
          /*rssi_dbm=*/(WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0);
    }
    if (now - s_mqtt_health_ms >= power_gate::routine_interval_ms(60000UL, pmode)) {
      s_mqtt_health_ms = now;
      // Attach the real battery state when one is wired (HW ADC mode);
      // nullptr keeps the mains semantics (battery=100) for USB-only
      // devices so HA never sees a false low-battery state.
      const csi_mqtt::MqttBatteryInfo* batt_ptr = nullptr;
      #if FEATURE_POWER_MONITOR
      csi_mqtt::MqttBatteryInfo batt;
      PowerState pwr_mqtt;
      if (power_monitor::get_state(&pwr_mqtt) && pwr_mqtt.battery_present) {
        batt.soc_pct      = pwr_mqtt.soc_pct;
        batt.health_pct   = power_monitor::health_pct();
        batt.battery_mv   = pwr_mqtt.voltage_mv;
        batt.charge_state = power_monitor::charge_state_name(pwr_mqtt.charge_state);
        batt_ptr = &batt;
      }
      #endif
      csi_mqtt::publish_health((uint32_t)ESP.getFreeHeap(),
                               (uint32_t)uptime_seconds(), batt_ptr);
    }

#if FEATURE_ACOUSTIC_EVENTS
    // Drain HA's mic-mute switch commands. audio_mute() defers the I2S
    // toggle to the next audio_process() tick; the mute callback then
    // signs the witness record and mirrors mic/state back to HA.
    {
      const int mic_cmd = csi_mqtt::take_pending_mic_mute();
      if (mic_cmd >= 0) {
        const bool want_mute = (mic_cmd == 1);
        if (audio_mute(want_mute, AUDIO_MUTE_SOURCE_MQTT)) {
          if (!audio_save_mute_intent(want_mute)) {
            log_health(SCV_LOG_WARNING, SCV_CAT_STORAGE,
                       "Mic mute intent NOT persisted", "NVS write failed");
          }
        }
      }
    }
    // Acoustic /sensing snapshot: publish immediately when an event
    // lands, once more to clear it back to "none" after the 30 s hold,
    // and every 60 s as a counters/mute heartbeat. HA's smoke/CO/
    // knock/doorbell/glass binary sensors template on acoustic_event.
    {
      static uint32_t s_mqtt_sensing_ms = 0;
      static bool s_sensing_event_held = false;
      const bool event_fresh = (g_audio_mqtt_event_ms != 0) &&
          (now - g_audio_mqtt_event_ms) < AUDIO_MQTT_EVENT_HOLD_MS;
      const bool need_clear = s_sensing_event_held && !event_fresh;
      if (g_audio_mqtt_dirty || need_clear || mqtt_just_connected ||
          (now - s_mqtt_sensing_ms >= 60000UL)) {
        g_audio_mqtt_dirty = false;
        s_mqtt_sensing_ms = now;
        s_sensing_event_held = event_fresh;
        audio_stats_t ast = {};
        audio_get_stats(&ast);
        char sensing_json[320];
        const int sn = snprintf(sensing_json, sizeof(sensing_json),
          "{"
            "\"acoustic_event\":\"%s\","
            "\"mic_muted\":%s,"
            "\"t3_detected\":%lu,"
            "\"t4_detected\":%lu,"
            "\"knock_detected\":%lu,"
            "\"doorbell_detected\":%lu,"
            "\"glass_break_detected\":%lu,"
            "\"i2s_read_errors\":%lu"
          "}",
          event_fresh ? g_audio_mqtt_event : "none",
          audio_is_muted() ? "true" : "false",
          (unsigned long)ast.t3_detected,
          (unsigned long)ast.t4_detected,
          (unsigned long)ast.knock_detected,
          (unsigned long)ast.doorbell_detected,
          (unsigned long)ast.glass_break_detected,
          (unsigned long)ast.i2s_read_errors);
        if (sn > 0 && sn < (int)sizeof(sensing_json)) {
          csi_mqtt::publish_sensing(sensing_json);
        }
      }
    }
#endif
    // Counts + chain: publish on each increment of records_created
    // (saves bandwidth vs. heartbeating, and matches HA's expectation
    // that the topic carries the latest total, not periodic noise).
    // We use g_device.chain_head rather than g_last_record.chain_hash
    // because the device-level head is always the canonical latest;
    // a transient g_last_record could lag if a verifier path failed.
    if (g_health.records_created != s_mqtt_counts_last) {
      s_mqtt_counts_last = g_health.records_created;
      csi_mqtt::publish_counts(g_health.records_created);
      csi_mqtt::publish_chain(g_device.seq, g_device.chain_head);
    }

    // Mesh coexistence snapshot — every 30 s, when the mesh feature is
    // compiled in. Surfaces the airtime governor + channel policy so HA
    // can render the entities published by csi_mqtt::publish_discovery
    // (mesh_airtime_pct, mesh_channel, mesh_channel_locked_to_sta).
    // Gated on FEATURE_MESH_NETWORK so non-mesh builds (canary-vision,
    // canary-ota) don't try to read mesh state that doesn't exist.
#if FEATURE_MESH_NETWORK
    static uint32_t s_mqtt_mesh_ms = 0;
    if (now - s_mqtt_mesh_ms >= power_gate::routine_interval_ms(30000UL, pmode)) {
      s_mqtt_mesh_ms = now;
      mesh_channel_policy::ChannelDecision d = mesh_channel_policy::current();
      airtime_governor::Stats s = airtime_governor::snapshot(now);
      csi_mqtt::publish_mesh(
          s.airtime_pct_x100,
          d.channel,
          d.locked_to_sta,
          d.locked_to_ap,
          d.fallback,
          s.routine_allowed,
          s.routine_denied,
          s.urgent_sends);

      // v0.3: also publish the Chirp NFPA-state surface for HA discovery.
      // The Chirp ChirpState enum is mapped 1:1 to the NFPA-72 four-state
      // model in the HA dashboard side; we publish the raw chirp state
      // string here. Beacon publishing follows in its own gate.
      csi_mqtt::publish_chirp_state(
          chirp_channel::state_name(chirp_channel::get_status().state));
    }
#endif

#if FEATURE_BEACON_CHANNEL
    static uint32_t s_mqtt_beacon_ms = 0;
    if (now - s_mqtt_beacon_ms >= power_gate::routine_interval_ms(30000UL, pmode)) {
      s_mqtt_beacon_ms = now;
      beacon_channel::BeaconStatus bs = beacon_channel::get_status();
      // Beacon-class airtime is rolled into the same Stats struct via
      // beacon_sends/beacon_airtime_us. Convert beacon airtime to pct_x100
      // over the same 10s window as the main airtime_pct.
      airtime_governor::Stats s = airtime_governor::snapshot(now);
      uint64_t total_window_us = (uint64_t)airtime_governor::WINDOW_MS * 1000u;
      uint16_t beacon_pct_x100 = total_window_us == 0 ? 0 :
          (uint16_t)(((uint64_t)s.beacon_airtime_us * 10000ull) / total_window_us);
      const char* active = "";
      if (bs.active_alarm) {
        active = "active";  // v0.3 publishes raw flag; richer mapping in v0.4
      }
      csi_mqtt::publish_beacon_state(
          beacon_channel::state_name(bs.state),
          beacon_pct_x100,
          active,
          s.beacon_sends,
          bs.beacon_set_size,
          bs.trouble_reasons);
    }
#endif
  }

  // Yield before witness record creation
  yield();

  // ════════════════════════════════════════════════════════════════════════════
  // WITNESS RECORD CREATION
  // ════════════════════════════════════════════════════════════════════════════

  uint32_t effective_record_interval_ms = g_record_interval_ms;
  #if FEATURE_POWER_POLICY
  {
    // On battery the policy stretches the record cadence (5 s / 30 s /
    // 60 s per mode). The operator setting still applies when slower.
    const PolicyFeatures* pf = power_policy::get_features();
    if (pf != nullptr && pf->record_interval_ms > effective_record_interval_ms)
      effective_record_interval_ms = pf->record_interval_ms;
  }
  #endif
  if (now - g_last_record_ms >= effective_record_interval_ms) {
    g_last_record_ms = now;

    uint8_t payload[512];
    size_t payload_len = 0;
    if (!build_witness_event(&g_fix, g_state, payload, sizeof(payload), &payload_len)) {
      log_health(SCV_LOG_ERROR, SCV_CAT_WITNESS, "Payload build failed", nullptr);
      return;
    }

    if (!create_witness_record(payload, payload_len, RECORD_WITNESS_EVENT, &g_last_record)) {
      log_health(SCV_LOG_ERROR, SCV_CAT_CRYPTO, "Record verification failed", nullptr);
    }

    // Print table row
    print_table_row(&g_last_record, &g_fix, g_state);

    // Periodic status every 20 records
    if (g_health.records_created % 20 == 0) {
      print_status_bar();
      print_table_header();
    }
  }
  
  // WiFi Presence threshold witness events
  #if FEATURE_WIFI_PRESENCE
  {
    if (wifi_presence::threshold_crossed(wifi_presence::DEFAULT_ALERT_THRESHOLD)) {
      uint8_t payload[128];
      CborWriter cb(payload, sizeof(payload));
      cb.write_map(3);
      cb.write_text("event_type"); cb.write_text("presence_threshold");
      cb.write_text("count"); cb.write_uint(wifi_presence::get_current_count());
      cb.write_text("threshold"); cb.write_uint(wifi_presence::DEFAULT_ALERT_THRESHOLD);
      if (cb.ok()) {
        WitnessRecord rec;
        create_witness_record(payload, cb.size(), RECORD_WITNESS_EVENT, &rec);
        log_health(SCV_LOG_NOTICE, SCV_CAT_WITNESS, "Presence threshold crossed", nullptr);
        #if FEATURE_AUDIBLE_CHIRP
        audible_chirp::chirp_alert();
        #endif
      }
    }
    if (wifi_presence::presence_cleared()) {
      uint8_t payload[64];
      CborWriter cb(payload, sizeof(payload));
      cb.write_map(2);
      cb.write_text("event_type"); cb.write_text("presence_cleared");
      cb.write_text("last_count"); cb.write_uint(wifi_presence::get_last_count());
      if (cb.ok()) {
        WitnessRecord rec;
        create_witness_record(payload, cb.size(), RECORD_WITNESS_EVENT, &rec);
        log_health(SCV_LOG_INFO, SCV_CAT_WITNESS, "Presence cleared", nullptr);
      }
    }
  }
  #endif

  // Periodic self-verification
  if (now - g_last_verify_ms >= VERIFY_INTERVAL_SEC * 1000) {
    g_last_verify_ms = now;
    
    if (!verify_record_signature(&g_last_record)) {
      log_health(SCV_LOG_CRITICAL, SCV_CAT_CRYPTO, "Self-verification FAILED", nullptr);
      g_health.crypto_healthy = false;
    } else {
      g_health.crypto_healthy = true;
    }
  }
  
  // Small delay to prevent tight loop
  delay(1);
}
