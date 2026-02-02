/*
 * SecuraCV Canary — Chirp Channel REST API Handlers
 *
 * HTTP handlers for the Community Witness Network endpoints.
 * Template-based messaging — NO free text allowed.
 * Philosophy: "Witness authority, not neighbors"
 *
 * All handlers follow the same pattern as mesh API handlers.
 */

#ifndef SECURACV_CHIRP_API_H
#define SECURACV_CHIRP_API_H

#include "esp_http_server.h"
#include "mesh_network.h"
#include <ArduinoJson.h>

namespace chirp_api {

// ════════════════════════════════════════════════════════════════════════════
// API HANDLERS
// ════════════════════════════════════════════════════════════════════════════

// GET /api/chirp - Chirp channel status
inline esp_err_t handle_chirp_status(httpd_req_t* req) {
  chirp_channel::ChirpStatus status = chirp_channel::get_status();

  StaticJsonDocument<768> doc;
  doc["state"] = chirp_channel::state_name(status.state);
  doc["session_emoji"] = status.session_emoji;
  doc["nearby_count"] = status.nearby_count;
  doc["recent_chirps"] = status.recent_chirp_count;
  doc["last_chirp_sent_ms"] = status.last_chirp_sent_ms;
  doc["cooldown_remaining_sec"] = status.cooldown_remaining_ms / 1000;
  doc["cooldown_tier"] = chirp_channel::get_cooldown_tier();
  doc["presence_met"] = chirp_channel::has_presence_requirement();
  doc["night_mode"] = chirp_channel::is_night_mode();
  doc["relay_enabled"] = status.relay_enabled;
  doc["muted"] = status.muted;
  doc["mute_remaining_sec"] = status.mute_remaining_ms / 1000;
  doc["can_send"] = chirp_channel::can_send_chirp();

  // If can't send, explain why
  if (!chirp_channel::can_send_chirp()) {
    if (status.state == chirp_channel::CHIRP_DISABLED) {
      doc["cannot_send_reason"] = "disabled";
    } else if (status.state == chirp_channel::CHIRP_COOLDOWN) {
      doc["cannot_send_reason"] = "cooldown";
    } else if (!chirp_channel::has_presence_requirement()) {
      doc["cannot_send_reason"] = "presence_required";
    }
  }

  char buffer[768];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// GET /api/chirp/nearby - Count of nearby chirp devices
inline esp_err_t handle_chirp_nearby(httpd_req_t* req) {
  size_t count;
  const chirp_channel::NearbyDevice* devices = chirp_channel::get_nearby_devices(&count);

  StaticJsonDocument<1024> doc;
  doc["count"] = count;

  JsonArray arr = doc.createNestedArray("devices");
  for (size_t i = 0; i < count && i < 16; i++) {
    JsonObject dev = arr.createNestedObject();
    dev["emoji"] = devices[i].emoji;
    dev["age_sec"] = (millis() - devices[i].last_seen_ms) / 1000;
    dev["rssi"] = devices[i].rssi;
    dev["listening"] = devices[i].listening;
  }

  char buffer[1024];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// GET /api/chirp/recent - Recent community chirps
inline esp_err_t handle_chirp_recent(httpd_req_t* req) {
  size_t count;
  const chirp_channel::ReceivedChirp* chirps = chirp_channel::get_recent_chirps(&count);

  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.createNestedArray("chirps");

  for (size_t i = 0; i < count; i++) {
    if (chirps[i].dismissed) continue;

    JsonObject c = arr.createNestedObject();
    c["emoji"] = chirps[i].sender_emoji;

    // Template-based message (no free text)
    c["template_id"] = (uint8_t)chirps[i].template_id;
    c["template_text"] = chirp_channel::get_template_text(chirps[i].template_id);
    c["detail"] = chirp_channel::get_detail_text(chirps[i].detail);

    // Category derived from template
    chirp_channel::ChirpCategory cat = (chirp_channel::ChirpCategory)(
      (uint8_t)chirps[i].template_id >> 4);  // Category from high nibble
    c["category"] = chirp_channel::category_name(cat);
    c["urgency"] = chirp_channel::urgency_name(chirps[i].urgency);
    c["hop_count"] = chirps[i].hop_count;
    c["age_sec"] = (millis() - chirps[i].received_ms) / 1000;
    c["confirm_count"] = chirps[i].confirm_count;
    c["validated"] = chirps[i].validated;
    c["status"] = chirp_channel::get_validation_status(&chirps[i]);
    c["relayed"] = chirps[i].relayed;
    c["suppressed"] = chirps[i].suppressed;

    // Encode nonce as hex for referencing
    char nonce_hex[17];
    for (int j = 0; j < 8; j++) {
      sprintf(nonce_hex + j * 2, "%02x", chirps[i].nonce[j]);
    }
    c["nonce"] = nonce_hex;
  }

  char* buffer = (char*)malloc(4096);
  if (!buffer) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    return ESP_FAIL;
  }

  serializeJson(doc, buffer, 4096);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t ret = httpd_resp_sendstr(req, buffer);
  free(buffer);
  return ret;
}

// POST /api/chirp/enable - Enable chirp channel
inline esp_err_t handle_chirp_enable(httpd_req_t* req) {
  bool success = chirp_channel::enable();

  StaticJsonDocument<128> doc;
  doc["success"] = success;
  if (success) {
    doc["session_emoji"] = chirp_channel::get_session_emoji();
  }

  char buffer[128];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// POST /api/chirp/disable - Disable chirp channel
inline esp_err_t handle_chirp_disable(httpd_req_t* req) {
  chirp_channel::disable();

  StaticJsonDocument<64> doc;
  doc["success"] = true;

  char buffer[64];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// POST /api/chirp/send - Send a chirp using TEMPLATE (HUMAN CONFIRMATION REQUIRED)
// Request: { "template_id": 0, "urgency": "info", "detail": 0, "ttl_minutes": 15 }
// NO FREE TEXT ALLOWED - must use predefined template IDs
inline esp_err_t handle_chirp_send(httpd_req_t* req) {
  // Read request body
  char content[256];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing request body");
    return ESP_FAIL;
  }
  content[content_len] = '\0';

  // Parse JSON
  StaticJsonDocument<256> input;
  DeserializationError err = deserializeJson(input, content);
  if (err) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  // Extract template ID (required)
  if (!input.containsKey("template_id")) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "template_id is required");
    return ESP_FAIL;
  }
  uint8_t template_id_raw = input["template_id"];
  chirp_channel::ChirpTemplate template_id = (chirp_channel::ChirpTemplate)template_id_raw;

  // Validate template exists
  if (!chirp_channel::is_valid_template(template_id)) {
    StaticJsonDocument<256> doc;
    doc["success"] = false;
    doc["error"] = "invalid_template";
    doc["message"] = "Unknown template ID";
    char buffer[256];
    serializeJson(doc, buffer);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buffer);
  }

  // Extract optional fields
  const char* urgency_str = input["urgency"] | "info";
  uint8_t detail_raw = input["detail"] | 0;
  uint8_t ttl = input["ttl_minutes"] | 15;

  // Map urgency string to enum
  chirp_channel::ChirpUrgency urgency = chirp_channel::CHIRP_URG_INFO;
  if (strcmp(urgency_str, "caution") == 0) urgency = chirp_channel::CHIRP_URG_CAUTION;
  else if (strcmp(urgency_str, "urgent") == 0) urgency = chirp_channel::CHIRP_URG_URGENT;

  chirp_channel::ChirpDetailSlot detail = (chirp_channel::ChirpDetailSlot)detail_raw;

  // Attempt to send
  bool success = chirp_channel::send_chirp(template_id, urgency, detail, ttl);

  StaticJsonDocument<384> doc;
  doc["success"] = success;

  if (success) {
    doc["template_text"] = chirp_channel::get_template_text(template_id);
    doc["cooldown_tier"] = chirp_channel::get_cooldown_tier();
  } else {
    if (!chirp_channel::is_enabled()) {
      doc["error"] = "chirp_disabled";
      doc["message"] = "Chirp channel is not enabled";
    } else if (!chirp_channel::has_presence_requirement()) {
      doc["error"] = "presence_required";
      doc["message"] = "Must be active for 10 minutes before sending";
    } else if (!chirp_channel::can_send_chirp()) {
      doc["error"] = "cooldown";
      doc["message"] = "Please wait before sending another chirp";
      doc["cooldown_remaining_sec"] = chirp_channel::get_cooldown_remaining_ms() / 1000;
      doc["cooldown_tier"] = chirp_channel::get_cooldown_tier();
    } else if (chirp_channel::is_night_mode()) {
      doc["error"] = "night_restricted";
      doc["message"] = "This template is not available during night hours (10pm-6am)";
    }
  }

  char buffer[384];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// GET /api/chirp/templates - List available templates
inline esp_err_t handle_chirp_templates(httpd_req_t* req) {
  DynamicJsonDocument doc(4096);

  // Authority templates
  JsonArray auth = doc.createNestedArray("authority");
  auth.add(JsonObject());
  auth[0]["id"] = 0x00; auth[0]["text"] = "police activity in area";
  auth.add(JsonObject());
  auth[1]["id"] = 0x01; auth[1]["text"] = "heavy law enforcement response";
  auth.add(JsonObject());
  auth[2]["id"] = 0x02; auth[2]["text"] = "road blocked by law enforcement";
  auth.add(JsonObject());
  auth[3]["id"] = 0x03; auth[3]["text"] = "helicopter circling area";
  auth.add(JsonObject());
  auth[4]["id"] = 0x04; auth[4]["text"] = "federal agents in area";

  // Infrastructure templates
  JsonArray infra = doc.createNestedArray("infrastructure");
  infra.add(JsonObject());
  infra[0]["id"] = 0x10; infra[0]["text"] = "power outage";
  infra.add(JsonObject());
  infra[1]["id"] = 0x11; infra[1]["text"] = "water service disruption";
  infra.add(JsonObject());
  infra[2]["id"] = 0x12; infra[2]["text"] = "gas smell - evacuate?";
  infra.add(JsonObject());
  infra[3]["id"] = 0x13; infra[3]["text"] = "internet outage in area";
  infra.add(JsonObject());
  infra[4]["id"] = 0x14; infra[4]["text"] = "road closed or blocked";

  // Emergency templates
  JsonArray emerg = doc.createNestedArray("emergency");
  emerg.add(JsonObject());
  emerg[0]["id"] = 0x20; emerg[0]["text"] = "fire or smoke visible";
  emerg.add(JsonObject());
  emerg[1]["id"] = 0x21; emerg[1]["text"] = "medical emergency scene";
  emerg.add(JsonObject());
  emerg[2]["id"] = 0x22; emerg[2]["text"] = "multiple ambulances responding";
  emerg.add(JsonObject());
  emerg[3]["id"] = 0x23; emerg[3]["text"] = "evacuation in progress";
  emerg.add(JsonObject());
  emerg[4]["id"] = 0x24; emerg[4]["text"] = "shelter in place advisory";

  // Weather templates
  JsonArray weather = doc.createNestedArray("weather");
  weather.add(JsonObject());
  weather[0]["id"] = 0x30; weather[0]["text"] = "severe weather warning";
  weather.add(JsonObject());
  weather[1]["id"] = 0x31; weather[1]["text"] = "tornado warning";
  weather.add(JsonObject());
  weather[2]["id"] = 0x32; weather[2]["text"] = "flooding reported";
  weather.add(JsonObject());
  weather[3]["id"] = 0x33; weather[3]["text"] = "dangerous lightning nearby";

  // Mutual aid templates
  JsonArray aid = doc.createNestedArray("mutual_aid");
  aid.add(JsonObject());
  aid[0]["id"] = 0x40; aid[0]["text"] = "neighbor may need help";
  aid.add(JsonObject());
  aid[1]["id"] = 0x41; aid[1]["text"] = "supplies needed in area";
  aid.add(JsonObject());
  aid[2]["id"] = 0x42; aid[2]["text"] = "offering assistance";

  // All clear templates
  JsonArray clear = doc.createNestedArray("all_clear");
  clear.add(JsonObject());
  clear[0]["id"] = 0x80; clear[0]["text"] = "situation resolved";
  clear.add(JsonObject());
  clear[1]["id"] = 0x81; clear[1]["text"] = "area appears safe now";
  clear.add(JsonObject());
  clear[2]["id"] = 0x82; clear[2]["text"] = "false alarm";

  // Details
  JsonArray details = doc.createNestedArray("details");
  details.add(JsonObject());
  details[0]["id"] = 1; details[0]["text"] = "few vehicles";
  details.add(JsonObject());
  details[1]["id"] = 2; details[1]["text"] = "many vehicles";
  details.add(JsonObject());
  details[2]["id"] = 3; details[2]["text"] = "massive response";
  details.add(JsonObject());
  details[3]["id"] = 10; details[3]["text"] = "ongoing";
  details.add(JsonObject());
  details[4]["id"] = 11; details[4]["text"] = "contained";
  details.add(JsonObject());
  details[5]["id"] = 12; details[5]["text"] = "spreading";

  char* buffer = (char*)malloc(4096);
  if (!buffer) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    return ESP_FAIL;
  }

  serializeJson(doc, buffer, 4096);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t ret = httpd_resp_sendstr(req, buffer);
  free(buffer);
  return ret;
}

// POST /api/chirp/ack - Acknowledge a chirp
inline esp_err_t handle_chirp_ack(httpd_req_t* req) {
  char content[128];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing request body");
    return ESP_FAIL;
  }
  content[content_len] = '\0';

  StaticJsonDocument<128> input;
  DeserializationError err = deserializeJson(input, content);
  if (err) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  const char* nonce_hex = input["nonce"] | "";
  const char* ack_type_str = input["type"] | "seen";

  // Parse nonce from hex
  uint8_t nonce[8];
  for (int i = 0; i < 8; i++) {
    sscanf(nonce_hex + i * 2, "%2hhx", &nonce[i]);
  }

  // Map ack type
  chirp_channel::ChirpAckType ack_type = chirp_channel::CHIRP_ACK_SEEN;
  if (strcmp(ack_type_str, "confirmed") == 0) ack_type = chirp_channel::CHIRP_ACK_CONFIRMED;
  else if (strcmp(ack_type_str, "resolved") == 0) ack_type = chirp_channel::CHIRP_ACK_RESOLVED;

  bool success = chirp_channel::acknowledge_chirp(nonce, ack_type);

  StaticJsonDocument<64> doc;
  doc["success"] = success;

  char buffer[64];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// POST /api/chirp/dismiss - Dismiss a chirp from display
inline esp_err_t handle_chirp_dismiss(httpd_req_t* req) {
  char content[64];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing request body");
    return ESP_FAIL;
  }
  content[content_len] = '\0';

  StaticJsonDocument<64> input;
  DeserializationError err = deserializeJson(input, content);
  if (err) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  const char* nonce_hex = input["nonce"] | "";

  uint8_t nonce[8];
  for (int i = 0; i < 8; i++) {
    sscanf(nonce_hex + i * 2, "%2hhx", &nonce[i]);
  }

  bool success = chirp_channel::dismiss_chirp(nonce);

  StaticJsonDocument<64> doc;
  doc["success"] = success;

  char buffer[64];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// POST /api/chirp/mute - Mute for duration
inline esp_err_t handle_chirp_mute(httpd_req_t* req) {
  char content[64];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing request body");
    return ESP_FAIL;
  }
  content[content_len] = '\0';

  StaticJsonDocument<64> input;
  DeserializationError err = deserializeJson(input, content);
  if (err) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  uint8_t duration = input["duration_minutes"] | 30;

  bool success = chirp_channel::mute(duration);

  StaticJsonDocument<64> doc;
  doc["success"] = success;
  if (!success) {
    doc["error"] = "invalid_duration";
    doc["message"] = "Duration must be 15, 30, 60, or 120 minutes";
  }

  char buffer[64];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// POST /api/chirp/unmute - Unmute chirps
inline esp_err_t handle_chirp_unmute(httpd_req_t* req) {
  chirp_channel::unmute();

  StaticJsonDocument<64> doc;
  doc["success"] = true;

  char buffer[64];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// POST /api/chirp/settings - Update chirp settings
inline esp_err_t handle_chirp_settings(httpd_req_t* req) {
  char content[128];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing request body");
    return ESP_FAIL;
  }
  content[content_len] = '\0';

  StaticJsonDocument<128> input;
  DeserializationError err = deserializeJson(input, content);
  if (err) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  // Update relay setting if provided
  if (input.containsKey("relay_enabled")) {
    chirp_channel::set_relay_enabled(input["relay_enabled"].as<bool>());
  }

  // Update urgency filter if provided
  if (input.containsKey("urgency_filter")) {
    const char* filter_str = input["urgency_filter"] | "info";
    chirp_channel::ChirpUrgency filter = chirp_channel::CHIRP_URG_INFO;
    if (strcmp(filter_str, "caution") == 0) filter = chirp_channel::CHIRP_URG_CAUTION;
    else if (strcmp(filter_str, "urgent") == 0) filter = chirp_channel::CHIRP_URG_URGENT;
    chirp_channel::set_urgency_filter(filter);
  }

  // Return current settings
  StaticJsonDocument<128> doc;
  doc["success"] = true;
  doc["relay_enabled"] = chirp_channel::is_relay_enabled();
  doc["urgency_filter"] = chirp_channel::urgency_name(chirp_channel::get_urgency_filter());

  char buffer[128];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// POST /api/chirp/confirm - Confirm you also witness this (human verification)
inline esp_err_t handle_chirp_confirm(httpd_req_t* req) {
  char content[64];
  int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
  if (content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing request body");
    return ESP_FAIL;
  }
  content[content_len] = '\0';

  StaticJsonDocument<64> input;
  DeserializationError err = deserializeJson(input, content);
  if (err) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  const char* nonce_hex = input["nonce"] | "";

  uint8_t nonce[8];
  for (int i = 0; i < 8; i++) {
    sscanf(nonce_hex + i * 2, "%2hhx", &nonce[i]);
  }

  bool success = chirp_channel::confirm_chirp(nonce);

  StaticJsonDocument<64> doc;
  doc["success"] = success;
  if (!success) {
    doc["error"] = "not_found";
    doc["message"] = "Chirp not found or already dismissed";
  }

  char buffer[64];
  serializeJson(doc, buffer);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, buffer);
}

// ════════════════════════════════════════════════════════════════════════════
// ROUTE REGISTRATION
// ════════════════════════════════════════════════════════════════════════════

// Call this to register all chirp API routes with the HTTP server
inline void register_routes(httpd_handle_t server) {
  // GET endpoints
  httpd_uri_t chirp_status = {
    .uri = "/api/chirp",
    .method = HTTP_GET,
    .handler = handle_chirp_status,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_status);

  httpd_uri_t chirp_nearby = {
    .uri = "/api/chirp/nearby",
    .method = HTTP_GET,
    .handler = handle_chirp_nearby,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_nearby);

  httpd_uri_t chirp_recent = {
    .uri = "/api/chirp/recent",
    .method = HTTP_GET,
    .handler = handle_chirp_recent,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_recent);

  httpd_uri_t chirp_templates = {
    .uri = "/api/chirp/templates",
    .method = HTTP_GET,
    .handler = handle_chirp_templates,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_templates);

  // POST endpoints
  httpd_uri_t chirp_enable = {
    .uri = "/api/chirp/enable",
    .method = HTTP_POST,
    .handler = handle_chirp_enable,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_enable);

  httpd_uri_t chirp_disable = {
    .uri = "/api/chirp/disable",
    .method = HTTP_POST,
    .handler = handle_chirp_disable,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_disable);

  httpd_uri_t chirp_send = {
    .uri = "/api/chirp/send",
    .method = HTTP_POST,
    .handler = handle_chirp_send,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_send);

  httpd_uri_t chirp_ack = {
    .uri = "/api/chirp/ack",
    .method = HTTP_POST,
    .handler = handle_chirp_ack,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_ack);

  httpd_uri_t chirp_dismiss = {
    .uri = "/api/chirp/dismiss",
    .method = HTTP_POST,
    .handler = handle_chirp_dismiss,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_dismiss);

  httpd_uri_t chirp_mute = {
    .uri = "/api/chirp/mute",
    .method = HTTP_POST,
    .handler = handle_chirp_mute,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_mute);

  httpd_uri_t chirp_unmute = {
    .uri = "/api/chirp/unmute",
    .method = HTTP_POST,
    .handler = handle_chirp_unmute,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_unmute);

  httpd_uri_t chirp_confirm = {
    .uri = "/api/chirp/confirm",
    .method = HTTP_POST,
    .handler = handle_chirp_confirm,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_confirm);

  httpd_uri_t chirp_settings = {
    .uri = "/api/chirp/settings",
    .method = HTTP_POST,
    .handler = handle_chirp_settings,
    .user_ctx = nullptr
  };
  httpd_register_uri_handler(server, &chirp_settings);
}

} // namespace chirp_api

#endif // SECURACV_CHIRP_API_H
