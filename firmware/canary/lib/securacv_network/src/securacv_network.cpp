/*
 * SecuraCV Canary — Network Management Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_network.h"
#include "securacv_witness.h"
#include "securacv_crypto.h"

#if FEATURE_WIFI_AP || FEATURE_HTTP_SERVER

#include <ArduinoJson.h>

#if FEATURE_SD_STORAGE
#include "securacv_storage.h"
#endif

#if FEATURE_CAMERA_PEEK
#include "securacv_camera.h"
#endif

#if FEATURE_OTA_UPDATE
#include <Update.h>
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
  : m_http_server(nullptr), m_scan_in_progress(false) {
  memset(&m_creds, 0, sizeof(m_creds));
  memset(&m_status, 0, sizeof(m_status));
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

bool NetworkManager::begin(const char* ap_ssid, const char* ap_password) {
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

  // Start mDNS
  if (MDNS.begin("canary")) {
    MDNS.addService("http", "tcp", 80);
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "mDNS started", "canary.local");
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

// Forward declarations for HTTP handlers
static esp_err_t handle_ui(httpd_req_t* req);
static esp_err_t handle_status(httpd_req_t* req);
static esp_err_t handle_chain(httpd_req_t* req);
static esp_err_t handle_logs(httpd_req_t* req);
static esp_err_t handle_log_ack(httpd_req_t* req);
static esp_err_t handle_ack_all(httpd_req_t* req);
static esp_err_t handle_reboot(httpd_req_t* req);

#if FEATURE_OTA_UPDATE
static esp_err_t handle_ota(httpd_req_t* req);
#endif

#if FEATURE_CAMERA_PEEK
static esp_err_t handle_peek_start(httpd_req_t* req);
static esp_err_t handle_peek_stream(httpd_req_t* req);
static esp_err_t handle_peek_stop(httpd_req_t* req);
static esp_err_t handle_peek_status(httpd_req_t* req);
#endif

bool NetworkManager::startHttpServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 8192;
  config.max_uri_handlers = 16;

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
}

// ════════════════════════════════════════════════════════════════════════════
// HTTP HANDLERS
// ════════════════════════════════════════════════════════════════════════════

// Declare webui extern (defined in securacv_webui library)
extern const char CANARY_UI_HTML[];

static esp_err_t handle_ui(httpd_req_t* req) {
  witness_get_health().http_requests++;
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, CANARY_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_status(httpd_req_t* req) {
  witness_get_health().http_requests++;

  DeviceIdentity& device = witness_get_device();
  SystemHealth& health = witness_get_health();

  StaticJsonDocument<1024> doc;
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

static esp_err_t handle_chain(httpd_req_t* req) {
  witness_get_health().http_requests++;

  DeviceIdentity& device = witness_get_device();
  WitnessRecord& last = witness_get_last_record();

  StaticJsonDocument<512> doc;
  doc["ok"] = true;

  char chain_hex[65];
  hex_to_str(chain_hex, device.chain_head, 32);
  doc["chain_head"] = chain_hex;
  doc["sequence"] = device.seq;

  if (last.seq > 0) {
    JsonArray blocks = doc.createNestedArray("blocks");
    JsonObject block = blocks.createNestedObject();
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
  witness_get_health().http_requests++;

  HealthLogRingEntry* ring = witness_get_health_log_ring();
  size_t count = witness_get_health_log_count();
  size_t head = witness_get_health_log_head();

  DynamicJsonDocument doc(4096);
  doc["ok"] = true;
  doc["total"] = count;

  JsonArray logs = doc.createNestedArray("logs");

  for (size_t i = 0; i < count; i++) {
    size_t idx = (head + 100 - 1 - i) % 100;
    HealthLogRingEntry& entry = ring[idx];

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
  witness_get_health().http_requests++;

  const char* uri = req->uri;
  const char* seq_start = strstr(uri, "/logs/");
  if (!seq_start) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
  }
  seq_start += 6;
  uint32_t seq = atoi(seq_start);

  bool success = acknowledge_log_entry(seq, ACK_STATUS_ACKNOWLEDGED, "");

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

  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["acknowledged"] = acked;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_reboot(httpd_req_t* req) {
  witness_get_health().http_requests++;

  log_health(LOG_LEVEL_NOTICE, LOG_CAT_USER, "Reboot requested", nullptr);

  DeviceIdentity& device = witness_get_device();
  nvs_store_u32(NVS_KEY_SEQ, device.seq);
  nvs_store_bytes(NVS_KEY_CHAIN, device.chain_head, 32);

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

#if FEATURE_OTA_UPDATE
static esp_err_t handle_ota(httpd_req_t* req) {
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
    int recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
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
  witness_get_health().http_requests++;

  if (!camera_is_initialized()) {
    return http_send_error(req, 503, "camera_not_initialized");
  }

  camera_set_peek_active(true);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek started", nullptr);

  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["message"] = "Peek stream activated";
  doc["resolution"] = camera_get_instance().getResolutionName();

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_peek_stream(httpd_req_t* req) {
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
  witness_get_health().http_requests++;

  camera_set_peek_active(false);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stopped", nullptr);

  return http_send_json(req, "{\"ok\":true,\"message\":\"Peek stopped\"}");
}

static esp_err_t handle_peek_status(httpd_req_t* req) {
  witness_get_health().http_requests++;

  CameraManager& cam = camera_get_instance();

  StaticJsonDocument<256> doc;
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
// CONVENIENCE FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

bool network_init(const char* ap_ssid, const char* ap_password) {
  return network_get_instance().begin(ap_ssid, ap_password);
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
