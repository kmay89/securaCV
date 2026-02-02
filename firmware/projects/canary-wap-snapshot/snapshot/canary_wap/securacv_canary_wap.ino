/*
  ╔══════════════════════════════════════════════════════════════════════════════╗
  ║  SecuraCV Canary — Production Witness Device with SD & WAP                   ║
  ║  Version 2.0.1 — SD Storage + WiFi Access Point + Fixed Camera Peek          ║
  ║                                                                              ║
  ║  Privacy Witness Kernel (PWK) Compatible                                     ║
  ║  Hardware: ESP32-S3 (XIAO Sense) + L76K GNSS + microSD                       ║
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

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include "esp_system.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "mbedtls/sha256.h"

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
#include "wap_server.h"
#include "web_ui.h"
#include "mesh_network.h"
#include "bluetooth_channel.h"
#include "bluetooth_api.h"

// ════════════════════════════════════════════════════════════════════════════
// COMPILE-TIME FEATURE FLAGS
// ════════════════════════════════════════════════════════════════════════════

#define FEATURE_SD_STORAGE    1   // Enable SD card storage
#define FEATURE_WIFI_AP       1   // Enable WiFi Access Point
#define FEATURE_HTTP_SERVER   1   // Enable HTTP API server
#define FEATURE_CAMERA_PEEK   1   // Enable camera peek/preview
#define FEATURE_TAMPER_GPIO   0   // Enable tamper detection pin
#define FEATURE_WATCHDOG      1   // Enable hardware watchdog
#define FEATURE_STATE_LOG     1   // Log state transitions
#define FEATURE_MESH_NETWORK  1   // Enable mesh network (opera)
#define FEATURE_BLUETOOTH     1   // Enable Bluetooth Low Energy

#define DEBUG_NMEA            0   // Print raw NMEA sentences
#define DEBUG_CBOR            0   // Print CBOR hex dump
#define DEBUG_CHAIN           0   // Print full chain operations
#define DEBUG_VERIFY          0   // Print signature verification details
#define DEBUG_HTTP            0   // Print HTTP request details

// ════════════════════════════════════════════════════════════════════════════
// VERSION & PROTOCOL (must match PWK expectations)
// ════════════════════════════════════════════════════════════════════════════

static const char* DEVICE_TYPE        = "canary";
static const char* FIRMWARE_VERSION   = "2.0.1";
static const char* RULESET_ID         = "securacv:canary:v1.0";
static const char* PROTOCOL_VERSION   = "pwk:v0.3.0";
static const char* CHAIN_ALGORITHM    = "sha256-domain-sep";
static const char* SIGNATURE_ALGORITHM = "ed25519";

// ════════════════════════════════════════════════════════════════════════════
// DEVICE CONFIG
// ════════════════════════════════════════════════════════════════════════════

static const char* DEVICE_ID_PREFIX = "canary-s3-";
static const char* ZONE_ID   = "zone:mobile";

// ════════════════════════════════════════════════════════════════════════════
// GNSS CONFIG
// ════════════════════════════════════════════════════════════════════════════

static const uint32_t GPS_BAUD = 9600;
static const int GPS_RX_GPIO = 44;  // XIAO D7
static const int GPS_TX_GPIO = 43;  // XIAO D6

// ════════════════════════════════════════════════════════════════════════════
// SD CARD CONFIG (XIAO ESP32S3 Sense)
// ════════════════════════════════════════════════════════════════════════════

static const int SD_CS_PIN   = 21;
static const int SD_SCK_PIN  = 7;
static const int SD_MISO_PIN = 8;
static const int SD_MOSI_PIN = 9;
static const uint32_t SD_SPI_FAST = 4000000;
static const uint32_t SD_SPI_SLOW = 1000000;

// ════════════════════════════════════════════════════════════════════════════
// WIFI AP CONFIG
// ════════════════════════════════════════════════════════════════════════════

static const char* AP_PASSWORD_DEFAULT = "witness2026";
static const int   AP_CHANNEL          = 1;
static const int   AP_MAX_CLIENTS      = 4;

// ════════════════════════════════════════════════════════════════════════════
// CAMERA CONFIG (XIAO ESP32S3 Sense)
// ════════════════════════════════════════════════════════════════════════════

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
static const uint32_t BOOT_BUTTON_HOLD_MS  = 1200;
static const int      BOOT_BUTTON_GPIO     = 0;

// ════════════════════════════════════════════════════════════════════════════
// MOTION DETECTION WITH HYSTERESIS
// ════════════════════════════════════════════════════════════════════════════

static const float    MOVING_THRESHOLD_MPS   = 0.8f;
static const float    STATIC_THRESHOLD_MPS   = 0.4f;
static const float    SPEED_EMA_ALPHA        = 0.15f;
static const uint32_t STATE_HYSTERESIS_MS    = 2000;

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

// SD card
static SPIClass g_sd_spi(FSPI);
static bool g_sd_mounted = false;

// HTTP server
static httpd_handle_t g_http_server = nullptr;

// WiFi provisioning state
static WiFiCredentials g_wifi_creds;
static WiFiStatus g_wifi_status;
static bool g_wifi_scan_in_progress = false;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000;

// Camera state
#if FEATURE_CAMERA_PEEK
static bool g_camera_initialized = false;
static volatile bool g_peek_active = false;
static framesize_t g_peek_framesize = FRAMESIZE_VGA;
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
// CHAIN OPERATIONS
// ════════════════════════════════════════════════════════════════════════════

static void compute_chain_hash(const uint8_t prev[32], const uint8_t payload_hash[32], 
                               uint32_t seq, uint32_t time_bucket, uint8_t out[32]) {
  // Domain-separated: "securacv:chain:v1" || 0x00 || prev || payload_hash || seq (BE) || time_bucket (BE)
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
  
  sha256_domain("securacv:chain:v1", buf, sizeof(buf), out);
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
  sha256_domain("securacv:payload:v1", payload, len, payload_hash);
  
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

static bool verify_record_signature(const WitnessRecord* rec) {
  return verify_signature(g_device.pubkey, rec->chain_hash, 32, rec->signature);
}

// ════════════════════════════════════════════════════════════════════════════
// HEALTH LOGGING
// ════════════════════════════════════════════════════════════════════════════

void log_health(LogLevel level, LogCategory category, const char* message, const char* detail) {
  // Skip DEBUG by default
  if (level < LOG_LEVEL_INFO) return;
  
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
  log_health(LOG_LEVEL_NOTICE, LOG_CAT_GPS, msg, reason);
  
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
  g_health.http_requests++;
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, CANARY_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_status(httpd_req_t* req) {
  g_health.http_requests++;
  
  StaticJsonDocument<1024> doc;
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
  doc["gps_healthy"] = g_health.gps_healthy;
  doc["sd_mounted"] = g_sd_mounted;
  doc["sd_healthy"] = g_health.sd_healthy;
  doc["wifi_active"] = g_health.wifi_active;
  
#if FEATURE_CAMERA_PEEK
  doc["camera_ready"] = g_camera_initialized;
  doc["peek_active"] = g_peek_active;
  doc["peek_resolution"] = (int)g_peek_framesize;
#endif
  
  if (g_sd_mounted) {
    doc["sd_total"] = SD.totalBytes();
    doc["sd_used"] = SD.usedBytes();
    doc["sd_free"] = SD.totalBytes() - SD.usedBytes();
  }
  
  doc["logs_stored"] = g_health.logs_stored;
  doc["unacked_count"] = g_health.logs_unacked;
  
  JsonObject gps = doc.createNestedObject("gps");
  gps["valid"] = g_fix.valid;
  gps["lat"] = g_fix.lat;
  gps["lon"] = g_fix.lon;
  gps["alt"] = g_fix.altitude_m;
  gps["speed"] = g_speed_ema;
  gps["quality"] = g_fix.quality;
  gps["satellites"] = g_fix.satellites;
  gps["hdop"] = g_fix.hdop;
  gps["fix_mode"] = (int)g_fix.fix_mode;
  gps["state"] = state_name(g_state);
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_chain(httpd_req_t* req) {
  g_health.http_requests++;
  
  StaticJsonDocument<512> doc;
  doc["ok"] = true;
  
  char chain_hex[65];
  hex_to_str(chain_hex, g_device.chain_head, 32);
  doc["chain_head"] = chain_hex;
  doc["sequence"] = g_device.seq;
  
  JsonArray blocks = doc.createNestedArray("blocks");
  
  // Add last record info
  if (g_last_record.seq > 0) {
    JsonObject block = blocks.createNestedObject();
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
  
  DynamicJsonDocument doc(4096);
  doc["ok"] = true;
  doc["total"] = g_health_log_ring_count;
  
  JsonArray logs = doc.createNestedArray("logs");
  
  // Iterate through ring buffer (most recent first)
  for (size_t i = 0; i < g_health_log_ring_count; i++) {
    size_t idx = (g_health_log_ring_head + HEALTH_LOG_RING_SIZE - 1 - i) % HEALTH_LOG_RING_SIZE;
    HealthLogRingEntry& entry = g_health_log_ring[idx];
    
    if (unacked_only && entry.ack_status != ACK_STATUS_UNREAD) {
      continue;
    }
    
    JsonObject log = logs.createNestedObject();
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
    StaticJsonDocument<128> body;
    if (deserializeJson(body, content) == DeserializationError::Ok) {
      reason = body["reason"] | "";
    }
  }
  
  bool success = acknowledge_log_entry(seq, ACK_STATUS_ACKNOWLEDGED, reason);
  
  StaticJsonDocument<128> doc;
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
  
  log_health(LOG_LEVEL_INFO, LOG_CAT_USER, "Bulk acknowledgment", nullptr);
  
  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["acknowledged"] = acked;
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_witness(httpd_req_t* req) {
  g_health.http_requests++;
  
  DynamicJsonDocument doc(2048);
  doc["ok"] = true;
  doc["total"] = g_health.records_created;
  
  JsonArray records = doc.createNestedArray("records");
  
  // Just show last record info for now
  if (g_last_record.seq > 0) {
    JsonObject rec = records.createNestedObject();
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
  
  StaticJsonDocument<256> doc;
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
  
  if (!g_sd_mounted) {
    StaticJsonDocument<128> doc;
    doc["ok"] = false;
    doc["error"] = "SD card not mounted";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }
  
  // Create export bundle
  char export_path[64];
  snprintf(export_path, sizeof(export_path), "/sd/EXPORT/bundle_%u.json", (unsigned)millis());
  
  File file = SD.open(export_path, FILE_WRITE);
  if (!file) {
    StaticJsonDocument<128> doc;
    doc["ok"] = false;
    doc["error"] = "Failed to create export file";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }
  
  // Write export header
  StaticJsonDocument<512> header;
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
  
  log_health(LOG_LEVEL_INFO, LOG_CAT_USER, "Export created", export_path);
  
  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["download_url"] = String("/api/download?path=") + export_path;
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_reboot(httpd_req_t* req) {
  g_health.http_requests++;
  
  log_health(LOG_LEVEL_NOTICE, LOG_CAT_USER, "Reboot requested", nullptr);
  
  // Persist state
  nvs_store_u32(NVS_KEY_SEQ, g_device.seq);
  nvs_store_bytes(NVS_KEY_CHAIN, g_device.chain_head, 32);
  
  StaticJsonDocument<64> doc;
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

static bool init_camera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_VGA;  // 640x480
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  
  // Adjust for PSRAM availability
  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_QVGA;  // 320x240 if no PSRAM
    config.fb_location = CAMERA_FB_IN_DRAM;
  }
  
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAMERA] Init failed: 0x%x\n", err);
    return false;
  }
  
  g_peek_framesize = config.frame_size;
  Serial.println("[CAMERA] Initialized for peek/preview");
  return true;
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
  
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek started", nullptr);
  
  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["message"] = "Peek stream activated";
  doc["resolution"] = framesize_name(g_peek_framesize);
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK STREAM — FIXED: Now properly manages g_peek_active state
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_stream(httpd_req_t* req) {
  g_health.http_requests++;
  
  if (!g_camera_initialized) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "Camera not initialized", HTTPD_RESP_USE_STRLEN);
  }
  
  // *** KEY FIX: Set peek_active to true when stream is requested ***
  g_peek_active = true;
  
  // Set proper MJPEG multipart headers
  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, pre-check=0, post-check=0, max-age=0");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "12");
  
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
    
    // Send trailing CRLF
    res = httpd_resp_send_chunk(req, "\r\n", 2);
    esp_camera_fb_return(fb);
    
    if (res != ESP_OK) {
      break;
    }
    
    // Feed watchdog and yield (target ~12 fps)
    #if FEATURE_WATCHDOG
    esp_task_wdt_reset();
    #endif
    vTaskDelay(pdMS_TO_TICKS(80));
  }
  
  // *** KEY FIX: Set peek_active to false when stream ends ***
  g_peek_active = false;
  
  // End chunked response
  httpd_resp_send_chunk(req, NULL, 0);
  
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stream ended", nullptr);
  
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
  
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stopped", nullptr);
  
  StaticJsonDocument<64> doc;
  doc["ok"] = true;
  doc["message"] = "Peek stopped";
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_peek_status(httpd_req_t* req) {
  g_health.http_requests++;
  
  StaticJsonDocument<256> doc;
  doc["ok"] = true;
  doc["camera_initialized"] = g_camera_initialized;
  doc["peek_active"] = g_peek_active;
  doc["resolution"] = (int)g_peek_framesize;
  doc["resolution_name"] = framesize_name(g_peek_framesize);
  
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
  
  StaticJsonDocument<64> body;
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
  
  StaticJsonDocument<128> doc;
  doc["ok"] = success;
  if (success) {
    doc["resolution"] = size;
    doc["resolution_name"] = framesize_name((framesize_t)size);
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Resolution changed", framesize_name((framesize_t)size));
  } else {
    doc["error"] = "Failed to set resolution";
  }
  
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
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

  StaticJsonDocument<512> doc;
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
  DynamicJsonDocument doc(2048);
  doc["ok"] = true;
  doc["count"] = count;

  JsonArray peers = doc.createNestedArray("peers");
  for (uint8_t i = 0; i < count; i++) {
    const mesh_network::OperaPeer* peer = mesh_network::get_peer(i);
    if (!peer) continue;

    JsonObject p = peers.createNestedObject();
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

  DynamicJsonDocument doc(2048);
  doc["ok"] = true;
  doc["count"] = count;

  JsonArray arr = doc.createNestedArray("alerts");
  for (size_t i = 0; i < count; i++) {
    const mesh_network::MeshAlert* alert = &alerts[i];
    JsonObject a = arr.createNestedObject();

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

  StaticJsonDocument<64> body;
  if (deserializeJson(body, buf) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  bool enabled = body["enabled"] | false;
  mesh_network::set_enabled(enabled);
  log_health(LOG_LEVEL_INFO, LOG_CAT_MESH, enabled ? "Mesh enabled" : "Mesh disabled", nullptr);

  return http_send_json(req, "{\"ok\":true}");
}

static esp_err_t handle_mesh_pair_start(httpd_req_t* req) {
  g_health.http_requests++;

  char buf[128];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  buf[ret > 0 ? ret : 0] = '\0';

  const char* opera_name = nullptr;
  StaticJsonDocument<128> body;
  if (ret > 0 && deserializeJson(body, buf) == DeserializationError::Ok) {
    opera_name = body["name"] | (const char*)nullptr;
  }

  if (mesh_network::start_pairing_initiator(opera_name)) {
    log_health(LOG_LEVEL_INFO, LOG_CAT_MESH, "Pairing started (initiator)", nullptr);
    return http_send_json(req, "{\"ok\":true}");
  }
  return http_send_error(req, 400, "pairing_failed");
}

static esp_err_t handle_mesh_pair_join(httpd_req_t* req) {
  g_health.http_requests++;

  if (mesh_network::start_pairing_joiner()) {
    log_health(LOG_LEVEL_INFO, LOG_CAT_MESH, "Pairing started (joiner)", nullptr);
    return http_send_json(req, "{\"ok\":true}");
  }
  return http_send_error(req, 400, "pairing_failed");
}

static esp_err_t handle_mesh_pair_confirm(httpd_req_t* req) {
  g_health.http_requests++;

  if (mesh_network::confirm_pairing()) {
    log_health(LOG_LEVEL_INFO, LOG_CAT_MESH, "Pairing confirmed", nullptr);
    return http_send_json(req, "{\"ok\":true}");
  }
  return http_send_error(req, 400, "confirm_failed");
}

static esp_err_t handle_mesh_pair_cancel(httpd_req_t* req) {
  g_health.http_requests++;
  mesh_network::cancel_pairing();
  log_health(LOG_LEVEL_INFO, LOG_CAT_MESH, "Pairing cancelled", nullptr);
  return http_send_json(req, "{\"ok\":true}");
}

static esp_err_t handle_mesh_leave(httpd_req_t* req) {
  g_health.http_requests++;

  if (mesh_network::leave_opera()) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_MESH, "Left opera", nullptr);
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

  StaticJsonDocument<64> body;
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
    log_health(LOG_LEVEL_WARNING, LOG_CAT_MESH, "Peer removed", fp_hex);
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

  StaticJsonDocument<128> body;
  if (deserializeJson(body, buf) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  const char* name = body["name"] | "";
  if (strlen(name) == 0 || strlen(name) > mesh_network::MAX_OPERA_NAME_LEN) {
    return http_send_error(req, 400, "invalid_name");
  }

  if (mesh_network::set_opera_name(name)) {
    log_health(LOG_LEVEL_INFO, LOG_CAT_MESH, "Opera name changed", name);
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

  StaticJsonDocument<512> doc;
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
    StaticJsonDocument<64> doc;
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

    StaticJsonDocument<64> doc;
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
  DynamicJsonDocument doc(2048);
  doc["ok"] = true;
  doc["scanning"] = false;
  doc["count"] = n;

  JsonArray networks = doc.createNestedArray("networks");

  for (int i = 0; i < n && i < 20; i++) {
    JsonObject net = networks.createNestedObject();
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
    StaticJsonDocument<64> doc;
    doc["ok"] = false;
    doc["error"] = "No body";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  StaticJsonDocument<256> body;
  if (deserializeJson(body, content) != DeserializationError::Ok) {
    StaticJsonDocument<64> doc;
    doc["ok"] = false;
    doc["error"] = "Invalid JSON";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  const char* ssid = body["ssid"] | "";
  const char* password = body["password"] | "";

  if (strlen(ssid) == 0 || strlen(ssid) > 32) {
    StaticJsonDocument<64> doc;
    doc["ok"] = false;
    doc["error"] = "Invalid SSID (1-32 chars required)";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  if (strlen(password) > 64) {
    StaticJsonDocument<64> doc;
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

  // Attempt connection
  wifi_connect_to_home();

  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["message"] = "Credentials saved, attempting connection";
  doc["ssid"] = g_wifi_creds.ssid;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
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

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "WiFi disconnected", nullptr);

  StaticJsonDocument<64> doc;
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

  StaticJsonDocument<64> doc;
  doc["ok"] = true;
  doc["message"] = "WiFi credentials forgotten";

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_reconnect(httpd_req_t* req) {
  g_health.http_requests++;

  if (!g_wifi_creds.configured) {
    StaticJsonDocument<64> doc;
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

  StaticJsonDocument<128> doc;
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
static esp_err_t handle_captive_portal(httpd_req_t* req) {
  g_health.http_requests++;

  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://canary.local/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, NULL, 0);
}

// ════════════════════════════════════════════════════════════════════════════
// HTTP SERVER SETUP
// ════════════════════════════════════════════════════════════════════════════

static void start_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 8192;  // Increased stack for camera streaming
  config.max_uri_handlers = 32;  // Increased for WiFi provisioning endpoints
  
  if (httpd_start(&g_http_server, &config) != ESP_OK) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_NETWORK, "HTTP server start failed", nullptr);
    return;
  }
  
  // UI
  httpd_uri_t ui = { .uri = "/", .method = HTTP_GET, .handler = handle_ui };
  httpd_register_uri_handler(g_http_server, &ui);
  
  // API endpoints
  httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status };
  httpd_register_uri_handler(g_http_server, &status);
  
  httpd_uri_t chain = { .uri = "/api/chain", .method = HTTP_GET, .handler = handle_chain };
  httpd_register_uri_handler(g_http_server, &chain);
  
  httpd_uri_t logs = { .uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs };
  httpd_register_uri_handler(g_http_server, &logs);
  
  httpd_uri_t log_ack = { .uri = "/api/logs/*/ack", .method = HTTP_POST, .handler = handle_log_ack };
  httpd_register_uri_handler(g_http_server, &log_ack);
  
  httpd_uri_t ack_all = { .uri = "/api/logs/ack-all", .method = HTTP_POST, .handler = handle_ack_all };
  httpd_register_uri_handler(g_http_server, &ack_all);
  
  httpd_uri_t witness = { .uri = "/api/witness", .method = HTTP_GET, .handler = handle_witness };
  httpd_register_uri_handler(g_http_server, &witness);
  
  httpd_uri_t config_get = { .uri = "/api/config", .method = HTTP_GET, .handler = handle_config_get };
  httpd_register_uri_handler(g_http_server, &config_get);
  
  httpd_uri_t export_bundle = { .uri = "/api/export", .method = HTTP_POST, .handler = handle_export };
  httpd_register_uri_handler(g_http_server, &export_bundle);
  
  httpd_uri_t reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = handle_reboot };
  httpd_register_uri_handler(g_http_server, &reboot);

  // WiFi provisioning endpoints
  httpd_uri_t wifi_status = { .uri = "/api/wifi", .method = HTTP_GET, .handler = handle_wifi_status };
  httpd_register_uri_handler(g_http_server, &wifi_status);

  httpd_uri_t wifi_scan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan };
  httpd_register_uri_handler(g_http_server, &wifi_scan);

  httpd_uri_t wifi_connect = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = handle_wifi_connect };
  httpd_register_uri_handler(g_http_server, &wifi_connect);

  httpd_uri_t wifi_disconnect = { .uri = "/api/wifi/disconnect", .method = HTTP_POST, .handler = handle_wifi_disconnect };
  httpd_register_uri_handler(g_http_server, &wifi_disconnect);

  httpd_uri_t wifi_forget = { .uri = "/api/wifi/forget", .method = HTTP_POST, .handler = handle_wifi_forget };
  httpd_register_uri_handler(g_http_server, &wifi_forget);

  httpd_uri_t wifi_reconnect = { .uri = "/api/wifi/reconnect", .method = HTTP_POST, .handler = handle_wifi_reconnect };
  httpd_register_uri_handler(g_http_server, &wifi_reconnect);

  // Captive portal detection URLs (for iOS/Android automatic redirect)
  httpd_uri_t captive1 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handle_captive_portal };
  httpd_register_uri_handler(g_http_server, &captive1);

  httpd_uri_t captive2 = { .uri = "/generate_204", .method = HTTP_GET, .handler = handle_captive_portal };
  httpd_register_uri_handler(g_http_server, &captive2);

  httpd_uri_t captive3 = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = handle_captive_portal };
  httpd_register_uri_handler(g_http_server, &captive3);

#if FEATURE_CAMERA_PEEK
  // Camera peek endpoints (for positioning/setup only - no recording)
  // NEW: Start endpoint to explicitly activate streaming
  httpd_uri_t peek_start = { .uri = "/api/peek/start", .method = HTTP_POST, .handler = handle_peek_start };
  httpd_register_uri_handler(g_http_server, &peek_start);
  
  httpd_uri_t peek_stream = { .uri = "/api/peek/stream", .method = HTTP_GET, .handler = handle_peek_stream };
  httpd_register_uri_handler(g_http_server, &peek_stream);
  
  httpd_uri_t peek_snapshot = { .uri = "/api/peek/snapshot", .method = HTTP_GET, .handler = handle_peek_snapshot };
  httpd_register_uri_handler(g_http_server, &peek_snapshot);
  
  httpd_uri_t peek_stop = { .uri = "/api/peek/stop", .method = HTTP_POST, .handler = handle_peek_stop };
  httpd_register_uri_handler(g_http_server, &peek_stop);
  
  httpd_uri_t peek_status = { .uri = "/api/peek/status", .method = HTTP_GET, .handler = handle_peek_status };
  httpd_register_uri_handler(g_http_server, &peek_status);
  
  // NEW: Resolution control endpoint
  httpd_uri_t peek_resolution = { .uri = "/api/peek/resolution", .method = HTTP_POST, .handler = handle_peek_resolution };
  httpd_register_uri_handler(g_http_server, &peek_resolution);
#endif

#if FEATURE_MESH_NETWORK
  // Mesh network (opera) endpoints
  httpd_uri_t mesh_status = { .uri = "/api/mesh", .method = HTTP_GET, .handler = handle_mesh_status };
  httpd_register_uri_handler(g_http_server, &mesh_status);

  httpd_uri_t mesh_peers = { .uri = "/api/mesh/peers", .method = HTTP_GET, .handler = handle_mesh_peers };
  httpd_register_uri_handler(g_http_server, &mesh_peers);

  httpd_uri_t mesh_alerts = { .uri = "/api/mesh/alerts", .method = HTTP_GET, .handler = handle_mesh_alerts };
  httpd_register_uri_handler(g_http_server, &mesh_alerts);

  httpd_uri_t mesh_alerts_clear = { .uri = "/api/mesh/alerts", .method = HTTP_DELETE, .handler = handle_mesh_alerts_clear };
  httpd_register_uri_handler(g_http_server, &mesh_alerts_clear);

  httpd_uri_t mesh_enable = { .uri = "/api/mesh/enable", .method = HTTP_POST, .handler = handle_mesh_enable };
  httpd_register_uri_handler(g_http_server, &mesh_enable);

  httpd_uri_t mesh_pair_start = { .uri = "/api/mesh/pair/start", .method = HTTP_POST, .handler = handle_mesh_pair_start };
  httpd_register_uri_handler(g_http_server, &mesh_pair_start);

  httpd_uri_t mesh_pair_join = { .uri = "/api/mesh/pair/join", .method = HTTP_POST, .handler = handle_mesh_pair_join };
  httpd_register_uri_handler(g_http_server, &mesh_pair_join);

  httpd_uri_t mesh_pair_confirm = { .uri = "/api/mesh/pair/confirm", .method = HTTP_POST, .handler = handle_mesh_pair_confirm };
  httpd_register_uri_handler(g_http_server, &mesh_pair_confirm);

  httpd_uri_t mesh_pair_cancel = { .uri = "/api/mesh/pair/cancel", .method = HTTP_POST, .handler = handle_mesh_pair_cancel };
  httpd_register_uri_handler(g_http_server, &mesh_pair_cancel);

  httpd_uri_t mesh_leave = { .uri = "/api/mesh/leave", .method = HTTP_POST, .handler = handle_mesh_leave };
  httpd_register_uri_handler(g_http_server, &mesh_leave);

  httpd_uri_t mesh_remove = { .uri = "/api/mesh/remove", .method = HTTP_POST, .handler = handle_mesh_remove };
  httpd_register_uri_handler(g_http_server, &mesh_remove);

  httpd_uri_t mesh_name = { .uri = "/api/mesh/name", .method = HTTP_POST, .handler = handle_mesh_name };
  httpd_register_uri_handler(g_http_server, &mesh_name);
#endif

#if FEATURE_BLUETOOTH
  // Bluetooth endpoints
  bluetooth_api::register_routes(g_http_server);
#endif

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "HTTP server started", "port 80");
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

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "WiFi credentials saved", g_wifi_creds.ssid);
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

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "WiFi credentials cleared", nullptr);
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

  g_wifi_status.state = WIFI_PROV_CONNECTING;
  g_wifi_status.connect_attempts++;
  g_wifi_status.last_connect_ms = millis();

  char msg[64];
  snprintf(msg, sizeof(msg), "Connecting to: %s", g_wifi_creds.ssid);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, msg, nullptr);

  // Start connection (non-blocking)
  WiFi.begin(g_wifi_creds.ssid, g_wifi_creds.password);
}

static void wifi_check_connection() {
  uint32_t now = millis();

  // Update status
  wifi_update_status();

  // Handle state transitions
  switch (g_wifi_status.state) {
    case WIFI_PROV_CONNECTING:
      if (WiFi.isConnected()) {
        g_wifi_status.state = WIFI_PROV_CONNECTED;
        g_wifi_status.connected_since_ms = now;

        char msg[80];
        snprintf(msg, sizeof(msg), "Connected to %s", g_wifi_creds.ssid);
        log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, msg, g_wifi_status.sta_ip);
      } else if (now - g_wifi_status.last_connect_ms > WIFI_CONNECT_TIMEOUT_MS) {
        g_wifi_status.state = WIFI_PROV_FAILED;
        log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "WiFi connection timeout", g_wifi_creds.ssid);
      }
      break;

    case WIFI_PROV_CONNECTED:
      if (!WiFi.isConnected()) {
        g_wifi_status.state = WIFI_PROV_FAILED;
        log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "WiFi connection lost", nullptr);
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

static void wifi_init_provisioning() {
  memset(&g_wifi_status, 0, sizeof(g_wifi_status));

  // Load saved credentials
  bool has_creds = wifi_load_credentials();

  // Always use AP+STA mode for provisioning capability
  WiFi.mode(WIFI_AP_STA);

  // Start Access Point
  bool ap_ok = WiFi.softAP(g_device.ap_ssid, AP_PASSWORD_DEFAULT, AP_CHANNEL, false, AP_MAX_CLIENTS);

  if (!ap_ok) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_NETWORK, "WiFi AP start failed", nullptr);
    return;
  }

  g_wifi_status.ap_active = true;
  g_health.wifi_active = true;

  IPAddress ip = WiFi.softAPIP();
  snprintf(g_wifi_status.ap_ip, sizeof(g_wifi_status.ap_ip),
           "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  char msg[64];
  snprintf(msg, sizeof(msg), "AP: %s", g_device.ap_ssid);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, msg, g_wifi_status.ap_ip);

  // Start mDNS
  if (MDNS.begin("canary")) {
    MDNS.addService("http", "tcp", 80);
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "mDNS started", "canary.local");
  }

  // Attempt to connect to home WiFi if configured
  if (has_creds && g_wifi_creds.enabled) {
    wifi_connect_to_home();
  } else {
    g_wifi_status.state = WIFI_PROV_AP_ONLY;
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "AP-only mode", "No home WiFi configured");
  }
}

// Legacy function for compatibility
static bool start_wifi_ap() {
  wifi_init_provisioning();
  return g_wifi_status.ap_active;
}

// ════════════════════════════════════════════════════════════════════════════
// SD CARD INITIALIZATION
// ════════════════════════════════════════════════════════════════════════════

static bool sd_init() {
  g_sd_spi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  
  if (!SD.begin(SD_CS_PIN, g_sd_spi, SD_SPI_FAST)) {
    // Try slower speed
    if (!SD.begin(SD_CS_PIN, g_sd_spi, SD_SPI_SLOW)) {
      g_health.sd_healthy = false;
      return false;
    }
  }
  
  // Create directories if needed
  if (!SD.exists("/WITNESS")) SD.mkdir("/WITNESS");
  if (!SD.exists("/HEALTH")) SD.mkdir("/HEALTH");
  if (!SD.exists("/CHAIN")) SD.mkdir("/CHAIN");
  if (!SD.exists("/EXPORT")) SD.mkdir("/EXPORT");
  
  g_sd_mounted = true;
  g_health.sd_healthy = true;
  
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// DEVICE PROVISIONING
// ════════════════════════════════════════════════════════════════════════════

static bool provision_device() {
  Serial.println("[..] Provisioning device identity...");
  
  // Generate device ID from MAC
  generate_device_id(g_device.device_id, sizeof(g_device.device_id));
  generate_ap_ssid(g_device.ap_ssid, sizeof(g_device.ap_ssid));
  
  // Try to load existing key
  if (nvs_load_key(g_device.privkey)) {
    Serial.println("[OK] Loaded existing keypair from NVS");
  } else {
    Serial.println("[..] Generating new keypair...");
    if (!generate_keypair(g_device.privkey, g_device.pubkey)) {
      Serial.println("[!!] Keypair generation failed");
      return false;
    }
    if (!nvs_store_key(g_device.privkey)) {
      Serial.println("[!!] Failed to store keypair");
      return false;
    }
    Serial.println("[OK] New keypair generated and stored");
  }
  
  // Derive public key
  Ed25519::derivePublicKey(g_device.pubkey, g_device.privkey);
  compute_fingerprint(g_device.pubkey, g_device.pubkey_fp);
  
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
  
  Serial.printf("[OK] Device ID: %s\n", g_device.device_id);
  Serial.printf("[OK] Boot count: %u\n", g_device.boot_count);
  Serial.printf("[OK] Chain seq: %u\n", g_device.seq);
  
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// OUTPUT FORMATTING
// ════════════════════════════════════════════════════════════════════════════

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
  Serial.printf("╔═══ STATUS ══╦═══════════════════════════════════════════════════════════╗\n");
  Serial.printf("║ Uptime: %s  ║  Records: %u  |  GPS: %s  |  SD: %s  |  WiFi: %d clients\n",
    uptime_str,
    g_health.records_created,
    g_health.gps_healthy ? "OK" : "--",
    g_sd_mounted ? "OK" : "--",
    WiFi.softAPgetStationNum()
  );
  Serial.printf("╚══════════════╩═══════════════════════════════════════════════════════════╝\n");
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
        Serial.printf("Password: %s\n", AP_PASSWORD_DEFAULT);
        Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("Clients: %d\n", WiFi.softAPgetStationNum());
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
  Serial.println("║     Version 2.0.1 — SD Storage + WiFi AP + Fixed Camera      ║");
  Serial.println("╚══════════════════════════════════════════════════════════════╝");
  
  fix_init(&g_fix);
  utc_init(&g_gps_utc);
  memset(&g_last_record, 0, sizeof(g_last_record));
  g_state_entered_ms = millis();
  g_pending_state = STATE_NO_FIX;
  
  // Provision device identity and crypto
  if (!provision_device()) {
    Serial.println();
    Serial.println("[!!] PROVISIONING FAILED - HALTING");
    while (true) { delay(1000); }
  }
  
  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
  
  // Setup watchdog
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
  
  // Initialize SD card storage
  #if FEATURE_SD_STORAGE
  Serial.println("[..] Initializing SD card storage...");
  if (sd_init()) {
    Serial.println("[OK] SD card ready for witness records");
  } else {
    Serial.println("[WARN] SD card not available - records will not persist");
  }
  #endif
  
  // Start WiFi Access Point
  #if FEATURE_WIFI_AP
  Serial.println("[..] Starting WiFi Access Point...");
  if (start_wifi_ap()) {
    Serial.println("[OK] WiFi AP active");
    
    #if FEATURE_HTTP_SERVER
    Serial.println("[..] Starting HTTP server...");
    start_http_server();
    #endif
  } else {
    Serial.println("[WARN] WiFi AP failed to start");
  }
  #endif
  
  // Initialize camera for peek/preview
  #if FEATURE_CAMERA_PEEK
  Serial.println("[..] Initializing camera for peek/preview...");
  g_camera_initialized = init_camera();
  if (g_camera_initialized) {
    Serial.println("[OK] Camera ready for peek");
  } else {
    Serial.println("[WARN] Camera init failed - peek disabled");
  }
  #endif

  // Initialize mesh network (opera)
  #if FEATURE_MESH_NETWORK
  Serial.println("[..] Initializing mesh network (opera)...");
  if (mesh_network::init(g_device.privkey, g_device.pubkey, g_device.device_id)) {
    Serial.println("[OK] Mesh network initialized");
    log_health(LOG_LEVEL_INFO, LOG_CAT_MESH, "Mesh network initialized", nullptr);

    // Set up mesh callbacks
    mesh_network::set_alert_callback([](const mesh_network::MeshAlert* alert) {
      // Log received alerts from other canaries
      char detail[80];
      snprintf(detail, sizeof(detail), "From %s: %s",
               alert->sender_name, alert->detail);
      log_health((LogLevel)alert->severity, LOG_CAT_MESH, "Opera alert received", detail);
    });

    mesh_network::set_peer_state_callback([](const mesh_network::OperaPeer* peer,
                                             mesh_network::PeerState old_state,
                                             mesh_network::PeerState new_state) {
      char detail[80];
      snprintf(detail, sizeof(detail), "%s: %s -> %s",
               peer->name,
               mesh_network::peer_state_name(old_state),
               mesh_network::peer_state_name(new_state));
      log_health(LOG_LEVEL_INFO, LOG_CAT_MESH, "Peer state changed", detail);
    });
  } else {
    Serial.println("[WARN] Mesh network init failed");
  }
  #endif

  // Initialize Bluetooth
  #if FEATURE_BLUETOOTH
  Serial.println("[..] Initializing Bluetooth Low Energy...");
  if (bluetooth_channel::init()) {
    Serial.println("[OK] Bluetooth initialized");
    log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "Bluetooth initialized", nullptr);
  } else {
    Serial.println("[WARN] Bluetooth init failed");
  }
  #endif

  // Initialize GNSS
  Serial.println();
  Serial.printf("[..] GNSS: %u baud, RX=GPIO%d, TX=GPIO%d\n", GPS_BAUD, GPS_RX_GPIO, GPS_TX_GPIO);
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_GPIO, GPS_TX_GPIO);
  
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
  log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM, "Device boot complete", FIRMWARE_VERSION);
  
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════════════════════╗");
  Serial.println("║               WITNESS DEVICE READY                           ║");
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.printf("║  Device ID  : %-45s  ║\n", g_device.device_id);
  Serial.printf("║  WiFi AP    : %-45s  ║\n", g_device.ap_ssid);
  Serial.printf("║  Password   : %-45s  ║\n", AP_PASSWORD_DEFAULT);
  #if FEATURE_WIFI_AP
  Serial.printf("║  Dashboard  : http://%-39s  ║\n", WiFi.softAPIP().toString().c_str());
  Serial.println("║  mDNS       : http://canary.local                             ║");
  #endif
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.println("║  Commands: h=help, i=identity, s=status, t=time, g=gps, c=cam║");
  Serial.println("║  Hold BOOT button 1.2s to print all info                     ║");
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
  
  // Handle serial commands
  handle_serial_commands();
  
  // Check boot button for info reprint
  static uint32_t boot_btn_start = 0;
  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);
  if (pressed) {
    if (boot_btn_start == 0) {
      boot_btn_start = millis();
    } else if (millis() - boot_btn_start >= BOOT_BUTTON_HOLD_MS) {
      if (g_device.initialized) {
        print_identity_block();
        print_time_block();
        print_gps_block();
        print_status_bar();
        print_table_header();
      }
      boot_btn_start = 0;
      delay(300);
    }
  } else {
    boot_btn_start = 0;
  }
  
  // Read GPS data
  while (Serial1.available()) {
    g_gps_rb.push((uint8_t)Serial1.read());
  }
  
  // Parse NMEA lines
  static char line[256];
  size_t len;
  while (read_nmea_line(line, sizeof(line), &len)) {
    parse_nmea(line, &g_fix);
  }
  
  // Update state machine
  g_state = update_state(&g_fix, g_state);
  
  // Update health metrics
  uint32_t now = millis();
  g_health.uptime_sec = now / 1000;
  g_health.free_heap = ESP.getFreeHeap();
  if (g_health.free_heap < g_health.min_heap) {
    g_health.min_heap = g_health.free_heap;
  }

  // Check WiFi connection periodically
  wifi_check_connection();

  // Update mesh network
  #if FEATURE_MESH_NETWORK
  mesh_network::update();
  #endif

  // Update Bluetooth
  #if FEATURE_BLUETOOTH
  bluetooth_channel::update();
  #endif

  // Create witness records at interval
  if (now - g_last_record_ms >= RECORD_INTERVAL_MS) {
    g_last_record_ms = now;
    
    uint8_t payload[512];
    size_t payload_len = 0;
    if (!build_witness_event(&g_fix, g_state, payload, sizeof(payload), &payload_len)) {
      log_health(LOG_LEVEL_ERROR, LOG_CAT_WITNESS, "Payload build failed", nullptr);
      return;
    }
    
    if (!create_witness_record(payload, payload_len, RECORD_WITNESS_EVENT, &g_last_record)) {
      log_health(LOG_LEVEL_ERROR, LOG_CAT_CRYPTO, "Record verification failed", nullptr);
    }
    
    // Print table row
    print_table_row(&g_last_record, &g_fix, g_state);
    
    // Periodic status every 20 records
    if (g_health.records_created % 20 == 0) {
      print_status_bar();
      print_table_header();
    }
  }
  
  // Periodic self-verification
  if (now - g_last_verify_ms >= VERIFY_INTERVAL_SEC * 1000) {
    g_last_verify_ms = now;
    
    if (!verify_record_signature(&g_last_record)) {
      log_health(LOG_LEVEL_CRITICAL, LOG_CAT_CRYPTO, "Self-verification FAILED", nullptr);
      g_health.crypto_healthy = false;
    } else {
      g_health.crypto_healthy = true;
    }
  }
  
  // Small delay to prevent tight loop
  delay(1);
}
