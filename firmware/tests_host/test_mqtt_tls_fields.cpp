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
// Since the security sweep it also holds the credential-carry rule: a stored
// broker password never follows the link to a new host or port.

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

// ── A stored password never follows the link to a new host or port ──────────
// The security sweep found POST /api/mqtt/config {"host":"<elsewhere>"}
// keeping the STORED username and password (the writer skipped empty fields)
// and the reload sending them to the new host in the next CONNECT packet.
// credential_carry() is the one rule; the API refuses on `refuse` before any
// write and the writer removes what is not kept.
static void a_stored_password_is_never_carried_to_a_new_endpoint() {
  // Same endpoint: whatever the body omitted stands — the /setup re-run that
  // changes only the password (test_canary_setup_page.test.js: "changing
  // only the hub password") or only the username ("re-saved in CA mode with
  // the box empty") keeps the other half of the row.
  CredentialCarry c = credential_carry("hub.lan", 8883, true, "hub.lan", 8883, /*pass_given=*/false);
  CHECK(!c.refuse && c.keep_user && c.keep_pass, "same host and port, no password in the body: keep both");
  c = credential_carry("hub.lan", 8883, true, "hub.lan", 8883, true);
  CHECK(!c.refuse && c.keep_user && c.keep_pass, "same endpoint with a new password: the username still stands");

  // The host compares the way DNS does — trimmed, case-insensitive — so a
  // person retyping the address is not asked for the password again.
  c = credential_carry("hub.lan", 8883, true, "  Hub.LAN ", 8883, false);
  CHECK(!c.refuse && c.keep_pass, "trimmed, case-insensitive host is the same endpoint");
  c = credential_carry("Homeassistant.Local", 1883, true, "homeassistant.local", 1883, false);
  CHECK(!c.refuse && c.keep_pass, "case only");
  CHECK(same_broker_host("a.b", "A.B") && !same_broker_host("a.b", "a.b.") && !same_broker_host("", "") &&
            !same_broker_host(nullptr, "a") && !same_broker_host("a", nullptr) && !same_broker_host("ab", "a"),
        "the compare: case-folded, no suffix games, two empties are not an endpoint");

  // A new host with a stored password and no password in the body is the
  // finding itself: refused, nothing kept, nothing written.
  c = credential_carry("hub.lan", 8883, true, "attacker.example", 8883, false);
  CHECK(c.refuse, "new host + stored password + no password in the body: REFUSED");
  CHECK(!c.keep_user && !c.keep_pass, "and nothing is carried");
  // A port change alone is a new endpoint (the wizard's 'use 8883' button on
  // a unit that already holds a password asks for it again — deliberate).
  c = credential_carry("hub.lan", 1883, true, "hub.lan", 8883, false);
  CHECK(c.refuse, "new port only + stored password + no password: REFUSED");
  c = credential_carry("hub.lan", 1883, true, "HUB.LAN", 8883, false);
  CHECK(c.refuse, "same host, different port is still a new endpoint");

  // A new host WITH a password in the body: allowed, and the stored row is
  // not carried — the username is kept only if the body gives it (the
  // writer writes what the body carries and removes the rest).
  c = credential_carry("hub.lan", 8883, true, "other.lan", 8883, true);
  CHECK(!c.refuse, "new host with a password typed again: allowed");
  CHECK(!c.keep_user && !c.keep_pass, "the stored user and password do not follow; only the body's land");

  // A fresh unit has nothing to carry and nothing to refuse — the first-time
  // CA-mode save posts no password at all (test_canary_setup_page.test.js:
  // "a fresh unit set to CA mode") and must succeed.
  c = credential_carry("", 1883, false, "homeassistant.local", 8883, false);
  CHECK(!c.refuse && !c.keep_user && !c.keep_pass, "fresh unit (empty stored host), no password: allowed, nothing kept");
  c = credential_carry(nullptr, 1883, false, "homeassistant.local", 1883, false);
  CHECK(!c.refuse && !c.keep_user && !c.keep_pass, "fresh unit (null stored host): the same");
  // A stray stored password with no stored host is still a fresh unit: the
  // load path never reads a password without a host, so nothing to carry.
  c = credential_carry("", 1883, true, "hub.lan", 1883, false);
  CHECK(!c.refuse, "no stored host = fresh, whatever else the row claims");

  // An anonymous-broker row (a username, no password) moving to a new host is
  // allowed — there is no secret to protect — and the username goes with the
  // old endpoint unless the body resupplies it.
  c = credential_carry("hub.lan", 1883, false, "other.lan", 1883, false);
  CHECK(!c.refuse, "stored user, no stored password, new host: allowed");
  CHECK(!c.keep_user, "the stored username is removed unless the body gives one");
  CHECK(!c.keep_pass, "no password to keep either way");

  // The verdict the API answers with: a code the page can key on and a
  // sentence that names the password box — never a host, never a secret.
  Decision none;
  CHECK(std::strcmp(error_code(Verdict::PasswordRequiredForNewHost), "password_required_for_new_host") == 0,
        "error code");
  const char* why = reason(Verdict::PasswordRequiredForNewHost, none);
  CHECK(echoes(why, "password"), "the reason names the password: %s", why);
  CHECK(echoes(why, "never carried"), "and says the stored one is not carried: %s", why);
  CHECK(!echoes(why, "hub.lan") && !echoes(why, "attacker"), "constant text, no host in it");
}

// ── A CA the firmware cannot read back is named, never a free pass ──────────
// mqtt_tls_read_current used to report ca_set from isKey alone, while the
// transport's load() reads a stored CA longer than its buffer back as EMPTY:
// plan() said tls-ca Ok for a unit whose connect would refuse CaMissing. Now
// the reader sets ca_unreadable for a key it cannot read, and plan() refuses
// a CA-verified mode over it with a verdict that says what to do (409).
static void an_unreadable_stored_ca_is_named_not_passed() {
  Current cur;
  cur.ca_set = false;        // not usable...
  cur.ca_unreadable = true;  // ...but the key is there
  Plan p;
  Request req;
  req.has_mode = true;
  req.mode = 1;
  Verdict v = plan(cur, req, p);
  CHECK(v == Verdict::CaUnreadable, "CA mode over an unreadable CA is CaUnreadable, not Refused/CaMissing");
  CHECK(!p.set_mode && !p.set_fp && !p.clear_fp, "and plans no write");
  CHECK(p.decision.transport == Transport::Refused, "the decision is a refusal");
  CHECK(std::strcmp(error_code(v), "ca_unreadable") == 0, "error code");
  Decision none;
  const char* why = reason(v, p.decision);
  CHECK(echoes(why, "DELETE /api/mqtt/ca"), "the reason says how out: %s", why);
  CHECK(echoes(why, "upload it again"), "and to upload again: %s", why);
  CHECK(!echoes(why, "no CA"), "it does not claim no CA is stored");

  // A unit already in CA mode whose stored CA turned unreadable: a host-only
  // body is judged the same way (the connect would refuse).
  Current stored_ca_mode;
  stored_ca_mode.mode_byte = 1;
  stored_ca_mode.ca_unreadable = true;
  Request host_only;
  CHECK(plan(stored_ca_mode, host_only, p) == Verdict::CaUnreadable, "stored mode 1 + unreadable CA + host-only body: CaUnreadable");

  // Modes that do not read the CA are unaffected by an unreadable one.
  req.mode = 0;
  CHECK(plan(cur, req, p) == Verdict::Ok, "plain does not need the CA");
  req.mode = 3;
  CHECK(plan(cur, req, p) == Verdict::Ok, "lab does not need the CA");
  req.mode = 2;
  req.fp = kFpBare;
  CHECK(plan(cur, req, p) == Verdict::Ok, "pin mode with a pin does not need the CA");

  // A genuinely missing CA is still the plain CaMissing refusal.
  Current nothing;
  Request ca_mode;
  ca_mode.has_mode = true;
  ca_mode.mode = 1;
  v = plan(nothing, ca_mode, p);
  CHECK(v == Verdict::Refused && p.decision.reason == Reason::CaMissing, "no key at all: Refused / CaMissing as before");
  // And a readable one is Ok, as before.
  Current fine;
  fine.ca_set = true;
  CHECK(plan(fine, ca_mode, p) == Verdict::Ok, "a readable CA: Ok");
  (void)none;
}

// ── Every verdict has a code; every non-Ok verdict has text ─────────────────
static void every_verdict_has_a_code_and_text() {
  const Verdict all[] = {Verdict::Ok, Verdict::ModeInvalid, Verdict::FingerprintMalformed,
                         Verdict::Refused, Verdict::CaTooLarge, Verdict::CaMalformed,
                         Verdict::CaUnreadable, Verdict::PasswordRequiredForNewHost};
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
  a_stored_password_is_never_carried_to_a_new_endpoint();
  an_unreadable_stored_ca_is_named_not_passed();
  every_verdict_has_a_code_and_text();
  if (g_failures) {
    std::printf("test_mqtt_tls_fields: %d FAILED\n", g_failures);
    return 1;
  }
  std::puts("test_mqtt_tls_fields: all passed");
  return 0;
}
