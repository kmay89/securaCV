// Host tests for firmware/canary/lib/securacv_mqtt/src/mqtt_tls_fields.h —
// the API half of the canary's broker-TLS provisioning: the optional `tls` /
// `fp` fields of POST /api/mqtt/config and the body of POST /api/mqtt/ca,
// turned into the NVS writes the shared decision (mqtt_transport_logic.h)
// will accept, and refused with the header's own constant text otherwise.
//
// The property that matters: the API refuses EXACTLY what the firmware would
// refuse at connect, using the same decide_u8() / fingerprint_normalize() /
// ca_pem_looks_valid() — never a second regex or a second table — so a save
// that succeeds is a save that connects (or is the lab opt-in, which warns).
// And no text this header returns may carry the pin or the PEM it judged.

#include "../canary/lib/securacv_mqtt/src/mqtt_tls_fields.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace canary::net::mqtt_tls;
using namespace canary::net::mqtt_tls_fields;

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);       \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      g_failures++;                                            \
    }                                                          \
  } while (0)

static const char kPem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBszCCAVmgAwIBAgIUQ0FOQVJZLVRFU1QtQ0EwCgYIKoZIzj0EAwIwGjEYMBYG\n"
    "A1UEAwwPU2VjdXJhQ1YgVGVzdCBDQTAeFw0yNjAxMDEwMDAwMDBaFw0zNjAxMDEw\n"
    "-----END CERTIFICATE-----\n";

static const char kFpColons[] =
    "0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9:0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9";
static const char kFpBare[] =
    "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9";

static bool echoes(const char* hay, const char* needle) {
  return hay && needle && std::strstr(hay, needle) != nullptr;
}

// ── A body that names neither field leaves NVS alone ────────────────────────
static void absent_fields_write_nothing_and_keep_the_current_decision() {
  Current cur;  // fresh unit: no mode byte, no pin, no CA
  Request req;  // {"host":...} only
  Plan p;
  CHECK(plan(cur, req, p) == Verdict::Ok, "absent tls/fp is Ok on a plain unit");
  CHECK(!p.set_mode && !p.set_fp && !p.clear_fp, "absent fields plan no write");
  CHECK(p.decision.transport == Transport::Plain, "and the unit stays plain");

  // A unit already provisioned for a pin keeps it when a later config body
  // only changes the host.
  cur.mode_byte = 2;
  cur.fp = kFpColons;
  CHECK(plan(cur, req, p) == Verdict::Ok, "absent fields on a pinned unit is Ok");
  CHECK(!p.set_mode && !p.set_fp && !p.clear_fp, "no write for a pinned unit either");
  CHECK(p.decision.transport == Transport::TlsFingerprint, "the pin stays in force");
}

// ── The mode byte is validated by the header's table, nothing else ──────────
static void mode_outside_the_table_is_refused_with_the_headers_text() {
  Current cur;
  Plan p;
  const long bad[] = {4, -1, 255, 256, 300, 1000000};
  for (long m : bad) {
    Request req;
    req.has_mode = true;
    req.mode = m;
    const Verdict v = plan(cur, req, p);
    CHECK(v == Verdict::ModeInvalid, "mode %ld is refused", m);
    CHECK(std::strcmp(error_code(v), "tls_mode_invalid") == 0, "error code for mode %ld", m);
    CHECK(std::strcmp(reason(v, p.decision), reason_text(Reason::ModeUnknown)) == 0,
          "the reason is the header's ModeUnknown text, verbatim");
    CHECK(!p.set_mode, "a refused mode plans no write");
  }
  // Every byte in the table is a mode (whether it then connects is the
  // decision's business, below): on a fresh unit 0 and 3 plan the write, 1
  // and 2 are Refused for the missing CA / pin — and a refused body plans
  // nothing, so NVS is never left half-provisioned.
  for (long m = 0; m < (long)kModeCount; m++) {
    Request req;
    req.has_mode = true;
    req.mode = m;
    const Verdict v = plan(cur, req, p);
    CHECK(v != Verdict::ModeInvalid, "mode %ld is in the table", m);
    CHECK(v == Verdict::Ok || v == Verdict::Refused, "mode %ld is Ok or Refused, never malformed", m);
    CHECK(p.set_mode == (v == Verdict::Ok), "mode %ld is planned as a write only when Ok", m);
    if (v == Verdict::Ok) CHECK(p.mode == (uint8_t)m, "mode %ld is the planned byte", m);
  }
}

// ── Plain and lab need nothing else; lab is Ok and flagged to warn ──────────
static void plain_and_lab_need_no_secret_and_lab_warns() {
  Current cur;
  Plan p;
  Request req;
  req.has_mode = true;
  req.mode = 0;
  CHECK(plan(cur, req, p) == Verdict::Ok, "plain is Ok");
  CHECK(p.decision.transport == Transport::Plain && !p.decision.warn_insecure(), "plain socket");

  req.mode = 3;
  CHECK(plan(cur, req, p) == Verdict::Ok, "lab is Ok by name");
  CHECK(p.decision.transport == Transport::TlsInsecure, "lab socket");
  CHECK(p.decision.warn_insecure(), "and the caller must warn on every connect");
  CHECK(reason(Verdict::Ok, p.decision)[0] == '\0', "Ok carries no reason text");
}

// ── CA mode is refused until the separate upload has landed ─────────────────
static void ca_mode_is_refused_without_a_stored_ca_and_ok_with_one() {
  Current cur;
  Plan p;
  Request req;
  req.has_mode = true;
  req.mode = 1;
  Verdict v = plan(cur, req, p);
  CHECK(v == Verdict::Refused, "CA mode with no stored CA is refused at save time");
  CHECK(p.decision.reason == Reason::CaMissing, "for the header's CaMissing reason");
  CHECK(std::strcmp(error_code(v), "tls_refused") == 0, "error code");
  CHECK(std::strcmp(reason(v, p.decision), reason_text(Reason::CaMissing)) == 0,
        "the reason is the header's CaMissing text, verbatim");

  cur.ca_set = true;  // POST /api/mqtt/ca accepted a PEM earlier
  v = plan(cur, req, p);
  CHECK(v == Verdict::Ok, "CA mode with a stored CA is Ok");
  CHECK(p.decision.transport == Transport::TlsCa, "CA-verified socket");
  CHECK(p.set_mode && p.mode == 1, "mode 1 is planned");
  CHECK(ca_pem_looks_valid(kStoredCaStandIn), "the stand-in for a stored CA is PEM-shaped to the header");
}

// ── Fingerprint mode needs a pin: in this body, or already stored ───────────
static void fingerprint_mode_needs_a_pin_and_normalizes_it() {
  Current cur;
  Plan p;
  Request req;
  req.has_mode = true;
  req.mode = 2;

  Verdict v = plan(cur, req, p);
  CHECK(v == Verdict::Refused, "pin mode with no pin anywhere is refused");
  CHECK(p.decision.reason == Reason::FingerprintMissing, "as FingerprintMissing");
  CHECK(std::strcmp(reason(v, p.decision), reason_text(Reason::FingerprintMissing)) == 0,
        "verbatim header text");

  req.fp = kFpBare;  // lowercase, no separators — one of the accepted spellings
  v = plan(cur, req, p);
  CHECK(v == Verdict::Ok, "pin mode with a pin in the body is Ok");
  CHECK(p.set_fp && std::strcmp(p.fp, kFpColons) == 0, "the pin is planned in canonical form: %s", p.fp);
  CHECK(!p.clear_fp, "not a clear");
  CHECK(p.decision.transport == Transport::TlsFingerprint, "pinned socket");

  // A stored pin satisfies a later body that only sets the mode.
  Current pinned;
  pinned.fp = kFpColons;
  Request mode_only;
  mode_only.has_mode = true;
  mode_only.mode = 2;
  v = plan(pinned, mode_only, p);
  CHECK(v == Verdict::Ok, "pin mode with a stored pin is Ok");
  CHECK(!p.set_fp, "and does not rewrite the pin");

  // A malformed pin is refused with the header's text — and the text never
  // repeats what was pasted.
  const char* bad[] = {"not-a-pin", "0A:1B", "0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9:0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9:00",
                       "0G1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4E5F60718293A4B5C6D7E8F9"};
  for (const char* b : bad) {
    Request r;
    r.fp = b;
    const Verdict bv = plan(cur, r, p);
    CHECK(bv == Verdict::FingerprintMalformed, "'%s' is refused as malformed", b);
    CHECK(std::strcmp(error_code(bv), "fp_malformed") == 0, "error code");
    CHECK(std::strcmp(reason(bv, p.decision), reason_text(Reason::FingerprintMalformed)) == 0,
          "verbatim header text");
    CHECK(!echoes(reason(bv, p.decision), b), "the reason never echoes the pasted pin");
    CHECK(!p.set_fp, "a refused pin plans no write");
  }
}

// ── An empty fp clears the pin; the decision is then re-judged ──────────────
static void empty_fp_clears_the_pin_and_is_judged_against_the_mode() {
  Current cur;
  cur.mode_byte = 0;
  cur.fp = kFpColons;
  Plan p;
  Request req;
  req.fp = "";  // present-and-empty = forget the pin
  CHECK(plan(cur, req, p) == Verdict::Ok, "clearing the pin on a plain unit is Ok");
  CHECK(p.clear_fp && !p.set_fp, "planned as a clear");

  cur.mode_byte = 2;  // a pinned unit asked to forget its pin without leaving pin mode
  const Verdict v = plan(cur, req, p);
  CHECK(v == Verdict::Refused, "that would leave pin mode with no pin: refused");
  CHECK(p.decision.reason == Reason::FingerprintMissing, "as FingerprintMissing");

  // Leaving pin mode in the same body makes it fine.
  req.has_mode = true;
  req.mode = 0;
  CHECK(plan(cur, req, p) == Verdict::Ok, "clear + back to plain is Ok");
  CHECK(p.set_mode && p.mode == 0 && p.clear_fp, "both writes planned");
}

// ── A stored pin that is not a pin is reported as malformed, not missing ────
static void a_stored_malformed_pin_is_named_as_such() {
  Current cur;
  cur.mode_byte = 2;
  cur.fp = "?";  // what BrokerTransport::load keeps for a set-but-unparseable key
  Plan p;
  Request req;  // host-only body
  const Verdict v = plan(cur, req, p);
  CHECK(v == Verdict::Refused, "refused");
  CHECK(p.decision.reason == Reason::FingerprintMalformed, "as FingerprintMalformed, so the operator re-pastes it");
}

// ── The CA upload: bounded, PEM-shaped, never echoed ────────────────────────
static void ca_upload_is_bounded_and_pem_shaped() {
  CHECK(check_ca(kPem, std::strlen(kPem)) == Verdict::Ok, "a PEM is accepted");
  CHECK(std::strcmp(error_code(Verdict::Ok), "") == 0, "Ok has no error code");

  const Verdict m = check_ca(kFpColons, std::strlen(kFpColons));
  CHECK(m == Verdict::CaMalformed, "a fingerprint in the CA box is refused");
  CHECK(std::strcmp(error_code(m), "ca_malformed") == 0, "error code");
  Decision none;
  CHECK(std::strcmp(reason(m, none), reason_text(Reason::CaMalformed)) == 0, "verbatim header text");
  CHECK(!echoes(reason(m, none), kFpColons), "the reason never echoes the body");

  // Exactly the cap is fine; one more byte is not — the same kCaPemMax the
  // transport's 3072-byte buffer is sized to, so nothing accepted here reads
  // back empty from NVS later.
  std::string big = "-----BEGIN CERTIFICATE-----\n";
  while (big.size() < kCaPemMax - std::strlen("-----END CERTIFICATE-----\n")) big += 'A';
  big += "-----END CERTIFICATE-----\n";
  CHECK(big.size() == kCaPemMax, "fixture is exactly kCaPemMax (%zu)", big.size());
  CHECK(check_ca(big.c_str(), big.size()) == Verdict::Ok, "kCaPemMax bytes are accepted");
  big += 'A';
  const Verdict t = check_ca(big.c_str(), big.size());
  CHECK(t == Verdict::CaTooLarge, "kCaPemMax + 1 is refused as too large");
  CHECK(std::strcmp(error_code(t), "ca_too_large") == 0, "error code");
  CHECK(reason(t, none)[0] != '\0', "too-large has a reason");
  CHECK(!echoes(reason(t, none), "AAAA"), "the reason never echoes the body");

  CHECK(check_ca("", 0) == Verdict::CaMalformed, "an empty PEM is malformed (a clear is the caller's decision, not this check's)");
  CHECK(check_ca(nullptr, 0) == Verdict::CaMalformed, "nullptr is malformed");
}

// ── One request's writes land TLS-first, credentials last, nothing dropped ──
// The review of the first cut found POST /api/mqtt/config committing the
// credentials (and raising the main loop's reload) in a session of its own,
// with the mode byte still to come in a second: a reload in that gap connects
// with the new password on the old, plain socket. The order is now a pure
// sequence the firmware writer walks, so this holds it: every TLS step comes
// before the credentials, the pin before the mode, exactly one step per
// requested write.
static void config_writes_land_tls_first_and_credentials_last() {
  for (int mask = 0; mask < 16; mask++) {
    const bool set_fp = mask & 1, clear_fp = mask & 2, set_mode = mask & 4, creds = mask & 8;
    if (set_fp && clear_fp) continue;  // plan() never produces both
    const WriteOrder o = write_order(set_fp, clear_fp, set_mode, creds);
    const int expected = (int)set_fp + (int)clear_fp + (int)set_mode + (int)creds;
    CHECK(o.count == expected, "mask %d: %d steps planned, %d requested", mask, o.count, expected);
    CHECK(o.count <= 4, "mask %d: never more steps than the array holds", mask);
    int pos_pin = -1, pos_mode = -1, pos_creds = -1;
    int n_pin = 0, n_mode = 0, n_creds = 0;
    for (int i = 0; i < o.count; i++) {
      switch (o.steps[i]) {
        case Write::PinSet:
        case Write::PinClear:  pos_pin = i;   n_pin++;   break;
        case Write::ModeSet:   pos_mode = i;  n_mode++;  break;
        case Write::Credentials: pos_creds = i; n_creds++; break;
      }
    }
    CHECK(n_pin == (int)(set_fp || clear_fp), "mask %d: the pin step appears exactly when asked", mask);
    CHECK(n_mode == (int)set_mode, "mask %d: the mode step appears exactly when asked", mask);
    CHECK(n_creds == (int)creds, "mask %d: the credentials step appears exactly when asked", mask);
    if (creds && (set_fp || clear_fp || set_mode)) {
      CHECK(pos_creds == o.count - 1, "mask %d: the credentials land LAST (at %d of %d)", mask, pos_creds, o.count);
      if (pos_mode >= 0) CHECK(pos_mode < pos_creds, "mask %d: the mode byte lands before the credentials", mask);
      if (pos_pin >= 0) CHECK(pos_pin < pos_creds, "mask %d: the pin lands before the credentials", mask);
    }
    if (set_mode && (set_fp || clear_fp)) {
      CHECK(pos_pin < pos_mode, "mask %d: the pin lands before the mode byte, so a mode never lands without its secret", mask);
    }
    if (set_fp) CHECK(o.steps[pos_pin] == Write::PinSet, "mask %d: a set is a PinSet", mask);
    if (clear_fp) CHECK(o.steps[pos_pin] == Write::PinClear, "mask %d: a clear is a PinClear", mask);
  }

  // The Plan overload agrees with the flag form for what plan() produces:
  // the wizard's usual body (mode 2 + a pin + credentials) and the
  // credentials-only body (no TLS field named).
  Current cur;
  Request req;
  req.has_mode = true;
  req.mode = 2;
  req.fp = kFpBare;
  Plan p;
  CHECK(plan(cur, req, p) == Verdict::Ok, "pin + mode is Ok");
  const WriteOrder full = write_order(p, true);
  CHECK(full.count == 3, "pin, mode, credentials: three steps (%d)", full.count);
  CHECK(full.steps[0] == Write::PinSet && full.steps[1] == Write::ModeSet && full.steps[2] == Write::Credentials,
        "in that order");
  Request host_only;
  CHECK(plan(cur, host_only, p) == Verdict::Ok, "host-only is Ok");
  const WriteOrder creds_only = write_order(p, true);
  CHECK(creds_only.count == 1 && creds_only.steps[0] == Write::Credentials, "credentials alone: one step");
  const WriteOrder nothing = write_order(p, false);
  CHECK(nothing.count == 0, "no credentials and no TLS field: nothing to write");
}

// ── Every verdict has a code; every non-Ok verdict has text ─────────────────
static void every_verdict_has_a_code_and_text() {
  const Verdict all[] = {Verdict::Ok, Verdict::ModeInvalid, Verdict::FingerprintMalformed,
                         Verdict::Refused, Verdict::CaTooLarge, Verdict::CaMalformed};
  Decision refused;
  refused.transport = Transport::Refused;
  refused.reason = Reason::CaMissing;
  for (Verdict v : all) {
    CHECK(error_code(v) != nullptr, "code exists for %d", (int)v);
    CHECK(reason(v, refused) != nullptr, "reason exists for %d", (int)v);
    if (v != Verdict::Ok) {
      CHECK(error_code(v)[0] != '\0', "non-Ok %d has a code", (int)v);
      CHECK(reason(v, refused)[0] != '\0', "non-Ok %d has text", (int)v);
    }
    // API error codes are identifiers: lowercase, digits, underscore.
    for (const char* c = error_code(v); *c; c++) {
      CHECK((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_', "code char '%c'", *c);
    }
  }
}

int main() {
  absent_fields_write_nothing_and_keep_the_current_decision();
  mode_outside_the_table_is_refused_with_the_headers_text();
  plain_and_lab_need_no_secret_and_lab_warns();
  ca_mode_is_refused_without_a_stored_ca_and_ok_with_one();
  fingerprint_mode_needs_a_pin_and_normalizes_it();
  empty_fp_clears_the_pin_and_is_judged_against_the_mode();
  a_stored_malformed_pin_is_named_as_such();
  ca_upload_is_bounded_and_pem_shaped();
  config_writes_land_tls_first_and_credentials_last();
  every_verdict_has_a_code_and_text();
  if (g_failures) {
    std::printf("test_mqtt_tls_fields: %d FAILED\n", g_failures);
    return 1;
  }
  std::puts("test_mqtt_tls_fields: all passed");
  return 0;
}
