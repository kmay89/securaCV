/*
 * SecuraCV Canary — Network Management
 *
 * WiFi AP, mDNS, HTTP server, and OTA receiver.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_NETWORK_H
#define SECURACV_NETWORK_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_http_server.h"
#include "canary_config.h"

#if FEATURE_WIFI_AP || FEATURE_HTTP_SERVER

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

enum WiFiProvState : uint8_t {
  WIFI_PROV_IDLE         = 0,
  WIFI_PROV_SCANNING     = 1,
  WIFI_PROV_CONNECTING   = 2,
  WIFI_PROV_CONNECTED    = 3,
  WIFI_PROV_FAILED       = 4,
  WIFI_PROV_AP_ONLY      = 5
};

struct WiFiCredentials {
  char ssid[33];
  char password[65];
  bool enabled;
  bool configured;
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

// ════════════════════════════════════════════════════════════════════════════
// PEER DISCOVERY
// ════════════════════════════════════════════════════════════════════════════

// Maximum number of peer Canaries we cache from mDNS browse. Sized to fit in
// a single small struct array (no heap allocation). Homes with more than 8
// devices can still discover via the SPA's manual add flow.
#define PEER_CACHE_MAX        8

// How often to re-browse mDNS for _securacv._tcp peers when on home WiFi.
// Coupled to STA connectivity — never browses in AP-only mode.
#define PEER_BROWSE_INTERVAL_MS 30000UL

// Peers older than this are considered stale and dropped from the cache.
#define PEER_STALE_MS         (5UL * 60UL * 1000UL)

struct PeerEntry {
  char     device_id[32];      // From TXT record; falls back to hostname stem.
  char     name[32];           // From TXT record (optional).
  char     mdns_hostname[40];  // e.g. "canary-ab12.local"
  char     ip[16];             // dotted IPv4
  uint32_t last_seen_ms;       // millis() of last successful browse hit.
  bool     valid;
};

// Renamed from NetworkManager: Arduino-ESP32 core 3.x introduced its own
// global `class NetworkManager` (cores/.../libraries/Network), pulled in
// transitively via <WiFi.h> above. On core 3.x the two collide ("redefinition
// of class NetworkManager"); the Scv-prefixed name keeps this project's
// provisioning manager distinct. (Core 2.x had no such class, which is why the
// clash only appears on the core 3.x BLE build envs.)
class ScvNetworkManager {
public:
  ScvNetworkManager();

  // Initialize WiFi provisioning (AP + optional STA).
  //
  // mDNS hostname:
  //   The device always advertises itself as `canary.local` (RFC 6762
  //   conflict resolution handles the rare case of two Canaries on the
  //   same network — the second one auto-renames to canary-2.local,
  //   canary-3.local, etc., and the SPA's _securacv._tcp service browse
  //   still finds them all). Single-device households thus get a stable,
  //   human-typeable URL without needing to know the hex suffix.
  //
  // device_id:
  //   The per-device identifier ("canary-s3-XXXX") advertised in the
  //   _securacv._tcp TXT record. Used by the companion SPA's multi-
  //   device wizard to distinguish individual Canaries during browse.
  //   When null, the TXT record falls back to the mDNS hostname.
  // ap_password is REQUIRED — there is no compile-time default. Pass the
  // device-unique credential (main.cpp derives it from the pubkey
  // fingerprint); a shared fallback was removed so a forgotten password is
  // a compile error rather than a known-password AP.
  bool begin(const char* ap_ssid,
             const char* ap_password,
             const char* device_id = nullptr);

  // Returns the mDNS hostname registered for this device (without ".local").
  // Empty string if mDNS is not active.
  const char* getMdnsHostname() const { return m_mdns_hostname; }

  // Browse the local network for other Canaries advertising _securacv._tcp.
  // Updates the in-memory peer cache. Throttled internally to
  // PEER_BROWSE_INTERVAL_MS; cheap to call every loop iteration. No-op until
  // the STA interface is connected to home WiFi.
  void browsePeers();

  // Const access to the peer cache for HTTP handlers / diagnostics.
  const PeerEntry* getPeers() const { return m_peers; }
  size_t getPeerCount() const;

  // HTTP server
  bool startHttpServer();
  void stopHttpServer();

  // Status
  const WiFiStatus& getStatus() const { return m_status; }
  const WiFiCredentials& getCredentials() const { return m_creds; }
  void setCredentials(const WiFiCredentials& creds) { m_creds = creds; }

  // WiFi provisioning
  bool loadCredentials();
  bool saveCredentials();
  bool clearCredentials();
  void connectToHome();
  void updateStatus();
  void checkConnection();

  // State name
  static const char* stateName(WiFiProvState s);

  // HTTP server handle (for external handlers)
  httpd_handle_t getHttpServer() const { return m_http_server; }

private:
  void registerHttpHandlers();

  // F4 (Wi-Fi/BLE coexistence): tear down the SoftAP once the STA link is
  // healthy so the single 2.4 GHz radio runs STA + BLE (Espressif's stable Y
  // combo) instead of AP + STA + BLE (rated C1/unstable with a client joined),
  // and re-raise it on STA loss so the device stays reachable at canary.local.
  // Both are idempotent. Mirrors the canary-wap sketch's wifi_drop_ap /
  // wifi_raise_ap. See docs/esp32s3_ble_wap_audit.md.
  void dropAp();
  void raiseAp();

  WiFiCredentials m_creds;
  WiFiStatus m_status;
  httpd_handle_t m_http_server;
  bool m_scan_in_progress;
  char m_mdns_hostname[40];      // sanitized hostname registered with mDNS
                                  // (currently constant "canary"; see begin())
  char m_mdns_device_id[40];     // per-device id advertised in _securacv._tcp
                                  // TXT record. Used by browsePeers() to
                                  // self-filter — every device shares the
                                  // same hostname so we MUST compare TXT.
  PeerEntry m_peers[PEER_CACHE_MAX];
  uint32_t m_peers_last_browse_ms;
  char m_ap_ssid[33];            // stashed in begin() so raiseAp() can bring the
  char m_ap_password[65];        // management SoftAP back up after STA loss.
};

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE
// ════════════════════════════════════════════════════════════════════════════

ScvNetworkManager& network_get_instance();

// Convenience functions
bool network_init(const char* ap_ssid,
                  const char* ap_password,  // required — no default; see begin()
                  const char* device_id = nullptr);
bool network_start_http();
void network_update();
httpd_handle_t network_get_http_server();

// HTTP response helpers
esp_err_t http_send_json(httpd_req_t* req, const char* json);
esp_err_t http_send_error(httpd_req_t* req, int status_code, const char* error_code);

// ════════════════════════════════════════════════════════════════════════════
// WIFI POWER MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

// Enable or disable WiFi modem sleep. When enabled, the radio sleeps
// between DTIM beacons (WIFI_PS_MIN_MODEM), saving ~20 mA at the cost
// of ~100 ms extra latency on the first packet after sleep. When
// disabled, the radio stays fully powered (WIFI_PS_NONE).
void network_set_wifi_power_save(bool enable);

// Set the maximum WiFi TX power in quarter-dBm units (e.g. 34 = 8.5 dBm,
// 60 = 15 dBm — Seeed's weak-signal recommendation for the XIAO ESP32S3).
// Clamped internally to the ESP32-S3 valid range (8..84, i.e. 2..21 dBm).
void network_set_tx_power(int8_t quarter_dbm);

// Returns true when the STA interface has an IP from the home WiFi router.
bool network_is_sta_connected(void);

// ════════════════════════════════════════════════════════════════════════════
// RATE LIMITING
// ════════════════════════════════════════════════════════════════════════════

struct RateLimitState {
  uint32_t window_start_ms;
  uint16_t request_count;
  uint16_t action_count;
};

// Check rate limit. Returns true if request is allowed, false if rate-limited.
// Sends 429 response automatically if rate-limited.
bool rate_limit_check(httpd_req_t* req, bool is_action = false);

#endif // FEATURE_WIFI_AP || FEATURE_HTTP_SERVER

#endif // SECURACV_NETWORK_H
