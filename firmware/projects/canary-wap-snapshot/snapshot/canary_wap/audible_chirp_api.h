/*
 * SecuraCV Canary — Audible Chirp REST API Handlers
 *
 * HTTP handlers for local audible/visual alert tones.
 */

#ifndef SECURACV_AUDIBLE_CHIRP_API_H
#define SECURACV_AUDIBLE_CHIRP_API_H

#include "esp_http_server.h"
#include "audible_chirp.h"
#include <ArduinoJson.h>
#include <cstring>

namespace audible_chirp_api {

// ════════════════════════════════════════════════════════════════════════════
// API HANDLERS
// ════════════════════════════════════════════════════════════════════════════

// GET /api/audible-chirp — Chirp status and configuration
inline esp_err_t handle_chirp_status(httpd_req_t* req) {
  JsonDocument doc;

  doc["feature_available"] = (bool)FEATURE_AUDIBLE_CHIRP;

  #if FEATURE_AUDIBLE_CHIRP
  doc["available"] = audible_chirp::is_available();
  doc["gpio"] = audible_chirp::get_gpio();
  doc["visual_only"] = audible_chirp::is_visual_only();
  doc["chirps_played"] = audible_chirp::get_chirps_played();

  JsonArray patterns = doc["patterns"].to<JsonArray>();
  for (uint8_t i = 0; i < audible_chirp::PATTERN_COUNT; i++) {
    patterns.add(audible_chirp::PATTERN_NAMES[i]);
  }

  if (audible_chirp::is_visual_only()) {
    doc["note"] = "Visual mode: LED blink patterns. Connect passive buzzer for audio.";
  }
  #else
  doc["available"] = false;
  doc["reason"] = "Audible chirp not compiled (FEATURE_AUDIBLE_CHIRP=0)";
  #endif

  char buffer[384];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// POST /api/audible-chirp/play — Play a chirp pattern
// Body: {"pattern": "confirm"} or {"pattern": "alert"}
inline esp_err_t handle_chirp_play(httpd_req_t* req) {
  JsonDocument doc;

  #if FEATURE_AUDIBLE_CHIRP
  if (!audible_chirp::is_available()) {
    doc["success"] = false;
    doc["error"] = "Audible chirp not initialized";
    char buffer[128];
    serializeJson(doc, buffer);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buffer);
  }

  // Read request body
  char content[128];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    // No body = play default confirm pattern
    audible_chirp::chirp_confirm();
    doc["success"] = true;
    doc["pattern"] = "confirm";
    doc["note"] = "Default pattern (no body provided)";
    char buffer[128];
    serializeJson(doc, buffer);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, buffer);
  }
  content[content_len] = '\0';

  // Parse JSON
  JsonDocument input;
  DeserializationError err = deserializeJson(input, content);
  if (err) {
    doc["success"] = false;
    doc["error"] = "Invalid JSON";
    char buffer[128];
    serializeJson(doc, buffer);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buffer);
  }

  const char* pattern = input["pattern"] | "confirm";

  if (audible_chirp::play_by_name(pattern)) {
    doc["success"] = true;
    doc["pattern"] = pattern;
    doc["visual_only"] = audible_chirp::is_visual_only();
  } else {
    doc["success"] = false;
    doc["error"] = "Unknown pattern";
    doc["valid_patterns"] = "confirm, alert, tamper, success, error";
  }
  #else
  doc["success"] = false;
  doc["error"] = "Audible chirp not compiled (FEATURE_AUDIBLE_CHIRP=0)";
  #endif

  char buffer[256];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// POST /api/audible-chirp/test — Quick test chirp (confirm pattern)
inline esp_err_t handle_chirp_test(httpd_req_t* req) {
  JsonDocument doc;

  #if FEATURE_AUDIBLE_CHIRP
  if (audible_chirp::is_available()) {
    audible_chirp::chirp_confirm();
    doc["success"] = true;
    doc["pattern"] = "confirm";
    doc["visual_only"] = audible_chirp::is_visual_only();
  } else {
    doc["success"] = false;
    doc["error"] = "Audible chirp not initialized";
  }
  #else
  doc["success"] = false;
  doc["error"] = "Audible chirp not compiled (FEATURE_AUDIBLE_CHIRP=0)";
  #endif

  char buffer[128];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// POST /api/audible-chirp/config — Configure chirp settings
// Body: {"visual_only": true} or {"gpio": 2}
inline esp_err_t handle_chirp_config(httpd_req_t* req) {
  char content[128];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);

  JsonDocument doc;

  #if FEATURE_AUDIBLE_CHIRP
  if (content_len > 0) {
    content[content_len] = '\0';
    JsonDocument input;
    if (deserializeJson(input, content) == DeserializationError::Ok) {
      if (input["visual_only"].is<JsonVariant>()) {
        audible_chirp::set_visual_only(input["visual_only"].as<bool>());
      }
    }
  }

  doc["success"] = true;
  doc["gpio"] = audible_chirp::get_gpio();
  doc["visual_only"] = audible_chirp::is_visual_only();
  #else
  doc["success"] = false;
  doc["error"] = "Audible chirp not compiled";
  #endif

  char buffer[128];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// ════════════════════════════════════════════════════════════════════════════
// ROUTE REGISTRATION
// ════════════════════════════════════════════════════════════════════════════

inline void register_routes(httpd_handle_t server) {
  httpd_uri_t status = {
    .uri = "/api/audible-chirp", .method = HTTP_GET,
    .handler = handle_chirp_status, .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &status);

  httpd_uri_t play = {
    .uri = "/api/audible-chirp/play", .method = HTTP_POST,
    .handler = handle_chirp_play, .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &play);

  httpd_uri_t test = {
    .uri = "/api/audible-chirp/test", .method = HTTP_POST,
    .handler = handle_chirp_test, .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &test);

  httpd_uri_t config = {
    .uri = "/api/audible-chirp/config", .method = HTTP_POST,
    .handler = handle_chirp_config, .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &config);
}

} // namespace audible_chirp_api

#endif // SECURACV_AUDIBLE_CHIRP_API_H
