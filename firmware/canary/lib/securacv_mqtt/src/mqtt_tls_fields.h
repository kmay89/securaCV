/*
 * SecuraCV Canary — broker-TLS provisioning fields, the pure half.
 *
 * The API half of what network/mqtt_transport.h reads back at connect time:
 * POST /api/mqtt/config may carry an optional `tls` (the mode byte, 0-3) and
 * `fp` (a SHA-256 pin in any spelling the firmware accepts), and POST
 * /api/mqtt/ca carries a PEM. This header turns those into the NVS writes the
 * shared decision will accept — or refuses them, at save time, with the SAME
 * verdict and the SAME constant text the firmware would produce at connect.
 *
 * Why a header of its own: it is dependency-free (only the pure decision
 * header), so firmware/tests_host/test_mqtt_tls_fields.cpp proves the
 * validation on the host while the HTTP handler around it is compile-tested
 * by CI only. Nothing here formats the pin or the PEM into a string.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "network/mqtt_transport_logic.h"

namespace canary::net::mqtt_tls_fields {

using canary::net::mqtt_tls::Caps;
using canary::net::mqtt_tls::Decision;
using canary::net::mqtt_tls::Reason;
using canary::net::mqtt_tls::kCaPemMax;
using canary::net::mqtt_tls::kFingerprintTextLen;
using canary::net::mqtt_tls::kModeCount;

// What NVS holds now. The caller reads these before planning; a fresh unit
// has none of them (mode byte absent → 0, no pin, no CA).
struct Current {
  uint8_t mode_byte = 0;      // mqtt_tls as stored, 0 when the key is absent
  const char* fp = nullptr;   // mqtt_fp as stored (canonical), nullptr / "" when absent
  bool ca_set = false;        // mqtt_ca present — accepted by check_ca() before it was written
};

// What the request body carried. A field the body did not name is absent and
// leaves NVS alone; `fp` present-and-empty forgets the stored pin.
struct Request {
  bool has_mode = false;
  long mode = 0;              // as parsed from JSON; validated here, not by the caller
  const char* fp = nullptr;   // nullptr = absent; "" = clear; else a pin in any accepted spelling
};

// The writes to perform, and the decision the firmware will reach once they
// are in NVS — reported back to the caller so the response can name it.
struct Plan {
  bool set_mode = false;
  uint8_t mode = 0;
  bool set_fp = false;
  char fp[kFingerprintTextLen + 1] = {0};  // canonical "AA:BB:..." (95 chars)
  bool clear_fp = false;
  Decision decision{};
};

enum class Verdict : uint8_t {
  Ok,
  ModeInvalid,           // `tls` outside the header's table
  FingerprintMalformed,  // `fp` is not a 32-byte SHA-256
  Refused,               // the writes are well-formed but the firmware would refuse to connect
  CaTooLarge,            // the PEM does not fit the firmware's CA buffer
  CaMalformed,           // the body is not a PEM certificate
};

// A stored CA was accepted by check_ca() before it reached NVS, so its
// presence is its validity here: plan() hands decide_u8() this PEM-shaped
// stand-in instead of loading 3 KB onto the HTTP task's stack to re-prove it.
constexpr const char kStoredCaStandIn[] =
    "-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n";

// Decide what POST /api/mqtt/config's `tls` / `fp` mean against what NVS
// already holds. `out` is fully overwritten. Only Verdict::Ok plans writes;
// every other verdict plans none, so a refused body changes nothing.
inline Verdict plan(const Current& cur, const Request& req, Plan& out) {
  out = Plan{};

  uint8_t mode_byte = cur.mode_byte;
  if (req.has_mode) {
    if (req.mode < 0 || req.mode >= (long)kModeCount) {
      out.decision.transport = canary::net::mqtt_tls::Transport::Refused;
      out.decision.reason = Reason::ModeUnknown;
      return Verdict::ModeInvalid;
    }
    out.set_mode = true;
    out.mode = (uint8_t)req.mode;
    mode_byte = out.mode;
  }

  const char* effective_fp = cur.fp ? cur.fp : "";
  if (req.fp != nullptr) {
    if (req.fp[0] == '\0') {
      out.clear_fp = true;
      effective_fp = "";
    } else if (canary::net::mqtt_tls::fingerprint_normalize(req.fp, out.fp, sizeof(out.fp)) == 0) {
      out = Plan{};
      out.decision.transport = canary::net::mqtt_tls::Transport::Refused;
      out.decision.reason = Reason::FingerprintMalformed;
      return Verdict::FingerprintMalformed;
    } else {
      out.set_fp = true;
      effective_fp = out.fp;
    }
  }

  Caps caps;  // WiFiClientSecure: pinning and the lab opt-in are both available
  caps.fingerprint = true;
  caps.insecure = true;
  out.decision = canary::net::mqtt_tls::decide_u8(mode_byte, cur.ca_set ? kStoredCaStandIn : "",
                                                   effective_fp, caps);
  if (!out.decision.allowed()) {
    const Decision d = out.decision;
    out = Plan{};
    out.decision = d;
    return Verdict::Refused;
  }
  return Verdict::Ok;
}

// ── The order one request's writes land ─────────────────────────────────────
// POST /api/mqtt/config may write up to three keys and the credential row.
// They land in ONE NVS session, in this order, and the writer raises the
// main loop's reload ONCE after that session closes:
//
//   the pin (set or cleared)  →  the mode byte  →  the credentials
//
// The main task's reload re-reads NVS and reconnects at once, and the two
// tasks share one NVS handle, so a credential row committed in a session of
// its own — with the mode byte still to come in a second — is a window in
// which the link comes up with the NEW password on the OLD (plain) socket,
// and in which whichever task closes the shared handle first closes it under
// the other. Writing the mode before the credentials, and stopping at the
// first failed write, means a later step can only ever land on top of every
// earlier one: the credentials never sit next to a mode they were not asked
// for, and a mode never lands without its pin. Sequenced here, pure, so the
// host test holds the order; securacv_mqtt.cpp's mqtt_save_config walks it.
enum class Write : uint8_t { PinSet, PinClear, ModeSet, Credentials };

struct WriteOrder {
  uint8_t count = 0;
  Write steps[4] = {};
};

inline WriteOrder write_order(bool set_fp, bool clear_fp, bool set_mode, bool credentials) {
  WriteOrder o;
  if (set_fp)      o.steps[o.count++] = Write::PinSet;
  if (clear_fp)    o.steps[o.count++] = Write::PinClear;
  if (set_mode)    o.steps[o.count++] = Write::ModeSet;
  if (credentials) o.steps[o.count++] = Write::Credentials;
  return o;
}

inline WriteOrder write_order(const Plan& p, bool credentials) {
  return write_order(p.set_fp, p.clear_fp, p.set_mode, credentials);
}

// Judge the body of POST /api/mqtt/ca before it can reach NVS. `len` is the
// received byte count (the caller NUL-terminates). An empty body is NOT a
// valid PEM — whether it means "forget the CA" is the route's decision.
inline Verdict check_ca(const char* pem, size_t len) {
  if (len > kCaPemMax) return Verdict::CaTooLarge;
  if (!canary::net::mqtt_tls::ca_pem_looks_valid(pem)) return Verdict::CaMalformed;
  return Verdict::Ok;
}

// The API's machine-readable error code ("" for Ok).
inline const char* error_code(Verdict v) {
  switch (v) {
    case Verdict::Ok:                   return "";
    case Verdict::ModeInvalid:          return "tls_mode_invalid";
    case Verdict::FingerprintMalformed: return "fp_malformed";
    case Verdict::Refused:              return "tls_refused";
    case Verdict::CaTooLarge:           return "ca_too_large";
    case Verdict::CaMalformed:          return "ca_malformed";
  }
  return "tls_refused";
}

// The operator-facing text for a verdict: the shared header's constant for
// everything the header has a word for, so the API and the serial log agree
// to the letter. Never the pin, the PEM or a credential.
inline const char* reason(Verdict v, const Decision& d) {
  switch (v) {
    case Verdict::Ok:                   return "";
    case Verdict::ModeInvalid:          return canary::net::mqtt_tls::reason_text(Reason::ModeUnknown);
    case Verdict::FingerprintMalformed: return canary::net::mqtt_tls::reason_text(Reason::FingerprintMalformed);
    case Verdict::Refused:              return canary::net::mqtt_tls::reason_text(d.reason);
    case Verdict::CaMalformed:          return canary::net::mqtt_tls::reason_text(Reason::CaMalformed);
    case Verdict::CaTooLarge:
      return "broker CA is longer than this firmware stores (3071 bytes) - upload the one CA certificate that signed the broker's certificate, not the whole chain";
  }
  return "";
}

}  // namespace canary::net::mqtt_tls_fields
