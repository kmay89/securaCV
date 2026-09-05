/*
 * SecuraCV Canary — Beacon Channel REST API Handlers
 *
 * HTTP handlers for the harm-reduction Beacon channel.
 *
 * Bearer-gated from day one (see spec/beacon_channel_v0.md §10 and
 * docs/audit/mesh_and_chirp_audit_v1.md C12 closure for Chirp). Every route
 * is wrapped in the bcn_auth_gated trampoline; an Authorization: Bearer
 * <api_token> header is required for any successful response. Mirrors the
 * pattern established in bluetooth_api.h (#437) and household_api.h (#438).
 *
 * The handlers themselves are thin JSON wrappers around beacon_channel::*
 * public functions. The cryptographic origination, NFPA-72 state machine,
 * and audit log live in beacon_channel.cpp; this file does not duplicate
 * any of that logic.
 *
 * NOTE: this file is only included from canary_wap.ino when
 * FEATURE_BEACON_CHANNEL is enabled in build_config.h. Default OFF.
 */

#ifndef SECURACV_BEACON_API_H
#define SECURACV_BEACON_API_H

#include "build_config.h"

#if FEATURE_BEACON_CHANNEL

#include "esp_http_server.h"
#include "beacon_channel.h"
#include "api_auth.h"
#include <ArduinoJson.h>
#include <string.h>

namespace beacon_api {

// ════════════════════════════════════════════════════════════════════════════
// AUTH GATE — same template-trampoline as bluetooth_api.h
// ════════════════════════════════════════════════════════════════════════════

inline const char*& auth_token_storage() {
  static const char* token = nullptr;
  return token;
}

template<esp_err_t (*Real)(httpd_req_t*)>
static esp_err_t bcn_auth_gated(httpd_req_t* req) {
  const char* tok = auth_token_storage();
  if (tok && !api_auth_check(req, tok)) return ESP_OK;
  return Real(req);
}

// ════════════════════════════════════════════════════════════════════════════
// HELPERS
// ════════════════════════════════════════════════════════════════════════════

static inline esp_err_t send_json(httpd_req_t* req, const char* json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, json);
}

static inline esp_err_t send_success(httpd_req_t* req, const char* msg = nullptr) {
  JsonDocument doc;
  doc["success"] = true;
  if (msg) doc["message"] = msg;
  char buf[128];
  serializeJson(doc, buf);
  return send_json(req, buf);
}

static inline esp_err_t send_error(httpd_req_t* req, const char* err) {
  JsonDocument doc;
  doc["success"] = false;
  doc["error"] = err;
  char buf[128];
  serializeJson(doc, buf);
  return send_json(req, buf);
}

static inline void fp_to_hex(const uint8_t* fp, char* out) {
  for (size_t i = 0; i < beacon_channel::DEVICE_FP_SIZE; i++) {
    sprintf(out + i * 2, "%02x", fp[i]);
  }
  out[beacon_channel::DEVICE_FP_SIZE * 2] = '\0';
}

static inline const char* urgency_name(beacon_channel::BeaconUrgency u) {
  switch (u) {
    case beacon_channel::BCN_URG_IMMEDIATE: return "Immediate";
    case beacon_channel::BCN_URG_EXPECTED:  return "Expected";
    case beacon_channel::BCN_URG_FUTURE:    return "Future";
    case beacon_channel::BCN_URG_PAST:      return "Past";
    default:                                 return "Unknown";
  }
}

static inline const char* severity_name(beacon_channel::BeaconSeverity s) {
  switch (s) {
    case beacon_channel::BCN_SEV_EXTREME:  return "Extreme";
    case beacon_channel::BCN_SEV_SEVERE:   return "Severe";
    case beacon_channel::BCN_SEV_MODERATE: return "Moderate";
    case beacon_channel::BCN_SEV_MINOR:    return "Minor";
    default:                                return "Unknown";
  }
}

static inline const char* certainty_name(beacon_channel::BeaconCertainty c) {
  switch (c) {
    case beacon_channel::BCN_CERT_OBSERVED: return "Observed";
    case beacon_channel::BCN_CERT_LIKELY:   return "Likely";
    case beacon_channel::BCN_CERT_POSSIBLE: return "Possible";
    case beacon_channel::BCN_CERT_UNLIKELY: return "Unlikely";
    default:                                 return "Unknown";
  }
}

// ════════════════════════════════════════════════════════════════════════════
// HANDLERS
// ════════════════════════════════════════════════════════════════════════════

// GET /api/beacon — status (state, beacon_set_size, active_alarm, trouble)
inline esp_err_t handle_status(httpd_req_t* req) {
  beacon_channel::BeaconStatus s = beacon_channel::get_status();

  JsonDocument doc;
  doc["state"] = beacon_channel::state_name(s.state);
  doc["beacon_set_size"] = s.beacon_set_size;
  doc["active_alarm"] = s.active_alarm;
  if (s.active_alarm) {
    doc["active_template_id"] = s.active_template_id;
    doc["active_alarm_expires"] = (uint32_t)s.active_alarm_expires;
  }

  // Trouble reason bitmask exposed both as bitmask and as named list.
  doc["trouble_reasons_mask"] = s.trouble_reasons;
  JsonArray reasons = doc["trouble_reasons"].to<JsonArray>();
  if (s.trouble_reasons & beacon_channel::BCN_TROUBLE_TIME_UNSYNCED)
    reasons.add("time_unsynced");
  if (s.trouble_reasons & beacon_channel::BCN_TROUBLE_AIRTIME_SATURATED)
    reasons.add("airtime_saturated");
  if (s.trouble_reasons & beacon_channel::BCN_TROUBLE_NEIGHBOR_SELFTEST_GAP)
    reasons.add("neighbor_selftest_gap");
  if (s.trouble_reasons & beacon_channel::BCN_TROUBLE_KEY_SELFTEST_FAILED)
    reasons.add("key_selftest_failed");
  if (s.trouble_reasons & beacon_channel::BCN_TROUBLE_BEACON_SET_EMPTY)
    reasons.add("beacon_set_empty");

  char buf[512];
  serializeJson(doc, buf);
  return send_json(req, buf);
}

// GET /api/beacon/set — list beacon-set members (fingerprints + names only)
inline esp_err_t handle_set_list(httpd_req_t* req) {
  JsonDocument doc;
  JsonArray arr = doc["members"].to<JsonArray>();
  for (uint8_t i = 0; i < beacon_channel::get_beacon_set_size(); i++) {
    const beacon_channel::BeaconSetEntry* e = beacon_channel::get_beacon_set_entry(i);
    if (!e || !e->valid) continue;
    JsonObject m = arr.add<JsonObject>();
    char fp_hex[beacon_channel::DEVICE_FP_SIZE * 2 + 1];
    fp_to_hex(e->fingerprint, fp_hex);
    m["fingerprint"] = fp_hex;
    m["name"] = e->name;
    m["paired_at"] = (uint32_t)e->paired_at;
    m["last_selftest"] = (uint32_t)e->last_selftest;
    switch (e->trust_level) {
      case beacon_channel::BCN_TRUST_COSIGNER: m["trust_level"] = "cosigner"; break;
      case beacon_channel::BCN_TRUST_GATEWAY:  m["trust_level"] = "gateway";  break;
      case beacon_channel::BCN_TRUST_REVOKED:  m["trust_level"] = "revoked";  break;
      default: m["trust_level"] = "unknown";
    }
  }
  // Heap-allocate sized by measureJson + bounds-checked serializeJson
  // (gemini P1 — avoid a 2 KB stack buffer + potential overflow).
  const size_t needed = measureJson(doc) + 1;
  if (needed > 4096) return send_error(req, "set list too large");
  char* buf = (char*)malloc(needed);
  if (!buf) return send_error(req, "out of memory");
  serializeJson(doc, buf, needed);
  esp_err_t rc = send_json(req, buf);
  free(buf);
  return rc;
}

// POST /api/beacon/pair/start
inline esp_err_t handle_pair_start(httpd_req_t* req) {
  return beacon_channel::start_pair_init() ? send_success(req, "pair_init started")
                                            : send_error(req, "pair_init failed");
}

// POST /api/beacon/pair/join
inline esp_err_t handle_pair_join(httpd_req_t* req) {
  return beacon_channel::start_pair_join() ? send_success(req, "pair_join started")
                                            : send_error(req, "pair_join failed");
}

// POST /api/beacon/pair/confirm
inline esp_err_t handle_pair_confirm(httpd_req_t* req) {
  return beacon_channel::confirm_pair() ? send_success(req, "pair confirmed")
                                         : send_error(req, "pair confirm failed");
}

// POST /api/beacon/pair/cancel
inline esp_err_t handle_pair_cancel(httpd_req_t* req) {
  beacon_channel::cancel_pair();
  return send_success(req, "pair canceled");
}

// POST /api/beacon/revoke  body: { "fingerprint": "<hex>" }
inline esp_err_t handle_revoke(httpd_req_t* req) {
  char body[128];
  int len = httpd_req_recv(req, body, sizeof(body) - 1);
  if (len <= 0) return send_error(req, "empty body");
  body[len] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, body)) return send_error(req, "invalid JSON");
  const char* fp_hex = doc["fingerprint"];
  if (!fp_hex) return send_error(req, "missing 'fingerprint'");
  if (strlen(fp_hex) != beacon_channel::DEVICE_FP_SIZE * 2)
    return send_error(req, "fingerprint must be 32-char hex");

  uint8_t fp[beacon_channel::DEVICE_FP_SIZE];
  for (size_t i = 0; i < beacon_channel::DEVICE_FP_SIZE; i++) {
    unsigned v;
    if (sscanf(fp_hex + i * 2, "%02x", &v) != 1) return send_error(req, "bad hex");
    fp[i] = (uint8_t)v;
  }

  return beacon_channel::revoke_beacon_set_entry(fp)
      ? send_success(req, "revoked")
      : send_error(req, "fingerprint not in set");
}

// Helper: parse CAP-aligned enum from either string label OR uint8_t code.
// `ArduinoJson::JsonVariant | 0` returns 0 on string inputs, which would
// silently coerce every string-typed value to 0 (= Immediate / Extreme /
// Observed) — the WRONG behavior for an origination endpoint.
// gemini P1 closure: explicit string → enum mapping with a numeric fallback
// for callers that pass the wire-format code directly.

static uint8_t parse_urgency(JsonVariantConst v) {
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!strcmp(s, "Immediate")) return beacon_channel::BCN_URG_IMMEDIATE;
    if (!strcmp(s, "Expected"))  return beacon_channel::BCN_URG_EXPECTED;
    if (!strcmp(s, "Future"))    return beacon_channel::BCN_URG_FUTURE;
    if (!strcmp(s, "Past"))      return beacon_channel::BCN_URG_PAST;
    return beacon_channel::BCN_URG_UNKNOWN;
  }
  if (v.is<int>()) return (uint8_t)v.as<int>();
  return beacon_channel::BCN_URG_UNKNOWN;
}

static uint8_t parse_severity(JsonVariantConst v) {
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!strcmp(s, "Extreme"))   return beacon_channel::BCN_SEV_EXTREME;
    if (!strcmp(s, "Severe"))    return beacon_channel::BCN_SEV_SEVERE;
    if (!strcmp(s, "Moderate"))  return beacon_channel::BCN_SEV_MODERATE;
    if (!strcmp(s, "Minor"))     return beacon_channel::BCN_SEV_MINOR;
    return beacon_channel::BCN_SEV_UNKNOWN;
  }
  if (v.is<int>()) return (uint8_t)v.as<int>();
  return beacon_channel::BCN_SEV_UNKNOWN;
}

static uint8_t parse_certainty(JsonVariantConst v) {
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!strcmp(s, "Observed"))  return beacon_channel::BCN_CERT_OBSERVED;
    if (!strcmp(s, "Likely"))    return beacon_channel::BCN_CERT_LIKELY;
    if (!strcmp(s, "Possible"))  return beacon_channel::BCN_CERT_POSSIBLE;
    if (!strcmp(s, "Unlikely"))  return beacon_channel::BCN_CERT_UNLIKELY;
    return beacon_channel::BCN_CERT_UNKNOWN;
  }
  if (v.is<int>()) return (uint8_t)v.as<int>();
  return beacon_channel::BCN_CERT_UNKNOWN;
}

// POST /api/beacon/originate  body:
//   { "template_id": 0x23, "urgency": "Immediate",
//     "severity": "Severe", "certainty": "Likely",
//     "detail": 10, "ttl_minutes": 15 }
//
// `urgency`, `severity`, `certainty` accept either the CAP string label
// or the numeric enum code. `template_id` and `detail` are always numeric
// (16 templates × 256-value detail slot space — too many to enumerate as
// strings, and clients always have the IDs from /api/beacon/set anyway).
inline esp_err_t handle_originate(httpd_req_t* req) {
  char body[256];
  int len = httpd_req_recv(req, body, sizeof(body) - 1);
  if (len <= 0) return send_error(req, "empty body");
  body[len] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, body)) return send_error(req, "invalid JSON");

  // gemini P1: template_id is mandatory. The BeaconTemplate enum starts at
  // 0x10, so defaulting to 0 on missing key would silently produce an
  // invalid template; reject explicitly instead.
  if (!doc["template_id"].is<JsonVariantConst>() || doc["template_id"].isNull()) {
    return send_error(req, "missing 'template_id'");
  }
  uint8_t tpl_byte = (uint8_t)doc["template_id"].as<int>();
  uint8_t urg  = parse_urgency(doc["urgency"]);
  uint8_t sev  = parse_severity(doc["severity"]);
  uint8_t cert = parse_certainty(doc["certainty"]);
  uint8_t det  = (uint8_t)(doc["detail"] | 0);
  uint32_t ttl = (uint32_t)(doc["ttl_minutes"] | 15);

  bool ok = beacon_channel::originate_alert(
      (beacon_channel::BeaconTemplate)tpl_byte,
      (beacon_channel::BeaconUrgency)urg,
      (beacon_channel::BeaconSeverity)sev,
      (beacon_channel::BeaconCertainty)cert,
      (beacon_channel::BeaconDetailSlot)det,
      ttl);
  return ok ? send_success(req, "origination pending cosigner")
            : send_error(req, "origination refused (rate, presence, or no cosigner)");
}

// POST /api/beacon/originate-solo — single-device origination via BOOT button.
// Spec §6.2. Body shape mirrors /api/beacon/originate but `certainty` is
// ignored (server forces it to "Observed" per the solo invariant).
//
// The caller MUST be holding the physical BOOT button at the moment of
// this call; the firmware checks the GPIO state in real time. This is the
// load-bearing security gate for the solo path. If the button is not
// held, the call returns 400 with reason="boot_button_not_held".
inline esp_err_t handle_originate_solo(httpd_req_t* req) {
  char body[256];
  int len = httpd_req_recv(req, body, sizeof(body) - 1);
  if (len <= 0) return send_error(req, "empty body");
  body[len] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, body)) return send_error(req, "invalid JSON");

  // gemini P1: template_id is mandatory. The BeaconTemplate enum starts at
  // 0x10, so defaulting to 0 would silently produce an invalid template.
  if (!doc["template_id"].is<JsonVariantConst>() || doc["template_id"].isNull()) {
    return send_error(req, "missing 'template_id'");
  }
  uint8_t tpl_byte = (uint8_t)doc["template_id"].as<int>();
  uint8_t urg = parse_urgency(doc["urgency"]);
  uint8_t sev = parse_severity(doc["severity"]);
  // certainty intentionally ignored — forced to Observed in the firmware.
  uint8_t det = (uint8_t)(doc["detail"] | 0);
  uint32_t ttl = (uint32_t)(doc["ttl_minutes"] | 15);

  // Pre-checks so the user gets a useful error message instead of just
  // "origination refused (rate, presence, or no cosigner)".
  //
  // spec §6.2: solo is for a device with no paired neighbor able to cosign
  // right now. With one available, the answer is the two-device path
  // (/api/beacon/originate), and the reason says so by name.
  if (beacon_channel::paired_cosigner_available()) {
    return send_error(req, "paired_cosigner_available");
  }
  if (!beacon_channel::boot_button_held()) {
    return send_error(req, "boot_button_not_held");
  }

  bool ok = beacon_channel::originate_alert_solo(
      (beacon_channel::BeaconTemplate)tpl_byte,
      (beacon_channel::BeaconUrgency)urg,
      (beacon_channel::BeaconSeverity)sev,
      (beacon_channel::BeaconDetailSlot)det,
      ttl);
  return ok ? send_success(req, "solo ALERT broadcast (certainty=Observed)")
            : send_error(req, "solo origination refused (rate, time-unsynced, or button released)");
}

// POST /api/beacon/cosign  body: { "confirm": true|false }
inline esp_err_t handle_cosign(httpd_req_t* req) {
  char body[128];
  int len = httpd_req_recv(req, body, sizeof(body) - 1);
  if (len <= 0) return send_error(req, "empty body");
  body[len] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, body)) return send_error(req, "invalid JSON");
  bool confirm = doc["confirm"] | false;
  return beacon_channel::cosign_pending_request(confirm)
      ? send_success(req, confirm ? "cosigned" : "declined")
      : send_error(req, "no pending cosign request");
}

// POST /api/beacon/cancel — silence the active alarm on this device.
// Spec §10 defines this as originating a BEACON_MSG_CANCEL for the network;
// the firmware does not do that yet (see cancel_active_alarm), so the reply
// says what actually happened rather than "canceled". Paired devices stay in
// alarm until the alarm's own expiry.
inline esp_err_t handle_cancel(httpd_req_t* req) {
  return beacon_channel::cancel_active_alarm()
      ? send_success(req, "alarm silenced on this device only — no network CANCEL was sent; "
                          "paired devices stay in alarm until it expires")
      : send_error(req, "no active alarm");
}

// GET /api/beacon/active — active alarm details, if any
inline esp_err_t handle_active(httpd_req_t* req) {
  const beacon_channel::BeaconAlertCanonical* a = beacon_channel::get_active_alarm();
  JsonDocument doc;
  if (!a) {
    doc["active"] = false;
  } else {
    doc["active"] = true;
    doc["template_id"] = a->template_id;
    doc["msg_type"] = a->msg_type;
    doc["urgency"] = urgency_name((beacon_channel::BeaconUrgency)a->urgency);
    doc["severity"] = severity_name((beacon_channel::BeaconSeverity)a->severity);
    doc["certainty"] = certainty_name((beacon_channel::BeaconCertainty)a->certainty);
    doc["effective"] = (uint32_t)a->effective;
    doc["expires"] = (uint32_t)a->expires;

    char fp_hex[beacon_channel::DEVICE_FP_SIZE * 2 + 1];
    fp_to_hex(a->originator_fp, fp_hex); doc["originator_fp"] = fp_hex;
    fp_to_hex(a->cosigner_fp, fp_hex);   doc["cosigner_fp"]   = fp_hex;
  }
  char buf[768];
  serializeJson(doc, buf);
  return send_json(req, buf);
}

// GET /api/beacon/audit — paged audit log (Bearer-gated).
//
// Pagination query params:
//   ?offset=N  (default 0)  — oldest-first index into the audit log
//   ?limit=M   (default 16, capped to AUDIT_PAGE_MAX)
//
// gemini P1 closure: the prior implementation allocated a 4 KB buffer on
// the stack (dangerous on ESP32) and called serializeJson without a size
// argument (potential buffer overflow). v0.3 paginates to keep responses
// small, sizes the heap buffer via measureJson, and uses the
// size-checked serializeJson(doc, buf, size) overload.
static const size_t AUDIT_PAGE_MAX = 32;

inline esp_err_t handle_audit(httpd_req_t* req) {
  // Parse query string (best-effort; absent params → defaults).
  size_t offset = 0;
  size_t limit = 16;
  {
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 128) {
      char q[128];
      if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(q, "offset", val, sizeof(val)) == ESP_OK) {
          offset = (size_t)strtoul(val, nullptr, 10);
        }
        if (httpd_query_key_value(q, "limit", val, sizeof(val)) == ESP_OK) {
          limit = (size_t)strtoul(val, nullptr, 10);
        }
      }
    }
  }
  if (limit > AUDIT_PAGE_MAX) limit = AUDIT_PAGE_MAX;
  if (limit == 0) limit = 1;

  const size_t total = beacon_channel::get_audit_log_count();
  const size_t end = (offset + limit > total) ? total : (offset + limit);

  JsonDocument doc;
  doc["count"] = (uint32_t)total;
  doc["offset"] = (uint32_t)offset;
  doc["limit"] = (uint32_t)limit;
  doc["returned"] = (uint32_t)(end > offset ? end - offset : 0);
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (size_t i = offset; i < end; i++) {
    const beacon_channel::BeaconAuditEntry* e = beacon_channel::get_audit_log_entry(i);
    if (!e) continue;
    JsonObject ent = arr.add<JsonObject>();
    ent["received_at"] = (uint32_t)e->received_at;
    ent["template_id"] = e->canonical.template_id;
    ent["msg_type"] = e->canonical.msg_type;
    ent["urgency"] = urgency_name((beacon_channel::BeaconUrgency)e->canonical.urgency);
    ent["severity"] = severity_name((beacon_channel::BeaconSeverity)e->canonical.severity);
    ent["certainty"] = certainty_name((beacon_channel::BeaconCertainty)e->canonical.certainty);
    ent["hop_count"] = e->hop_count;
    char fp_hex[beacon_channel::DEVICE_FP_SIZE * 2 + 1];
    fp_to_hex(e->canonical.originator_fp, fp_hex); ent["originator_fp"] = fp_hex;
    fp_to_hex(e->canonical.cosigner_fp, fp_hex);   ent["cosigner_fp"]   = fp_hex;
  }

  // Size the output buffer from the document, allocate on the heap, and use
  // the bounds-checked serializeJson overload so a doc that grew beyond
  // measureJson's estimate cannot overflow.
  const size_t needed = measureJson(doc) + 1;
  // Cap the per-response size at a safe ceiling. AUDIT_PAGE_MAX=32 with
  // ~150 bytes per entry plus header ≈ 5 KB worst-case.
  const size_t MAX_RESPONSE = 8192;
  if (needed > MAX_RESPONSE) return send_error(req, "response too large; reduce limit");

  char* buf = (char*)malloc(needed);
  if (!buf) return send_error(req, "out of memory");
  serializeJson(doc, buf, needed);
  esp_err_t rc = send_json(req, buf);
  free(buf);
  return rc;
}

// POST /api/beacon/selftest — force a selftest broadcast (testing aid)
inline esp_err_t handle_selftest(httpd_req_t* req) {
  return beacon_channel::emit_selftest() ? send_success(req, "selftest emitted")
                                          : send_error(req, "selftest refused");
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

inline void register_routes(httpd_handle_t server, const char* api_token = nullptr) {
  auth_token_storage() = api_token;

  register_api_handler(server, "/api/beacon",              HTTP_GET,  bcn_auth_gated<handle_status>);
  register_api_handler(server, "/api/beacon/set",          HTTP_GET,  bcn_auth_gated<handle_set_list>);
  register_api_handler(server, "/api/beacon/active",       HTTP_GET,  bcn_auth_gated<handle_active>);
  register_api_handler(server, "/api/beacon/audit",        HTTP_GET,  bcn_auth_gated<handle_audit>);
  register_api_handler(server, "/api/beacon/pair/start",   HTTP_POST, bcn_auth_gated<handle_pair_start>);
  register_api_handler(server, "/api/beacon/pair/join",    HTTP_POST, bcn_auth_gated<handle_pair_join>);
  register_api_handler(server, "/api/beacon/pair/confirm", HTTP_POST, bcn_auth_gated<handle_pair_confirm>);
  register_api_handler(server, "/api/beacon/pair/cancel",  HTTP_POST, bcn_auth_gated<handle_pair_cancel>);
  register_api_handler(server, "/api/beacon/revoke",       HTTP_POST, bcn_auth_gated<handle_revoke>);
  register_api_handler(server, "/api/beacon/originate",      HTTP_POST, bcn_auth_gated<handle_originate>);
  register_api_handler(server, "/api/beacon/originate-solo", HTTP_POST, bcn_auth_gated<handle_originate_solo>);
  register_api_handler(server, "/api/beacon/cosign",         HTTP_POST, bcn_auth_gated<handle_cosign>);
  register_api_handler(server, "/api/beacon/cancel",       HTTP_POST, bcn_auth_gated<handle_cancel>);
  register_api_handler(server, "/api/beacon/selftest",     HTTP_POST, bcn_auth_gated<handle_selftest>);
}

} // namespace beacon_api

#endif // FEATURE_BEACON_CHANNEL

#endif // SECURACV_BEACON_API_H
