/*
 * SecuraCV Canary — WiFi Access Point & HTTP Server Implementation
 *
 * Stub implementation for Arduino sketch linking.
 * WiFi AP and HTTP server are currently implemented inline in the
 * main .ino file. These stubs satisfy the linker for wap_server.h
 * namespace declarations.
 */

#include "wap_server.h"

namespace wap_server {

// ════════════════════════════════════════════════════════════════════════════
// INITIALIZATION
// ════════════════════════════════════════════════════════════════════════════

bool init() {
  // WiFi/HTTP init is handled directly by the .ino
  return false;
}

void deinit() {
}

bool is_running() {
  return false;
}

ServerStatus get_status() {
  ServerStatus s = {};
  s.state = SERVER_STATE_STOPPED;
  return s;
}

// ════════════════════════════════════════════════════════════════════════════
// ACCESS POINT MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

bool start_access_point(const char* ssid, const char* password) {
  return false;
}

bool stop_access_point() {
  return false;
}

bool set_ap_credentials(const char* ssid, const char* password) {
  return false;
}

uint8_t get_connected_clients() {
  return 0;
}

// ════════════════════════════════════════════════════════════════════════════
// HTTP SERVER
// ════════════════════════════════════════════════════════════════════════════

bool start_http_server() {
  return false;
}

bool stop_http_server() {
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// mDNS
// ════════════════════════════════════════════════════════════════════════════

bool start_mdns(const char* hostname) {
  return false;
}

bool stop_mdns() {
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// RATE LIMITING
// ════════════════════════════════════════════════════════════════════════════

bool check_rate_limit(const char* client_ip, bool is_action) {
  return true;  // Allow by default
}

void reset_rate_limits() {
}

// ════════════════════════════════════════════════════════════════════════════
// RESPONSE HELPERS
// ════════════════════════════════════════════════════════════════════════════

esp_err_t send_json_response(httpd_req_t* req, const char* json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, json);
}

esp_err_t send_json_error(httpd_req_t* req, int status_code,
                          const char* error_code, const char* message) {
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"error\":\"%s\",\"message\":\"%s\"}",
    error_code ? error_code : "error",
    message ? message : "Unknown error");

  char status_str[8];
  snprintf(status_str, sizeof(status_str), "%d", status_code);
  httpd_resp_set_status(req, status_str);
  return send_json_response(req, buf);
}

esp_err_t send_file_response(httpd_req_t* req, const char* path,
                             const char* content_type) {
  return ESP_ERR_NOT_FOUND;
}

// ════════════════════════════════════════════════════════════════════════════
// SECURITY HELPERS
// ════════════════════════════════════════════════════════════════════════════

bool validate_request(httpd_req_t* req) {
  return true;
}

void set_security_headers(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

} // namespace wap_server
