/**
 * @file identity_json.h
 * @brief The canary-wap's two identity answers, GET /api/device-info and the
 *        provisioning receipt, measured and escaped (sweep F212).
 *
 * Both answers were spelled with one snprintf() each into a fixed stack
 * buffer (768 and 1024 bytes) that nothing measured, and both wrote their
 * strings with %s unescaped. GET /api/device-info carries the device name a
 * person typed: the routes in front of setup_wizard::set_device_name()
 * (POST /api/wifi/connect's `device_name` and POST /api/device-name) require
 * 1 to 32 bytes of which one survives the mDNS-label sanitizer, so `"`, `\`
 * and control bytes are all accepted, and a name holding `"` made the answer
 * unparseable. That route answers without a token, and the device's own
 * pages read it (the dashboard, the companion page, the CSI dashboard), so
 * a name holding `"` broke what each of them reads from it.
 *
 * Here every string goes through wap_json_writer.h (escaped as JSON
 * requires), and each answer is measured before it is written: called with a
 * NULL buffer a builder returns the answer's length, so the handler
 * allocates exactly that plus the NUL (the measureJson() pattern of F196) and
 * no field's length or bytes has to be assumed. Given a buffer too small, a
 * builder writes nothing (out[0] = '\0') and returns the length it needed:
 * never a cut answer. The bytes are the old answers' bytes for every string
 * that needs no escape (pinned by the host test against the old formats).
 *
 * Pure hosted C++ (no Arduino, ESP-IDF or heap): host-tested in
 * tests_host/test_identity_json.cpp.
 */

#ifndef CANARY_WAP_IDENTITY_JSON_H
#define CANARY_WAP_IDENTITY_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "wap_json_writer.h"

namespace identity_json {

/* GET /api/device-info: what the handler reads. NULL strings read as "". */
struct DeviceInfo {
  const char* device_id;     /* g_device.device_id */
  const char* device_name;   /* setup_wizard::get_device_name(): person-typed */
  const char* mdns_host;     /* g_device.mdns_hostname */
  const char* firmware;      /* FIRMWARE_VERSION */
  const char* pubkey_fp;     /* g_device.fingerprint_hex */
  const char* hw_token;      /* device_pseudonym::device_id_hex() */
  uint64_t    uptime_ms;     /* millis() */
  uint64_t    chain_length;  /* g_device.seq */
  uint64_t    born_day;      /* g_device.born_day: days since the epoch, 0 = never dated */
  bool        born_exact;    /* g_device.born_exact */
  bool        tls_enabled;   /* g_tls_enabled */
  const char* ap_auth;       /* g_wifi_status.ap_auth, or "unknown" */
};

/* The provisioning receipt: what the handler reads. */
struct Receipt {
  const char* device_id;     /* g_device.device_id */
  bool        tls_enabled;   /* g_tls_enabled: the base_url's scheme */
  const char* ap_ip;         /* WiFi.softAPIP().toString() */
  const char* token;         /* g_device.api_token_str */
  const char* pubkey_fp;     /* g_device.fingerprint_hex */
  const char* firmware;      /* FIRMWARE_VERSION */
  const char* hw_token;      /* device_pseudonym::device_id_hex() */
  const char* ap_ssid;       /* g_device.ap_ssid */
  const char* ap_password;   /* g_device.ap_password */
  const char* tls_cert_fp;   /* g_tls_cert_fp_hex */
  uint64_t    boot_count;    /* g_device.boot_count */
};

inline void write_device_info(wap_json::Writer& w, const DeviceInfo& in) {
  wap_json::raw(w, "{\"device_id\":");
  wap_json::str(w, in.device_id);
  wap_json::raw(w, ",\"device_name\":");
  wap_json::str(w, in.device_name);
  wap_json::raw(w, ",\"mdns_host\":");
  wap_json::str(w, in.mdns_host);
  wap_json::raw(w, ",\"firmware\":");
  wap_json::str(w, in.firmware);
  wap_json::raw(w, ",\"pubkey_fp\":");
  wap_json::str(w, in.pubkey_fp);
  wap_json::raw(w, ",\"hw_token\":");
  wap_json::str(w, in.hw_token);
  wap_json::raw(w, ",\"uptime_ms\":");
  wap_json::number(w, in.uptime_ms);
  wap_json::raw(w, ",\"chain_length\":");
  wap_json::number(w, in.chain_length);
  wap_json::raw(w, ",\"born_day\":");
  wap_json::number(w, in.born_day);
  wap_json::raw(w, ",\"born_exact\":");
  wap_json::boolean_value(w, in.born_exact);
  wap_json::raw(w, ",\"auth_required\":true,\"tls_enabled\":");
  wap_json::boolean_value(w, in.tls_enabled);
  wap_json::raw(w, ",\"ap_auth\":");
  wap_json::str(w, in.ap_auth);
  wap_json::raw(w, ",\"provisioning_gate\":\"physical_button\"}");
}

inline void write_receipt(wap_json::Writer& w, const Receipt& in) {
  wap_json::raw(w, "{\n  \"device_id\": ");
  wap_json::str(w, in.device_id);
  wap_json::raw(w, ",\n  \"base_url\": \"");
  wap_json::raw(w, in.tls_enabled ? "https" : "http");
  wap_json::raw(w, "://");
  wap_json::escaped(w, in.ap_ip);
  wap_json::raw(w, "\",\n  \"token\": ");
  wap_json::str(w, in.token);
  wap_json::raw(w, ",\n  \"pubkey_fp\": ");
  wap_json::str(w, in.pubkey_fp);
  wap_json::raw(w, ",\n  \"firmware\": ");
  wap_json::str(w, in.firmware);
  wap_json::raw(w, ",\n  \"hw_token\": ");
  wap_json::str(w, in.hw_token);
  wap_json::raw(w, ",\n  \"ap_ssid\": ");
  wap_json::str(w, in.ap_ssid);
  wap_json::raw(w, ",\n  \"ap_password\": ");
  wap_json::str(w, in.ap_password);
  wap_json::raw(w, ",\n  \"tls_cert_fp\": ");
  wap_json::str(w, in.tls_cert_fp);
  wap_json::raw(w, ",\n  \"provisioned_at\": \"boot:");
  wap_json::number(w, in.boot_count);
  wap_json::raw(w, "\"\n}");
}

/* Measures with `write`, then writes into out[cap] when the answer and its
 * NUL fit; otherwise out[0] = '\0'. Returns the answer's length either way
 * (the NUL excluded), like snprintf(): NULL `out` only measures. */
template <typename In>
inline size_t build(void (*write)(wap_json::Writer&, const In&), const In& in, char* out, size_t cap) {
  wap_json::Writer m;
  wap_json::begin_measure(m);
  write(m, in);
  const size_t need = m.len;
  if (out == nullptr || cap == 0) return need;
  out[0] = '\0';
  if (need >= cap) return need;
  wap_json::Writer w;
  wap_json::begin(w, out, cap);
  write(w, in);
  if (!wap_json::ok(w)) out[0] = '\0';
  return need;
}

/* GET /api/device-info's answer into out[cap]; see build(). */
inline size_t device_info(const DeviceInfo& in, char* out, size_t cap) {
  return build(write_device_info, in, out, cap);
}

/* The provisioning receipt into out[cap]; see build(). */
inline size_t provisioning_receipt(const Receipt& in, char* out, size_t cap) {
  return build(write_receipt, in, out, cap);
}

}  // namespace identity_json

#endif  // CANARY_WAP_IDENTITY_JSON_H
