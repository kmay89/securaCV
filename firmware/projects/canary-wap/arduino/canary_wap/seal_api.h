/*
 * seal_api.h — GET /api/device/seal, the sealed bearer's handshake, and the
 * hook api_auth.h opens a `Sealed` Authorization header through.
 *
 * The protocol, its limits and the pure code are api_seal.h; this file is
 * the Arduino glue: the query, the throttle, the random source, the
 * identity key's signature (device_signature::sign_seal) and the measured
 * answer. Included by canary_wap.ino alone: api_seal_open() below is the
 * strong definition of the weak hook api_auth.h declares, once.
 *
 * Unauthenticated by design, like GET /api/device/enroll: the answer is the
 * public enroll card, a fresh ephemeral public key and a signature over
 * them. No secret is read or written here (check_route_security.py's
 * PUBLIC_ALLOWLIST names it).
 */

#ifndef CANARY_WAP_SEAL_API_H
#define CANARY_WAP_SEAL_API_H

#include <Arduino.h>
#include <esp_http_server.h>
#include <esp_random.h>

#include <stdlib.h>
#include <string.h>

#include "api_seal.h"
#include "device_signature.h"

namespace seal_api {

static void fill_random(void* buf, size_t len) { esp_fill_random(buf, len); }

static esp_err_t send_fixed(httpd_req_t* req, const char* status, const char* body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, body);
}

/* GET /api/device/seal?nonce=<16-64 lowercase hex>&client_pub=<64 lowercase hex> */
static esp_err_t handle_seal(httpd_req_t* req) {
  char nonce[80] = {0};
  char client_pub[80] = {0};
  {
    /* nonce= + 64 and client_pub= + 64 and the '&': 146 at most. */
    char query[192];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "nonce", nonce, sizeof(nonce)) != ESP_OK ||
        httpd_query_key_value(query, "client_pub", client_pub, sizeof(client_pub)) != ESP_OK) {
      return send_fixed(req, "400 Bad Request",
                        "{\"error\":\"bad_request\",\"hint\":\"?nonce=<16-64 lowercase hex>&client_pub=<64 lowercase hex>\"}");
    }
  }
  if (!device_signature::whoami_nonce_ok(nonce)) {
    return send_fixed(req, "400 Bad Request",
                      "{\"error\":\"bad_nonce\",\"hint\":\"16-64 lowercase hex chars\"}");
  }
  if (!device_signature::seal_pub_hex_ok(client_pub)) {
    return send_fixed(req, "400 Bad Request",
                      "{\"error\":\"bad_client_pub\",\"hint\":\"64 lowercase hex chars (an X25519 public key)\"}");
  }
  /* A signing endpoint with no auth gets the enroll proof's throttle: about
   * four handshakes a second. A fleet book opens one per device per quarter
   * hour; only a hammering client sees 429. */
  static uint32_t s_last_ms = 0;
  const uint32_t now = millis();
  if (s_last_ms != 0 && (uint32_t)(now - s_last_ms) < 250) {
    return send_fixed(req, "429 Too Many Requests", "{\"error\":\"slow_down\"}");
  }
  s_last_ms = now;

  api_seal::Opened opened;
  const api_seal::Open o = api_seal::open_session(api_seal::g_table, device_signature::device_id(),
                                                   nonce, client_pub, fill_random, now, opened);
  if (o != api_seal::Open::OK) {
    return send_fixed(req, "500 Internal Server Error", "{\"error\":\"seal_failed\"}");
  }
  char sig_hex[device_signature::SIG_HEX_CAP];
  if (!device_signature::sign_seal(nonce, client_pub, opened.device_pub_hex, sig_hex, sizeof(sig_hex))) {
    return send_fixed(req, "500 Internal Server Error", "{\"error\":\"seal_failed\"}");
  }

  api_seal::Answer a = {};
  a.device_id       = device_signature::device_id();
  a.pubkey_hex      = device_signature::pubkey_hex();
  a.fingerprint_hex = device_signature::fingerprint_hex();
  a.nonce           = nonce;
  a.sid_hex         = opened.sid_hex;
  a.device_pub_hex  = opened.device_pub_hex;
  a.sig_hex         = sig_hex;
  a.ttl_s           = api_seal::TTL_MS / 1000;
  /* Measured, then written into exactly that much heap (sweep F212). */
  const size_t need = api_seal::answer(a, nullptr, 0) + 1;
  char* json = (char*)malloc(need);
  if (!json) return send_fixed(req, "500 Internal Server Error", "{\"error\":\"out_of_memory\"}");
  if (api_seal::answer(a, json, need) >= need) {
    free(json);
    return send_fixed(req, "503 Service Unavailable", "{\"error\":\"answer_changed\"}");
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const esp_err_t ret = httpd_resp_sendstr(req, json);
  free(json);
  return ret;
}

/* The request's method as the client spelled it in the seal's associated
 * data: the two verbs the fleet book rings, and the rest by name. */
static const char* method_name(int method) {
  switch (method) {
    case HTTP_GET:    return "GET";
    case HTTP_POST:   return "POST";
    case HTTP_PUT:    return "PUT";
    case HTTP_DELETE: return "DELETE";
    default:          return "OTHER";
  }
}

}  /* namespace seal_api */

/* api_auth.h's hook: open `value` (the header after "Sealed ") for this
 * request into token_out; false with `reason` set otherwise. */
bool api_seal_open(httpd_req_t* req, const char* value, char* token_out, size_t token_cap,
                   const char** reason) {
  const api_seal::Unseal u = api_seal::unseal(api_seal::g_table, seal_api::method_name(req->method),
                                              req->uri, value, millis(), token_out, token_cap);
  if (reason) *reason = api_seal::reason(u);
  return u == api_seal::Unseal::OK;
}

#endif  /* CANARY_WAP_SEAL_API_H */
