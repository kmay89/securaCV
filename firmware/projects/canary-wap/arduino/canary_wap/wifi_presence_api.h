/*
 * SecuraCV Canary — WiFi Presence REST API Handlers
 *
 * HTTP handlers for WiFi probe request presence detection.
 * Returns ONLY aggregate counts — no MAC addresses or identifiers.
 * Uses String-based JSON serialization to avoid fixed-buffer overflows.
 */

#ifndef SECURACV_WIFI_PRESENCE_API_H
#define SECURACV_WIFI_PRESENCE_API_H

#include "esp_http_server.h"
#include "wifi_presence.h"
#include <ArduinoJson.h>

namespace wifi_presence_api {

// ════════════════════════════════════════════════════════════════════════════
// HELPER
// ════════════════════════════════════════════════════════════════════════════

static inline esp_err_t send_json(httpd_req_t* req, const JsonDocument& doc) {
  String response;
  serializeJson(doc, response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// API HANDLERS
// ════════════════════════════════════════════════════════════════════════════

// GET /api/presence/wifi — WiFi presence status
inline esp_err_t handle_wifi_presence_status(httpd_req_t* req) {
  JsonDocument doc;

  doc["feature_available"] = (bool)FEATURE_WIFI_PRESENCE;

  #if FEATURE_WIFI_PRESENCE
  doc["enabled"] = wifi_presence::is_enabled();
  doc["current_count"] = wifi_presence::get_current_count();
  doc["last_count"] = wifi_presence::get_last_count();
  doc["peak_count"] = wifi_presence::get_peak_count();
  doc["total_probes"] = wifi_presence::get_total_probes();
  doc["queue_drops"] = wifi_presence::get_queue_drops();
  doc["bucket_duration_ms"] = (uint32_t)wifi_presence::BUCKET_DURATION_MS;
  doc["bucket_elapsed_ms"] = wifi_presence::get_bucket_elapsed_ms();

  // History sparkline data
  uint8_t history[wifi_presence::HISTORY_BUCKETS];
  uint8_t history_len;
  wifi_presence::get_history(history, &history_len);
  JsonArray hist = doc["history"].to<JsonArray>();
  for (uint8_t i = 0; i < history_len; i++) {
    hist.add(history[i]);
  }
  #else
  doc["enabled"] = false;
  doc["reason"] = "WiFi presence not compiled (FEATURE_WIFI_PRESENCE=0)";
  #endif

  return send_json(req, doc);
}

// POST /api/presence/wifi/start — Enable WiFi presence monitoring
inline esp_err_t handle_wifi_presence_start(httpd_req_t* req) {
  JsonDocument doc;

  #if FEATURE_WIFI_PRESENCE
  if (wifi_presence::start()) {
    doc["success"] = true;
    doc["message"] = "WiFi presence monitoring started";
  } else {
    doc["success"] = false;
    doc["error"] = "Failed to start WiFi presence monitoring";
  }
  #else
  doc["success"] = false;
  doc["error"] = "WiFi presence not compiled (FEATURE_WIFI_PRESENCE=0)";
  #endif

  return send_json(req, doc);
}

// POST /api/presence/wifi/stop — Disable WiFi presence monitoring
inline esp_err_t handle_wifi_presence_stop(httpd_req_t* req) {
  JsonDocument doc;

  #if FEATURE_WIFI_PRESENCE
  wifi_presence::stop();
  doc["success"] = true;
  doc["message"] = "WiFi presence monitoring stopped";
  doc["final_count"] = wifi_presence::get_last_count();
  #else
  doc["success"] = false;
  doc["error"] = "WiFi presence not compiled (FEATURE_WIFI_PRESENCE=0)";
  #endif

  return send_json(req, doc);
}

// GET /api/presence — Combined presence status (WiFi + BLE if available)
inline esp_err_t handle_presence_combined(httpd_req_t* req) {
  JsonDocument doc;

  // WiFi presence
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  #if FEATURE_WIFI_PRESENCE
  wifi["available"] = true;
  wifi["enabled"] = wifi_presence::is_enabled();
  wifi["current_count"] = wifi_presence::get_current_count();
  wifi["last_count"] = wifi_presence::get_last_count();
  wifi["peak_count"] = wifi_presence::get_peak_count();
  wifi["bucket_duration_ms"] = (uint32_t)wifi_presence::BUCKET_DURATION_MS;
  wifi["bucket_elapsed_ms"] = wifi_presence::get_bucket_elapsed_ms();

  uint8_t history[wifi_presence::HISTORY_BUCKETS];
  uint8_t history_len;
  wifi_presence::get_history(history, &history_len);
  JsonArray hist = wifi["history"].to<JsonArray>();
  for (uint8_t i = 0; i < history_len; i++) {
    hist.add(history[i]);
  }
  #else
  wifi["available"] = false;
  wifi["reason"] = "Not compiled (FEATURE_WIFI_PRESENCE=0)";
  #endif

  // BLE presence (compile-time: OFF by default for security)
  JsonObject ble = doc["ble"].to<JsonObject>();
  #if FEATURE_BLE
  ble["available"] = true;
  ble["note"] = "BLE presence available via BLE Discovery subsystem";
  #else
  ble["available"] = false;
  ble["reason"] = "BLE disabled at compile time (security: no BT binary blobs)";
  ble["enable_instructions"] = "Rebuild with FEATURE_BLE=1 and CONFIG_BT_ENABLED=y";
  #endif

  return send_json(req, doc);
}

// ════════════════════════════════════════════════════════════════════════════
// ROUTE REGISTRATION
// ════════════════════════════════════════════════════════════════════════════

inline void register_routes(httpd_handle_t server) {
  httpd_uri_t combined = {
    .uri = "/api/presence", .method = HTTP_GET,
    .handler = handle_presence_combined, .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &combined);

  httpd_uri_t wifi_status = {
    .uri = "/api/presence/wifi", .method = HTTP_GET,
    .handler = handle_wifi_presence_status, .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &wifi_status);

  httpd_uri_t wifi_start = {
    .uri = "/api/presence/wifi/start", .method = HTTP_POST,
    .handler = handle_wifi_presence_start, .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &wifi_start);

  httpd_uri_t wifi_stop = {
    .uri = "/api/presence/wifi/stop", .method = HTTP_POST,
    .handler = handle_wifi_presence_stop, .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &wifi_stop);
}

} // namespace wifi_presence_api

#endif // SECURACV_WIFI_PRESENCE_API_H
