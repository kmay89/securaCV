/*
 * SecuraCV Canary — Network Management Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_network.h"
#include "securacv_witness.h"
#include "securacv_crypto.h"
#include "securacv_auth.h"

#if FEATURE_WIFI_AP || FEATURE_HTTP_SERVER

#include <ArduinoJson.h>

#if FEATURE_SD_STORAGE
#include "securacv_storage.h"
#endif

#if FEATURE_WATCHDOG
#include "esp_task_wdt.h"
#endif

#if FEATURE_CAMERA_PEEK
#include "securacv_camera.h"
#endif

#if FEATURE_OTA_UPDATE
#include <Update.h>
#endif

#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
#include "securacv_sensing.h"
#endif
#if FEATURE_CSI
#include "securacv_csi.h"
#endif
#if FEATURE_ACOUSTIC_EVENTS
#include "securacv_audio.h"
#endif
#if FEATURE_TOUCH
#include "securacv_touch.h"
#endif
#if FEATURE_IR_RMT
#include "securacv_ir.h"
#endif
#if FEATURE_TEMP_TAMPER
#include "securacv_envsens.h"
#endif
/* Lowpower HAL is always compiled when any sensing is on, so the
 * Sensing endpoint can surface the wake reason and capability bits. */
#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
#include "securacv_lowpower.h"
#endif

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE
// ════════════════════════════════════════════════════════════════════════════

static NetworkManager s_network;

NetworkManager& network_get_instance() {
  return s_network;
}

// ════════════════════════════════════════════════════════════════════════════
// HTTP RESPONSE HELPERS
// ════════════════════════════════════════════════════════════════════════════

esp_err_t http_send_json(httpd_req_t* req, const char* json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t http_send_error(httpd_req_t* req, int status_code, const char* error_code) {
  httpd_resp_set_status(req, status_code == 400 ? "400 Bad Request" :
                              status_code == 404 ? "404 Not Found" :
                              status_code == 500 ? "500 Internal Server Error" : "400 Bad Request");
  char response[128];
  snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", error_code);
  return http_send_json(req, response);
}

// ════════════════════════════════════════════════════════════════════════════
// NETWORK MANAGER IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

NetworkManager::NetworkManager()
  : m_http_server(nullptr),
    m_scan_in_progress(false),
    m_peers_last_browse_ms(0) {
  memset(&m_creds, 0, sizeof(m_creds));
  memset(&m_status, 0, sizeof(m_status));
  memset(m_peers, 0, sizeof(m_peers));
  m_mdns_hostname[0] = '\0';
}

// mDNS hostname rules (RFC 6762 §16): only [a-z0-9-], must not start/end with
// hyphen. We lowercase the device_id and replace any other byte with '-'.
static void sanitize_mdns_hostname(const char* in, char* out, size_t cap) {
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
  // Trim leading/trailing hyphens
  while (j > 0 && out[j-1] == '-') j--;
  size_t start = 0;
  while (start < j && out[start] == '-') start++;
  if (start > 0) {
    memmove(out, out + start, j - start);
    j -= start;
  }
  if (j == 0) {
    // Fallback to a safe default rather than emitting an empty name.
    const char* fb = "canary";
    size_t fb_len = strlen(fb);
    if (fb_len >= cap) fb_len = cap - 1;
    memcpy(out, fb, fb_len);
    j = fb_len;
  }
  out[j] = '\0';
}

const char* NetworkManager::stateName(WiFiProvState s) {
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

bool NetworkManager::begin(const char* ap_ssid, const char* ap_password,
                           const char* mdns_hostname) {
  // Load saved credentials
  bool has_creds = loadCredentials();

  // Always use AP+STA mode
  WiFi.mode(WIFI_AP_STA);

  // Start Access Point
  bool ap_ok = WiFi.softAP(ap_ssid, ap_password, AP_CHANNEL, false, AP_MAX_CONNECTIONS);

  if (!ap_ok) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_NETWORK, "WiFi AP start failed", nullptr);
    return false;
  }

  m_status.ap_active = true;
  witness_get_health().wifi_active = true;

  IPAddress ip = WiFi.softAPIP();
  snprintf(m_status.ap_ip, sizeof(m_status.ap_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  char msg[64];
  snprintf(msg, sizeof(msg), "AP: %s", ap_ssid);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, msg, m_status.ap_ip);

  // Start mDNS with a per-device hostname so multiple Canaries on the
  // same home network do not collide on `canary.local`. Falls back to
  // "canary" if no identity is supplied (legacy single-device path).
  sanitize_mdns_hostname(mdns_hostname ? mdns_hostname : "canary",
                         m_mdns_hostname, sizeof(m_mdns_hostname));
  if (MDNS.begin(m_mdns_hostname)) {
    MDNS.addService("http", "tcp", 80);

    // Advertise the SecuraCV-specific service so peer Canaries (and the
    // companion SPA) can browse the network without subnet scanning.
    // TXT records mirror the discovery protocol in
    // canary-vision/docs/discovery.md.
    MDNS.addService("securacv", "tcp", 80);
    MDNS.addServiceTxt("securacv", "tcp", "device_id",
                       mdns_hostname ? mdns_hostname : m_mdns_hostname);
    MDNS.addServiceTxt("securacv", "tcp", "fw", FIRMWARE_VERSION);
    MDNS.addServiceTxt("securacv", "tcp", "model", "XIAO ESP32S3");

    char fqdn[64];
    snprintf(fqdn, sizeof(fqdn), "%s.local", m_mdns_hostname);
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "mDNS started", fqdn);
  }

  // Attempt to connect to home WiFi if configured
  if (has_creds && m_creds.enabled) {
    connectToHome();
  } else {
    m_status.state = WIFI_PROV_AP_ONLY;
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "AP-only mode", "No home WiFi configured");
  }

  return true;
}

bool NetworkManager::loadCredentials() {
  memset(&m_creds, 0, sizeof(m_creds));

  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;

  size_t ssid_len = nvs.getBytesLength(NVS_KEY_WIFI_SSID);
  if (ssid_len > 0 && ssid_len <= 32) {
    nvs.getBytes(NVS_KEY_WIFI_SSID, m_creds.ssid, ssid_len);
    m_creds.ssid[ssid_len] = '\0';

    size_t pass_len = nvs.getBytesLength(NVS_KEY_WIFI_PASS);
    if (pass_len > 0 && pass_len <= 64) {
      nvs.getBytes(NVS_KEY_WIFI_PASS, m_creds.password, pass_len);
      m_creds.password[pass_len] = '\0';
    }

    m_creds.enabled = nvs.getBool(NVS_KEY_WIFI_EN, true);
    m_creds.configured = (strlen(m_creds.ssid) > 0);
  }

  nvs.end();
  return m_creds.configured;
}

bool NetworkManager::saveCredentials() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;

  nvs.putBytes(NVS_KEY_WIFI_SSID, m_creds.ssid, strlen(m_creds.ssid));
  nvs.putBytes(NVS_KEY_WIFI_PASS, m_creds.password, strlen(m_creds.password));
  nvs.putBool(NVS_KEY_WIFI_EN, m_creds.enabled);

  nvs.end();
  m_creds.configured = true;

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "WiFi credentials saved", m_creds.ssid);
  return true;
}

bool NetworkManager::clearCredentials() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;

  nvs.remove(NVS_KEY_WIFI_SSID);
  nvs.remove(NVS_KEY_WIFI_PASS);
  nvs.remove(NVS_KEY_WIFI_EN);

  nvs.end();

  memset(&m_creds, 0, sizeof(m_creds));
  m_status.state = WIFI_PROV_AP_ONLY;

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "WiFi credentials cleared", nullptr);
  return true;
}

void NetworkManager::connectToHome() {
  if (!m_creds.configured || !m_creds.enabled) {
    m_status.state = WIFI_PROV_AP_ONLY;
    return;
  }

  if (strlen(m_creds.ssid) == 0) {
    m_status.state = WIFI_PROV_AP_ONLY;
    return;
  }

  m_status.state = WIFI_PROV_CONNECTING;
  m_status.connect_attempts++;
  m_status.last_connect_ms = millis();

  char msg[64];
  snprintf(msg, sizeof(msg), "Connecting to: %s", m_creds.ssid);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, msg, nullptr);

  WiFi.begin(m_creds.ssid, m_creds.password);
}

void NetworkManager::updateStatus() {
  m_status.ap_active = (WiFi.getMode() & WIFI_AP) != 0;
  m_status.sta_connected = WiFi.isConnected();
  m_status.ap_clients = WiFi.softAPgetStationNum();

  if (m_status.sta_connected) {
    m_status.rssi = WiFi.RSSI();
    IPAddress ip = WiFi.localIP();
    snprintf(m_status.sta_ip, sizeof(m_status.sta_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  } else {
    m_status.rssi = 0;
    m_status.sta_ip[0] = '\0';
  }

  IPAddress apip = WiFi.softAPIP();
  snprintf(m_status.ap_ip, sizeof(m_status.ap_ip), "%d.%d.%d.%d", apip[0], apip[1], apip[2], apip[3]);
}

size_t NetworkManager::getPeerCount() const {
  size_t n = 0;
  for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
    if (m_peers[i].valid) n++;
  }
  return n;
}

// Insert or refresh a peer in the cache. Slot is found by device_id match,
// then by mdns_hostname, then by the first invalid slot, then by oldest
// last_seen — guarantees the most recently seen PEER_CACHE_MAX peers stay.
static void peer_upsert(PeerEntry* peers,
                        const char* device_id,
                        const char* name,
                        const char* mdns_hostname,
                        const char* ip) {
  if (!device_id || !device_id[0]) return;

  int slot = -1;
  for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
    if (peers[i].valid && strncmp(peers[i].device_id, device_id,
                                  sizeof(peers[i].device_id)) == 0) {
      slot = (int)i; break;
    }
  }
  if (slot < 0 && mdns_hostname) {
    for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
      if (peers[i].valid && strncmp(peers[i].mdns_hostname, mdns_hostname,
                                    sizeof(peers[i].mdns_hostname)) == 0) {
        slot = (int)i; break;
      }
    }
  }
  if (slot < 0) {
    for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
      if (!peers[i].valid) { slot = (int)i; break; }
    }
  }
  if (slot < 0) {
    uint32_t oldest = UINT32_MAX;
    for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
      if (peers[i].last_seen_ms < oldest) {
        oldest = peers[i].last_seen_ms; slot = (int)i;
      }
    }
  }
  if (slot < 0) return;

  PeerEntry& p = peers[slot];
  strncpy(p.device_id, device_id, sizeof(p.device_id) - 1);
  p.device_id[sizeof(p.device_id) - 1] = '\0';
  if (name) {
    strncpy(p.name, name, sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = '\0';
  } else if (!p.valid) {
    p.name[0] = '\0';
  }
  if (mdns_hostname) {
    strncpy(p.mdns_hostname, mdns_hostname, sizeof(p.mdns_hostname) - 1);
    p.mdns_hostname[sizeof(p.mdns_hostname) - 1] = '\0';
  }
  if (ip) {
    strncpy(p.ip, ip, sizeof(p.ip) - 1);
    p.ip[sizeof(p.ip) - 1] = '\0';
  }
  p.last_seen_ms = millis();
  p.valid = true;
}

void NetworkManager::browsePeers() {
  // Only browse when on home WiFi; in AP-only mode there's no LAN to browse.
  if (!m_status.sta_connected) return;
  if (m_mdns_hostname[0] == '\0') return;

  uint32_t now = millis();
  if (m_peers_last_browse_ms != 0 &&
      (now - m_peers_last_browse_ms) < PEER_BROWSE_INTERVAL_MS) {
    // Even when not browsing, prune stale entries so the cache reflects
    // peers that have actually disappeared.
    for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
      if (m_peers[i].valid && (now - m_peers[i].last_seen_ms) > PEER_STALE_MS) {
        m_peers[i].valid = false;
      }
    }
    return;
  }
  m_peers_last_browse_ms = now;

  // queryService blocks for up to ~1s waiting for responses. That's fine
  // here because we only call it on the 30s cadence above.
  int n = MDNS.queryService("securacv", "tcp");
  for (int i = 0; i < n; i++) {
    String host = MDNS.hostname(i);
    if (host.length() == 0) continue;

    // Filter ourselves out — we know our own hostname.
    String me(m_mdns_hostname);
    if (host.equalsIgnoreCase(me)) continue;

    IPAddress ip = MDNS.IP(i);
    char ip_str[16] = {0};
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

    char fqdn[40] = {0};
    snprintf(fqdn, sizeof(fqdn), "%s.local", host.c_str());

    // TXT records are advertised by addServiceTxt() in begin(); read them
    // back. ESPmDNS returns empty string when a key is absent.
    String tx_id   = MDNS.txt(i, "device_id");
    String tx_name = MDNS.txt(i, "name");

    const char* device_id = tx_id.length() > 0 ? tx_id.c_str() : host.c_str();
    const char* name      = tx_name.length() > 0 ? tx_name.c_str() : nullptr;

    peer_upsert(m_peers, device_id, name, fqdn, ip_str);
  }

  // Drop entries we didn't refresh and that have aged past the stale window.
  for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
    if (m_peers[i].valid && (now - m_peers[i].last_seen_ms) > PEER_STALE_MS) {
      m_peers[i].valid = false;
    }
  }
}

void NetworkManager::checkConnection() {
  uint32_t now = millis();
  updateStatus();

  switch (m_status.state) {
    case WIFI_PROV_CONNECTING:
      if (WiFi.isConnected()) {
        m_status.state = WIFI_PROV_CONNECTED;
        m_status.connected_since_ms = now;

        char msg[80];
        snprintf(msg, sizeof(msg), "Connected to %s", m_creds.ssid);
        log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, msg, m_status.sta_ip);
      } else if (now - m_status.last_connect_ms > WIFI_CONNECT_TIMEOUT_MS) {
        m_status.state = WIFI_PROV_FAILED;
        log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "WiFi connection timeout", m_creds.ssid);
      }
      break;

    case WIFI_PROV_CONNECTED:
      if (!WiFi.isConnected()) {
        m_status.state = WIFI_PROV_FAILED;
        log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "WiFi connection lost", nullptr);
      }
      break;

    case WIFI_PROV_FAILED:
      if (m_creds.configured && m_creds.enabled &&
          now - m_status.last_connect_ms > WIFI_RECONNECT_INTERVAL_MS) {
        connectToHome();
      }
      break;

    default:
      break;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// RATE LIMITING
// ════════════════════════════════════════════════════════════════════════════

static RateLimitState s_rate_limit = {0, 0, 0};

bool rate_limit_check(httpd_req_t* req, bool is_action) {
  uint32_t now = millis();

  // Reset window if expired
  if (now - s_rate_limit.window_start_ms >= RATE_LIMIT_WINDOW_MS) {
    s_rate_limit.window_start_ms = now;
    s_rate_limit.request_count = 0;
    s_rate_limit.action_count = 0;
  }

  s_rate_limit.request_count++;
  if (is_action) {
    s_rate_limit.action_count++;
  }

  bool limited = (s_rate_limit.request_count > RATE_LIMIT_MAX_REQUESTS) ||
                 (is_action && s_rate_limit.action_count > RATE_LIMIT_MAX_ACTIONS);

  if (limited) {
    uint32_t remaining_ms = RATE_LIMIT_WINDOW_MS - (now - s_rate_limit.window_start_ms);
    uint32_t retry_after = (remaining_ms / 1000) + 1;

    char retry_str[8];
    snprintf(retry_str, sizeof(retry_str), "%lu", (unsigned long)retry_after);

    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Retry-After", retry_str);
    httpd_resp_sendstr(req, "{\"error\":\"rate_limited\"}");

    witness_get_health().http_errors++;
    return false;
  }

  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// AUTH GATE
// ════════════════════════════════════════════════════════════════════════════
//
// Wraps auth_check() against the device-provisioned API bearer token.
// On failure the AuthManager has already written the 401/403/429 response,
// so the handler can return ESP_OK directly after this returns false.
// Tracks http_errors so the status endpoint surfaces rejected calls.

static bool auth_gate(httpd_req_t* req) {
  const char* token = auth_get_token();
  if (!token || token[0] == '\0') {
    // Fail closed: if the bearer credential isn't provisioned, refuse.
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"not_provisioned\"}");
    witness_get_health().http_errors++;
    return false;
  }
  if (!auth_check(req, token)) {
    witness_get_health().http_errors++;
    return false;
  }
  return true;
}

// Forward declarations for HTTP handlers
static esp_err_t handle_ui(httpd_req_t* req);
static esp_err_t handle_status(httpd_req_t* req);
static esp_err_t handle_chain(httpd_req_t* req);
static esp_err_t handle_logs(httpd_req_t* req);
static esp_err_t handle_log_ack(httpd_req_t* req);
static esp_err_t handle_ack_all(httpd_req_t* req);
static esp_err_t handle_reboot(httpd_req_t* req);
static esp_err_t handle_export(httpd_req_t* req);

// WiFi API endpoints (for MQTT STA configuration)
static esp_err_t handle_wifi_status(httpd_req_t* req);
static esp_err_t handle_wifi_scan(httpd_req_t* req);
static esp_err_t handle_wifi_connect(httpd_req_t* req);
static esp_err_t handle_wifi_disconnect(httpd_req_t* req);

// Peer discovery
static esp_err_t handle_peers(httpd_req_t* req);

#if FEATURE_HA_MQTT
static esp_err_t handle_mqtt_status(httpd_req_t* req);
static esp_err_t handle_mqtt_config(httpd_req_t* req);
#endif

#if FEATURE_OTA_UPDATE
static esp_err_t handle_ota(httpd_req_t* req);
#endif

#if FEATURE_CAMERA_PEEK
static esp_err_t handle_peek_start(httpd_req_t* req);
static esp_err_t handle_peek_stream(httpd_req_t* req);
static esp_err_t handle_peek_stop(httpd_req_t* req);
static esp_err_t handle_peek_status(httpd_req_t* req);
#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
static esp_err_t handle_sensing(httpd_req_t* req);
#endif
#endif

#if FEATURE_ACOUSTIC_EVENTS
// Microphone testability + privacy controls (see docs/getting_started_canary.md).
static esp_err_t handle_audio_level(httpd_req_t* req);
static esp_err_t handle_audio_mute(httpd_req_t* req);
static esp_err_t handle_audio_test_start(httpd_req_t* req);
static esp_err_t handle_audio_test_status(httpd_req_t* req);
#endif

bool NetworkManager::startHttpServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 8192;
  config.max_uri_handlers = 29;  /* +1 sensing; +4 for the audio test endpoints */

  if (httpd_start(&m_http_server, &config) != ESP_OK) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_NETWORK, "HTTP server start failed", nullptr);
    return false;
  }

  registerHttpHandlers();
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "HTTP server started", "port 80");
  return true;
}

void NetworkManager::stopHttpServer() {
  if (m_http_server) {
    httpd_stop(m_http_server);
    m_http_server = nullptr;
  }
}

void NetworkManager::registerHttpHandlers() {
  // UI
  httpd_uri_t ui = { .uri = "/", .method = HTTP_GET, .handler = handle_ui };
  httpd_register_uri_handler(m_http_server, &ui);

  // API endpoints
  httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status };
  httpd_register_uri_handler(m_http_server, &status);

  httpd_uri_t chain = { .uri = "/api/chain", .method = HTTP_GET, .handler = handle_chain };
  httpd_register_uri_handler(m_http_server, &chain);

  httpd_uri_t logs = { .uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs };
  httpd_register_uri_handler(m_http_server, &logs);

  httpd_uri_t log_ack = { .uri = "/api/logs/*/ack", .method = HTTP_POST, .handler = handle_log_ack };
  httpd_register_uri_handler(m_http_server, &log_ack);

  httpd_uri_t ack_all = { .uri = "/api/logs/ack-all", .method = HTTP_POST, .handler = handle_ack_all };
  httpd_register_uri_handler(m_http_server, &ack_all);

  httpd_uri_t reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = handle_reboot };
  httpd_register_uri_handler(m_http_server, &reboot);

  httpd_uri_t export_ep = { .uri = "/api/export", .method = HTTP_POST, .handler = handle_export };
  httpd_register_uri_handler(m_http_server, &export_ep);

  // WiFi management endpoints
  httpd_uri_t wifi_status = { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = handle_wifi_status };
  httpd_register_uri_handler(m_http_server, &wifi_status);

  httpd_uri_t wifi_scan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan };
  httpd_register_uri_handler(m_http_server, &wifi_scan);

  httpd_uri_t wifi_connect = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = handle_wifi_connect };
  httpd_register_uri_handler(m_http_server, &wifi_connect);

  httpd_uri_t wifi_disconnect = { .uri = "/api/wifi/disconnect", .method = HTTP_POST, .handler = handle_wifi_disconnect };
  httpd_register_uri_handler(m_http_server, &wifi_disconnect);

  // Peer list (mDNS browse cache). Path matches canary-vision/docs/discovery.md
  // and the SPA's CanaryAPI.request(... '/api/v1/peers').
  httpd_uri_t peers_ep = { .uri = "/api/v1/peers", .method = HTTP_GET, .handler = handle_peers };
  httpd_register_uri_handler(m_http_server, &peers_ep);

  #if FEATURE_HA_MQTT
  httpd_uri_t mqtt_stat = { .uri = "/api/mqtt/status", .method = HTTP_GET, .handler = handle_mqtt_status };
  httpd_register_uri_handler(m_http_server, &mqtt_stat);

  httpd_uri_t mqtt_cfg = { .uri = "/api/mqtt/config", .method = HTTP_POST, .handler = handle_mqtt_config };
  httpd_register_uri_handler(m_http_server, &mqtt_cfg);
  #endif

  #if FEATURE_OTA_UPDATE
  httpd_uri_t ota = { .uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota };
  httpd_register_uri_handler(m_http_server, &ota);
  #endif

  #if FEATURE_CAMERA_PEEK
  httpd_uri_t peek_start = { .uri = "/api/peek/start", .method = HTTP_POST, .handler = handle_peek_start };
  httpd_register_uri_handler(m_http_server, &peek_start);

  httpd_uri_t peek_stream = { .uri = "/api/peek/stream", .method = HTTP_GET, .handler = handle_peek_stream };
  httpd_register_uri_handler(m_http_server, &peek_stream);

  httpd_uri_t peek_stop = { .uri = "/api/peek/stop", .method = HTTP_POST, .handler = handle_peek_stop };
  httpd_register_uri_handler(m_http_server, &peek_stop);

  httpd_uri_t peek_status = { .uri = "/api/peek/status", .method = HTTP_GET, .handler = handle_peek_status };
  httpd_register_uri_handler(m_http_server, &peek_status);
  #endif

  #if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
  httpd_uri_t sensing_ep = { .uri = "/api/sensing", .method = HTTP_GET, .handler = handle_sensing };
  httpd_register_uri_handler(m_http_server, &sensing_ep);
  #endif

  #if FEATURE_ACOUSTIC_EVENTS
  // Live RMS for the UI level meter — same number the hysteresis uses,
  // not a second audio path. Returns 0 when muted.
  httpd_uri_t audio_level = { .uri = "/api/audio/level", .method = HTTP_GET, .handler = handle_audio_level };
  httpd_register_uri_handler(m_http_server, &audio_level);

  // Hard mute (physically uninstalls the I2S driver) — persisted in NVS.
  httpd_uri_t audio_mute_ep = { .uri = "/api/audio/mute", .method = HTTP_POST, .handler = handle_audio_mute };
  httpd_register_uri_handler(m_http_server, &audio_mute_ep);

  // Alarm-pattern self-test (relaxed thresholds, normal event callback
  // suppressed so a TEST-button press does NOT flow into HA automations).
  httpd_uri_t audio_test_start = { .uri = "/api/audio/test/start", .method = HTTP_POST, .handler = handle_audio_test_start };
  httpd_register_uri_handler(m_http_server, &audio_test_start);
  httpd_uri_t audio_test_status = { .uri = "/api/audio/test/status", .method = HTTP_GET, .handler = handle_audio_test_status };
  httpd_register_uri_handler(m_http_server, &audio_test_status);
  #endif
}

// ════════════════════════════════════════════════════════════════════════════
// HTTP HANDLERS
// ════════════════════════════════════════════════════════════════════════════

// Include triggers PlatformIO LDF to build+link the securacv_webui library
#include "securacv_webui.h"

static esp_err_t handle_ui(httpd_req_t* req) {
  witness_get_health().http_requests++;
  httpd_resp_set_type(req, "text/html");

  // Phase 2.5: inject the bearer credential into the embedded SPA so its
  // fetch() helper can send `Authorization: Bearer cv_…`. The SPA is tens
  // of KB and ESP32 heap fragments fast, so we stream prefix/token/suffix
  // as three chunks rather than allocating a rendered copy.
  static const char kTokenPlaceholder[] = "__CV_TOKEN__";
  const size_t placeholder_len = sizeof(kTokenPlaceholder) - 1;

  const char* needle = strstr(CANARY_UI_HTML, kTokenPlaceholder);
  if (!needle) {
    return httpd_resp_send(req, CANARY_UI_HTML, HTTPD_RESP_USE_STRLEN);
  }

  const char* token = auth_get_token();
  if (!token) token = "";
  const size_t token_len = strlen(token);
  const size_t prefix_len = needle - CANARY_UI_HTML;

  esp_err_t result = httpd_resp_send_chunk(req, CANARY_UI_HTML, prefix_len);
  if (result != ESP_OK) return result;

  if (token_len > 0) {
    result = httpd_resp_send_chunk(req, token, token_len);
    if (result != ESP_OK) return result;
  }

  result = httpd_resp_send_chunk(req, needle + placeholder_len, HTTPD_RESP_USE_STRLEN);
  if (result != ESP_OK) return result;

  return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t handle_status(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  DeviceIdentity& device = witness_get_device();
  SystemHealth& health = witness_get_health();

  JsonDocument doc;
  doc["ok"] = true;
  doc["device_id"] = device.device_id;
  doc["device_type"] = DEVICE_TYPE;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["ruleset"] = RULESET_ID;

  char fp_hex[17];
  hex_to_str(fp_hex, device.pubkey_fp, 8);
  doc["fingerprint"] = fp_hex;

  char pubkey_hex[65];
  hex_to_str(pubkey_hex, device.pubkey, 32);
  doc["pubkey"] = pubkey_hex;

  doc["uptime_sec"] = uptime_seconds();
  doc["boot_count"] = device.boot_count;
  doc["chain_seq"] = device.seq;
  doc["witness_count"] = health.records_created;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["min_heap"] = health.min_heap;

  doc["crypto_healthy"] = health.crypto_healthy;
  doc["gps_healthy"] = health.gps_healthy;
  doc["sd_healthy"] = health.sd_healthy;
  doc["wifi_active"] = health.wifi_active;

  doc["logs_stored"] = health.logs_stored;
  doc["unacked_count"] = health.logs_unacked;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// GET /api/v1/peers
// Returns the cached list of other Canaries this device has discovered via
// mDNS (_securacv._tcp). The cache is populated by NetworkManager::browsePeers
// on a slow cadence; this handler is read-only and never blocks on the
// network. Response shape matches canary-vision/docs/discovery.md.
static esp_err_t handle_peers(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  // Refresh opportunistically — internally throttled so this is cheap if
  // we already browsed within the interval.
  network_get_instance().browsePeers();

  const PeerEntry* peers = network_get_instance().getPeers();

  JsonDocument doc;
  doc["ok"] = true;
  JsonArray arr = doc["peers"].to<JsonArray>();
  uint32_t now = millis();
  for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
    if (!peers[i].valid) continue;
    JsonObject p = arr.add<JsonObject>();
    p["device_id"]      = peers[i].device_id;
    if (peers[i].name[0]) p["name"] = peers[i].name;
    if (peers[i].ip[0])   p["ip"]   = peers[i].ip;
    if (peers[i].mdns_hostname[0]) p["mdns_hostname"] = peers[i].mdns_hostname;
    p["last_seen_ms_ago"] = (uint32_t)(now - peers[i].last_seen_ms);
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_chain(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  DeviceIdentity& device = witness_get_device();
  WitnessRecord& last = witness_get_last_record();

  JsonDocument doc;
  doc["ok"] = true;

  char chain_hex[65];
  hex_to_str(chain_hex, device.chain_head, 32);
  doc["chain_head"] = chain_hex;
  doc["sequence"] = device.seq;

  if (last.seq > 0) {
    JsonArray blocks = doc["blocks"].to<JsonArray>();
    JsonObject block = blocks.add<JsonObject>();
    char hash[65];
    hex_to_str(hash, last.chain_hash, 32);
    block["seq"] = last.seq;
    block["hash"] = hash;
    block["type"] = record_type_name(last.type);
    block["verified"] = last.verified;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_logs(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  HealthLogRingEntry* ring = witness_get_health_log_ring();
  size_t count = witness_get_health_log_count();
  size_t head = witness_get_health_log_head();

  JsonDocument doc;
  doc["ok"] = true;
  doc["total"] = count;

  JsonArray logs = doc["logs"].to<JsonArray>();

  for (size_t i = 0; i < count; i++) {
    size_t idx = (head + 100 - 1 - i) % 100;
    HealthLogRingEntry& entry = ring[idx];

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
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  const char* uri = req->uri;
  const char* seq_start = strstr(uri, "/logs/");
  if (!seq_start) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
  }
  seq_start += 6;
  uint32_t seq = atoi(seq_start);

  bool success = acknowledge_log_entry(seq, ACK_STATUS_ACKNOWLEDGED, "");

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
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  HealthLogRingEntry* ring = witness_get_health_log_ring();
  size_t count = witness_get_health_log_count();

  uint32_t acked = 0;
  for (size_t i = 0; i < count; i++) {
    if (ring[i].ack_status == ACK_STATUS_UNREAD) {
      ring[i].ack_status = ACK_STATUS_ACKNOWLEDGED;
      acked++;
    }
  }
  witness_get_health().logs_unacked = 0;

  log_health(LOG_LEVEL_INFO, LOG_CAT_USER, "Bulk acknowledgment", nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["acknowledged"] = acked;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_reboot(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  log_health(LOG_LEVEL_NOTICE, LOG_CAT_USER, "Reboot requested", nullptr);

  DeviceIdentity& device = witness_get_device();
  nvs_store_u32(NVS_KEY_SEQ, device.seq);
  nvs_store_bytes(NVS_KEY_CHAIN, device.chain_head, 32);

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

#if FEATURE_OTA_UPDATE
static esp_err_t handle_ota(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  if (req->content_len <= 0 || req->content_len > 2 * 1024 * 1024) {
    return http_send_error(req, 400, "invalid_size");
  }

  if (!Update.begin(req->content_len)) {
    return http_send_error(req, 500, "ota_begin_failed");
  }

  char buf[4096];
  int remaining = req->content_len;
  while (remaining > 0) {
    int recv_len = httpd_req_recv(req, buf, (remaining < (int)sizeof(buf)) ? remaining : sizeof(buf));
    if (recv_len <= 0) {
      Update.abort();
      return http_send_error(req, 500, "receive_failed");
    }
    if (Update.write((uint8_t*)buf, recv_len) != recv_len) {
      Update.abort();
      return http_send_error(req, 500, "write_failed");
    }
    remaining -= recv_len;
  }

  if (Update.end(true)) {
    const char* resp = "{\"ok\":true,\"message\":\"Rebooting...\"}";
    http_send_json(req, resp);
    delay(500);
    ESP.restart();
    return ESP_OK;
  } else {
    return http_send_error(req, 500, "ota_end_failed");
  }
}
#endif

#if FEATURE_CAMERA_PEEK
static esp_err_t handle_peek_start(httpd_req_t* req) {
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  if (!camera_is_initialized()) {
    return http_send_error(req, 503, "camera_not_initialized");
  }

  camera_set_peek_active(true);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek started", nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Peek stream activated";
  doc["resolution"] = camera_get_instance().getResolutionName();

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_peek_stream(httpd_req_t* req) {
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  CameraManager& cam = camera_get_instance();
  if (!cam.isInitialized()) {
    return httpd_resp_send(req, "Camera not initialized", HTTPD_RESP_USE_STRLEN);
  }

  cam.setPeekActive(true);

  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (cam.isPeekActive()) {
    camera_fb_t* fb = cam.captureFrame();
    if (!fb) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    char part_buf[128];
    int part_len = snprintf(part_buf, sizeof(part_buf),
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      (unsigned)fb->len);

    esp_err_t res = httpd_resp_send_chunk(req, part_buf, part_len);
    if (res != ESP_OK) {
      cam.returnFrame(fb);
      break;
    }

    res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    if (res != ESP_OK) {
      cam.returnFrame(fb);
      break;
    }

    res = httpd_resp_send_chunk(req, "\r\n", 2);
    cam.returnFrame(fb);

    if (res != ESP_OK) break;

    #if FEATURE_WATCHDOG
    esp_task_wdt_reset();
    #endif
    vTaskDelay(pdMS_TO_TICKS(80));
  }

  cam.setPeekActive(false);
  httpd_resp_send_chunk(req, NULL, 0);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stream ended", nullptr);

  return ESP_OK;
}

static esp_err_t handle_peek_stop(httpd_req_t* req) {
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  camera_set_peek_active(false);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stopped", nullptr);

  return http_send_json(req, "{\"ok\":true,\"message\":\"Peek stopped\"}");
}

static esp_err_t handle_peek_status(httpd_req_t* req) {
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  CameraManager& cam = camera_get_instance();

  JsonDocument doc;
  doc["ok"] = true;
  doc["camera_initialized"] = cam.isInitialized();
  doc["peek_active"] = cam.isPeekActive();
  doc["resolution"] = (int)cam.getResolution();
  doc["resolution_name"] = cam.getResolutionName();

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}
#endif

// ════════════════════════════════════════════════════════════════════════════
// EXPORT ENDPOINT
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_export(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  DeviceIdentity& device = witness_get_device();
  SystemHealth& health = witness_get_health();
  WitnessRecord& last = witness_get_last_record();

  JsonDocument doc;
  doc["ok"] = true;
  doc["version"] = PROTOCOL_VERSION;
  doc["device_id"] = device.device_id;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["ruleset"] = RULESET_ID;
  doc["export_time_ms"] = millis();
  doc["chain_seq"] = device.seq;
  doc["records_total"] = health.records_created;

  char pubkey_hex[65];
  hex_to_str(pubkey_hex, device.pubkey, 32);
  doc["pubkey"] = pubkey_hex;

  char chain_hex[65];
  hex_to_str(chain_hex, device.chain_head, 32);
  doc["chain_head"] = chain_hex;

  if (last.seq > 0) {
    JsonObject last_rec = doc["last_record"].to<JsonObject>();
    last_rec["seq"] = last.seq;
    char hash[65];
    hex_to_str(hash, last.chain_hash, 32);
    last_rec["hash"] = hash;
    last_rec["type"] = record_type_name(last.type);
    last_rec["verified"] = last.verified;
  }

  doc["sd_available"] = health.sd_healthy;

  log_health(LOG_LEVEL_INFO, LOG_CAT_USER, "Export bundle created", nullptr);

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// WIFI API ENDPOINTS
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_wifi_status(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  NetworkManager& net = network_get_instance();
  const WiFiStatus& status = net.getStatus();

  JsonDocument doc;
  doc["ok"] = true;
  doc["state"] = NetworkManager::stateName(status.state);
  doc["ap_active"] = status.ap_active;
  doc["sta_connected"] = status.sta_connected;
  doc["ap_ip"] = status.ap_ip;
  if (status.sta_connected) {
    doc["sta_ip"] = status.sta_ip;
    doc["rssi"] = status.rssi;
  }
  doc["ap_clients"] = status.ap_clients;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_scan(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  witness_get_health().http_requests++;

  int n = WiFi.scanNetworks(false, false, false, 300);

  JsonDocument doc;
  doc["ok"] = true;
  JsonArray networks = doc["networks"].to<JsonArray>();

  for (int i = 0; i < n && i < 20; i++) {
    JsonObject net = networks.add<JsonObject>();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = WiFi.RSSI(i);
    net["channel"] = WiFi.channel(i);
    net["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "wpa";
  }

  WiFi.scanDelete();

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_connect(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  char body[256];
  int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) {
    return http_send_error(req, 400, "empty_body");
  }
  body[recv] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  const char* ssid = input["ssid"];
  const char* password = input["password"];

  if (!ssid || strlen(ssid) == 0) {
    return http_send_error(req, 400, "missing_ssid");
  }

  NetworkManager& net = network_get_instance();
  WiFiCredentials creds;
  memset(&creds, 0, sizeof(creds));
  strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
  if (password) {
    strncpy(creds.password, password, sizeof(creds.password) - 1);
  }
  creds.enabled = true;
  creds.configured = true;

  // Transfer local credentials to the manager, then save and connect
  net.setCredentials(creds);
  net.saveCredentials();
  net.connectToHome();

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Connecting to WiFi...";
  doc["ssid"] = ssid;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_disconnect(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  WiFi.disconnect(false);

  NetworkManager& net = network_get_instance();
  net.clearCredentials();

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Disconnected from home WiFi";

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// MQTT API ENDPOINTS
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_HA_MQTT
#include "securacv_mqtt.h"

static esp_err_t handle_mqtt_status(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  JsonDocument doc;
  doc["ok"] = true;
  doc["connected"] = mqtt_connected();

  MqttCredentials creds;
  if (mqtt_load_credentials(&creds)) {
    doc["enabled"] = creds.enabled;
    doc["host"] = creds.host;
    doc["port"] = creds.port;
    // Do not expose username/password in API response
  } else {
    doc["enabled"] = false;
    doc["configured"] = false;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_mqtt_config(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

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

  MqttCredentials creds;
  memset(&creds, 0, sizeof(creds));

  const char* host = input["host"];
  if (!host || strlen(host) == 0) {
    return http_send_error(req, 400, "missing_host");
  }

  strncpy(creds.host, host, sizeof(creds.host) - 1);
  creds.port = input["port"] | MQTT_PORT;

  const char* user = input["username"];
  if (user) strncpy(creds.username, user, sizeof(creds.username) - 1);

  const char* pass = input["password"];
  if (pass) strncpy(creds.password, pass, sizeof(creds.password) - 1);

  creds.enabled = input["enabled"] | true;
  creds.configured = true;

  if (!mqtt_save_credentials(&creds)) {
    return http_send_error(req, 500, "save_failed");
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "MQTT configuration saved. Reboot to apply.";

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}
#endif // FEATURE_HA_MQTT

// ════════════════════════════════════════════════════════════════════════════
// CSI SENSING ENDPOINT — privacy-safe scalars + bar-graph arrays
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
// GET /api/sensing — returns the live aggregated sensing snapshot for the
// dashboard's Sensing panel. No raw subcarrier samples, no MAC/BSSID, no
// audio samples, no per-frame timestamps. The data exposed here is the
// distilled scalars + small int8 bar-graph arrays that the in-tree
// aggregator (securacv_sensing) builds from CSI feature callbacks and
// audio cadence-detector callbacks. Both are optional at compile time.
static esp_err_t handle_sensing(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  sensing_state_t s;
  sensing_snapshot(&s);

  JsonDocument doc;
  doc["ok"] = true;

#if FEATURE_CSI
  csi_stats_t stats = {0};
  csi_get_stats(&stats);
  doc["enabled"] = csi_is_running();

  // Headline scalars for the Apple-style status card.
  doc["motion"]    = s.motion_score;
  doc["breathing"] = s.breathing_score;
  doc["label"]     = sensing_label_name(s.activity_label);

  doc["rssi_dbm"] = (int)s.rssi_dbm;
  doc["rssi_std"] = (int)s.rssi_std;
  doc["frames_in_window"] = s.frames_in_window;
  doc["dropped_estimate"] = s.dropped_estimate;
  doc["channel"]         = s.channel;
  doc["bandwidth_code"]  = s.bandwidth_code;
  doc["time_bucket"]     = s.time_bucket;
  doc["windows_seen"]    = s.windows_seen;
  doc["last_window_age_ms"] =
      s.last_window_ms == 0 ? -1L : (long)(millis() - s.last_window_ms);

  // Bar-graph arrays — int8 buckets straight from the feature vector.
  JsonArray amp = doc["amp_bands"].to<JsonArray>();
  for (int i = 0; i < 8; i++) amp.add((int)s.amp_bands[i]);
  JsonArray dop = doc["doppler"].to<JsonArray>();
  for (int i = 0; i < 4; i++) dop.add((int)s.doppler[i]);
  JsonArray br = doc["breathing_bins"].to<JsonArray>();
  for (int i = 0; i < 8; i++) br.add((int)s.breathing_bins[i]);

  // Driver-level counters for the diagnostics tile.
  JsonObject st = doc["stats"].to<JsonObject>();
  st["frames_received"]     = stats.frames_received;
  st["frames_dropped_rssi"] = stats.frames_dropped_rssi;
  st["frames_dropped_rate"] = stats.frames_dropped_rate;
  st["frames_dropped_full"] = stats.frames_dropped_full;
  st["windows_emitted"]     = stats.windows_emitted;
  st["windows_degraded"]    = stats.windows_degraded;
#endif // FEATURE_CSI

#if FEATURE_ACOUSTIC_EVENTS
  audio_stats_t a_stats = {0};
  audio_get_stats(&a_stats);

  JsonObject ac = doc["acoustic"].to<JsonObject>();
  ac["enabled"]     = audio_is_running();
  ac["muted"]       = audio_is_muted();
  ac["last_event"]  = audio_event_name(s.last_audio_event_type);
  ac["confidence"]  = s.last_audio_event_conf;
  ac["cycle_count"] = s.last_audio_event_count;
  ac["last_event_age_ms"] =
      s.last_audio_event_ms == 0 ? -1L : (long)(millis() - s.last_audio_event_ms);

  /* Last applied mute toggle — lets the dashboard show "Muted by Home
   * Assistant · 2 min ago" so the user can tell who flipped the mic.
   * source is 0=boot, 1=http (dashboard), 2=mqtt (Home Assistant). */
  audio_mute_info_t mi;
  audio_get_mute_info(&mi);
  if (mi.age_ms != UINT32_MAX) {
    ac["last_mute_source"]  = mi.source;
    ac["last_mute_age_ms"]  = (long)mi.age_ms;
  } else {
    ac["last_mute_source"]  = -1;
    ac["last_mute_age_ms"]  = -1L;
  }

  JsonObject ast = ac["stats"].to<JsonObject>();
  ast["frames_processed"] = a_stats.frames_processed;
  ast["on_transitions"]   = a_stats.on_transitions;
  ast["off_transitions"]  = a_stats.off_transitions;
  ast["t3_detected"]      = a_stats.t3_detected;
  ast["t4_detected"]      = a_stats.t4_detected;
  ast["i2s_read_errors"]  = a_stats.i2s_read_errors;
#endif // FEATURE_ACOUSTIC_EVENTS

#if FEATURE_TOUCH
  touch_stats_t t_stats = {0};
  touch_get_stats(&t_stats);

  JsonObject tc = doc["touch"].to<JsonObject>();
  tc["enabled"]     = touch_is_running();
  tc["last_event"]  = touch_event_name(s.last_touch_event_type);
  tc["confidence"]  = s.last_touch_event_conf;
  tc["pad_channel"] = s.last_touch_pad_channel;
  tc["last_event_age_ms"] =
      s.last_touch_event_ms == 0 ? -1L : (long)(millis() - s.last_touch_event_ms);
  tc["baseline_locked"] = t_stats.baseline_locked;
  tc["baseline_value"]  = t_stats.baseline_value;
  tc["last_value"]      = t_stats.last_value;

  JsonObject tst = tc["stats"].to<JsonObject>();
  tst["reads_total"]     = t_stats.reads_total;
  tst["panic_events"]    = t_stats.panic_events;
  tst["tamper_events"]   = t_stats.tamper_events;
  tst["approach_events"] = t_stats.approach_events;
#endif // FEATURE_TOUCH

#if FEATURE_IR_RMT
  ir_stats_t ir_stats = {0};
  ir_get_stats(&ir_stats);

  JsonObject ir_obj = doc["ir"].to<JsonObject>();
  ir_obj["enabled"]      = ir_is_running();
  ir_obj["last_protocol"] = ir_protocol_name(s.last_ir_category);
  ir_obj["hash_bucket"]  = s.last_ir_hash_bucket;
  ir_obj["confidence"]   = s.last_ir_confidence;
  ir_obj["last_event_age_ms"] =
      s.last_ir_event_ms == 0 ? -1L : (long)(millis() - s.last_ir_event_ms);

  JsonObject ist = ir_obj["stats"].to<JsonObject>();
  ist["frames_received"] = ir_stats.frames_received;
  ist["frames_decoded"]  = ir_stats.frames_decoded;
  ist["frames_unknown"]  = ir_stats.frames_unknown;
  ist["events_emitted"]  = ir_stats.events_emitted;
#endif // FEATURE_IR_RMT

#if FEATURE_TEMP_TAMPER
  envsens_stats_t e_stats = {0};
  envsens_get_stats(&e_stats);

  JsonObject ts = doc["temp"].to<JsonObject>();
  ts["enabled"]         = envsens_is_running();
  ts["confidence"]      = s.last_temp_drift_conf;
  ts["baseline_locked"] = e_stats.baseline_locked;
  ts["last_event_age_ms"] =
      s.last_temp_drift_ms == 0 ? -1L : (long)(millis() - s.last_temp_drift_ms);

  JsonObject est = ts["stats"].to<JsonObject>();
  est["samples_taken"] = e_stats.samples_taken;
  est["drift_events"]  = e_stats.drift_events;
  /* Whole-degree rounding only; never exposes raw temperature. */
  est["baseline_c"]    = (int)e_stats.baseline_c_rounded;
  est["last_c"]        = (int)e_stats.last_c_rounded;
#endif // FEATURE_TEMP_TAMPER

#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
  /* Lowpower wake reason + capability bits — useful for the installer
   * to confirm the device booted from a touch-pad wake (forensic trail)
   * vs a normal cold boot, and for the dashboard to surface what wake
   * sources are available on the chip. */
  JsonObject lp = doc["lowpower"].to<JsonObject>();
  lp["wake_reason"] = lowpower_wake_reason_name(lowpower_get_wake_reason());
  lp["wake_touch_pad"] = lowpower_get_wake_touch_pad();
  lp["caps"] = lowpower_get_caps();
#endif

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}
#endif // FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS

#if FEATURE_ACOUSTIC_EVENTS
// ════════════════════════════════════════════════════════════════════════════
// AUDIO TESTABILITY + PRIVACY ENDPOINTS
//
// These exist so a user can (a) verify the mic is alive without setting off
// their actual smoke alarm and (b) turn the mic off at runtime in a way they
// can verify (the I2S driver is uninstalled and GPIO 41/42 are released).
//
// The "live level" endpoint exposes the SAME 20 ms RMS scalar the on/off
// hysteresis uses — not a new audio path. Self-test mode runs the existing
// T3/T4 matcher with relaxed timing tolerance and DOES NOT fire the normal
// event callback, so a TEST-button press never flows into Home Assistant.
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_audio_level(httpd_req_t* req) {
  if (!rate_limit_check(req, false)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  uint16_t rms = 0;
  uint32_t age_ms = 0;
  const bool running = audio_get_live_level(&rms, &age_ms);

  /* Fetch the LIVE thresholds (which init() may have customized) rather
   * than the compile-time defaults — keeps the UI level-meter notches
   * accurate if a future build tunes them at runtime. */
  audio_config_t cfg = AUDIO_CONFIG_DEFAULT;
  audio_get_config(&cfg);

  JsonDocument doc;
  doc["ok"] = true;
  doc["running"] = running;
  doc["muted"]   = audio_is_muted();
  doc["rms"]     = rms;
  doc["rms_on_threshold"]  = cfg.rms_on_threshold;
  doc["rms_off_threshold"] = cfg.rms_off_threshold;
  doc["envelope_high"]     = (rms >= cfg.rms_on_threshold);
  doc["age_ms"] = (age_ms == UINT32_MAX) ? -1L : (long)age_ms;

  /* Last 8 transitions, newest first, for the cadence-trace view. */
  audio_transition_t trans[8];
  const size_t n = audio_get_recent_transitions(trans, 8, 0);
  JsonArray arr = doc["transitions"].to<JsonArray>();
  for (size_t i = 0; i < n; i++) {
    JsonObject e = arr.add<JsonObject>();
    e["on"]     = (bool)trans[i].is_on;
    e["age_ms"] = trans[i].age_ms;
    e["dur_ms"] = trans[i].dur_ms;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_audio_mute(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  char body[64];
  const int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) return http_send_error(req, 400, "empty_body");
  body[recv] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }
  if (!input["muted"].is<bool>()) {
    return http_send_error(req, 400, "missing_muted_bool");
  }
  const bool want_muted = input["muted"].as<bool>();

  /* Apply at runtime. audio_mute(true) physically uninstalls I2S so the
   * GPIOs go tri-state — a user-verifiable hardware-level mute. The
   * source byte is recorded in the witness chain audit trail. */
  const bool ok = audio_mute(want_muted, AUDIO_MUTE_SOURCE_HTTP);
  if (!ok && !want_muted) {
    /* Unmute failed (I2S didn't come up). Still persist the user's
     * intent — they may have hardware issues we can't paper over. */
  }

  /* Persist user intent regardless of apply result, using the shared
   * helper so HTTP, MQTT, and any future control path stay in sync on
   * the NVS namespace + key. */
  const bool persisted = audio_save_mute_intent(want_muted);

  JsonDocument doc;
  doc["ok"] = true;
  doc["muted"] = audio_is_muted();
  doc["running"] = audio_is_running();
  doc["persisted"] = persisted;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_audio_test_start(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  if (audio_is_muted() || !audio_is_running()) {
    return http_send_error(req, 400, "mic_unavailable");
  }

  /* Body is optional: { "duration_ms": N } (clamped 5_000..60_000). */
  uint32_t duration_ms = 30000;
  char body[64] = {0};
  const int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv > 0) {
    body[recv] = '\0';
    JsonDocument input;
    if (deserializeJson(input, body) == DeserializationError::Ok) {
      if (input["duration_ms"].is<uint32_t>()) {
        duration_ms = input["duration_ms"].as<uint32_t>();
      }
    }
  }
  if (duration_ms < 5000)  duration_ms = 5000;
  if (duration_ms > 60000) duration_ms = 60000;

  if (!audio_selftest_start(duration_ms)) {
    return http_send_error(req, 500, "selftest_start_failed");
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["duration_ms"] = duration_ms;
  doc["note"] = "Press your alarm's TEST button now. A match in this mode "
                "does NOT fire any Home Assistant automation.";
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_audio_test_status(httpd_req_t* req) {
  if (!rate_limit_check(req, false)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  audio_selftest_status_t st;
  audio_selftest_status(&st);

  JsonDocument doc;
  doc["ok"]               = true;
  doc["active"]           = (bool)st.active;
  doc["remaining_ms"]     = st.remaining_ms;
  doc["matched"]          = audio_event_name(st.matched_type);
  doc["confidence"]       = st.matched_conf;
  doc["transitions_seen"] = st.transitions_seen;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}
#endif // FEATURE_ACOUSTIC_EVENTS

// ════════════════════════════════════════════════════════════════════════════
// CONVENIENCE FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

bool network_init(const char* ap_ssid, const char* ap_password,
                  const char* mdns_hostname) {
  return network_get_instance().begin(ap_ssid, ap_password, mdns_hostname);
}

bool network_start_http() {
  return network_get_instance().startHttpServer();
}

void network_update() {
  network_get_instance().checkConnection();
}

httpd_handle_t network_get_http_server() {
  return network_get_instance().getHttpServer();
}

#endif // FEATURE_WIFI_AP || FEATURE_HTTP_SERVER
