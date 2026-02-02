/*
 * SecuraCV Canary — Bluetooth REST API Handlers
 *
 * HTTP handlers for Bluetooth Low Energy management endpoints.
 * Enables mobile app connectivity and device management.
 *
 * All handlers follow the same pattern as other API handlers.
 */

#ifndef SECURACV_BLUETOOTH_API_H
#define SECURACV_BLUETOOTH_API_H

#include "esp_http_server.h"
#include "bluetooth_channel.h"
#include <ArduinoJson.h>

namespace bluetooth_api {

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

// GET /api/bluetooth - Bluetooth status
inline esp_err_t handle_bluetooth_status(httpd_req_t* req) {
  bluetooth_channel::BluetoothStatus status = bluetooth_channel::get_status();

  DynamicJsonDocument doc(2048);

  doc["state"] = bluetooth_channel::state_name(status.state);
  doc["enabled"] = status.enabled;
  doc["advertising"] = status.advertising;
  doc["scanning"] = status.scanning;
  doc["connected"] = status.connected;
  doc["device_name"] = status.device_name;
  doc["local_address"] = status.local_address;
  doc["tx_power"] = status.tx_power;
  doc["paired_count"] = status.paired_count;
  doc["scanned_count"] = status.scanned_count;

  // Connection info
  if (status.connected) {
    JsonObject conn = doc.createNestedObject("connection");
    char addr[18];
    bluetooth_channel::format_address(status.connection.address, addr);
    conn["address"] = addr;
    conn["name"] = status.connection.name;
    conn["rssi"] = status.connection.rssi;
    conn["security"] = bluetooth_channel::security_level_name(status.connection.security);
    conn["connected_sec"] = (millis() - status.connection.connected_since_ms) / 1000;
    conn["bytes_sent"] = status.connection.bytes_sent;
    conn["bytes_received"] = status.connection.bytes_received;
  }

  // Pairing info
  if (status.pairing.state != bluetooth_channel::PAIR_NONE) {
    JsonObject pair = doc.createNestedObject("pairing");
    pair["state"] = bluetooth_channel::pairing_state_name(status.pairing.state);
    if (status.pairing.pin_displayed) {
      pair["pin"] = status.pairing.pin_code;
    }
    char addr[18];
    bluetooth_channel::format_address(status.pairing.peer_address, addr);
    pair["peer_address"] = addr;
    pair["peer_name"] = status.pairing.peer_name;
  }

  // Statistics
  JsonObject stats = doc.createNestedObject("stats");
  stats["total_connections"] = status.total_connections;
  stats["total_bytes_sent"] = status.total_bytes_sent;
  stats["total_bytes_received"] = status.total_bytes_received;
  stats["advertising_time_sec"] = status.advertising_time_ms / 1000;
  stats["connected_time_sec"] = status.connected_time_ms / 1000;

  String buffer;
  if (!buffer.reserve(2048)) {
    return send_error(req, "Memory allocation failed");
  }
  serializeJson(doc, buffer);
  return send_json_response(req, buffer.c_str());
}

// POST /api/bluetooth/enable - Enable Bluetooth
inline esp_err_t handle_bluetooth_enable(httpd_req_t* req) {
  if (bluetooth_channel::enable()) {
    return send_success(req, "Bluetooth enabled");
  }
  return send_error(req, "Failed to enable Bluetooth");
}

// POST /api/bluetooth/disable - Disable Bluetooth
inline esp_err_t handle_bluetooth_disable(httpd_req_t* req) {
  bluetooth_channel::disable();
  return send_success(req, "Bluetooth disabled");
}

// POST /api/bluetooth/advertise/start - Start advertising
inline esp_err_t handle_bluetooth_advertise_start(httpd_req_t* req) {
  if (bluetooth_channel::start_advertising()) {
    return send_success(req, "Advertising started");
  }
  return send_error(req, "Failed to start advertising");
}

// POST /api/bluetooth/advertise/stop - Stop advertising
inline esp_err_t handle_bluetooth_advertise_stop(httpd_req_t* req) {
  bluetooth_channel::stop_advertising();
  return send_success(req, "Advertising stopped");
}

// POST /api/bluetooth/scan/start - Start scanning
inline esp_err_t handle_bluetooth_scan_start(httpd_req_t* req) {
  // Read optional duration from body
  uint32_t duration_ms = bluetooth_channel::SCAN_DURATION_MS;

  char content[64];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len > 0) {
    content[content_len] = '\0';
    StaticJsonDocument<64> input;
    if (deserializeJson(input, content) == DeserializationError::Ok) {
      if (input.containsKey("duration_sec")) {
        duration_ms = input["duration_sec"].as<uint32_t>() * 1000;
      }
    }
  }

  if (bluetooth_channel::start_scan(duration_ms)) {
    StaticJsonDocument<128> doc;
    doc["success"] = true;
    doc["message"] = "Scan started";
    doc["duration_sec"] = duration_ms / 1000;

    char buffer[128];
    serializeJson(doc, buffer);
    return send_json_response(req, buffer);
  }
  return send_error(req, "Failed to start scan");
}

// POST /api/bluetooth/scan/stop - Stop scanning
inline esp_err_t handle_bluetooth_scan_stop(httpd_req_t* req) {
  bluetooth_channel::stop_scan();
  return send_success(req, "Scan stopped");
}

// GET /api/bluetooth/scan/results - Get scan results
inline esp_err_t handle_bluetooth_scan_results(httpd_req_t* req) {
  size_t count;
  const bluetooth_channel::ScannedDevice* devices = bluetooth_channel::get_scanned_devices(&count);

  DynamicJsonDocument doc(4096);
  doc["scanning"] = bluetooth_channel::is_scanning();
  doc["count"] = count;

  JsonArray arr = doc.createNestedArray("devices");
  for (size_t i = 0; i < count; i++) {
    JsonObject dev = arr.createNestedObject();

    char addr[18];
    bluetooth_channel::format_address(devices[i].address, addr);
    dev["address"] = addr;
    dev["name"] = devices[i].name;
    dev["rssi"] = devices[i].rssi;
    dev["type"] = bluetooth_channel::device_type_name(devices[i].type);
    dev["connectable"] = devices[i].connectable;
    dev["is_securacv"] = devices[i].has_securacv_service;
    dev["age_sec"] = (millis() - devices[i].last_seen_ms) / 1000;
  }

  String buffer;
  if (!buffer.reserve(4096)) {
    return send_error(req, "Memory allocation failed");
  }
  serializeJson(doc, buffer);
  return send_json_response(req, buffer.c_str());
}

// DELETE /api/bluetooth/scan/results - Clear scan results
inline esp_err_t handle_bluetooth_scan_clear(httpd_req_t* req) {
  bluetooth_channel::clear_scan_results();
  return send_success(req, "Scan results cleared");
}

// POST /api/bluetooth/pair/start - Start pairing mode
inline esp_err_t handle_bluetooth_pair_start(httpd_req_t* req) {
  if (bluetooth_channel::start_pairing()) {
    return send_success(req, "Pairing mode started");
  }
  return send_error(req, "Failed to start pairing");
}

// POST /api/bluetooth/pair/cancel - Cancel pairing
inline esp_err_t handle_bluetooth_pair_cancel(httpd_req_t* req) {
  bluetooth_channel::cancel_pairing();
  return send_success(req, "Pairing cancelled");
}

// POST /api/bluetooth/pair/confirm - Confirm pairing PIN
inline esp_err_t handle_bluetooth_pair_confirm(httpd_req_t* req) {
  char content[64];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  StaticJsonDocument<64> input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input.containsKey("pin")) {
    return send_error(req, "Missing 'pin' field");
  }

  uint32_t pin = input["pin"].as<uint32_t>();
  if (bluetooth_channel::confirm_pairing(pin)) {
    return send_success(req, "Pairing confirmed");
  }
  return send_error(req, "Invalid PIN");
}

// POST /api/bluetooth/pair/reject - Reject pairing
inline esp_err_t handle_bluetooth_pair_reject(httpd_req_t* req) {
  if (bluetooth_channel::reject_pairing()) {
    return send_success(req, "Pairing rejected");
  }
  return send_error(req, "No active pairing to reject");
}

// GET /api/bluetooth/paired - Get paired devices
inline esp_err_t handle_bluetooth_paired_list(httpd_req_t* req) {
  size_t count;
  const bluetooth_channel::PairedDevice* devices = bluetooth_channel::get_paired_devices(&count);

  DynamicJsonDocument doc(2048);
  doc["count"] = count;

  JsonArray arr = doc.createNestedArray("devices");
  for (size_t i = 0; i < count; i++) {
    JsonObject dev = arr.createNestedObject();

    char addr[18];
    bluetooth_channel::format_address(devices[i].address, addr);
    dev["address"] = addr;
    dev["name"] = devices[i].name;
    dev["paired_timestamp"] = devices[i].paired_timestamp;
    dev["last_connected_sec"] = (millis() - devices[i].last_connected_ms) / 1000;
    dev["connection_count"] = devices[i].connection_count;
    dev["security"] = bluetooth_channel::security_level_name(devices[i].security);
    dev["trusted"] = devices[i].trusted;
    dev["blocked"] = devices[i].blocked;
  }

  String buffer;
  if (!buffer.reserve(2048)) {
    return send_error(req, "Memory allocation failed");
  }
  serializeJson(doc, buffer);
  return send_json_response(req, buffer.c_str());
}

// DELETE /api/bluetooth/paired - Remove a paired device
inline esp_err_t handle_bluetooth_paired_remove(httpd_req_t* req) {
  char content[64];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  StaticJsonDocument<64> input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input.containsKey("address")) {
    return send_error(req, "Missing 'address' field");
  }

  uint8_t addr[6];
  if (!bluetooth_channel::parse_address(input["address"].as<const char*>(), addr)) {
    return send_error(req, "Invalid address format");
  }

  if (bluetooth_channel::remove_paired_device(addr)) {
    return send_success(req, "Device removed");
  }
  return send_error(req, "Device not found");
}

// DELETE /api/bluetooth/paired/all - Clear all paired devices
inline esp_err_t handle_bluetooth_paired_clear(httpd_req_t* req) {
  if (bluetooth_channel::clear_all_paired_devices()) {
    return send_success(req, "All paired devices cleared");
  }
  return send_error(req, "Failed to clear paired devices");
}

// POST /api/bluetooth/paired/trust - Set device trust status
inline esp_err_t handle_bluetooth_paired_trust(httpd_req_t* req) {
  char content[128];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  StaticJsonDocument<128> input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input.containsKey("address") || !input.containsKey("trusted")) {
    return send_error(req, "Missing 'address' or 'trusted' field");
  }

  uint8_t addr[6];
  if (!bluetooth_channel::parse_address(input["address"].as<const char*>(), addr)) {
    return send_error(req, "Invalid address format");
  }

  bool trusted = input["trusted"].as<bool>();
  if (bluetooth_channel::set_device_trusted(addr, trusted)) {
    return send_success(req, trusted ? "Device trusted" : "Device untrusted");
  }
  return send_error(req, "Device not found");
}

// POST /api/bluetooth/paired/block - Set device block status
inline esp_err_t handle_bluetooth_paired_block(httpd_req_t* req) {
  char content[128];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  StaticJsonDocument<128> input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input.containsKey("address") || !input.containsKey("blocked")) {
    return send_error(req, "Missing 'address' or 'blocked' field");
  }

  uint8_t addr[6];
  if (!bluetooth_channel::parse_address(input["address"].as<const char*>(), addr)) {
    return send_error(req, "Invalid address format");
  }

  bool blocked = input["blocked"].as<bool>();
  if (bluetooth_channel::set_device_blocked(addr, blocked)) {
    return send_success(req, blocked ? "Device blocked" : "Device unblocked");
  }
  return send_error(req, "Device not found");
}

// POST /api/bluetooth/disconnect - Disconnect current connection
inline esp_err_t handle_bluetooth_disconnect(httpd_req_t* req) {
  if (bluetooth_channel::disconnect()) {
    return send_success(req, "Disconnected");
  }
  return send_error(req, "No active connection");
}

// GET /api/bluetooth/settings - Get Bluetooth settings
inline esp_err_t handle_bluetooth_settings_get(httpd_req_t* req) {
  bluetooth_channel::BluetoothSettings settings = bluetooth_channel::get_settings();

  StaticJsonDocument<512> doc;
  doc["enabled"] = settings.enabled;
  doc["auto_advertise"] = settings.auto_advertise;
  doc["allow_pairing"] = settings.allow_pairing;
  doc["require_pin"] = settings.require_pin;
  doc["device_name"] = settings.device_name;
  doc["tx_power"] = settings.tx_power;
  doc["inactivity_timeout_sec"] = settings.inactivity_timeout_ms / 1000;
  doc["notify_on_connect"] = settings.notify_on_connect;

  char buffer[512];
  serializeJson(doc, buffer);
  return send_json_response(req, buffer);
}

// POST /api/bluetooth/settings - Update Bluetooth settings
inline esp_err_t handle_bluetooth_settings_set(httpd_req_t* req) {
  char content[512];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  StaticJsonDocument<512> input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  bluetooth_channel::BluetoothSettings settings = bluetooth_channel::get_settings();

  if (input.containsKey("enabled")) {
    settings.enabled = input["enabled"].as<bool>();
  }
  if (input.containsKey("auto_advertise")) {
    settings.auto_advertise = input["auto_advertise"].as<bool>();
  }
  if (input.containsKey("allow_pairing")) {
    settings.allow_pairing = input["allow_pairing"].as<bool>();
  }
  if (input.containsKey("require_pin")) {
    settings.require_pin = input["require_pin"].as<bool>();
  }
  if (input.containsKey("device_name")) {
    strncpy(settings.device_name, input["device_name"].as<const char*>(),
            bluetooth_channel::MAX_DEVICE_NAME_LEN);
    settings.device_name[bluetooth_channel::MAX_DEVICE_NAME_LEN] = '\0';
  }
  if (input.containsKey("tx_power")) {
    settings.tx_power = input["tx_power"].as<int8_t>();
  }
  if (input.containsKey("inactivity_timeout_sec")) {
    settings.inactivity_timeout_ms = input["inactivity_timeout_sec"].as<uint32_t>() * 1000;
  }
  if (input.containsKey("notify_on_connect")) {
    settings.notify_on_connect = input["notify_on_connect"].as<bool>();
  }

  if (bluetooth_channel::set_settings(settings)) {
    return send_success(req, "Settings updated");
  }
  return send_error(req, "Failed to update settings");
}

// POST /api/bluetooth/name - Set device name
inline esp_err_t handle_bluetooth_name_set(httpd_req_t* req) {
  char content[128];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  StaticJsonDocument<128> input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input.containsKey("name")) {
    return send_error(req, "Missing 'name' field");
  }

  const char* name = input["name"].as<const char*>();
  if (bluetooth_channel::set_device_name(name)) {
    return send_success(req, "Device name updated");
  }
  return send_error(req, "Invalid name");
}

// POST /api/bluetooth/power - Set TX power
inline esp_err_t handle_bluetooth_power_set(httpd_req_t* req) {
  char content[64];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  StaticJsonDocument<64> input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input.containsKey("power")) {
    return send_error(req, "Missing 'power' field");
  }

  int8_t power = input["power"].as<int8_t>();
  if (bluetooth_channel::set_tx_power(power)) {
    StaticJsonDocument<128> doc;
    doc["success"] = true;
    doc["message"] = "TX power updated";
    doc["power"] = power;

    char buffer[128];
    serializeJson(doc, buffer);
    return send_json_response(req, buffer);
  }
  return send_error(req, "Invalid power level (-12 to +9 dBm)");
}

// ════════════════════════════════════════════════════════════════════════════
// ROUTE REGISTRATION
// ════════════════════════════════════════════════════════════════════════════

// Helper to reduce boilerplate in route registration
static inline void register_api_handler(httpd_handle_t server, const char* uri,
                                        httpd_method_t method,
                                        esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t route = { .uri = uri, .method = method, .handler = handler, .user_ctx = nullptr };
  httpd_register_uri_handler(server, &route);
}

// Call this to register all Bluetooth API routes with the HTTP server
inline void register_routes(httpd_handle_t server) {
  // GET endpoints
  register_api_handler(server, "/api/bluetooth", HTTP_GET, handle_bluetooth_status);
  register_api_handler(server, "/api/bluetooth/scan/results", HTTP_GET, handle_bluetooth_scan_results);
  register_api_handler(server, "/api/bluetooth/paired", HTTP_GET, handle_bluetooth_paired_list);
  register_api_handler(server, "/api/bluetooth/settings", HTTP_GET, handle_bluetooth_settings_get);

  // POST endpoints
  register_api_handler(server, "/api/bluetooth/enable", HTTP_POST, handle_bluetooth_enable);
  register_api_handler(server, "/api/bluetooth/disable", HTTP_POST, handle_bluetooth_disable);
  register_api_handler(server, "/api/bluetooth/advertise/start", HTTP_POST, handle_bluetooth_advertise_start);
  register_api_handler(server, "/api/bluetooth/advertise/stop", HTTP_POST, handle_bluetooth_advertise_stop);
  register_api_handler(server, "/api/bluetooth/scan/start", HTTP_POST, handle_bluetooth_scan_start);
  register_api_handler(server, "/api/bluetooth/scan/stop", HTTP_POST, handle_bluetooth_scan_stop);
  register_api_handler(server, "/api/bluetooth/pair/start", HTTP_POST, handle_bluetooth_pair_start);
  register_api_handler(server, "/api/bluetooth/pair/cancel", HTTP_POST, handle_bluetooth_pair_cancel);
  register_api_handler(server, "/api/bluetooth/pair/confirm", HTTP_POST, handle_bluetooth_pair_confirm);
  register_api_handler(server, "/api/bluetooth/pair/reject", HTTP_POST, handle_bluetooth_pair_reject);
  register_api_handler(server, "/api/bluetooth/disconnect", HTTP_POST, handle_bluetooth_disconnect);
  register_api_handler(server, "/api/bluetooth/settings", HTTP_POST, handle_bluetooth_settings_set);
  register_api_handler(server, "/api/bluetooth/name", HTTP_POST, handle_bluetooth_name_set);
  register_api_handler(server, "/api/bluetooth/power", HTTP_POST, handle_bluetooth_power_set);
  register_api_handler(server, "/api/bluetooth/paired/trust", HTTP_POST, handle_bluetooth_paired_trust);
  register_api_handler(server, "/api/bluetooth/paired/block", HTTP_POST, handle_bluetooth_paired_block);

  // DELETE endpoints
  register_api_handler(server, "/api/bluetooth/scan/results", HTTP_DELETE, handle_bluetooth_scan_clear);
  register_api_handler(server, "/api/bluetooth/paired", HTTP_DELETE, handle_bluetooth_paired_remove);
  register_api_handler(server, "/api/bluetooth/paired/all", HTTP_DELETE, handle_bluetooth_paired_clear);
}

} // namespace bluetooth_api

#endif // SECURACV_BLUETOOTH_API_H
