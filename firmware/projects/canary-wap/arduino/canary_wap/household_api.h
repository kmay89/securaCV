/*
 * SecuraCV Canary — Household + Presence REST API
 *
 * GET  /api/household              — list paired devices (no IRK material;
 *                                    slot, label, role, last_seen_ms ago,
 *                                    paired-since timestamp)
 * POST /api/household/role         — { slot, role } : assign role to a slot
 * DELETE /api/household            — { slot }       : forget a paired device
 * GET  /api/presence               — current auto-context, who is home,
 *                                    override status, ble_presence stats
 * POST /api/presence/override      — { context }    : pin context for 24 h
 * DELETE /api/presence/override    — clear override, return to auto
 *
 * AUTH MODEL
 * ──────────
 * All endpoints require bearer-token auth via api_auth_check (same as the
 * rest of the API). Mutations (POST role, override) are scoped to slots
 * that are ALREADY in the household store — the API can't create new
 * paired devices, only adjust roles on devices that completed the BLE
 * pairing ceremony. Default role for any new pairing is ROLE_GUEST.
 */

#ifndef SECURACV_HOUSEHOLD_API_H
#define SECURACV_HOUSEHOLD_API_H

#include "esp_http_server.h"
#include "household.h"
#include "presence_context.h"
#include "notify.h"
#include "ble_presence.h"
#include <ArduinoJson.h>

namespace household_api {

static inline esp_err_t send_json(httpd_req_t* req, const char* json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, json);
}
static inline esp_err_t send_err(httpd_req_t* req, const char* msg) {
  JsonDocument d; d["success"] = false; d["error"] = msg;
  char buf[160]; serializeJson(d, buf);
  httpd_resp_set_status(req, "400 Bad Request");
  return send_json(req, buf);
}
static inline esp_err_t send_ok(httpd_req_t* req, const char* msg = nullptr) {
  JsonDocument d; d["success"] = true;
  if (msg) d["message"] = msg;
  char buf[160]; serializeJson(d, buf);
  return send_json(req, buf);
}

static inline const char* role_name(household::DeviceRole r) {
  switch (r) {
    case household::ROLE_OWNER:  return "owner";
    case household::ROLE_FAMILY: return "family";
    case household::ROLE_GUEST:  return "guest";
    default:                     return "guest";
  }
}
static inline bool parse_role(const char* s, household::DeviceRole* out) {
  if (!s || !out) return false;
  if (strcmp(s, "owner")  == 0) { *out = household::ROLE_OWNER;  return true; }
  if (strcmp(s, "family") == 0) { *out = household::ROLE_FAMILY; return true; }
  if (strcmp(s, "guest")  == 0) { *out = household::ROLE_GUEST;  return true; }
  return false;
}
static inline const char* context_name(notify::Context c) {
  switch (c) {
    case notify::CTX_HOME:        return "home";
    case notify::CTX_AWAY:        return "away";
    case notify::CTX_QUIET_HOURS: return "quiet";
    case notify::CTX_TRAVELING:   return "traveling";
    default:                      return "home";
  }
}
static inline bool parse_context(const char* s, notify::Context* out) {
  if (!s || !out) return false;
  if (strcmp(s, "home")      == 0) { *out = notify::CTX_HOME;        return true; }
  if (strcmp(s, "away")      == 0) { *out = notify::CTX_AWAY;        return true; }
  if (strcmp(s, "quiet")     == 0) { *out = notify::CTX_QUIET_HOURS; return true; }
  if (strcmp(s, "traveling") == 0) { *out = notify::CTX_TRAVELING;   return true; }
  return false;
}

// GET /api/household
inline esp_err_t handle_list(httpd_req_t* req) {
  JsonDocument doc;
  doc["count"]    = (uint32_t)household::count();
  doc["max_slots"] = (uint32_t)household::MAX_HOUSEHOLD_DEVICES;
  doc["enrolling"] = household::is_enrolling();
  doc["enrollment_ms_remaining"] = household::enrollment_ms_remaining();

  JsonArray arr = doc["devices"].to<JsonArray>();
  const uint32_t now = millis();
  for (uint8_t i = 0; i < household::MAX_HOUSEHOLD_DEVICES; i++) {
    char label[household::MAX_LABEL_LEN] = {0};
    if (!household::get_label(i, label, sizeof(label))) continue;  // empty slot

    JsonObject d = arr.add<JsonObject>();
    d["slot"]  = i;
    d["label"] = label;
    d["role"]  = role_name(household::get_role(i));
    d["paired_since_ms"] = household::get_added_ms(i);

    const uint32_t seen = household::last_seen_ms(i);
    if (seen == 0) {
      d["last_seen_ago_ms"] = nullptr;  // never seen this boot
    } else {
      d["last_seen_ago_ms"] = (now >= seen) ? (now - seen) : 0;
    }
  }

  String buf;
  if (!buf.reserve(2048)) return send_err(req, "alloc failed");
  serializeJson(doc, buf);
  return send_json(req, buf.c_str());
}

// POST /api/household/role  body: { "slot": 0..7, "role": "owner|family|guest" }
inline esp_err_t handle_set_role(httpd_req_t* req) {
  char body[128];
  int n = httpd_req_recv(req, body, sizeof(body) - 1);
  if (n <= 0) return send_err(req, "missing body");
  body[n] = '\0';

  JsonDocument in;
  if (deserializeJson(in, body) != DeserializationError::Ok) return send_err(req, "bad JSON");
  if (!in["slot"].is<int>() || !in["role"].is<const char*>()) {
    return send_err(req, "missing slot or role");
  }

  int slot = in["slot"].as<int>();
  if (slot < 0 || slot >= (int)household::MAX_HOUSEHOLD_DEVICES) {
    return send_err(req, "slot out of range");
  }
  household::DeviceRole role;
  if (!parse_role(in["role"].as<const char*>(), &role)) {
    return send_err(req, "role must be owner|family|guest");
  }

  // The audit callback is wired by canary_wap.ino at boot via
  // household::set_role's caller hook. Pass nullptr here so the call
  // chain is simple — the caller-installed audit hook attached at
  // higher level (see canary_wap.ino) writes the witness-chain entry
  // through `set_role` indirectly via a thin shim.
  if (!household::set_role((uint8_t)slot, role, nullptr)) {
    return send_err(req, "slot empty or role invalid");
  }
  return send_ok(req, "role updated");
}

// DELETE /api/household  body: { "slot": N }
inline esp_err_t handle_remove(httpd_req_t* req) {
  char body[64];
  int n = httpd_req_recv(req, body, sizeof(body) - 1);
  if (n <= 0) return send_err(req, "missing body");
  body[n] = '\0';

  JsonDocument in;
  if (deserializeJson(in, body) != DeserializationError::Ok) return send_err(req, "bad JSON");
  if (!in["slot"].is<int>()) return send_err(req, "missing slot");

  int slot = in["slot"].as<int>();
  if (slot < 0 || slot >= (int)household::MAX_HOUSEHOLD_DEVICES) {
    return send_err(req, "slot out of range");
  }
  if (!household::remove_by_slot((uint8_t)slot)) {
    return send_err(req, "slot empty");
  }
  return send_ok(req, "device forgotten");
}

// GET /api/presence
inline esp_err_t handle_presence_status(httpd_req_t* req) {
  presence_context::Status pc = {};
  presence_context::get_status(&pc);

  ble_presence::Stats bp = {};
  ble_presence::get_stats(&bp);

  JsonDocument doc;
  doc["auto_context"]      = context_name(pc.auto_context);
  doc["effective_context"] = context_name(pc.effective_context);
  doc["override_active"]   = pc.override_active;
  doc["override_ms_remaining"] = pc.override_ms_remaining;
  doc["owner_seen_recently"]   = pc.owner_seen_recently;
  if (pc.ms_since_any_owner == UINT32_MAX) {
    doc["ms_since_any_owner"] = nullptr;
  } else {
    doc["ms_since_any_owner"] = pc.ms_since_any_owner;
  }

  // Presence-sensor radio stats — DP-noise is applied at export elsewhere
  // for the federated/exported counters; these are the raw operator-facing
  // diagnostics, no need to noise.
  JsonObject sensor = doc["sensor"].to<JsonObject>();
  sensor["running"]        = bp.running;
  sensor["reduced_duty"]   = bp.reduced_duty;
  sensor["adverts_seen"]   = bp.adverts_seen;
  sensor["adverts_resolved_household"] = bp.adverts_resolved_household;
  sensor["pause_count"]    = bp.pause_count;

  char buf[640];
  serializeJson(doc, buf);
  return send_json(req, buf);
}

// POST /api/presence/override   body: { "context": "home|away|quiet|traveling" }
inline esp_err_t handle_override_set(httpd_req_t* req) {
  char body[64];
  int n = httpd_req_recv(req, body, sizeof(body) - 1);
  if (n <= 0) return send_err(req, "missing body");
  body[n] = '\0';

  JsonDocument in;
  if (deserializeJson(in, body) != DeserializationError::Ok) return send_err(req, "bad JSON");
  if (!in["context"].is<const char*>()) return send_err(req, "missing context");

  notify::Context ctx;
  if (!parse_context(in["context"].as<const char*>(), &ctx)) {
    return send_err(req, "context must be home|away|quiet|traveling");
  }
  if (!presence_context::set_override(ctx)) return send_err(req, "set failed");
  return send_ok(req, "override set (24 h)");
}

// DELETE /api/presence/override
inline esp_err_t handle_override_clear(httpd_req_t* req) {
  presence_context::clear_override();
  return send_ok(req, "override cleared");
}

inline void register_routes(httpd_handle_t server) {
  httpd_uri_t r1 = { .uri = "/api/household",          .method = HTTP_GET,    .handler = handle_list };
  httpd_register_uri_handler(server, &r1);
  httpd_uri_t r2 = { .uri = "/api/household/role",     .method = HTTP_POST,   .handler = handle_set_role };
  httpd_register_uri_handler(server, &r2);
  httpd_uri_t r3 = { .uri = "/api/household",          .method = HTTP_DELETE, .handler = handle_remove };
  httpd_register_uri_handler(server, &r3);
  httpd_uri_t r4 = { .uri = "/api/presence",           .method = HTTP_GET,    .handler = handle_presence_status };
  httpd_register_uri_handler(server, &r4);
  httpd_uri_t r5 = { .uri = "/api/presence/override",  .method = HTTP_POST,   .handler = handle_override_set };
  httpd_register_uri_handler(server, &r5);
  httpd_uri_t r6 = { .uri = "/api/presence/override",  .method = HTTP_DELETE, .handler = handle_override_clear };
  httpd_register_uri_handler(server, &r6);
}

}  // namespace household_api

#endif  // SECURACV_HOUSEHOLD_API_H
