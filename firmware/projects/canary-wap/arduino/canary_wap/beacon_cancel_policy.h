/*
 * SecuraCV Canary — Beacon CANCEL origination policy (host-testable)
 *
 * Arduino-free: beacon_wire.h + string.h only. beacon_channel.cpp owns the
 * radio, the keys and the rate buckets; the DECISIONS of a network all-clear
 * live here so a host g++ run (tests_host/test_beacon_cancel_origination.cpp)
 * can pin them:
 *
 *   - which all-clear template a reason names (spec §4: 0x80–0x82), and
 *     what a cancel request body may hold (every field checked, none
 *     silently defaulted from a malformed value),
 *   - the canonical a CANCEL signs (spec §5.2 / §5.4: msg_type = Cancel,
 *     ref_canceled_nonce naming the alarm in force, CAP defaults for the
 *     all-clear templates, scope = Private, solo forces certainty = Observed),
 *   - the order the refusals are checked in, so the REST layer can name the
 *     reason instead of reporting a generic "refused",
 *   - what a cosigner agrees to be asked to sign (spec §6.1 step 3),
 *   - how an emitted frame's unsigned header follows its signed canonical, and
 *   - what an originator does with a frame it emitted itself.
 *
 * AGENTS.md Beacon invariants this file does not relax: a CANCEL is a Beacon
 * frame like any other, so it needs two signatures from two distinct paired
 * pubkeys (invariant 2), or — solo — the physical BOOT button held at the
 * moment of origination, BCN_FLAG_SOLO_ORIGIN on the header and
 * certainty = Observed. It is originated only by an explicit user action
 * (invariant 1); nothing here is called from a sensor path.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_BEACON_CANCEL_POLICY_H
#define SECURACV_BEACON_CANCEL_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "beacon_wire.h"

namespace beacon_cancel_policy {

// ── Reason → all-clear template ─────────────────────────────────────────────

enum CancelReason : uint8_t {
  CANCEL_REASON_RESOLVED    = 0,  // "situation resolved"
  CANCEL_REASON_SAFE        = 1,  // "area appears safe now"
  CANCEL_REASON_FALSE_ALARM = 2,  // "false alarm"
};

// The REST body's `reason` string. An unknown word is refused rather than
// mapped to a default: an all-clear that says something the operator did not
// choose is worse than no all-clear.
inline bool parse_cancel_reason(const char* s, CancelReason* out) {
  if (!s || !out) return false;
  if (strcmp(s, "resolved") == 0)    { *out = CANCEL_REASON_RESOLVED;    return true; }
  if (strcmp(s, "safe") == 0)        { *out = CANCEL_REASON_SAFE;        return true; }
  if (strcmp(s, "false_alarm") == 0) { *out = CANCEL_REASON_FALSE_ALARM; return true; }
  return false;
}

inline beacon_channel::BeaconTemplate cancel_template_for(CancelReason r) {
  switch (r) {
    case CANCEL_REASON_SAFE:        return beacon_channel::BCN_CLR_SAFE;
    case CANCEL_REASON_FALSE_ALARM: return beacon_channel::BCN_CLR_FALSE_ALARM;
    case CANCEL_REASON_RESOLVED:
    default:                        return beacon_channel::BCN_CLR_RESOLVED;
  }
}

// spec §4: the three all-clear templates are the only ones a CANCEL carries.
inline bool is_cancel_template(uint8_t id) {
  return id == beacon_channel::BCN_CLR_RESOLVED ||
         id == beacon_channel::BCN_CLR_SAFE ||
         id == beacon_channel::BCN_CLR_FALSE_ALARM;
}

// ── The REST body ───────────────────────────────────────────────────────────

// What one optional field of a cancel request holds, as the REST layer
// classified it (beacon_api.h classify_field, over ArduinoJson). The adapter
// only classifies; every decision is here, where the host test reaches it.
enum FieldKind : uint8_t {
  FIELD_ABSENT  = 0,  // key missing, or JSON null
  FIELD_INTEGER = 1,  // a JSON integer that fits in int32_t (`integer`)
  FIELD_STRING  = 2,  // a JSON string (`text`, non-null)
  FIELD_OTHER   = 3,  // anything else: a float, bool, array, object, or an
                      // integer outside int32_t
};

struct BodyField {
  FieldKind kind;
  int32_t integer;
  const char* text;
};

struct CancelRequestFields {
  beacon_channel::BeaconTemplate tpl;
  uint8_t certainty;
  uint32_t ttl_minutes;
};

// The CAP certainty labels (spec §5.2), exact spelling.
inline bool parse_certainty_label(const char* s, uint8_t* out) {
  if (!s || !out) return false;
  if (strcmp(s, "Observed") == 0) { *out = beacon_channel::BCN_CERT_OBSERVED; return true; }
  if (strcmp(s, "Likely") == 0)   { *out = beacon_channel::BCN_CERT_LIKELY;   return true; }
  if (strcmp(s, "Possible") == 0) { *out = beacon_channel::BCN_CERT_POSSIBLE; return true; }
  if (strcmp(s, "Unlikely") == 0) { *out = beacon_channel::BCN_CERT_UNLIKELY; return true; }
  if (strcmp(s, "Unknown") == 0)  { *out = beacon_channel::BCN_CERT_UNKNOWN;  return true; }
  return false;
}

static const uint32_t CANCEL_TTL_DEFAULT_MIN = 15;
static const uint32_t CANCEL_TTL_MAX_MIN = 1440;

// Validate a cancel request body. Every field is optional and an empty body
// is an empty object, taking the defaults: reason "resolved", certainty
// Likely, ttl_minutes 15. A field that is present is used only if it is
// well formed — a malformed one is refused by name, never replaced with the
// default, because an all-clear that says something the operator did not
// choose is worse than none:
//   - the body must be a JSON object (not an array, string or number);
//   - reason: one of resolved / safe / false_alarm;
//   - certainty: a CAP label or an integer 0..4, range-checked as an int
//     before it is narrowed (260 is refused, not wrapped to 4);
//   - ttl_minutes: an integer 1..1440 (so the frame is neither born expired
//     nor outlives a day) — not a string, not a fraction.
// Returns nullptr and fills `out`, or the refusal the REST layer sends.
inline const char* validate_cancel_request(bool body_is_object,
                                           const BodyField& reason,
                                           const BodyField& certainty,
                                           const BodyField& ttl_minutes,
                                           CancelRequestFields* out) {
  if (!out) return "internal error";
  if (!body_is_object) return "body must be a JSON object";

  CancelReason r = CANCEL_REASON_RESOLVED;
  if (reason.kind != FIELD_ABSENT) {
    if (reason.kind != FIELD_STRING || !parse_cancel_reason(reason.text, &r)) {
      return "reason must be resolved, safe or false_alarm";
    }
  }

  uint8_t cert = beacon_channel::BCN_CERT_LIKELY;
  if (certainty.kind == FIELD_INTEGER) {
    if (certainty.integer < beacon_channel::BCN_CERT_OBSERVED ||
        certainty.integer > beacon_channel::BCN_CERT_UNKNOWN) {
      return "certainty out of range";
    }
    cert = (uint8_t)certainty.integer;
  } else if (certainty.kind == FIELD_STRING) {
    if (!parse_certainty_label(certainty.text, &cert)) {
      return "certainty must be Observed, Likely, Possible, Unlikely, Unknown or 0..4";
    }
  } else if (certainty.kind != FIELD_ABSENT) {
    return "certainty must be Observed, Likely, Possible, Unlikely, Unknown or 0..4";
  }

  uint32_t ttl = CANCEL_TTL_DEFAULT_MIN;
  if (ttl_minutes.kind == FIELD_INTEGER) {
    if (ttl_minutes.integer < 1 || (uint32_t)ttl_minutes.integer > CANCEL_TTL_MAX_MIN) {
      return "ttl_minutes must be 1..1440";
    }
    ttl = (uint32_t)ttl_minutes.integer;
  } else if (ttl_minutes.kind != FIELD_ABSENT) {
    return "ttl_minutes must be an integer 1..1440";
  }

  out->tpl = cancel_template_for(r);
  out->certainty = cert;
  out->ttl_minutes = ttl;
  return nullptr;
}

// ── Refusal order ───────────────────────────────────────────────────────────

enum CancelRefusal : uint8_t {
  CANCEL_OK                        = 0,
  CANCEL_NO_ACTIVE_ALARM           = 1,  // nothing in force to name
  CANCEL_TIME_UNSYNCED             = 2,  // receivers would drop it as stale
  CANCEL_NO_COSIGNER               = 3,  // two-device path: nobody to ask
  CANCEL_PAIRED_COSIGNER_AVAILABLE = 4,  // solo path: use the two-device path
  CANCEL_BOOT_NOT_HELD             = 5,  // solo path: physical gate not met
};

// The stateless gates, in the order they are checked. The per-pubkey rate
// bucket is charged by the caller only after this returns CANCEL_OK, so a
// refusal for any reason here costs no budget.
//
// Solo mirrors originate_alert_solo (spec §6.2): refused while a fresh paired
// cosigner exists — the BOOT button stands in for a second key that is
// missing, not for one the operator would rather not ask — and then refused
// unless the BOOT button is held right now.
inline CancelRefusal decide_cancel_origination(bool active_alarm_valid,
                                               bool time_synced,
                                               bool solo,
                                               bool cosigner_available,
                                               bool boot_held) {
  if (!active_alarm_valid) return CANCEL_NO_ACTIVE_ALARM;
  if (!time_synced) return CANCEL_TIME_UNSYNCED;
  if (!solo) {
    return cosigner_available ? CANCEL_OK : CANCEL_NO_COSIGNER;
  }
  if (cosigner_available) return CANCEL_PAIRED_COSIGNER_AVAILABLE;
  if (!boot_held) return CANCEL_BOOT_NOT_HELD;
  return CANCEL_OK;
}

// ── The canonical a CANCEL signs ────────────────────────────────────────────

inline bool nonce_is_zero(const uint8_t n[beacon_channel::BEACON_NONCE_SIZE]) {
  for (size_t i = 0; i < beacon_channel::BEACON_NONCE_SIZE; i++) {
    if (n[i] != 0) return false;
  }
  return true;
}

// Fill `out` with the canonical for a CANCEL of the alarm whose header nonce
// is `active_nonce`. Urgency/severity are the spec §4 defaults for the
// all-clear templates (Past / Minor); scope is always Private (invariant 5);
// detail is none. A solo CANCEL is certainty = Observed whatever the caller
// passed, and names this device as both signers (spec §6.2 step 4). For the
// two-device path the cosigner fingerprint is filled in by the caller once a
// candidate is picked — before signing, since the signatures cover it.
inline void fill_cancel_canonical(beacon_channel::BeaconAlertCanonical& out,
                                  uint64_t now_unix, uint32_t ttl_minutes,
                                  uint8_t template_id, uint8_t certainty,
                                  const uint8_t active_nonce[beacon_channel::BEACON_NONCE_SIZE],
                                  const uint8_t originator_fp[beacon_channel::DEVICE_FP_SIZE],
                                  const uint8_t cosigner_fp[beacon_channel::DEVICE_FP_SIZE],
                                  bool solo) {
  memset(&out, 0, sizeof(out));
  out.effective = now_unix;
  out.expires = now_unix + (uint64_t)ttl_minutes * 60ULL;
  out.template_id = template_id;
  out.msg_type = beacon_channel::BEACON_MSG_CANCEL;
  out.urgency = beacon_channel::BCN_URG_PAST;
  out.severity = beacon_channel::BCN_SEV_MINOR;
  out.certainty = solo ? (uint8_t)beacon_channel::BCN_CERT_OBSERVED : certainty;
  out.scope = beacon_channel::BCN_SCOPE_PRIVATE;
  out.detail_slot = beacon_channel::BCN_DETAIL_NONE;
  memcpy(out.ref_canceled_nonce, active_nonce, beacon_channel::BEACON_NONCE_SIZE);
  memcpy(out.originator_fp, originator_fp, beacon_channel::DEVICE_FP_SIZE);
  memcpy(out.cosigner_fp, solo ? originator_fp : cosigner_fp,
         beacon_channel::DEVICE_FP_SIZE);
}

// ── What a cosigner agrees to be asked ──────────────────────────────────────

// A COSIGN_REQ this device will put in front of its user. ALERT is the
// long-standing path. A CANCEL is signable only when it is an all-clear
// template and names — by a non-zero nonce — the alarm THIS device holds: a
// cosigner attests an all-clear for an alarm it saw, never one it did not.
// UPDATE and EXERCISE (and anything else) are refused until each has its own
// reviewed origination path; accepting them here would open one silently.
inline bool cosign_request_acceptable(
    uint8_t msg_type, uint8_t template_id,
    const uint8_t ref_nonce[beacon_channel::BEACON_NONCE_SIZE],
    bool local_active_valid,
    const uint8_t local_active_nonce[beacon_channel::BEACON_NONCE_SIZE]) {
  if (msg_type == beacon_channel::BEACON_MSG_ALERT) return true;
  if (msg_type != beacon_channel::BEACON_MSG_CANCEL) return false;
  if (!is_cancel_template(template_id)) return false;
  if (!local_active_valid) return false;
  if (nonce_is_zero(ref_nonce)) return false;
  return memcmp(ref_nonce, local_active_nonce, beacon_channel::BEACON_NONCE_SIZE) == 0;
}

// ── The unsigned header an emitted frame wears ──────────────────────────────

// The signatures cover the canonical only, and every receiver drops a frame
// whose header msg_type disagrees with it (beacon_channel.cpp
// handle_alert_frame). So the header is derived from the canonical, never
// hardcoded: a CANCEL is emitted as a CANCEL.
inline uint8_t frame_header_msg_type(const beacon_channel::BeaconAlertCanonical& c) {
  return c.msg_type;
}

// Same rule for the flags receivers check: EXERCISE <=> BCN_FLAG_IS_EXERCISE
// (spec §5.4, a biconditional on receive), and BCN_FLAG_SOLO_ORIGIN exactly
// when the frame is a BOOT-button solo origination (spec §6.2).
inline uint8_t frame_header_flags(const beacon_channel::BeaconAlertCanonical& c,
                                  bool solo) {
  uint8_t f = 0;
  if (c.msg_type == beacon_channel::BEACON_MSG_EXERCISE) f |= beacon_channel::BCN_FLAG_IS_EXERCISE;
  if (solo) f |= beacon_channel::BCN_FLAG_SOLO_ORIGIN;
  return f;
}

// ── What the originator does with its own frame ─────────────────────────────

// ESP-NOW does not deliver a broadcast back to its sender, so a device that
// emits an ALERT would otherwise never enter ALARM itself and would hold no
// nonce to name in a later CANCEL. The originator adopts its own frame at
// hop 0 instead. The effect depends only on the signed msg_type and, for a
// CANCEL, on whether it names the alarm in force. Adoption takes no rate
// bucket — the originator's bucket was charged when it originated, and
// receivers charge theirs — which is why this decision has no rate input.
enum AdoptEffect : uint8_t {
  ADOPT_AUDIT_ONLY  = 0,  // audited, no state change
  ADOPT_RAISE_ALARM = 1,  // ALERT: this device enters ALARM, keeps the nonce
  ADOPT_CLEAR_ALARM = 2,  // CANCEL naming the alarm in force → SUPERVISORY
};

inline AdoptEffect adopt_effect(uint8_t msg_type, bool references_active_alarm) {
  if (msg_type == beacon_channel::BEACON_MSG_ALERT) return ADOPT_RAISE_ALARM;
  if (msg_type == beacon_channel::BEACON_MSG_CANCEL && references_active_alarm) {
    return ADOPT_CLEAR_ALARM;
  }
  return ADOPT_AUDIT_ONLY;
}

}  // namespace beacon_cancel_policy

#endif  // SECURACV_BEACON_CANCEL_POLICY_H
