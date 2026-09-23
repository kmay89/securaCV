/*
 * SecuraCV Canary — broker-TLS provisioning fields, the pure half.
 *
 * The API half of what network/mqtt_transport.h reads back at connect time:
 * POST /api/mqtt/config may carry an optional `tls` (the mode byte, 0-3) and
 * `fp` (a SHA-256 pin in any spelling the firmware accepts), and POST
 * /api/mqtt/ca carries a PEM. This header turns those into the NVS writes the
 * shared decision will accept — or refuses them, at save time, with the SAME
 * verdict and the SAME constant text the firmware would produce at connect.
 * It also holds the one rule for what a config body may carry over from the
 * stored credential row (credential_carry, below): a stored broker password
 * never follows the link to a new host or port.
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
#include <string.h>  // strlen — credential_carry's host compare

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
  bool ca_set = false;        // mqtt_ca present AND readable: it fits the transport's CA buffer
  bool ca_unreadable = false; // mqtt_ca present but NOT readable — the key exists and the
                              // transport's load() reads it back empty (too long for the
                              // buffer, or not string-typed), so the connect would refuse
                              // CaMissing for a CA that is there. Never both flags at once.
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
  CaUnreadable,          // a CA key exists but the firmware cannot read it back (409: DELETE and re-upload)
  PasswordRequiredForNewHost,  // the endpoint changes, a password is stored, the body gave none (400)
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
  // An unreadable CA is no CA to the decision — exactly what load() hands
  // decide() at connect — so a CA-verified mode over it is refused here too.
  const bool ca_usable = cur.ca_set && !cur.ca_unreadable;
  out.decision = canary::net::mqtt_tls::decide_u8(mode_byte, ca_usable ? kStoredCaStandIn : "",
                                                   effective_fp, caps);
  if (!out.decision.allowed()) {
    const Decision d = out.decision;
    out = Plan{};
    out.decision = d;
    // "No CA stored" would be a false statement when a key exists: name the
    // real condition, with the way out (DELETE /api/mqtt/ca, upload again).
    if (d.reason == Reason::CaMissing && cur.ca_unreadable) return Verdict::CaUnreadable;
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

// ── What a config body may carry over from the stored credential row ────────
// POST /api/mqtt/config always rewrites host / port / enabled; the username
// and password it may leave out. The first cut kept the STORED user and
// password under a body that omitted them — whatever the host. So a body of
// {"host":"<somewhere else>"} repointed the link AND sent the household's
// stored broker password to that new address in the next CONNECT packet: one
// request, from anyone holding the bearer, to harvest the credential.
//
// The rule, pure and host-tested (test_mqtt_tls_fields.cpp), the API enforces
// (handle_mqtt_config) and the writer obeys (write_credentials):
//
//   · an endpoint is host + port; the host compares trimmed and
//     ASCII-case-insensitively (DNS is), the port exactly;
//   · the SAME endpoint keeps whatever the body omitted — the re-run of
//     /setup that changes only a password, or only a username, and the
//     first-time CA save that posts no password at all, all stand as today;
//   · a NEW endpoint (host or port changed) carries NOTHING: the stored
//     username and password are removed unless the body supplies them again —
//     and when a password IS stored and the body gives none, the request is
//     REFUSED before any write (Verdict::PasswordRequiredForNewHost, 400),
//     because the only thing that request could do is send the stored secret
//     somewhere it was never given for;
//   · a fresh unit (no stored host) or an anonymous-broker row (no stored
//     password) moving to a new endpoint is allowed: there is no secret to
//     carry, and any stored username goes with the old endpoint.
//
// A port-only change (the wizard's "use 8883" button on a unit that already
// has a password) therefore asks for the password again — a deliberate
// friction the reason sentence names. Relaxing the rule to host-only later
// is a change to THIS function, never to the writer: keep_user / keep_pass
// are separate so a later rule can carry one and not the other.
struct CredentialCarry {
  bool refuse = false;     // answer 400 PasswordRequiredForNewHost; write nothing
  bool keep_user = false;  // the stored username stands when the body omits one; false = removed unless given
  bool keep_pass = false;  // the stored password stands when the body omits one; false = removed unless given
};

// Trimmed (spaces / tabs at either end), ASCII-case-insensitive host equality.
// Two empties are NOT the same endpoint: an empty stored host is a fresh unit.
inline bool same_broker_host(const char* stored, const char* fresh) {
  if (!stored || !fresh) return false;
  auto trim = [](const char*& p, size_t& n) {
    n = strlen(p);
    while (n && (p[0] == ' ' || p[0] == '\t')) { p++; n--; }
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
  };
  size_t ns = 0, nf = 0;
  trim(stored, ns);
  trim(fresh, nf);
  if (ns == 0 || ns != nf) return false;
  for (size_t i = 0; i < ns; i++) {
    char a = stored[i], b = fresh[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return true;
}

// `stored_host` / `stored_port` / `stored_pass` describe the row NVS holds
// (an empty or null host = a fresh unit); `new_host` / `new_port` the body's
// endpoint; `pass_given` whether the body carries a non-empty password.
inline CredentialCarry credential_carry(const char* stored_host, uint16_t stored_port, bool stored_pass,
                                        const char* new_host, uint16_t new_port, bool pass_given) {
  CredentialCarry c;
  const bool fresh = !stored_host || stored_host[0] == '\0';
  const bool same_endpoint = !fresh && same_broker_host(stored_host, new_host) && stored_port == new_port;
  if (same_endpoint) {
    c.keep_user = true;
    c.keep_pass = true;
    return c;
  }
  // A new endpoint. Only a stored password with nothing to replace it makes
  // this a refusal; everything else is allowed with nothing carried.
  c.refuse = !fresh && stored_pass && !pass_given;
  return c;
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
    case Verdict::CaUnreadable:         return "ca_unreadable";
    case Verdict::PasswordRequiredForNewHost: return "password_required_for_new_host";
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
    case Verdict::CaUnreadable:
      return "the stored broker CA is unreadable (it does not fit this firmware's 3071-byte buffer, so the connect would refuse it): DELETE /api/mqtt/ca and upload it again";
    case Verdict::PasswordRequiredForNewHost:
      return "moving the hub link to a new address or port needs the hub password typed again; a stored password is never carried to a new endpoint";
  }
  return "";
}

}  // namespace canary::net::mqtt_tls_fields
