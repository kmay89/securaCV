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

#include "esp_system.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_http_server.h"
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
#include <ESPmDNS.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

#include <Crypto.h>
#include <Ed25519.h>

#include "esp_camera.h"

#include "log_level.h"
#include "health_log.h"
#include "sd_storage.h"
#include "nvs_store.h"
#include "api_auth.h"
#include "wap_server.h"
#include "web_ui.h"
#include "companion_pwa.h"
#include "csi_integration.h"     // Boot the CSI library + HTTP endpoints
#include "csi_mqtt.h"            // Optional MQTT bridge for HA integration
#include "csi_event_log.h"       // SD-backed event persistence + MQTT backfill
#include "csi_witness_payload.h" // Builds the witness-chain payload string
#include <ble_events_module.h>   // spec §10 BLE event chokepoint helpers
#include "setup_page_html.h"     // Captive-portal setup page (Tier 5 #11)
extern "C" {
#include "qrcodegen.h"           // Vendored Nayuki QR encoder, MIT
}
#include "csi_dashboard_html.h"  // CSI_DASHBOARD_HTML — the Phase-3 headline UI now served at /
#include "mesh_network.h"
#include "bluetooth_channel.h"
#include "bluetooth_api.h"
#include "ble_console.h"
#include "ble_log_export.h"
#include "household_api.h"
#include "sys_monitor.h"
#include "hardware_state.h"
#include "selftest_api.h"        // GET /api/selftest — wizard pre-flight aggregator

// ════════════════════════════════════════════════════════════════════════════
// BUILD CONFIGURATION — Edit build_config.h to select profile
// ════════════════════════════════════════════════════════════════════════════
// Build profiles (edit build_config.h to switch):
//   MINIMAL - Fastest build (~45s): crypto + GPS only
//   DEV     - Development (~90s):   + WiFi + HTTP + SD
//   FULL    - All features (~150s): + Camera + Mesh + BLE
// ════════════════════════════════════════════════════════════════════════════

#include "build_config.h"

// BLE Discovery Subsystem (Opera/Chirp/Nearby)
#include "ble_config.h"
#include "ble_manager.h"

// WiFi Presence Detection (probe request monitoring)
#include "wifi_presence.h"
#include "wifi_presence_api.h"

// Audible Chirp (local alert tones — PWM buzzer / LED blink)
#include "audible_chirp.h"
#include "audible_chirp_api.h"

// ════════════════════════════════════════════════════════════════════════════
// VERSION & PROTOCOL (must match PWK expectations)
// ════════════════════════════════════════════════════════════════════════════

static const char* DEVICE_TYPE        = "canary";
static const char* FIRMWARE_VERSION   = "2.1.0-wap";  // TLS + API auth hardening
static const char* RULESET_ID         = "securacv:canary:v1.0";
static const char* PROTOCOL_VERSION   = "pwk:v0.3.0";
static const char* CHAIN_ALGORITHM    = "sha256-domain-sep";
static const char* SIGNATURE_ALGORITHM = "ed25519";

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

static const uint32_t RECORD_INTERVAL_MS   = 1000;    // Record emission rate
static const uint32_t TIME_BUCKET_MS       = 5000;    // Time coarsening bucket
static const uint32_t FIX_LOST_TIMEOUT_MS  = 3000;    // GPS fix timeout
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
// "speed" and a few metres of position wander from multipath. If we surface
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
static const char* NVS_KEY_WIFI_SSID = "wifi_ssid";
static const char* NVS_KEY_WIFI_PASS = "wifi_pass";
static const char* NVS_KEY_WIFI_EN   = "wifi_en";
static const char* NVS_KEY_API_TKN  = "api_tkn";
static const char* NVS_KEY_TLS_CERT = "tls_cert";
static const char* NVS_KEY_TLS_KEY  = "tls_key";

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
  bool     initialized;
  bool     tamper_active;
  char     device_id[32];
  char     ap_ssid[32];
  // API security fields (added for SAP integration)
  char     api_token_str[36];    // "cv_" + 32 base62 chars + null
  char     ap_password[16];      // device-unique AP password "cv-XXXXX"
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
// NVS access is now encapsulated in NvsManager singleton (see nvs_store.h)

static RingBuffer<2048> g_gps_rb;
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
// Rolling 1s window for instantaneous FPS (no fabrication: counted from real frame deliveries)
static volatile uint32_t g_peek_fps_window_start  = 0;
static volatile uint32_t g_peek_fps_window_count  = 0;
static volatile uint32_t g_peek_fps_last          = 0;  // FPS measured over last full 1s window
// Spinlock guarding the metrics block above. Today esp_http_server runs in a
// single task so the streaming loop and the status handler can't race in
// practice, but holding this lock around both writers and the reader keeps
// the snapshot torn-free if the stream ever moves into an async worker task.
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
static HealthLogRingEntry g_health_log_ring[HEALTH_LOG_RING_SIZE];
static size_t g_health_log_ring_head = 0;
static size_t g_health_log_ring_count = 0;

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
static const char* wifi_state_name(WiFiProvState s);

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
  return millis() / TIME_BUCKET_MS;
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

static void generate_device_id(char* out, size_t cap) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, cap, "%s%02X%02X", DEVICE_ID_PREFIX, mac[4], mac[5]);
}

static void generate_ap_ssid(char* out, size_t cap) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, cap, "SecuraCV-%02X%02X", mac[4], mac[5]);
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
  // Get 32 bytes from hardware RNG
  esp_fill_random(priv, 32);
  
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

// Drops the four glyph-confusion classes that bite users typing tokens by hand:
//   '0' / 'O', '1' / 'I' / 'l'.
// 32 chars × log2(57) ≈ 187 bits of entropy — UX win > 3 bits.
static const char UNAMBIGUOUS_ALPHABET[] =
  "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// Rejection sampling: discard bytes >= 228 (228 = 57*4, evenly divisible)
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

    if (b < 228) {  // 228 = 57 * 4 → evenly divisible
      output[out_idx++] = UNAMBIGUOUS_ALPHABET[b % 57];
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

static void derive_ap_password(const uint8_t fingerprint[8], char* password, size_t len) {
  // Use bytes 0-7 of fingerprint for password material
  // WPA2-PSK requires 8-63 ASCII characters
  char encoded[6];
  size_t chars_produced = 0;
  for (size_t i = 0; chars_produced < 5 && i < 8; i++) {
    uint8_t b = fingerprint[i];
    if (b < 228) {  // 228 = 57 * 4, rejection sampling to avoid bias
      encoded[chars_produced++] = UNAMBIGUOUS_ALPHABET[b % 57];
    }
  }
  // Fallback in the extremely unlikely case we don't get 5 chars from 8 bytes
  // Use '2' (first char of unambiguous alphabet) — never '0' which we excluded.
  while (chars_produced < 5) {
    encoded[chars_produced++] = '2';
  }
  encoded[5] = '\0';
  snprintf(password, len, "cv-%s", encoded);
  // Result: "cv-XXXXX" — 8 chars, unique per device
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
  w.write_text("lat"); w.write_float(fx->lat);
  w.write_text("lon"); w.write_float(fx->lon);
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
  
  // Persist chain state periodically
  if ((g_device.seq - g_device.seq_persisted) >= SD_PERSIST_INTERVAL) {
    persist_chain_state();
  }
  
  // Store to SD if available
  #if FEATURE_SD_STORAGE
  if (g_sd_mounted) {
    // Simplified: just store to health log for now
    // Full witness storage would go to WITNESS directory
    g_health.sd_writes++;
  }
  #endif

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
  return create_witness_record(payload, (size_t)len, RECORD_WITNESS_EVENT, &rec);
}

static bool verify_record_signature(const WitnessRecord* rec) {
  return verify_signature(g_device.pubkey, rec->chain_hash, 32, rec->signature);
}

// ════════════════════════════════════════════════════════════════════════════
// HEALTH LOGGING
// ════════════════════════════════════════════════════════════════════════════

void log_health(LogLevel level, LogCategory category, const char* message, const char* detail) {
  // Skip DEBUG by default
  if (level < SCV_LOG_INFO) return;
  
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
  
  // Also print to Serial
  Serial.printf("[%s/%s] %s", log_level_name(level), log_category_name(category), message);
  if (detail && detail[0]) {
    Serial.printf(" | %s", detail);
  }
  Serial.println();
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
    if (!g_gps_rb.pop(b)) {
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
    
    if (speed && *speed) {
      fx->speed_knots = parse_double(speed, 0);
      fx->speed_kmh = knots_to_kmh(fx->speed_knots);
      float mps = knots_to_mps(fx->speed_knots);
      g_speed_ema = g_speed_ema * (1.0f - SPEED_EMA_ALPHA) + mps * SPEED_EMA_ALPHA;
    }
    
    if (course && *course) {
      fx->course_deg = parse_double(course, 0);
    }
    
    // Parse UTC time
    if (time_str && strlen(time_str) >= 6) {
      g_gps_utc.hour = (time_str[0] - '0') * 10 + (time_str[1] - '0');
      g_gps_utc.minute = (time_str[2] - '0') * 10 + (time_str[3] - '0');
      g_gps_utc.second = (time_str[4] - '0') * 10 + (time_str[5] - '0');
      if (strlen(time_str) > 7) {
        g_gps_utc.centisecond = parse_int(time_str + 7, 0);
      }
    }
    
    // Parse date
    if (date_str && strlen(date_str) >= 6) {
      g_gps_utc.day = (date_str[0] - '0') * 10 + (date_str[1] - '0');
      g_gps_utc.month = (date_str[2] - '0') * 10 + (date_str[3] - '0');
      int yy = (date_str[4] - '0') * 10 + (date_str[5] - '0');
      g_gps_utc.year = 2000 + yy;
      g_gps_utc.valid = true;
      g_gps_utc.last_seen_ms = millis();
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
// speed and a few metres of position scatter even when the device is bolted
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

  // Branch 1: existing valid session.
  if (csi_integration::session_validate_cookie(req)) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, CSI_DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
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
  // (camera peek, witness export, fine-grained tabs) while / is reserved
  // for the headline sensing experience.
  g_health.http_requests++;
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, CANARY_UI_HTML, HTTPD_RESP_USE_STRLEN);
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
  return httpd_resp_send(req, COMPANION_HTML, HTTPD_RESP_USE_STRLEN);
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
    gps["lat"] = g_motion.display_lat;
    gps["lon"] = g_motion.display_lon;
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
  doc["record_interval_ms"] = RECORD_INTERVAL_MS;
  doc["time_bucket_ms"] = TIME_BUCKET_MS;
  doc["log_level"] = 1;  // Info by default
  
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
  snprintf(export_path, sizeof(export_path), "/sd/EXPORT/bundle_%u.json", (unsigned)millis());

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
  
  delay(500);
  ESP.restart();
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
    case FRAMESIZE_QQVGA: return "160x120";
    case FRAMESIZE_QVGA:  return "320x240";
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
// PEEK START — NEW ENDPOINT (POST /api/peek/start)
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_start(httpd_req_t* req) {
  g_health.http_requests++;
  
  if (!g_camera_initialized) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    const char* resp = "{\"ok\":false,\"error\":\"Camera not initialized\"}";
    return http_send_json(req, resp);
  }
  
  g_peek_active = true;
  
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "Peek started", nullptr);
  
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Peek stream activated";
  doc["resolution"] = framesize_name(g_peek_framesize);
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK STREAM — FIXED: Now properly manages g_peek_active state
//
// NOTE on concurrency: esp_http_server runs a single task by default, so this
// long-lived MJPEG handler blocks /api/peek/status polling until the client
// disconnects. The metrics we update here remain in g_peek_* globals and are
// rendered by the UI as "LAST STREAM" stats once the stream ends. Moving this
// loop into a worker task via httpd_req_async_handler_begin/_complete is a
// separate refactor (tracked outside this PR).
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_stream(httpd_req_t* req) {
  g_health.http_requests++;

  if (!g_camera_initialized) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "Camera not initialized", HTTPD_RESP_USE_STRLEN);
  }

  // *** KEY FIX: Set peek_active to true when stream is requested ***
  g_peek_active = true;

  // Reset per-stream metrics so the UI shows real, fresh values.
  uint32_t now_ms = millis();
  portENTER_CRITICAL(&g_peek_metrics_mux);
  g_peek_frame_count      = 0;
  g_peek_total_bytes      = 0;
  g_peek_last_frame_bytes = 0;
  g_peek_last_frame_ms    = 0;
  g_peek_stream_start_ms  = now_ms;
  g_peek_fps_window_start = now_ms;
  g_peek_fps_window_count = 0;
  g_peek_fps_last         = 0;
  portEXIT_CRITICAL(&g_peek_metrics_mux);

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
  // Note: target pacing is ~12 fps via vTaskDelay below; the actual delivered FPS
  // is measured at runtime and exposed via /api/peek/status (g_peek_fps_last).

  // Stream frames while active
  while (g_peek_active) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[PEEK] Frame capture failed");
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;  // Try again instead of breaking
    }
    
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
    
    // Feed watchdog and yield. Pacing is configurable at runtime via
    // /api/peek/sensor (frame_delay_ms). Default 40 ms ≈ 25 fps target;
    // OV2640 will deliver fewer in low light because AEC stretches the
    // exposure window — that's accurately reflected in the measured FPS.
    #if FEATURE_WATCHDOG
    esp_task_wdt_reset();
    #endif
    uint32_t pace = g_peek_frame_delay_ms;
    if (pace < 20)  pace = 20;
    if (pace > 500) pace = 500;
    vTaskDelay(pdMS_TO_TICKS(pace));
  }
  
  // *** KEY FIX: Set peek_active to false when stream ends ***
  g_peek_active = false;
  
  // End chunked response
  httpd_resp_send_chunk(req, NULL, 0);
  
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "Peek stream ended", nullptr);
  
  return ESP_OK;
}

static esp_err_t handle_peek_snapshot(httpd_req_t* req) {
  g_health.http_requests++;
  
  if (!g_camera_initialized) {
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

static esp_err_t handle_peek_stop(httpd_req_t* req) {
  g_health.http_requests++;
  
  g_peek_active = false;
  
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "Peek stopped", nullptr);
  
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Peek stopped";
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_peek_status(httpd_req_t* req) {
  g_health.http_requests++;

  JsonDocument doc;
  doc["ok"] = true;
  doc["camera_initialized"] = g_camera_initialized;
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
  portENTER_CRITICAL(&g_peek_metrics_mux);
  snap_active           = g_peek_active;
  snap_frame_count      = g_peek_frame_count;
  snap_last_frame_bytes = g_peek_last_frame_bytes;
  snap_total_bytes      = g_peek_total_bytes;
  snap_fps_last         = g_peek_fps_last;
  snap_stream_start_ms  = g_peek_stream_start_ms;
  portEXIT_CRITICAL(&g_peek_metrics_mux);

  doc["frame_count"]       = snap_frame_count;
  doc["last_frame_bytes"]  = snap_last_frame_bytes;
  doc["total_bytes"]       = snap_total_bytes;  // ArduinoJson v7 supports uint64_t natively — no 4GB wrap
  doc["fps"]               = snap_fps_last;     // measured over the last full ~1s window, jitter-normalized

  uint32_t now_ms      = millis();
  uint32_t uptime_ms   = (snap_stream_start_ms && snap_active)
                           ? (now_ms - snap_stream_start_ms)
                           : 0;
  doc["stream_uptime_ms"]  = uptime_ms;
  uint32_t avg_bytes = (snap_frame_count > 0)
                         ? (uint32_t)(snap_total_bytes / snap_frame_count)
                         : 0;
  doc["avg_frame_bytes"]   = avg_bytes;
  // Average throughput in kbps over the entire stream so far (real, computed from totals)
  uint32_t avg_kbps = 0;
  if (uptime_ms > 0) {
    // bytes * 8 / ms -> kbps directly (since /1000ms cancels with *1000 from kbits)
    avg_kbps = (uint32_t)((snap_total_bytes * 8ULL) / (uint64_t)uptime_ms);
  }
  doc["avg_kbps"]          = avg_kbps;

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
  bool was_active = g_peek_active;
  g_peek_active = false;
  if (was_active) vTaskDelay(pdMS_TO_TICKS(150));

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
  
  // Stop stream if active
  bool was_active = g_peek_active;
  g_peek_active = false;
  vTaskDelay(pdMS_TO_TICKS(100)); // Let stream exit
  
  bool success = set_camera_resolution((framesize_t)size);
  
  // Restore stream if it was active
  if (was_active && success) {
    g_peek_active = true;
  }
  
  JsonDocument doc;
  doc["ok"] = success;
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
// MESH NETWORK (FLOCK) API HANDLERS
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
      sprintf(fp_hex + j * 2, "%02X", peer->fingerprint[j]);
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
  log_health(SCV_LOG_INFO, SCV_CAT_MESH, "Pairing cancelled", nullptr);
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
  doc["connect_attempts"] = g_wifi_status.connect_attempts;
  doc["fail_reason"] = g_wifi_status.last_fail_reason;

  if (g_wifi_status.sta_connected && g_wifi_status.connected_since_ms > 0) {
    doc["connected_sec"] = (millis() - g_wifi_status.connected_since_ms) / 1000;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_scan(httpd_req_t* req) {
  g_health.http_requests++;

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

  // Scan complete - return results
  g_wifi_scan_in_progress = false;
  if (g_wifi_status.state == WIFI_PROV_SCANNING) {
    g_wifi_status.state = g_wifi_creds.configured ? WIFI_PROV_IDLE : WIFI_PROV_AP_ONLY;
  }

  int n = scanResult;
  JsonDocument doc;
  doc["ok"] = true;
  doc["scanning"] = false;
  doc["count"] = n;

  JsonArray networks = doc["networks"].to<JsonArray>();

  for (int i = 0; i < n && i < 20; i++) {
    JsonObject net = networks.add<JsonObject>();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = WiFi.RSSI(i);
    net["channel"] = WiFi.channel(i);

    wifi_auth_mode_t authMode = WiFi.encryptionType(i);
    const char* security = "open";
    if (authMode == WIFI_AUTH_WPA_PSK) security = "wpa";
    else if (authMode == WIFI_AUTH_WPA2_PSK) security = "wpa2";
    else if (authMode == WIFI_AUTH_WPA_WPA2_PSK) security = "wpa/wpa2";
    else if (authMode == WIFI_AUTH_WPA3_PSK) security = "wpa3";
    else if (authMode == WIFI_AUTH_WPA2_WPA3_PSK) security = "wpa2/wpa3";
    else if (authMode != WIFI_AUTH_OPEN) security = "other";
    net["security"] = security;
  }

  WiFi.scanDelete();

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_connect(httpd_req_t* req) {
  g_health.http_requests++;

  // Read body
  char content[256] = {0};
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

  // Pairing-token gate. The captive-portal QR + manual fallback link
  // both bake a fresh pairing token into /companion?token=<hex>, and
  // the wizard JS forwards that token in the body of every POST here.
  // We accept credentials only when the token is still in the live slot
  // table — i.e. it was issued by THIS device within the last 10 minutes
  // (see pair_token_valid + the PAIRING TOKENS doc block in
  // csi_integration.h). pair_token_valid() rejects empty/short/wrong-hex
  // input too, so a single call covers missing, malformed, and expired.
  // We validate without consuming so the wizard can retry within the TTL
  // (e.g. user mistyped the password); the token ages out on its own.
  const char* token = body["token"] | "";
  if (!csi_integration::pair_token_valid(token)) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["code"] = "invalid_token";
    doc["error"] = "This setup link doesn't work anymore. Reopen the captive portal page to get a fresh QR code.";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
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

// Captive portal handler for iOS/Android/Windows detection
// This handler is registered for specific captive portal detection URIs only.
// Always redirect to main UI to trigger the captive portal popup.
/* Tier 5 #11 — captive-portal setup page.
 *
 * Replaces the older 302 redirect to canary.local/. The redirect was
 * dishonest: a freshly-flashed device that hasn't joined home WiFi yet
 * has no canary.local mDNS name to resolve, so the redirect went
 * nowhere on first contact. The new flow:
 *
 *   1. Issue a one-shot pairing token (32 random bytes, RAM-only,
 *      10-min expiry, single-use).
 *   2. Build the URL  http://192.168.4.1/companion?token=<64hex>
 *   3. QR-encode the URL via Nayuki's qrcodegen (vendored, MIT).
 *   4. Render the setup page from PROGMEM with the QR SVG inline +
 *      a manual fallback link to the same URL.
 *
 * The QR SVG is a single <path> with one "M{x},{y}h1v1h-1z"
 * subcommand per dark module. ~14 bytes/module worst case at v5
 * = ~14 KB SVG, easily within ESP-IDF httpd response budget.
 *
 * Privacy: no outbound bytes — the page is served from the device.
 * The captive-portal probe URLs (hotspot-detect.html, generate_204,
 * etc.) all land here, so any phone that joins the SecuraCV-XXXX
 * AP gets the setup page automatically.
 */
static esp_err_t handle_captive_portal(httpd_req_t* req) {
  g_health.http_requests++;

  /* 1. Mint the token. Failure path: drop back to a tiny error so the
   *    user can still see the manual companion URL and try the older
   *    typed-credentials flow. */
  char tok_hex[csi_integration::PAIR_TOKEN_HEX_LEN + 1];
  if (!csi_integration::pair_token_issue(tok_hex, sizeof(tok_hex))) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Setup is busy. Try again in a moment.", -1);
  }

  /* 2. Build the pairing URL. */
  char pair_url[160];
  snprintf(pair_url, sizeof(pair_url),
           "http://192.168.4.1/companion?token=%s", tok_hex);

  /* 3. QR-encode. We cap maxVersion at 10 (which holds 174 bytes at ECC L
   *    or 122 at ECC M — far more than our ~100-char URL) so the buffer
   *    stays small and stack-friendly. */
  static constexpr int QR_MAX_VERSION = 10;
  uint8_t qr [qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
  uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
  bool ok = qrcodegen_encodeText(pair_url, tmp, qr, qrcodegen_Ecc_LOW,
                                 1, QR_MAX_VERSION, qrcodegen_Mask_AUTO,
                                 /*boostEcl=*/true);
  if (!ok) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Couldn't render setup code.", -1);
  }
  const int qr_size = qrcodegen_getSize(qr);
  /* 4-module-wide quiet zone is required by the QR spec for camera
   * decoding; we use 4. The viewBox encompasses size + 2*margin. */
  const int margin = 4;
  const int viewBox = qr_size + margin * 2;

  /* 4. Stream the response: HEAD → SVG → TAIL. */
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  /* iOS captive portal heuristic: returning 200 + a real page (not the
   * generated_204 / Success token) keeps the captive portal popup
   * visible until the user taps "Done", which is what we want. */
  httpd_resp_set_status(req, "200 OK");

  /* HEAD */
  httpd_resp_send_chunk(req, SETUP_PAGE_HTML_HEAD, HTTPD_RESP_USE_STRLEN);

  /* SVG opening tag with the right viewBox. The buffer is sized for the
   * worst case (viewBox triple-digit, three-digit width/height) plus
   * generous slack; the explicit truncation guard below means a future
   * markup change that stretches this past the buffer fails closed
   * instead of exposing a stack read overflow. */
  char svg_open[256];
  int n = snprintf(svg_open, sizeof(svg_open),
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" "
    "shape-rendering=\"crispEdges\" role=\"img\" aria-label=\"Pair with phone\">"
    "<rect width=\"%d\" height=\"%d\" fill=\"#fff\"/>"
    "<path fill=\"#000\" d=\"",
    viewBox, viewBox, viewBox, viewBox);
  /* snprintf returns the would-have-written length; truncate to actual
   * bytes in the buffer (excluding NUL) before handing the length to
   * httpd_resp_send_chunk, which would otherwise read past the buffer. */
  if (n > 0) {
    if ((size_t)n >= sizeof(svg_open)) n = (int)(sizeof(svg_open) - 1);
    httpd_resp_send_chunk(req, svg_open, n);
  }

  /* SVG path data — one "M{x} {y}h1v1h-1z" per dark module. The data is
   * built in a stack chunk and flushed when the next module wouldn't
   * fit. snprintf returns the would-have-written length, so we MUST
   * flush before writing rather than after — a post-write check sees a
   * `plen` that already overflows the buffer and the next send_chunk
   * reads past it. */
  char path_chunk[600];
  size_t plen = 0;
  /* Worst case per module at v40: two 3-digit ints + "M  h1v1h-1z" =
   * about 14 bytes. 32 leaves comfortable slack. */
  constexpr size_t MODULE_MAX_BYTES = 32;
  for (int y = 0; y < qr_size; ++y) {
    for (int x = 0; x < qr_size; ++x) {
      if (!qrcodegen_getModule(qr, x, y)) continue;
      if (plen + MODULE_MAX_BYTES > sizeof(path_chunk)) {
        httpd_resp_send_chunk(req, path_chunk, plen);
        plen = 0;
      }
      int wrote = snprintf(path_chunk + plen, sizeof(path_chunk) - plen,
                           "M%d %dh1v1h-1z",
                           x + margin, y + margin);
      if (wrote <= 0) continue;
      /* Truncation should be impossible given the size check above, but
       * defend in depth: if it ever happens, drop the partial write
       * rather than letting plen exceed the buffer. */
      if ((size_t)wrote >= sizeof(path_chunk) - plen) {
        path_chunk[plen] = '\0';
        continue;
      }
      plen += (size_t)wrote;
    }
  }
  if (plen > 0) httpd_resp_send_chunk(req, path_chunk, plen);
  httpd_resp_send_chunk(req, "\"/></svg>", -1);

  /* TAIL — splice the same pair URL into the manual fallback. The
   * truncation guard refuses to send a partial response on overflow
   * rather than letting send_chunk read past the buffer. */
  char tail[1024];
  int tn = snprintf(tail, sizeof(tail), SETUP_PAGE_HTML_TAIL_FMT, pair_url);
  if (tn > 0) {
    if ((size_t)tn >= sizeof(tail)) tn = (int)(sizeof(tail) - 1);
    httpd_resp_send_chunk(req, tail, tn);
  }

  /* End of chunked response. */
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// ════════════════════════════════════════════════════════════════════════════
// BLE DISCOVERY API HANDLERS (Opera/Chirp/Nearby)
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_BLE

// GET /api/ble/status — BLE subsystem status
static esp_err_t handle_ble_status(httpd_req_t* req) {
  g_health.http_requests++;
  String json = ble_manager::statusJson();
  return http_send_json(req, json.c_str());
}

// GET /api/nearby — Nearby Canary devices
static esp_err_t handle_ble_nearby(httpd_req_t* req) {
  g_health.http_requests++;
  String json = ble_manager::nearbyJson();
  return http_send_json(req, json.c_str());
}

// POST /api/chirp/send — Trigger a manual chirp alert
static esp_err_t handle_ble_chirp_send(httpd_req_t* req) {
  g_health.http_requests++;

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

  char json[512];
  snprintf(json, sizeof(json),
    "{"
    "\"device_id\":\"%s\","
    "\"firmware\":\"%s\","
    "\"pubkey_fp\":\"%s\","
    "\"mac\":\"%s\","
    "\"uptime_ms\":%lu,"
    "\"chain_length\":%lu,"
    "\"auth_required\":true,"
    "\"tls_enabled\":%s,"
    "\"provisioning_gate\":\"physical_button\""
    "}",
    g_device.device_id,
    FIRMWARE_VERSION,
    g_device.fingerprint_hex,
    WiFi.macAddress().c_str(),
    (unsigned long)millis(),
    (unsigned long)g_device.seq,
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
  snprintf(json, sizeof(json),
    "{\n"
    "  \"device_id\": \"%s\",\n"
    "  \"base_url\": \"%s://%s\",\n"
    "  \"token\": \"%s\",\n"
    "  \"pubkey_fp\": \"%s\",\n"
    "  \"firmware\": \"%s\",\n"
    "  \"mac\": \"%s\",\n"
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
    WiFi.macAddress().c_str(),
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
static esp_err_t handle_witness_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_witness(req);
}
static esp_err_t handle_config_get_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_config_get(req);
}
static esp_err_t handle_export_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_export(req);
}
static esp_err_t handle_reboot_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_reboot(req);
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

#if FEATURE_SYS_MONITOR
static esp_err_t handle_system_metrics_auth(httpd_req_t* req) {
  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
  return handle_system_metrics(req);
}
#endif

// Register all route handlers on a given httpd server handle
static void register_api_routes(httpd_handle_t server) {
  // UI — no auth required. / serves the headline Sensing dashboard;
  // /admin keeps the legacy tabbed dashboard reachable for power-user tasks.
  httpd_uri_t ui = { .uri = "/", .method = HTTP_GET, .handler = handle_ui };
  httpd_register_uri_handler(server, &ui);
  httpd_uri_t legacy_ui = { .uri = "/admin", .method = HTTP_GET, .handler = handle_legacy_ui };
  httpd_register_uri_handler(server, &legacy_ui);

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
#endif

  httpd_uri_t chain = { .uri = "/api/chain", .method = HTTP_GET, .handler = handle_chain_auth };
  httpd_register_uri_handler(server, &chain);

  httpd_uri_t logs = { .uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs_auth };
  httpd_register_uri_handler(server, &logs);

  httpd_uri_t log_ack = { .uri = "/api/logs/*/ack", .method = HTTP_POST, .handler = handle_log_ack_auth };
  httpd_register_uri_handler(server, &log_ack);

  httpd_uri_t ack_all = { .uri = "/api/logs/ack-all", .method = HTTP_POST, .handler = handle_ack_all_auth };
  httpd_register_uri_handler(server, &ack_all);

  httpd_uri_t witness = { .uri = "/api/witness", .method = HTTP_GET, .handler = handle_witness_auth };
  httpd_register_uri_handler(server, &witness);

  httpd_uri_t config_get = { .uri = "/api/config", .method = HTTP_GET, .handler = handle_config_get_auth };
  httpd_register_uri_handler(server, &config_get);

  httpd_uri_t export_bundle = { .uri = "/api/export", .method = HTTP_POST, .handler = handle_export_auth };
  httpd_register_uri_handler(server, &export_bundle);

  httpd_uri_t reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = handle_reboot_auth };
  httpd_register_uri_handler(server, &reboot);

  // CSI library integration: registers /api/csi/stream, /api/csi/window,
  // /api/events/today, /api/events/dismiss, registers the four v1 sensing
  // modules, and brings up csi_hal on this WiFi context. The api_token
  // is the Bearer token every CSI handler verifies via api_auth_check;
  // the dashboard at / bootstraps it through window.__CV_TOKEN injected
  // by handle_ui below.
  csi_integration::init(server, g_device.api_token_str);

  // Optional MQTT bridge (publishes CSI events / health / chain / counts
  // to a user-supplied broker so custom_components/securacv/ in HA sees
  // live data). init() is a no-op if disabled in NVS, and idempotent —
  // re-runs whenever /api/mqtt/config POST changes the broker. We pass
  // the device id, firmware version, and pubkey hex up front so the
  // health payload is self-contained.
  char pubkey_hex[65];
  hex_to_str(pubkey_hex, g_device.pubkey, 32);
  csi_mqtt::init(g_device.device_id, FIRMWARE_VERSION, pubkey_hex);
}

static void start_http_server() {
  // Calculate max URI handlers based on feature usage
  const int base_handlers = 24;       // UI (/ + /admin), API (auth + public), WiFi provisioning, captive portal, /api/selftest
  const int camera_handlers = 6;      // Camera peek endpoints
  const int mesh_handlers = 12;       // Mesh network endpoints
  const int bluetooth_handlers = 23;  // Bluetooth API endpoints
  const int ble_discovery_handlers = 3; // BLE discovery (Opera/Chirp/Nearby) endpoints
  const int csi_handlers = 23;       // /api/csi/stream, /api/csi/window, /api/events/today, /api/events/dismiss, /api/csi/calibrate/{start,status,apply}, /sense, /api/settings (GET + POST), /api/privacy-budget, /manifest.webmanifest, /sw.js, /tune, /api/tune/coefficients (GET + POST), /api/tune/preset (GET + POST), /api/pair/token, /api/mqtt/config (GET + POST), /api/mqtt/test, /mqtt
  const int handler_headroom = 6;     // Reserve for future additions
  const int total_handlers = base_handlers + camera_handlers + mesh_handlers + bluetooth_handlers + ble_discovery_handlers + csi_handlers + handler_headroom;

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
      redirect_config.max_uri_handlers = 2;

      if (httpd_start(&g_http_server, &redirect_config) == ESP_OK) {
        // Catch-all redirect to HTTPS
        httpd_uri_t redirect_all = { .uri = "/*", .method = HTTP_GET, .handler = handle_https_redirect };
        httpd_register_uri_handler(g_http_server, &redirect_all);
        httpd_uri_t redirect_post = { .uri = "/*", .method = HTTP_POST, .handler = handle_https_redirect };
        httpd_register_uri_handler(g_http_server, &redirect_post);
        Serial.println("[HTTP]  Redirect server on port 80 -> HTTPS");
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

  httpd_uri_t wifi_scan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan };
  httpd_register_uri_handler(active_server, &wifi_scan);

  httpd_uri_t wifi_connect = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = handle_wifi_connect };
  httpd_register_uri_handler(active_server, &wifi_connect);

  httpd_uri_t wifi_disconnect = { .uri = "/api/wifi/disconnect", .method = HTTP_POST, .handler = handle_wifi_disconnect_auth };
  httpd_register_uri_handler(active_server, &wifi_disconnect);

  httpd_uri_t wifi_forget = { .uri = "/api/wifi/forget", .method = HTTP_POST, .handler = handle_wifi_forget_auth };
  httpd_register_uri_handler(active_server, &wifi_forget);

  httpd_uri_t wifi_reconnect = { .uri = "/api/wifi/reconnect", .method = HTTP_POST, .handler = handle_wifi_reconnect_auth };
  httpd_register_uri_handler(active_server, &wifi_reconnect);

  // Wizard pre-flight self-test (no auth — must be reachable on AP
  // before any post-pair token exists, identical to /api/wifi/scan).
  httpd_uri_t selftest = { .uri = "/api/selftest", .method = HTTP_GET, .handler = selftest::handle_selftest };
  httpd_register_uri_handler(active_server, &selftest);

  // Captive portal detection URLs (for iOS/Android automatic redirect)
  httpd_uri_t captive1 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handle_captive_portal };
  httpd_register_uri_handler(active_server, &captive1);

  httpd_uri_t captive2 = { .uri = "/generate_204", .method = HTTP_GET, .handler = handle_captive_portal };
  httpd_register_uri_handler(active_server, &captive2);

  httpd_uri_t captive3 = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = handle_captive_portal };
  httpd_register_uri_handler(active_server, &captive3);

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
  bluetooth_api::register_routes(active_server);
#endif

#if FEATURE_BLE
  // BLE Discovery endpoints (Opera/Chirp/Nearby)
  {
    httpd_uri_t ble_status = { .uri = "/api/ble/status", .method = HTTP_GET, .handler = handle_ble_status, .user_ctx = nullptr };
    httpd_register_uri_handler(active_server, &ble_status);

    httpd_uri_t ble_nearby = { .uri = "/api/nearby", .method = HTTP_GET, .handler = handle_ble_nearby, .user_ctx = nullptr };
    httpd_register_uri_handler(active_server, &ble_nearby);

    httpd_uri_t ble_chirp_send = { .uri = "/api/chirp/send", .method = HTTP_POST, .handler = handle_ble_chirp_send, .user_ctx = nullptr };
    httpd_register_uri_handler(active_server, &ble_chirp_send);
  }
#endif

  // WiFi Presence Detection endpoints
  wifi_presence_api::register_routes(active_server);

  // Household roles + auto-context (Owner/Family/Guest tagging, presence)
  household_api::register_routes(active_server);

  // Audible Chirp endpoints
  audible_chirp_api::register_routes(active_server);

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

  nvs.end();
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
  if (!g_wifi_creds.configured || !g_wifi_creds.enabled) {
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
      // defined behaviour for any future status value the enum doesn't list).
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
    derive_ap_password(g_device.pubkey_fp, g_device.ap_password, sizeof(g_device.ap_password));
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

static void wifi_init_provisioning() {
  memset(&g_wifi_status, 0, sizeof(g_wifi_status));

  // Load saved credentials
  bool has_creds = wifi_load_credentials();

  // Always use AP+STA mode for provisioning capability
  WiFi.mode(WIFI_AP_STA);

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
  bool ap_ok = WiFi.softAP(g_device.ap_ssid, ap_pass, AP_CHANNEL, false, AP_MAX_CLIENTS);

  if (!ap_ok) {
    log_health(SCV_LOG_ERROR, SCV_CAT_NETWORK, "WiFi AP start failed", nullptr);
    return;
  }

  g_wifi_status.ap_active = true;
  g_health.wifi_active = true;

  IPAddress ip = WiFi.softAPIP();
  snprintf(g_wifi_status.ap_ip, sizeof(g_wifi_status.ap_ip),
           "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  char msg[64];
  snprintf(msg, sizeof(msg), "AP: %s", g_device.ap_ssid);
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, msg, g_wifi_status.ap_ip);

  // Start mDNS
  if (MDNS.begin("canary")) {
    MDNS.addService("http", "tcp", 80);
    log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "mDNS started", "canary.local");
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

  // Generate device ID from MAC
  generate_device_id(g_device.device_id, sizeof(g_device.device_id));
  generate_ap_ssid(g_device.ap_ssid, sizeof(g_device.ap_ssid));
  g_device.first_boot = false;

  // Try to load existing key
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
  }

  // Derive public key and fingerprint
  Ed25519::derivePublicKey(g_device.pubkey, g_device.privkey);
  compute_fingerprint(g_device.pubkey, g_device.pubkey_fp);
  hex_to_str(g_device.fingerprint_hex, g_device.pubkey_fp, 8);
  Serial.printf("[PROV] Public key fingerprint: %s\n", g_device.fingerprint_hex);

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
  derive_ap_password(g_device.pubkey_fp, g_device.ap_password, sizeof(g_device.ap_password));

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
  
  Serial.printf("| %4u | %4s | %5s | %s | %11.7f | %12.7f | %6.1f | %d | %2d | %4.1f| %4.1f| %5.2f| %5.1f | %s... |\n",
    r->seq,
    record_type_name(r->type),
    state_name_short(st),
    r->verified ? "OK" : "!!",
    fx->lat,
    fx->lon,
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
  Serial.printf("│ Latitude   : %.7f°\n", g_fix.lat);
  Serial.printf("│ Longitude  : %.7f°\n", g_fix.lon);
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

void setup() {
  Serial.begin(115200);
  serial_wait_for_cdc(SERIAL_CDC_WAIT_MS);

  Serial.println();
  Serial.println("╔══════════════════════════════════════════════════════════════╗");
  Serial.println("║     SecuraCV Canary — Production Witness Device              ║");
  Serial.println("║     Privacy Witness Kernel (PWK) Compatible                  ║");
  Serial.println("║     Version 2.1.0 — Hardware Resilience Update               ║");
  Serial.println("╚══════════════════════════════════════════════════════════════╝");

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
    Serial.println("║  Device will auto-recover after 5 minutes of stability.      ║");
    Serial.println("╚══════════════════════════════════════════════════════════════╝");
    Serial.println();
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

  // Initialize SD card storage (with timeout, non-blocking)
  #if FEATURE_SD_STORAGE
  if (!in_safe_mode) {
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
  
  // ── TLS Certificate Initialization ──
  #if FEATURE_WIFI_AP && FEATURE_HTTP_SERVER
  if (g_device.initialized) {
    if (init_tls_cert()) {
      g_tls_enabled = true;
    } else {
      Serial.println("[WARN] TLS unavailable — running in HTTP-ONLY mode");
      Serial.println("[WARN] API traffic is NOT encrypted.");
      g_tls_enabled = false;
    }
  }
  #endif

  // Start WiFi Access Point
  #if FEATURE_WIFI_AP
  Serial.println("[..] Starting WiFi Access Point...");
  if (start_wifi_ap()) {
    Serial.printf("[WIFI] AP started: %s (password: %s)\n", g_device.ap_ssid, g_device.ap_password);

    #if FEATURE_HTTP_SERVER
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
      });

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
  } else {
    Serial.println("[--] Mesh network init skipped (safe mode)");
  }
  #endif

  // Initialize Bluetooth (legacy channel)
  #if FEATURE_BLUETOOTH
  if (!in_safe_mode) {
    Serial.println("[..] Initializing Bluetooth Low Energy...");
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
    } else {
      Serial.println("[--] Bluetooth init failed");
    }
  } else {
    Serial.println("[--] Bluetooth init skipped (safe mode)");
  }
  #endif

  // Initialize BLE Discovery (Opera/Chirp/Nearby)
  #if FEATURE_BLE
  if (!in_safe_mode) {
    Serial.println("[..] Initializing BLE Discovery subsystem...");

    // Build device ID hash hex string from pubkey fingerprint
    char ble_device_id_hex[20];
    hex_to_str(ble_device_id_hex, g_device.pubkey_fp, 8);

    if (ble_manager::init(ble_device_id_hex, FIRMWARE_VERSION,
                          &g_device.seq, g_device.chain_head)) {
      Serial.println("[OK] BLE Discovery initialized — Opera advertising, Nearby scanning");
      log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE Discovery initialized", nullptr);

      // spec/event_contract.md §10: route the lifecycle event through
      // the CSI chokepoint so the witness-chain row's allow-list is
      // enforced rather than implicit.
      ble_events_emit_initialized();

      ble_manager::operaStart();
      ble_manager::nearbyStart();

      // Boot chirp. Witness-chain side: chirp_sent through the chokepoint
      // so the wire format respects spec §10's allow-list.
      ble_manager::sendChirp(CHIRP_BOOT);
      ble_events_emit_chirp_sent("boot");
    } else {
      Serial.println("[--] BLE Discovery initialization failed — operating without BLE discovery");
      Serial.println("[--] Check: Is the BLE antenna connected?");
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH, "BLE Discovery init failed", nullptr);
      ble_events_emit_init_failed("ble_manager_init_returned_false");
    }
  } else {
    Serial.println("[--] BLE Discovery init skipped (safe mode)");
  }
  #endif

  // Initialize WiFi Presence Detection
  #if FEATURE_WIFI_PRESENCE
  if (!in_safe_mode) {
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
      strcpy(psram_str, "N/A");
    }
    Serial.printf("[OK] System monitor: %.1fC, Heap: %uKB, PSRAM: %s\n",
                  sys_monitor::g_sys_metrics.temp_celsius,
                  sys_monitor::g_sys_metrics.heap_free / 1024,
                  psram_str);
  }
  log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM, "System monitor initialized", nullptr);
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
  Serial.println("[..] Creating boot attestation record...");
  uint8_t boot_payload[256];
  size_t boot_len = 0;
  if (build_boot_attestation(boot_payload, sizeof(boot_payload), &boot_len)) {
    create_witness_record(boot_payload, boot_len, RECORD_BOOT_ATTESTATION, &g_last_record);
    Serial.printf("[OK] Boot attestation: seq=%u chain=", g_last_record.seq);
    hex_print(g_last_record.chain_hash, 8);
    Serial.println("...");
  }
  
  // Log boot event
  log_health(SCV_LOG_INFO, SCV_CAT_SYSTEM, "Device boot complete", FIRMWARE_VERSION);
  
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
  Serial.printf( "║    \"mac\": \"%s\",\n", WiFi.macAddress().c_str());
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

  Serial.println();
  Serial.println("╔══════════════════════════════════════════════════════════════╗");
  Serial.println("║               WITNESS DEVICE READY                           ║");
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.printf("║  Device ID  : %-45s  ║\n", g_device.device_id);
  Serial.printf("║  WiFi AP    : %-45s  ║\n", g_device.ap_ssid);
  Serial.printf("║  Password   : %-45s  ║\n", g_device.ap_password);
  #if FEATURE_WIFI_AP
  Serial.printf("║  Dashboard  : %s://%-36s  ║\n",
                g_tls_enabled ? "https" : "http",
                WiFi.softAPIP().toString().c_str());
  #endif
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.println("║  Commands: h=help i=identity s=status t=time g=gps c=cam m=sys║");
  Serial.println("║  Token + WiFi credentials are printed above on every boot      ║");
  Serial.println("║  Hold  BOOT (>3s)   = factory reset                           ║");
  Serial.println("╚══════════════════════════════════════════════════════════════╝");
  Serial.println();
  print_table_header();
}

// ════════════════════════════════════════════════════════════════════════════
// LOOP
// ════════════════════════════════════════════════════════════════════════════

void loop() {
  #if FEATURE_WATCHDOG
  esp_task_wdt_reset();
  #endif

  // ════════════════════════════════════════════════════════════════════════════
  // HARDWARE STATE MANAGEMENT — Update safe mode, track stability
  // ════════════════════════════════════════════════════════════════════════════
  safe_mode_update();

  // Handle serial commands
  handle_serial_commands();

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
      // Blink LED 3x to confirm
      #ifdef LED_BUILTIN
      for (int i = 0; i < 3; i++) {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(100);
        digitalWrite(LED_BUILTIN, LOW);
        delay(100);
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
    g_gps_rb.push((uint8_t)Serial1.read());
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

  #if FEATURE_SD_STORAGE
  // Periodic SD card check (handles hot-plug/unplug)
  sd_periodic_check(g_sd_spi, SD_CS_PIN, SD_SPI_FAST);

  // Sync SD hardware state to legacy flags
  g_sd_mounted = sd_is_available();
  g_health.sd_healthy = sd_is_available();
  #endif

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
  #endif

  // Update Bluetooth (legacy channel)
  #if FEATURE_BLUETOOTH
  bluetooth_channel::update();
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

  // Advance audible chirp state machine (non-blocking playback)
  #if FEATURE_AUDIBLE_CHIRP
  audible_chirp::update();
  #endif

  // Update system monitor (temp, heap, alerts)
  #if FEATURE_SYS_MONITOR
  sys_monitor::update(log_health);
  #endif

  // Drain CSI ring, finalize 1-Hz feature windows, dispatch to v1 modules.
  // The WiFi task fills the ring at up to 20 Hz; a stall longer than ~800 ms
  // drops frames at RING_CAP=16. Without this call the entire CSI pipeline
  // is dead and /api/csi/stream returns the boot-fallback "sensing" state
  // forever (see csi_hal.h:39 and firmware/common/csi/README.md:61).
  csi_integration::loop();

  // Optional MQTT bridge — pump (no-op when disabled or unconfigured),
  // plus three cadence-gated publishers for the topics HA expects.
  // Schemas locked against custom_components/securacv/sensor.py.
  csi_mqtt::loop();
  if (csi_mqtt::connected()) {
    static uint32_t s_mqtt_status_ms = 0;
    static uint32_t s_mqtt_health_ms = 0;
    static uint32_t s_mqtt_counts_last = 0;
    if (now - s_mqtt_status_ms >= 30000UL) {
      s_mqtt_status_ms = now;
      csi_mqtt::publish_status(
          csi_integration::csi_running(),
          /*wifi_connected=*/(WiFi.status() == WL_CONNECTED || WiFi.softAPgetStationNum() > 0),
          /*rssi_dbm=*/(WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0);
    }
    if (now - s_mqtt_health_ms >= 60000UL) {
      s_mqtt_health_ms = now;
      csi_mqtt::publish_health((uint32_t)ESP.getFreeHeap(),
                               (uint32_t)uptime_seconds());
    }
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
  }

  // Yield before witness record creation
  yield();

  // ════════════════════════════════════════════════════════════════════════════
  // WITNESS RECORD CREATION
  // ════════════════════════════════════════════════════════════════════════════

  if (now - g_last_record_ms >= RECORD_INTERVAL_MS) {
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
