/**
 * @file csi_event_wire.h
 * @brief The ONE builder of the MQTT `events` body a committed csi_event
 *        publishes as — shared by the canary-wap sketch (csi_mqtt.cpp) and
 *        the canary PIO tree (csi_event_egress.cpp), so the two hosts
 *        cannot drift on the wire Home Assistant parses.
 *
 * Header-only and pure: snprintf into a caller buffer, no allocation, no
 * Arduino, no crypto. The signature is INJECTED — the caller passes
 * device_signature::sign_event plus the identity strings — so a host test
 * (firmware/tests_host/test_csi_event_wire.cpp) can pin the bytes with a
 * fake signer. Staged into the canary-wap sketch by setup.sh with the rest
 * of this directory; check_csi_sync.sh byte-gates the copy.
 *
 * Wire shape (locked against custom_components/securacv/sensor.py and
 * signature.py's verify_event, which rebuilds the canonical
 * `securacv-canary-sig|v1|event|<device>|<event_id>|<state>|<category>|
 * <privacy>|<motion>|<breath>|<bpm>` from these fields):
 *
 *   {"event_id":N,"event_type":S,"timestamp":N,"zone":"","confidence":S,
 *    "signed":B,"module":S,"type":S,"category":S,"privacy":S,"state":S,
 *    "motion":N,"breathing":N,"bpm":N,"duration_sec":N,"bundled":N,
 *    "replay":B,"v":1[,"alg":S,"fp":S,"sig":S]}
 *
 * `signed` is true only when a signature rides the body. (Before this
 * builder was shared, the canary-wap wrote `"signed":true` even on the
 * early-boot publish that carried no signature — a claim the payload
 * could not back.) Strings are chokepoint-sanitized ASCII (state names,
 * confidence words, module/type ids), so no JSON escaping is needed.
 */

#ifndef SECURACV_CSI_EVENT_WIRE_H
#define SECURACV_CSI_EVENT_WIRE_H

#include "csi_event.h"   /* csi_event_values_t, categories, privacy classes */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace csi_event_wire {

/* device_signature::sign_event's exact signature: sign the event canonical
 * and write the b64url signature into sig_out. Returns false when it
 * cannot sign (not initialized yet, buffer too small). */
typedef bool (*SignEventFn)(uint32_t event_id, const char* state,
                            const char* category, const char* privacy,
                            int motion, int breath, int bpm,
                            char* sig_out, size_t sig_cap);

/* Ed25519 signature, b64url without padding: 86 chars + NUL. Equal to
 * device_signature::SIG_B64URL_CAP (callers static_assert it). */
constexpr size_t SIG_B64URL_CAP = 87;

struct Signer {
  SignEventFn sign;             /* nullptr: publish unsigned */
  int         schema_v;         /* device_signature::SCHEMA_V */
  const char* alg;              /* device_signature::ALG_NAME */
  const char* fingerprint_hex;  /* device_signature::fingerprint_hex() */
};

inline const char* category_word(csi_event_category_t c) {
  return (c == CSI_CATEGORY_AMBIENT) ? "ambient"
       : (c == CSI_CATEGORY_ANOMALY) ? "anomaly" : "event";
}

inline const char* privacy_word(csi_privacy_class_t p) {
  return (p == CSI_PRIVACY_P2) ? "p2" : (p == CSI_PRIVACY_P1) ? "p1" : "p0";
}

/* Build the body. timestamp_ms is the device-monotonic millisecond mark the
 * event committed at (published as whole seconds); is_replay marks a
 * backfill republish so HA device triggers can skip it. Returns the byte
 * count written (excluding NUL), or 0 on overflow — never a clipped body. */
inline size_t build_event_body(char* body, size_t cap,
                               uint32_t                  event_id,
                               const char*               module_id,
                               const char*               type_name,
                               csi_event_category_t      category,
                               csi_privacy_class_t       privacy,
                               const csi_event_values_t* values,
                               uint32_t                  timestamp_ms,
                               uint16_t                  bundled_count,
                               bool                      is_replay,
                               const Signer&             signer) {
  if (!values || !body || cap < 32) return 0;
  const char* cat_s = category_word(category);
  const char* priv_s = privacy_word(privacy);
  const char* state_s = values->state_name[0] ? values->state_name : "unknown";
  const uint32_t ts_sec = timestamp_ms / 1000UL;

  /* Sign the canonical (device_id, event_id, state, category, privacy,
   * motion, breath, bpm) tuple; HA rebuilds the same string from the
   * parsed JSON to verify. If signing is unavailable or fails, the body
   * goes out unsigned — HA marks the device unverified but still accepts
   * the publish, and `signed` says false. */
  char sig_b64[SIG_B64URL_CAP] = "";
  const bool signed_ok = signer.sign != nullptr && signer.fingerprint_hex != nullptr &&
      signer.sign(event_id, state_s, cat_s, priv_s,
                  (int)values->motion_score,
                  (int)values->breathing_score,
                  (int)values->breathing_rate_bpm,
                  sig_b64, sizeof(sig_b64));

  /* event_id is a top-level field so HA's canonical rebuilder can read it
   * without parsing the topic; it is monotonic per device, so HA also uses
   * it for replay detection alongside the signature. */
  char sig_kv[SIG_B64URL_CAP + 64] = "";
  int kv_n;
  if (signed_ok) {
    kv_n = snprintf(sig_kv, sizeof(sig_kv),
             ",\"v\":%d,\"alg\":\"%s\",\"fp\":\"%s\",\"sig\":\"%s\"",
             signer.schema_v,
             signer.alg ? signer.alg : "",
             signer.fingerprint_hex,
             sig_b64);
  } else {
    kv_n = snprintf(sig_kv, sizeof(sig_kv), ",\"v\":%d", signer.schema_v);
  }
  /* Truncation guard: a clipped sig_kv would be appended verbatim and
   * produce invalid JSON (`,"sig":"AAA` with no closing quote). Clear it
   * on overflow — at worst the body carries no envelope and the verifier
   * reads it as unsigned. */
  bool envelope = signed_ok;
  if (kv_n <= 0 || (size_t)kv_n >= sizeof(sig_kv)) {
    sig_kv[0] = '\0';
    envelope = false;
  }

  const int n = snprintf(body, cap,
    "{"
      "\"event_id\":%lu,"
      "\"event_type\":\"%s\","
      "\"timestamp\":%lu,"
      "\"zone\":\"\","
      "\"confidence\":\"%s\","
      "\"signed\":%s,"
      "\"module\":\"%s\","
      "\"type\":\"%s\","
      "\"category\":\"%s\","
      "\"privacy\":\"%s\","
      "\"state\":\"%s\","
      "\"motion\":%u,"
      "\"breathing\":%u,"
      "\"bpm\":%u,"
      "\"duration_sec\":%u,"
      "\"bundled\":%u,"
      "\"replay\":%s"
      "%s"
    "}",
    (unsigned long)event_id,
    state_s,
    (unsigned long)ts_sec,
    values->confidence[0] ? values->confidence : "tentative",
    envelope ? "true" : "false",
    module_id ? module_id : "",
    type_name ? type_name : "",
    cat_s, priv_s,
    state_s,
    (unsigned)values->motion_score,
    (unsigned)values->breathing_score,
    (unsigned)values->breathing_rate_bpm,
    (unsigned)values->duration_sec,
    (unsigned)bundled_count,
    is_replay ? "true" : "false",
    sig_kv);
  if (n <= 0 || (size_t)n >= cap) return 0;
  return (size_t)n;
}

/* The tamper-topic bridge body for a system.integrity row: the shape the HA
 * per-type tamper sensors match ({"type": <kind>}; the general sensor fires
 * on any publish). Returns 0 unless the row IS a system.integrity tamper
 * with a kind, or on overflow. */
inline size_t build_tamper_bridge_body(char* out, size_t cap,
                                       const char* module_id,
                                       const char* type_name,
                                       const csi_event_values_t* values) {
  if (!out || cap == 0 || !module_id || !type_name || !values) return 0;
  if (!values->state_name[0]) return 0;
  if (strcmp(module_id, "system.integrity") != 0) return 0;
  if (strcmp(type_name, "tamper") != 0) return 0;
  const int n = snprintf(out, cap, "{\"type\":\"%s\",\"severity\":\"tamper\"}",
                         values->state_name);
  if (n <= 0 || (size_t)n >= cap) return 0;
  return (size_t)n;
}

}  // namespace csi_event_wire

#endif  // SECURACV_CSI_EVENT_WIRE_H
