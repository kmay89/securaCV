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
#include <cstring>  // for strcmp

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

// Helper to clamp value within bounds
template<typename T>
static inline T clamp(T value, T min_val, T max_val) {
  if (value < min_val) return min_val;
  if (value > max_val) return max_val;
  return value;
}

// POST /api/rf/settings - Update RF presence settings
// All values are validated and clamped to safe ranges
inline esp_err_t handle_rf_settings_set(httpd_req_t* req) {
  char content[256];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  if (content_len >= static_cast<int>(sizeof(content) - 1)) {
    return send_error(req, "Request body too large");
  }
  content[content_len] = '\0';

  StaticJsonDocument<256> input;
  DeserializationError json_err = deserializeJson(input, content);
  if (json_err != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  rf_presence::RfPresenceSettings settings = rf_presence::get_settings();
  bool any_clamped = false;

  if (input.containsKey("enabled")) {
    settings.enabled = input["enabled"].as<bool>();
  }

  if (input.containsKey("presence_threshold_sec")) {
    uint32_t val_ms = input["presence_threshold_sec"].as<uint32_t>() * 1000;
    uint32_t clamped = clamp(val_ms,
      rf_presence::MIN_PRESENCE_THRESHOLD_MS,
      rf_presence::MAX_PRESENCE_THRESHOLD_MS);
    if (clamped != val_ms) any_clamped = true;
    settings.presence_threshold_ms = clamped;
  }

  if (input.containsKey("dwell_threshold_sec")) {
    uint32_t val_ms = input["dwell_threshold_sec"].as<uint32_t>() * 1000;
    uint32_t clamped = clamp(val_ms,
      rf_presence::MIN_DWELL_THRESHOLD_MS,
      rf_presence::MAX_DWELL_THRESHOLD_MS);
    if (clamped != val_ms) any_clamped = true;
    settings.dwell_threshold_ms = clamped;
  }

  if (input.containsKey("lost_timeout_sec")) {
    uint32_t val_ms = input["lost_timeout_sec"].as<uint32_t>() * 1000;
    uint32_t clamped = clamp(val_ms,
      rf_presence::MIN_LOST_TIMEOUT_MS,
      rf_presence::MAX_LOST_TIMEOUT_MS);
    if (clamped != val_ms) any_clamped = true;
    settings.lost_timeout_ms = clamped;
  }

  if (input.containsKey("min_presence_count")) {
    uint8_t val = input["min_presence_count"].as<uint8_t>();
    uint8_t clamped = clamp(val,
      rf_presence::MIN_PRESENCE_COUNT_SETTING,
      rf_presence::MAX_PRESENCE_COUNT_SETTING);
    if (clamped != val) any_clamped = true;
    settings.min_presence_count = clamped;
  }

  if (input.containsKey("emit_impulse_events")) {
    settings.emit_impulse_events = input["emit_impulse_events"].as<bool>();
  }

  if (input.containsKey("emit_narrative_hints")) {
    settings.emit_narrative_hints = input["emit_narrative_hints"].as<bool>();
  }

  if (rf_presence::set_settings(settings)) {
    if (any_clamped) {
      return send_success(req, "Settings updated (some values clamped to valid range)");
    }
    return send_success(req, "Settings updated");
  }
  return send_error(req, "Failed to update settings");
}

// GET /api/rf/conformance - Run conformance checks
// For privacy verification testing
// WARNING: The token_rotation check has a SIDE EFFECT - it actually rotates the session!
// Use the skip_rotation parameter to avoid this if needed
inline esp_err_t handle_rf_conformance(httpd_req_t* req) {
  // Check for skip_rotation query parameter
  char query_buf[64] = {0};
  bool skip_rotation = false;
  if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
    char param_val[8] = {0};
    if (httpd_query_key_value(query_buf, "skip_rotation", param_val, sizeof(param_val)) == ESP_OK) {
      skip_rotation = (strcmp(param_val, "true") == 0 || strcmp(param_val, "1") == 0);
    }
  }

  StaticJsonDocument<384> doc;

  doc["no_mac_storage"] = rf_presence::conformance_check_no_mac_storage();
  doc["aggregate_only"] = rf_presence::conformance_check_aggregate_only();
  doc["secure_wipe"] = rf_presence::conformance_check_secure_wipe();

  // Token rotation test has side effects
  if (skip_rotation) {
    doc["token_rotation"] = "skipped";
    doc["token_rotation_note"] = "Use skip_rotation=false to run (rotates session)";
  } else {
    doc["token_rotation"] = rf_presence::conformance_check_token_rotation();
    doc["token_rotation_note"] = "Session was rotated as part of this test";
  }

  // Calculate overall pass status
  bool all_passed = doc["no_mac_storage"].as<bool>() &&
                    doc["aggregate_only"].as<bool>() &&
                    doc["secure_wipe"].as<bool>() &&
                    (skip_rotation || doc["token_rotation"].as<bool>());
  doc["all_passed"] = all_passed;
  doc["session_epoch"] = rf_presence::get_session_epoch();

  char buffer[384];
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
