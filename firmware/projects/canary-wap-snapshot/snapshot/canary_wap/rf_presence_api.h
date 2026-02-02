/*
 * SecuraCV Canary — RF Presence REST API Handlers
 *
 * HTTP handlers for RF presence detection endpoints.
 * Implements privacy-preserving signal observation per spec/canary_free_signals_v0.md
 *
 * IMPORTANT: These endpoints expose ONLY aggregate, anonymized data.
 * No MAC addresses, device names, or identifiable information is ever returned.
 */

#ifndef SECURACV_RF_PRESENCE_API_H
#define SECURACV_RF_PRESENCE_API_H

#include "esp_http_server.h"
#include "rf_presence.h"
#include <ArduinoJson.h>

namespace rf_presence_api {

// ════════════════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

static inline esp_err_t send_json_response(httpd_req_t* req, const char* json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, json);
}

static inline esp_err_t send_success(httpd_req_t* req, const char* message = nullptr) {
  StaticJsonDocument<128> doc;
  doc["success"] = true;
  if (message) doc["message"] = message;

  char buffer[128];
  serializeJson(doc, buffer);
  return send_json_response(req, buffer);
}

static inline esp_err_t send_error(httpd_req_t* req, const char* error) {
  StaticJsonDocument<128> doc;
  doc["success"] = false;
  doc["error"] = error;

  char buffer[128];
  serializeJson(doc, buffer);
  return send_json_response(req, buffer);
}

// ════════════════════════════════════════════════════════════════════════════
// API HANDLERS
// ════════════════════════════════════════════════════════════════════════════

// GET /api/rf/status - RF presence status
// Returns: state, confidence, device_count (aggregate), rssi_mean
// NEVER returns: MAC addresses, device names, identifiers
inline esp_err_t handle_rf_status(httpd_req_t* req) {
  rf_presence::RfStateSnapshot snapshot = rf_presence::get_snapshot();

  StaticJsonDocument<512> doc;

  // State information
  doc["state"] = snapshot.state_name;
  doc["enabled"] = rf_presence::is_enabled();

  // Confidence (aggregate metric only)
  const char* conf_names[] = {"uncertain", "low", "moderate", "high"};
  doc["confidence"] = conf_names[snapshot.confidence];

  // Aggregate metrics only - NO identifiers
  doc["device_count"] = snapshot.device_count;  // Anonymous count
  doc["rssi_mean"] = snapshot.rssi_mean;        // Aggregate RSSI

  // Dwell classification
  const char* dwell_names[] = {"transient", "lingering", "sustained"};
  doc["dwell_class"] = dwell_names[snapshot.dwell_class];
  doc["state_duration_sec"] = snapshot.state_duration_ms / 1000;

  // System info
  doc["uptime_sec"] = snapshot.uptime_s;
  doc["last_event"] = snapshot.last_event;

  // Session info (for privacy verification)
  doc["session_epoch"] = rf_presence::get_session_epoch();

  char buffer[512];
  serializeJson(doc, buffer);
  return send_json_response(req, buffer);
}

// POST /api/rf/enable - Enable RF presence detection
inline esp_err_t handle_rf_enable(httpd_req_t* req) {
  if (rf_presence::enable()) {
    return send_success(req, "RF presence enabled");
  }
  return send_error(req, "Failed to enable RF presence");
}

// POST /api/rf/disable - Disable RF presence detection
inline esp_err_t handle_rf_disable(httpd_req_t* req) {
  rf_presence::disable();
  return send_success(req, "RF presence disabled");
}

// POST /api/rf/rotate - Force session rotation (privacy measure)
inline esp_err_t handle_rf_rotate(httpd_req_t* req) {
  rf_presence::rotate_session();

  StaticJsonDocument<128> doc;
  doc["success"] = true;
  doc["message"] = "Session rotated";
  doc["new_epoch"] = rf_presence::get_session_epoch();

  char buffer[128];
  serializeJson(doc, buffer);
  return send_json_response(req, buffer);
}

// GET /api/rf/settings - Get RF presence settings
inline esp_err_t handle_rf_settings_get(httpd_req_t* req) {
  rf_presence::RfPresenceSettings settings = rf_presence::get_settings();

  StaticJsonDocument<256> doc;
  doc["enabled"] = settings.enabled;
  doc["presence_threshold_sec"] = settings.presence_threshold_ms / 1000;
  doc["dwell_threshold_sec"] = settings.dwell_threshold_ms / 1000;
  doc["lost_timeout_sec"] = settings.lost_timeout_ms / 1000;
  doc["min_presence_count"] = settings.min_presence_count;
  doc["emit_impulse_events"] = settings.emit_impulse_events;
  doc["emit_narrative_hints"] = settings.emit_narrative_hints;

  char buffer[256];
  serializeJson(doc, buffer);
  return send_json_response(req, buffer);
}

// POST /api/rf/settings - Update RF presence settings
inline esp_err_t handle_rf_settings_set(httpd_req_t* req) {
  char content[256];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  StaticJsonDocument<256> input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  rf_presence::RfPresenceSettings settings = rf_presence::get_settings();

  if (input.containsKey("enabled")) {
    settings.enabled = input["enabled"].as<bool>();
  }
  if (input.containsKey("presence_threshold_sec")) {
    settings.presence_threshold_ms = input["presence_threshold_sec"].as<uint32_t>() * 1000;
  }
  if (input.containsKey("dwell_threshold_sec")) {
    settings.dwell_threshold_ms = input["dwell_threshold_sec"].as<uint32_t>() * 1000;
  }
  if (input.containsKey("lost_timeout_sec")) {
    settings.lost_timeout_ms = input["lost_timeout_sec"].as<uint32_t>() * 1000;
  }
  if (input.containsKey("min_presence_count")) {
    settings.min_presence_count = input["min_presence_count"].as<uint8_t>();
  }
  if (input.containsKey("emit_impulse_events")) {
    settings.emit_impulse_events = input["emit_impulse_events"].as<bool>();
  }
  if (input.containsKey("emit_narrative_hints")) {
    settings.emit_narrative_hints = input["emit_narrative_hints"].as<bool>();
  }

  if (rf_presence::set_settings(settings)) {
    return send_success(req, "Settings updated");
  }
  return send_error(req, "Failed to update settings");
}

// GET /api/rf/conformance - Run conformance checks
// For privacy verification testing
inline esp_err_t handle_rf_conformance(httpd_req_t* req) {
  StaticJsonDocument<256> doc;

  doc["no_mac_storage"] = rf_presence::conformance_check_no_mac_storage();
  doc["token_rotation"] = rf_presence::conformance_check_token_rotation();
  doc["aggregate_only"] = rf_presence::conformance_check_aggregate_only();

  bool all_passed = doc["no_mac_storage"].as<bool>() &&
                    doc["token_rotation"].as<bool>() &&
                    doc["aggregate_only"].as<bool>();
  doc["all_passed"] = all_passed;

  char buffer[256];
  serializeJson(doc, buffer);
  return send_json_response(req, buffer);
}

// ════════════════════════════════════════════════════════════════════════════
// ROUTE REGISTRATION
// ════════════════════════════════════════════════════════════════════════════

static inline void register_api_handler(httpd_handle_t server, const char* uri,
                                        httpd_method_t method,
                                        esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t route = { .uri = uri, .method = method, .handler = handler, .user_ctx = nullptr };
  httpd_register_uri_handler(server, &route);
}

// Call this to register all RF Presence API routes with the HTTP server
inline void register_routes(httpd_handle_t server) {
  // GET endpoints
  register_api_handler(server, "/api/rf/status", HTTP_GET, handle_rf_status);
  register_api_handler(server, "/api/rf/settings", HTTP_GET, handle_rf_settings_get);
  register_api_handler(server, "/api/rf/conformance", HTTP_GET, handle_rf_conformance);

  // POST endpoints
  register_api_handler(server, "/api/rf/enable", HTTP_POST, handle_rf_enable);
  register_api_handler(server, "/api/rf/disable", HTTP_POST, handle_rf_disable);
  register_api_handler(server, "/api/rf/rotate", HTTP_POST, handle_rf_rotate);
  register_api_handler(server, "/api/rf/settings", HTTP_POST, handle_rf_settings_set);
}

} // namespace rf_presence_api

#endif // SECURACV_RF_PRESENCE_API_H
