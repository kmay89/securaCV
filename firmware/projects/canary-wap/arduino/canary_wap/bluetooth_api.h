/*
 * SecuraCV Canary — Bluetooth REST API Handlers
 *
 * HTTP handlers for Bluetooth Low Energy management endpoints.
 * Enables mobile app connectivity and device management.
 *
 * All handlers follow the same pattern as other API handlers.
 *
 * Sweep F111: the handlers run on esp_http_server's task, and the state a
 * POST or DELETE changes (the pairing session and the pending
 * Numeric-Comparison pairing, the scan, the connection, the settings) is
 * also bluetooth_channel::update()'s, on the loop task, with no lock. So no
 * handler here changes it: each validates its body as before, hands a
 * bluetooth_channel::Command to bluetooth_channel::submit() and waits (up
 * to 2 s for the loop task to start it), then answers from the Result in
 * the shape it always had. A command that did not run answers 409
 * bluetooth_busy or 503 bluetooth_timeout (send_not_run). The one call a
 * handler still makes itself is bluetooth_channel::init() (bring_up), when
 * the owner turns Bluetooth on before it is up: it can block past the loop
 * task's watchdog, so it never runs there. firmware/scripts/
 * check_wap_loop_commands.py holds every handler here to that.
 *
 * Sweep F138: the GET routes read what the loop task last published
 * (bluetooth_channel::read_status, read_settings, read_scan, read_paired),
 * never the live state, which the loop task changes (and, before F143, the
 * NimBLE host task did): a read in place could mix two passes. They never
 * wait for the loop task. Every answer keeps its shape.
 */

#ifndef SECURACV_BLUETOOTH_API_H
#define SECURACV_BLUETOOTH_API_H

#include "esp_http_server.h"
#include "bluetooth_channel.h"
#include "ble_ota.h"
#include "api_auth.h"
#include "http_status_line.h"
#include <ArduinoJson.h>

namespace bluetooth_api {

// ════════════════════════════════════════════════════════════════════════════
// AUTH GATE
// ════════════════════════════════════════════════════════════════════════════
//
// Every /api/bluetooth/* endpoint in this module is post-setup-only — BLE
// initializes after WiFi is up, so there's no captive-portal flow that
// needs to hit these routes without an API token. The dashboard's
// secureFetch already attaches `Authorization: Bearer <api_token>` on
// every call, so wrapping handlers in api_auth_check is a plumbing-only
// fix (mirrors #435/#436 for the wifi-mgmt + mesh families).
//
// Mechanism: register_routes() stashes the API token pointer in a
// module-local static, then wraps every real handler in the bt_auth_gated
// function template. The trampoline runs api_auth_check + delegates;
// non-authenticated callers receive the standard 401 response that
// api_auth_check writes back, and the real handler never runs.
//
// The template is instantiated once per real handler at compile time, so
// the cost is one extra function-pointer indirection per request (the
// trampoline) and zero extra heap state.
inline const char*& auth_token_storage() {
  static const char* token = nullptr;
  return token;
}

template<esp_err_t (*Real)(httpd_req_t*)>
static esp_err_t bt_auth_gated(httpd_req_t* req) {
  const char* tok = auth_token_storage();
  // tok == nullptr means register_routes was called without a token —
  // historical signature. Keep the open behavior in that case so a
  // miswired caller doesn't lock itself out, but every in-tree caller
  // now supplies a real token (see canary_wap.ino).
  if (tok && !api_auth_check(req, tok)) return ESP_OK;
  return Real(req);
}

// ════════════════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

static inline esp_err_t send_json_response(httpd_req_t* req, const char* json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, json);
}

// Every answer here goes out at its own length (sweep F196). serializeJson()
// into a char array writes no further than the array, but terminates only an
// answer shorter than it (ArduinoJson 7.4.1), and httpd_resp_sendstr() then
// sends a cut answer and whatever memory follows it, up to the next zero
// byte: the Chirp confirm and mute refusals went out that way (F174). The
// fixed buffers this file had all held their longest answers (an ArduinoJson
// 7.4.1 scratch harness of these handlers: the init error at most 112 of 128
// bytes, the OTA status 261 of 320 with a 63-byte error of escaped bytes, the
// settings 261 of 512 with a 32-byte name of escaped bytes), but a longer
// message or a new key would not have said so. So each answer is measured
// (measureJson()) and serialized into a String reserved to that length, a
// failed allocation answered with the fixed error below, never a cut one.
// firmware/scripts/check_wap_loop_commands.py (rule BV6) holds every
// serializeJson() of this file to send_doc().
static inline esp_err_t send_doc(httpd_req_t* req, const JsonDocument& doc) {
  String out;
  if (!out.reserve(measureJson(doc) + 1)) {
    return send_json_response(req, "{\"success\":false,\"error\":\"Memory allocation failed\"}");
  }
  serializeJson(doc, out);
  return send_json_response(req, out.c_str());
}

static inline esp_err_t send_success(httpd_req_t* req, const char* message = nullptr) {
  JsonDocument doc;
  doc["success"] = true;
  if (message) doc["message"] = message;
  return send_doc(req, doc);
}

static inline esp_err_t send_error(httpd_req_t* req, const char* error) {
  JsonDocument doc;
  doc["success"] = false;
  doc["error"] = error;
  return send_doc(req, doc);
}

// ════════════════════════════════════════════════════════════════════════════
// API HANDLERS
// ════════════════════════════════════════════════════════════════════════════

// GET /api/bluetooth - Bluetooth status (the last pass published, F138)
inline esp_err_t handle_bluetooth_status(httpd_req_t* req) {
  bluetooth_channel::BluetoothStatus status;
  bluetooth_channel::read_status(&status);

  JsonDocument doc;

  doc["state"] = bluetooth_channel::state_name(status.state);
  doc["enabled"] = status.enabled;
  doc["advertising"] = status.advertising;
  // Why the radio is off, when init was refused/failed — lets the settings
  // tab say "internal RAM too fragmented" instead of a dead-looking panel.
  if (bluetooth_channel::init_fail_reason()[0]) {
    doc["init_fail_reason"] = bluetooth_channel::init_fail_reason();
  }
  doc["scanning"] = status.scanning;
  doc["connected"] = status.connected;
  doc["device_name"] = status.device_name;
  doc["local_address"] = status.local_address;
  doc["tx_power"] = status.tx_power;
  doc["mtu"] = status.mtu;
  doc["battery_pct"] = status.battery_pct;
  doc["paired_count"] = status.paired_count;
  doc["scanned_count"] = status.scanned_count;

  // Connection info
  if (status.connected) {
    JsonObject conn = doc["connection"].to<JsonObject>();
    char addr[18];
    bluetooth_channel::format_address(status.connection.address, addr);
    conn["address"] = addr;
    conn["name"] = status.connection.name;
    conn["rssi"] = status.connection.rssi;
    // Coarse distance hint, Find-My style. Treat as a "near/nearby/far"
    // signal — BLE RSSI is too noisy for survey-grade distance.
    float dist = bluetooth_channel::estimate_distance_m(status.connection.rssi);
    conn["distance_m"] = dist;
    conn["distance_label"] = bluetooth_channel::distance_label(dist);
    conn["security"] = bluetooth_channel::security_level_name(status.connection.security);
    conn["connected_sec"] = (millis() - status.connection.connected_since_ms) / 1000;
    conn["bytes_sent"] = status.connection.bytes_sent;
    conn["bytes_received"] = status.connection.bytes_received;
  }

  // Pairing info
  if (status.pairing.state != bluetooth_channel::PAIR_NONE) {
    JsonObject pair = doc["pairing"].to<JsonObject>();
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
  JsonObject stats = doc["stats"].to<JsonObject>();
  stats["total_connections"] = status.total_connections;
  stats["total_bytes_sent"] = status.total_bytes_sent;
  stats["total_bytes_received"] = status.total_bytes_received;
  stats["advertising_time_sec"] = status.advertising_time_ms / 1000;
  stats["connected_time_sec"] = status.connected_time_ms / 1000;

  return send_doc(req, doc);
}

// GET /api/bluetooth/ota - BLE OTA session status
inline esp_err_t handle_bluetooth_ota_status(httpd_req_t* req) {
  JsonDocument doc;
  doc["state"] = ble_ota::state_name(ble_ota::get_state());
  doc["progress_pct"] = ble_ota::get_progress_percent();
  doc["image_size"] = ble_ota::get_image_size();
  doc["bytes_received"] = ble_ota::get_bytes_received();
  const char* err = ble_ota::last_error();
  if (err && err[0]) doc["last_error"] = err;
  // True when the most recent accepted BEGIN went through break-glass
  // (owner-armed rescue past the anti-rollback floor) — an operator
  // reading the dashboard should not have to find the health log first.
  doc["break_glass"] = ble_ota::last_break_glass();

  // At its own length (send_doc(), F196): with the widest numbers and a
  // 63-byte error of bytes JSON escapes it is 261 bytes.
  return send_doc(req, doc);
}

// Compose an operator-actionable error. When the radio never initialized,
// the recorded refusal reason ("internal RAM too low (largest block N KB…)")
// beats a generic verb or the old stale "check antenna / NimBLE library"
// guess — the guard's verdict is the actual answer to "why won't it start".
inline esp_err_t send_bt_error(httpd_req_t* req, const char* fallback) {
  const char* reason = bluetooth_channel::init_fail_reason();
  if (!bluetooth_channel::is_initialized() && reason[0]) {
    return send_error(req, reason);
  }
  return send_error(req, fallback);
}

// ── The loop task's answer (sweep F111) ────────────────────────────────

// A command the loop task did not run: every slot was taken (409
// bluetooth_busy), or it did not start within its wait and was withdrawn
// (503 bluetooth_timeout). Nothing changed. The body is the routes' error
// shape, which the dashboard reads.
inline esp_err_t send_not_run(httpd_req_t* req, loop_command_ring::Wait w) {
  httpd_resp_set_status(req, http_status_line(bluetooth_channel::not_run_status(w)));
  return send_error(req, bluetooth_channel::not_run_error(w));
}

// The stack, brought up on this task when the owner turns Bluetooth on
// before it is up: the init() bluetooth_channel::enable() ran here before
// F111. A command never runs it: NimBLE init can block past the loop task's
// watchdog (the BLE bring-up worker's note in canary_wap.ino). init() admits
// one caller at a time (the bring-up worker may be inside it).
inline bool bring_up() {
  return bluetooth_channel::is_initialized() || bluetooth_channel::init();
}

// POST /api/bluetooth/enable - Enable Bluetooth
inline esp_err_t handle_bluetooth_enable(httpd_req_t* req) {
  if (!bring_up()) {
    return send_bt_error(req, "Failed to enable Bluetooth");
  }
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_ENABLE), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
    return send_success(req, "Bluetooth enabled");
  }
  return send_bt_error(req, "Failed to enable Bluetooth");
}

// POST /api/bluetooth/disable - Disable Bluetooth
inline esp_err_t handle_bluetooth_disable(httpd_req_t* req) {
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_DISABLE), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  return send_success(req, "Bluetooth disabled");
}

// POST /api/bluetooth/advertise/start - Start advertising
inline esp_err_t handle_bluetooth_advertise_start(httpd_req_t* req) {
  // Auto-enable: clicking "Start Advertising" is unambiguous user intent.
  // Without this the call silently returns false when enabled=false in NVS.
  // The stack comes up here; the enable itself is the command's.
  if (!bluetooth_channel::is_enabled() && !bring_up()) {
    return send_bt_error(req, "Bluetooth init failed");
  }
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_ADVERTISE_START), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.refusal == bluetooth_channel::BT_REFUSED_NOT_ENABLED) {
    return send_bt_error(req, "Bluetooth init failed");
  }
  // The pre-condition, checked by the command before the start call so the
  // error message is specific and we avoid a no-op state transition.
  if (r.refusal == bluetooth_channel::BT_REFUSED_CONNECTED) {
    return send_error(req, "Cannot advertise while a device is connected");
  }
  if (r.ok) {
    return send_success(req, "Advertising started");
  }
  return send_error(req, "Failed to start advertising");
}

// POST /api/bluetooth/advertise/stop - Stop advertising
inline esp_err_t handle_bluetooth_advertise_stop(httpd_req_t* req) {
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_ADVERTISE_STOP), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
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
    JsonDocument input;
    if (deserializeJson(input, content) == DeserializationError::Ok) {
      if (input["duration_sec"].is<JsonVariant>()) {
        duration_ms = input["duration_sec"].as<uint32_t>() * 1000;
      }
    }
  }

  bluetooth_channel::Command cmd = bluetooth_channel::make_command(bluetooth_channel::BT_CMD_SCAN_START);
  cmd.duration_ms = duration_ms;
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(cmd, &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Scan started";
    doc["duration_sec"] = duration_ms / 1000;
    return send_doc(req, doc);
  }
  return send_bt_error(req, "Failed to start scan");
}

// POST /api/bluetooth/scan/stop - Stop scanning
inline esp_err_t handle_bluetooth_scan_stop(httpd_req_t* req) {
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_SCAN_STOP), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  return send_success(req, "Scan stopped");
}

// GET /api/bluetooth/scan/results - Get scan results (the last pass published, F138)
inline esp_err_t handle_bluetooth_scan_results(httpd_req_t* req) {
  bluetooth_channel::ScanView view;
  bluetooth_channel::read_scan(&view);
  const size_t count = view.count;
  const bluetooth_channel::ScannedDevice* devices = view.devices;

  JsonDocument doc;
  doc["scanning"] = view.scanning;
  doc["count"] = count;

  JsonArray arr = doc["devices"].to<JsonArray>();
  for (size_t i = 0; i < count; i++) {
    JsonObject dev = arr.add<JsonObject>();

    char addr[18];
    bluetooth_channel::format_address(devices[i].address, addr);
    dev["address"] = addr;
    dev["name"] = devices[i].name;
    dev["rssi"] = devices[i].rssi;
    // Coarse Find-My-style distance hint. BLE RSSI is noisy enough that we
    // surface both the raw dBm and a friendly bucket so the UI can pick.
    float dist = bluetooth_channel::estimate_distance_m(devices[i].rssi);
    dev["distance_m"] = dist;
    dev["distance_label"] = bluetooth_channel::distance_label(dist);
    dev["type"] = bluetooth_channel::device_type_name(devices[i].type);
    dev["connectable"] = devices[i].connectable;
    dev["is_securacv"] = devices[i].has_securacv_service;
    dev["age_sec"] = (millis() - devices[i].last_seen_ms) / 1000;
  }

  return send_doc(req, doc);
}

// DELETE /api/bluetooth/scan/results - Clear scan results
inline esp_err_t handle_bluetooth_scan_clear(httpd_req_t* req) {
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_SCAN_CLEAR), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  return send_success(req, "Scan results cleared");
}

// POST /api/bluetooth/pair/start - Start pairing mode
inline esp_err_t handle_bluetooth_pair_start(httpd_req_t* req) {
  // Auto-enable: same rationale as advertise/start.
  if (!bluetooth_channel::is_enabled() && !bring_up()) {
    return send_bt_error(req, "Bluetooth init failed");
  }
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_PAIR_START), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.refusal == bluetooth_channel::BT_REFUSED_NOT_ENABLED) {
    return send_bt_error(req, "Bluetooth init failed");
  }
  if (r.ok) {
    return send_success(req, "Pairing mode started");
  }
  if (!r.allow_pairing) {
    return send_error(req, "Pairing disabled in settings — toggle 'Allow new pairings' first");
  }
  return send_error(req, "Failed to start pairing");
}

// POST /api/bluetooth/pair/cancel - Cancel pairing
inline esp_err_t handle_bluetooth_pair_cancel(httpd_req_t* req) {
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_PAIR_CANCEL), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  return send_success(req, "Pairing canceled");
}

// POST /api/bluetooth/pair/confirm - Confirm pairing PIN
inline esp_err_t handle_bluetooth_pair_confirm(httpd_req_t* req) {
  char content[64];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  JsonDocument input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input["pin"].is<JsonVariant>()) {
    return send_error(req, "Missing 'pin' field");
  }

  bluetooth_channel::Command cmd = bluetooth_channel::make_command(bluetooth_channel::BT_CMD_PAIR_CONFIRM);
  cmd.pin = input["pin"].as<uint32_t>();
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(cmd, &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
    return send_success(req, "Pairing confirmed");
  }
  return send_error(req, "Invalid PIN");
}

// POST /api/bluetooth/pair/reject - Reject pairing
inline esp_err_t handle_bluetooth_pair_reject(httpd_req_t* req) {
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_PAIR_REJECT), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
    return send_success(req, "Pairing rejected");
  }
  return send_error(req, "No active pairing to reject");
}

// GET /api/bluetooth/paired - Get paired devices (the last pass published, F138)
inline esp_err_t handle_bluetooth_paired_list(httpd_req_t* req) {
  bluetooth_channel::PairedView view;
  bluetooth_channel::read_paired(&view);
  const size_t count = view.count;
  const bluetooth_channel::PairedDevice* devices = view.devices;

  JsonDocument doc;
  doc["count"] = count;

  JsonArray arr = doc["devices"].to<JsonArray>();
  for (size_t i = 0; i < count; i++) {
    JsonObject dev = arr.add<JsonObject>();

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

  return send_doc(req, doc);
}

// DELETE /api/bluetooth/paired - Remove a paired device
inline esp_err_t handle_bluetooth_paired_remove(httpd_req_t* req) {
  char content[64];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  JsonDocument input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input["address"].is<JsonVariant>()) {
    return send_error(req, "Missing 'address' field");
  }

  bluetooth_channel::Command cmd = bluetooth_channel::make_command(bluetooth_channel::BT_CMD_PAIRED_REMOVE);
  if (!bluetooth_channel::parse_address(input["address"].as<const char*>(), cmd.address)) {
    return send_error(req, "Invalid address format");
  }

  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(cmd, &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
    return send_success(req, "Device removed");
  }
  if (r.refusal == bluetooth_channel::BT_REFUSED_BOND_KEPT) {
    return send_error(req, "The phone's bond was not removed; try again");
  }
  if (r.refusal == bluetooth_channel::BT_REFUSED_NOT_UP) {
    return send_error(req, "Bluetooth is not up yet; nothing was removed");
  }
  return send_error(req, "Device not found");
}

// DELETE /api/bluetooth/paired/all - Clear all paired devices
inline esp_err_t handle_bluetooth_paired_clear(httpd_req_t* req) {
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_PAIRED_CLEAR), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
    return send_success(req, "All paired devices cleared");
  }
  if (r.refusal == bluetooth_channel::BT_REFUSED_BOND_KEPT) {
    return send_error(req, "Not every phone's bond was removed; try again");
  }
  if (r.refusal == bluetooth_channel::BT_REFUSED_NOT_UP) {
    return send_error(req, "Bluetooth is not up yet; nothing was cleared");
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

  JsonDocument input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input["address"].is<JsonVariant>() || !input["trusted"].is<JsonVariant>()) {
    return send_error(req, "Missing 'address' or 'trusted' field");
  }

  bluetooth_channel::Command cmd = bluetooth_channel::make_command(bluetooth_channel::BT_CMD_PAIRED_TRUST);
  if (!bluetooth_channel::parse_address(input["address"].as<const char*>(), cmd.address)) {
    return send_error(req, "Invalid address format");
  }

  bool trusted = input["trusted"].as<bool>();
  cmd.flag = trusted;
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(cmd, &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
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

  JsonDocument input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input["address"].is<JsonVariant>() || !input["blocked"].is<JsonVariant>()) {
    return send_error(req, "Missing 'address' or 'blocked' field");
  }

  bluetooth_channel::Command cmd = bluetooth_channel::make_command(bluetooth_channel::BT_CMD_PAIRED_BLOCK);
  if (!bluetooth_channel::parse_address(input["address"].as<const char*>(), cmd.address)) {
    return send_error(req, "Invalid address format");
  }

  bool blocked = input["blocked"].as<bool>();
  cmd.flag = blocked;
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(cmd, &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
    return send_success(req, blocked ? "Device blocked" : "Device unblocked");
  }
  return send_error(req, "Device not found");
}

// POST /api/bluetooth/disconnect - Disconnect current connection
inline esp_err_t handle_bluetooth_disconnect(httpd_req_t* req) {
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(
      bluetooth_channel::make_command(bluetooth_channel::BT_CMD_DISCONNECT), &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
    return send_success(req, "Disconnected");
  }
  return send_error(req, "No active connection");
}

// GET /api/bluetooth/settings - Get Bluetooth settings (the last pass published, F138)
inline esp_err_t handle_bluetooth_settings_get(httpd_req_t* req) {
  bluetooth_channel::BluetoothSettings settings = bluetooth_channel::read_settings();

  JsonDocument doc;
  doc["enabled"] = settings.enabled;
  doc["auto_advertise"] = settings.auto_advertise;
  doc["allow_pairing"] = settings.allow_pairing;
  doc["require_pin"] = settings.require_pin;
  doc["device_name"] = settings.device_name;
  doc["tx_power"] = settings.tx_power;
  doc["inactivity_timeout_sec"] = settings.inactivity_timeout_ms / 1000;
  doc["notify_on_connect"] = settings.notify_on_connect;
  doc["long_range_mode"] = settings.long_range_mode;
  return send_doc(req, doc);
}

// POST /api/bluetooth/settings - Update Bluetooth settings
inline esp_err_t handle_bluetooth_settings_set(httpd_req_t* req) {
  char content[512];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    return send_error(req, "Missing request body");
  }
  content[content_len] = '\0';

  JsonDocument input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  // The fields the POST names; the loop task applies them to the settings
  // it holds (sweep F111).
  bluetooth_channel::Command cmd = bluetooth_channel::make_command(bluetooth_channel::BT_CMD_SETTINGS);
  bluetooth_channel::BluetoothSettings& settings = cmd.settings;

  if (input["enabled"].is<JsonVariant>()) {
    settings.enabled = input["enabled"].as<bool>();
    cmd.set_mask |= bluetooth_channel::BT_SET_ENABLED;
  }
  if (input["auto_advertise"].is<JsonVariant>()) {
    settings.auto_advertise = input["auto_advertise"].as<bool>();
    cmd.set_mask |= bluetooth_channel::BT_SET_AUTO_ADVERTISE;
  }
  if (input["allow_pairing"].is<JsonVariant>()) {
    settings.allow_pairing = input["allow_pairing"].as<bool>();
    cmd.set_mask |= bluetooth_channel::BT_SET_ALLOW_PAIRING;
  }
  if (input["require_pin"].is<JsonVariant>()) {
    settings.require_pin = input["require_pin"].as<bool>();
    cmd.set_mask |= bluetooth_channel::BT_SET_REQUIRE_PIN;
  }
  if (input["device_name"].is<JsonVariant>()) {
    const char* name = input["device_name"].as<const char*>();
    strncpy(settings.device_name, name != nullptr ? name : "",
            bluetooth_channel::MAX_DEVICE_NAME_LEN);
    settings.device_name[bluetooth_channel::MAX_DEVICE_NAME_LEN] = '\0';
    cmd.set_mask |= bluetooth_channel::BT_SET_DEVICE_NAME;
  }
  if (input["tx_power"].is<JsonVariant>()) {
    settings.tx_power = input["tx_power"].as<int8_t>();
    cmd.set_mask |= bluetooth_channel::BT_SET_TX_POWER;
  }
  if (input["inactivity_timeout_sec"].is<JsonVariant>()) {
    settings.inactivity_timeout_ms = input["inactivity_timeout_sec"].as<uint32_t>() * 1000;
    cmd.set_mask |= bluetooth_channel::BT_SET_INACTIVITY;
  }
  if (input["notify_on_connect"].is<JsonVariant>()) {
    settings.notify_on_connect = input["notify_on_connect"].as<bool>();
    cmd.set_mask |= bluetooth_channel::BT_SET_NOTIFY_ON_CONNECT;
  }
  if (input["long_range_mode"].is<JsonVariant>()) {
    settings.long_range_mode = input["long_range_mode"].as<bool>();
    cmd.set_mask |= bluetooth_channel::BT_SET_LONG_RANGE;
  }

  // "enabled": true turns Bluetooth on the way POST /api/bluetooth/enable
  // does (sweep F144): the stack comes up here, on this task, and the
  // command turns it on. A command never brings the stack up.
  if ((cmd.set_mask & bluetooth_channel::BT_SET_ENABLED) && settings.enabled && !bring_up()) {
    return send_bt_error(req, "Bluetooth init failed");
  }

  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(cmd, &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.refusal == bluetooth_channel::BT_REFUSED_NOT_ENABLED) {
    return send_bt_error(req, "Bluetooth init failed");
  }
  if (r.ok) {
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

  JsonDocument input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input["name"].is<JsonVariant>()) {
    return send_error(req, "Missing 'name' field");
  }

  // One byte past the longest name kept, so a name too long stays too long
  // and the command refuses it, as set_device_name() always did.
  const char* name = input["name"].as<const char*>();
  bluetooth_channel::Command cmd = bluetooth_channel::make_command(bluetooth_channel::BT_CMD_NAME);
  strncpy(cmd.name, name != nullptr ? name : "", sizeof(cmd.name) - 1);
  cmd.name[sizeof(cmd.name) - 1] = '\0';
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(cmd, &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
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

  JsonDocument input;
  if (deserializeJson(input, content) != DeserializationError::Ok) {
    return send_error(req, "Invalid JSON");
  }

  if (!input["power"].is<JsonVariant>()) {
    return send_error(req, "Missing 'power' field");
  }

  int8_t power = input["power"].as<int8_t>();
  bluetooth_channel::Command cmd = bluetooth_channel::make_command(bluetooth_channel::BT_CMD_POWER);
  cmd.power = power;
  bluetooth_channel::Result r;
  const loop_command_ring::Wait w = bluetooth_channel::submit(cmd, &r);
  if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);
  if (r.ok) {
    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "TX power updated";
    doc["power"] = power;
    return send_doc(req, doc);
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

// Call this to register all Bluetooth API routes with the HTTP server.
// api_token: pointer to the device's persistent api_token_str. Must outlive
// the HTTP server (in practice: backed by g_device, lives for the process).
// nullptr keeps the legacy open behavior for backwards compatibility.
inline void register_routes(httpd_handle_t server, const char* api_token = nullptr) {
  auth_token_storage() = api_token;

  // GET endpoints
  register_api_handler(server, "/api/bluetooth", HTTP_GET, bt_auth_gated<handle_bluetooth_status>);
  register_api_handler(server, "/api/bluetooth/scan/results", HTTP_GET, bt_auth_gated<handle_bluetooth_scan_results>);
  register_api_handler(server, "/api/bluetooth/paired", HTTP_GET, bt_auth_gated<handle_bluetooth_paired_list>);
  register_api_handler(server, "/api/bluetooth/settings", HTTP_GET, bt_auth_gated<handle_bluetooth_settings_get>);
  register_api_handler(server, "/api/bluetooth/ota", HTTP_GET, bt_auth_gated<handle_bluetooth_ota_status>);

  // POST endpoints
  register_api_handler(server, "/api/bluetooth/enable", HTTP_POST, bt_auth_gated<handle_bluetooth_enable>);
  register_api_handler(server, "/api/bluetooth/disable", HTTP_POST, bt_auth_gated<handle_bluetooth_disable>);
  register_api_handler(server, "/api/bluetooth/advertise/start", HTTP_POST, bt_auth_gated<handle_bluetooth_advertise_start>);
  register_api_handler(server, "/api/bluetooth/advertise/stop", HTTP_POST, bt_auth_gated<handle_bluetooth_advertise_stop>);
  register_api_handler(server, "/api/bluetooth/scan/start", HTTP_POST, bt_auth_gated<handle_bluetooth_scan_start>);
  register_api_handler(server, "/api/bluetooth/scan/stop", HTTP_POST, bt_auth_gated<handle_bluetooth_scan_stop>);
  register_api_handler(server, "/api/bluetooth/pair/start", HTTP_POST, bt_auth_gated<handle_bluetooth_pair_start>);
  register_api_handler(server, "/api/bluetooth/pair/cancel", HTTP_POST, bt_auth_gated<handle_bluetooth_pair_cancel>);
  register_api_handler(server, "/api/bluetooth/pair/confirm", HTTP_POST, bt_auth_gated<handle_bluetooth_pair_confirm>);
  register_api_handler(server, "/api/bluetooth/pair/reject", HTTP_POST, bt_auth_gated<handle_bluetooth_pair_reject>);
  register_api_handler(server, "/api/bluetooth/disconnect", HTTP_POST, bt_auth_gated<handle_bluetooth_disconnect>);
  register_api_handler(server, "/api/bluetooth/settings", HTTP_POST, bt_auth_gated<handle_bluetooth_settings_set>);
  register_api_handler(server, "/api/bluetooth/name", HTTP_POST, bt_auth_gated<handle_bluetooth_name_set>);
  register_api_handler(server, "/api/bluetooth/power", HTTP_POST, bt_auth_gated<handle_bluetooth_power_set>);
  register_api_handler(server, "/api/bluetooth/paired/trust", HTTP_POST, bt_auth_gated<handle_bluetooth_paired_trust>);
  register_api_handler(server, "/api/bluetooth/paired/block", HTTP_POST, bt_auth_gated<handle_bluetooth_paired_block>);

  // DELETE endpoints
  register_api_handler(server, "/api/bluetooth/scan/results", HTTP_DELETE, bt_auth_gated<handle_bluetooth_scan_clear>);
  register_api_handler(server, "/api/bluetooth/paired", HTTP_DELETE, bt_auth_gated<handle_bluetooth_paired_remove>);
  register_api_handler(server, "/api/bluetooth/paired/all", HTTP_DELETE, bt_auth_gated<handle_bluetooth_paired_clear>);
}

} // namespace bluetooth_api

#endif // SECURACV_BLUETOOTH_API_H
