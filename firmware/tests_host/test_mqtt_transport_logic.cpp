// Host tests for firmware/common/network/mqtt_transport_logic.h — the one
// place the fleet decides what the broker socket will be.
//
// The property that earns this suite its place is fail-closed: a TLS mode
// with a missing or malformed CA / pin must come back REFUSED, never as an
// unverified socket. canary-wap shipped the other behavior for a while (a
// `tls` bool that produced mqtts:// with no CA, i.e. encrypted-but-anyone),
// and it looked exactly like a working secured link. Every other case here is
// the vocabulary the four clients share: mode bytes, transport names, the
// pin spelling WiFiClientSecure::verify() parses, the port a mode implies,
// and the failure texts — which must exist for every code path and must
// never carry the secret they describe.

#include "../common/network/mqtt_transport_logic.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace canary::net::mqtt_tls;

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

// 32 bytes, three spellings the operator might paste.
static const char kFpColons[] =
    "0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9:0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9";
static const char kFpBare[] =
    "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9";
static const char kFpSpaces[] =
    "0A 1B 2C 3D 4E 5F 60 71 82 93 A4 B5 C6 D7 E8 F9 0A 1B 2C 3D 4E 5F 60 71 82 93 A4 B5 C6 D7 E8 F9";
// The ESP8266-era SHA-1 length — must be rejected, not silently accepted.
static const char kFpSha1[] = "0A1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D";

static Caps both() { Caps c; c.fingerprint = true; c.insecure = true; return c; }
static Caps esp_mqtt_like() { Caps c; c.fingerprint = false; c.insecure = true; return c; }
static Caps locked_down() { Caps c; c.fingerprint = false; c.insecure = false; return c; }

static Decision run(Mode m, const char* pem, const char* fp, const Caps& caps) {
  Settings s;
  s.mode = m;
  s.ca_pem = pem;
  s.fingerprint = fp;
  return decide(s, caps);
}

static void plain_is_the_default_and_stays_plain() {
  std::puts("plain_is_the_default_and_stays_plain");
  const Decision d = run(Mode::Plain, nullptr, nullptr, both());
  CHECK(d.transport == Transport::Plain, "plain mode must yield the plain transport");
  CHECK(d.allowed() && !d.tls() && !d.warn_insecure(), "plain: allowed, not tls, no warning");
  CHECK(d.reason == Reason::None, "plain: no reason");
  // A CA or pin left over in NVS does not silently upgrade a plain link.
  const Decision e = run(Mode::Plain, kPem, kFpColons, both());
  CHECK(e.transport == Transport::Plain, "plain ignores leftover CA/pin");
  CHECK(std::strcmp(uri_scheme(Transport::Plain), "mqtt") == 0, "plain scheme");
  CHECK(default_port(Mode::Plain) == 1883, "plain default port");
  // The byte every already-flashed device has (no key → 0) decodes to Plain.
  bool known = false;
  CHECK(mode_from_u8(0, &known) == Mode::Plain && known, "byte 0 is Plain");
  Decision z = decide_u8(0, nullptr, nullptr, both());
  CHECK(z.transport == Transport::Plain, "decide_u8(0) is plain");
}

static void unknown_mode_byte_fails_closed() {
  std::puts("unknown_mode_byte_fails_closed");
  bool known = true;
  (void)mode_from_u8(4, &known);
  CHECK(!known, "byte 4 is outside the table");
  (void)mode_from_u8(255, &known);
  CHECK(!known, "byte 255 is outside the table");
  for (uint8_t b = kModeCount; b != 0; b++) {  // wraps at 255 → stops at 0
    const Decision d = decide_u8(b, kPem, kFpColons, both());
    CHECK(d.transport == Transport::Refused, "byte %u must be refused", (unsigned)b);
    CHECK(d.reason == Reason::ModeUnknown, "byte %u reason", (unsigned)b);
    CHECK(!d.allowed(), "byte %u not allowed", (unsigned)b);
  }
}

static void ca_mode_needs_a_real_pem() {
  std::puts("ca_mode_needs_a_real_pem");
  Decision d = run(Mode::Ca, kPem, nullptr, both());
  CHECK(d.transport == Transport::TlsCa && d.reason == Reason::None, "CA + PEM → tls-ca");
  CHECK(d.tls() && d.allowed() && !d.warn_insecure(), "tls-ca: tls, allowed, silent");

  d = run(Mode::Ca, nullptr, nullptr, both());
  CHECK(d.transport == Transport::Refused, "CA without PEM must be REFUSED, never insecure");
  CHECK(d.reason == Reason::CaMissing, "CA missing reason");
  d = run(Mode::Ca, "", nullptr, both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::CaMissing, "empty PEM is missing");

  d = run(Mode::Ca, "not a certificate at all", nullptr, both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::CaMalformed, "junk PEM refused");
  d = run(Mode::Ca, "-----BEGIN CERTIFICATE-----\nMIIB\n", nullptr, both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::CaMalformed, "truncated PEM (no END) refused");
  // A fingerprint pasted into the CA box is the paste accident this catches.
  d = run(Mode::Ca, kFpColons, nullptr, both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::CaMalformed, "fingerprint in CA slot refused");

  // Oversize PEM (beyond what the NVS/buffer contract carries) is refused
  // rather than truncated into the handshake.
  std::string big = "-----BEGIN CERTIFICATE-----\n";
  big.append(kCaPemMax, 'A');
  big += "\n-----END CERTIFICATE-----\n";
  d = run(Mode::Ca, big.c_str(), nullptr, both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::CaMalformed, "oversize PEM refused");

  // A pin lying around does not turn CA mode into pin mode.
  d = run(Mode::Ca, nullptr, kFpColons, both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::CaMissing, "pin does not satisfy CA mode");
  CHECK(std::strcmp(uri_scheme(Transport::TlsCa), "mqtts") == 0, "tls-ca scheme");
  CHECK(default_port(Mode::Ca) == 8883, "CA default port");
}

static void fingerprint_spellings_normalize_to_one_form() {
  std::puts("fingerprint_spellings_normalize_to_one_form");
  char out[kFingerprintTextLen + 1];
  CHECK(fingerprint_normalize(kFpColons, out, sizeof(out)) == kFingerprintTextLen, "colons parse");
  CHECK(std::strcmp(out, kFpColons) == 0, "colon form is already canonical");
  CHECK(fingerprint_normalize(kFpBare, out, sizeof(out)) == kFingerprintTextLen, "bare hex parses");
  CHECK(std::strcmp(out, kFpColons) == 0, "bare lowercase → canonical uppercase colons (got %s)", out);
  CHECK(fingerprint_normalize(kFpSpaces, out, sizeof(out)) == kFingerprintTextLen, "spaces parse");
  CHECK(std::strcmp(out, kFpColons) == 0, "space form → canonical");
  CHECK(fingerprint_is_valid(kFpColons) && fingerprint_is_valid(kFpBare) && fingerprint_is_valid(kFpSpaces),
        "all three spellings valid");

  CHECK(!fingerprint_is_valid(kFpSha1), "a 20-byte SHA-1 is NOT a valid pin on this core (SHA-256 only)");
  CHECK(!fingerprint_is_valid(""), "empty invalid");
  CHECK(!fingerprint_is_valid(nullptr), "null invalid");
  CHECK(!fingerprint_is_valid("0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9:0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9:00"),
        "33 bytes invalid");
  CHECK(!fingerprint_is_valid("0G1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4E5F60718293A4B5C6D7E8F9"), "non-hex invalid");
  CHECK(!fingerprint_is_valid("0A1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4E5F60718293A4B5C6D7E8F"), "odd nibble count invalid");
  CHECK(!fingerprint_is_valid("0A1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4E5F60718293A4B5C6D7E8F9-"), "trailing junk invalid");
  CHECK(!fingerprint_is_valid("0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9:0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F"),
        "separator inside a pair invalid");
  // Validate-only call and a too-small output buffer both behave.
  CHECK(fingerprint_normalize(kFpBare, nullptr, 0) == kFingerprintTextLen, "validate-only works");
  char tiny[8];
  CHECK(fingerprint_normalize(kFpBare, tiny, sizeof(tiny)) == 0, "too-small buffer → 0, no overflow");
}

static void fingerprint_mode_needs_a_valid_pin_and_a_capable_transport() {
  std::puts("fingerprint_mode_needs_a_valid_pin_and_a_capable_transport");
  Decision d = run(Mode::Fingerprint, nullptr, kFpBare, both());
  CHECK(d.transport == Transport::TlsFingerprint && d.reason == Reason::None, "pin → tls-fingerprint");
  CHECK(d.tls() && d.allowed() && !d.warn_insecure(), "tls-fingerprint: tls, allowed, silent");

  d = run(Mode::Fingerprint, nullptr, nullptr, both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::FingerprintMissing, "no pin refused");
  d = run(Mode::Fingerprint, nullptr, "", both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::FingerprintMissing, "empty pin refused");
  d = run(Mode::Fingerprint, nullptr, kFpSha1, both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::FingerprintMalformed, "SHA-1 pin refused");
  d = run(Mode::Fingerprint, nullptr, "?", both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::FingerprintMalformed, "placeholder refused");
  // A CA lying around does not turn pin mode into CA mode.
  d = run(Mode::Fingerprint, kPem, nullptr, both());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::FingerprintMissing, "CA does not satisfy pin mode");

  // esp_mqtt (canary-wap) has no fingerprint hook: a pinned provisioning
  // must be refused there with the reason that names the fix (use CA).
  d = run(Mode::Fingerprint, nullptr, kFpBare, esp_mqtt_like());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::FingerprintUnsupported,
        "pin on a transport that cannot pin → refused/unsupported");
  CHECK(default_port(Mode::Fingerprint) == 8883, "pin default port");
}

static void insecure_is_opt_in_by_name_only_and_always_warns() {
  std::puts("insecure_is_opt_in_by_name_only_and_always_warns");
  Decision d = run(Mode::InsecureLab, nullptr, nullptr, both());
  CHECK(d.transport == Transport::TlsInsecure && d.reason == Reason::None, "lab mode → tls-insecure");
  CHECK(d.tls() && d.allowed(), "lab: tls, allowed");
  CHECK(d.warn_insecure(), "lab mode MUST carry the per-connect warning");
  CHECK(insecure_warning()[0] != '\0' && std::strstr(insecure_warning(), "NOT verified") != nullptr,
        "warning text says what is missing");

  // Nothing else warns, and nothing else ever reaches TlsInsecure.
  const Decision plain = run(Mode::Plain, nullptr, nullptr, both());
  const Decision ca = run(Mode::Ca, kPem, nullptr, both());
  const Decision fp = run(Mode::Fingerprint, nullptr, kFpBare, both());
  CHECK(!plain.warn_insecure() && !ca.warn_insecure() && !fp.warn_insecure(), "only lab warns");
  const Mode modes[] = {Mode::Plain, Mode::Ca, Mode::Fingerprint};
  const char* pems[] = {nullptr, "", "junk", kPem};
  const char* fps[] = {nullptr, "", "junk", kFpBare};
  for (Mode m : modes) {
    for (const char* pem : pems) {
      for (const char* f : fps) {
        const Decision x = run(m, pem, f, both());
        CHECK(x.transport != Transport::TlsInsecure,
              "mode %s with pem=%s fp=%s must never become insecure", mode_name(m),
              pem ? pem : "null", f ? f : "null");
      }
    }
  }
  // A build/config that cannot skip verification refuses the lab mode too.
  d = run(Mode::InsecureLab, nullptr, nullptr, locked_down());
  CHECK(d.transport == Transport::Refused && d.reason == Reason::InsecureUnsupported, "lab on a locked transport refused");
  CHECK(default_port(Mode::InsecureLab) == 8883, "lab default port");
}

static void every_name_and_reason_has_text_and_none_carries_a_secret() {
  std::puts("every_name_and_reason_has_text_and_none_carries_a_secret");
  const Transport ts[] = {Transport::Plain, Transport::TlsCa, Transport::TlsFingerprint,
                          Transport::TlsInsecure, Transport::Refused};
  for (Transport t : ts) CHECK(transport_name(t)[0] != '\0', "transport name non-empty");
  const Mode ms[] = {Mode::Plain, Mode::Ca, Mode::Fingerprint, Mode::InsecureLab};
  for (Mode m : ms) CHECK(mode_name(m)[0] != '\0' && std::strcmp(mode_name(m), "unknown") != 0, "mode name");
  const Reason rs[] = {Reason::ModeUnknown, Reason::CaMissing, Reason::CaMalformed,
                       Reason::FingerprintMissing, Reason::FingerprintMalformed,
                       Reason::FingerprintUnsupported, Reason::InsecureUnsupported};
  for (Reason r : rs) {
    const char* t = reason_text(r);
    CHECK(t[0] != '\0', "reason %d has text", (int)r);
    CHECK(std::strstr(t, "refusing") != nullptr || std::strstr(t, "use the CA") != nullptr,
          "reason %d names the consequence or the fix: %s", (int)r, t);
  }
  CHECK(reason_text(Reason::None)[0] == '\0', "None has no text");
  // The texts are constants: none can contain a PEM, a pin, or a password.
  for (Reason r : rs) {
    CHECK(std::strstr(reason_text(r), "-----BEGIN") == nullptr, "reason text never quotes a PEM");
    CHECK(std::strstr(reason_text(r), kFpBare) == nullptr, "reason text never quotes a pin");
  }
}

static void failure_lines_name_the_tls_reason_without_the_secret() {
  std::puts("failure_lines_name_the_tls_reason_without_the_secret");
  char line[256];

  // Refused: the reason, no code.
  size_t n = format_failure(line, sizeof(line), Transport::Refused, Reason::CaMissing, 0, 0, false);
  CHECK(n > 0 && std::strncmp(line, "refused: ", 9) == 0, "refused prefix: %s", line);
  CHECK(std::strstr(line, "no CA certificate is provisioned") != nullptr, "refused names the gap");
  CHECK(std::strstr(line, "mbedtls") == nullptr, "refused carries no handshake code");

  // Pin mismatch beats any handshake code (the handshake succeeded; the pin did not).
  n = format_failure(line, sizeof(line), Transport::TlsFingerprint, Reason::None, 0, 0, true);
  CHECK(n > 0 && std::strstr(line, "fingerprint does not match") != nullptr, "mismatch text: %s", line);
  CHECK(std::strncmp(line, "tls-fingerprint: ", 17) == 0, "mismatch prefix");

  // The CA-verify failure every wrong-CA setup produces.
  n = format_failure(line, sizeof(line), Transport::TlsCa, Reason::None, kMbedX509VerifyFailed, 0, false);
  CHECK(std::strstr(line, "did not verify against the provisioned CA") != nullptr, "verify text: %s", line);
  CHECK(std::strstr(line, "(mbedtls -0x2700)") != nullptr, "code appended: %s", line);

  // Plain listener on a TLS-configured port: the 1883-vs-8883 mistake.
  n = format_failure(line, sizeof(line), Transport::TlsCa, Reason::None, kMbedSslInvalidRecord, 0, false);
  CHECK(std::strstr(line, "did not speak TLS on this port") != nullptr, "wrong-port text: %s", line);
  CHECK(std::strstr(line, "1883 vs 8883") != nullptr, "wrong-port hint names the ports");

  // esp-tls verify flags (canary-wap) win over the stack code when present.
  n = format_failure(line, sizeof(line), Transport::TlsCa, Reason::None, kMbedX509VerifyFailed,
                     kX509NotTrusted, false);
  CHECK(std::strstr(line, "does not chain to the provisioned CA") != nullptr, "flags text: %s", line);
  n = format_failure(line, sizeof(line), Transport::TlsCa, Reason::None, 0, kX509CnMismatch, false);
  CHECK(std::strstr(line, "different host name") != nullptr, "CN mismatch text: %s", line);
  n = format_failure(line, sizeof(line), Transport::TlsCa, Reason::None, 0, kX509Expired, false);
  CHECK(std::strstr(line, "expired") != nullptr, "expired text: %s", line);
  n = format_failure(line, sizeof(line), Transport::TlsCa, Reason::None, 0, kX509Future, false);
  CHECK(std::strstr(line, "not valid yet") != nullptr, "future text: %s", line);
  n = format_failure(line, sizeof(line), Transport::TlsCa, Reason::None, 0, 0x8000u, false);
  CHECK(std::strstr(line, "failed verification") != nullptr, "unknown flag still explained: %s", line);

  // An unrecognized handshake code is still actionable: generic words + the code.
  n = format_failure(line, sizeof(line), Transport::TlsInsecure, Reason::None, -0x1234, 0, false);
  CHECK(std::strstr(line, "TLS handshake failed") != nullptr && std::strstr(line, "-0x1234") != nullptr,
        "unknown code line: %s", line);

  // Nothing to say: no code, no flags, no mismatch → empty, returns 0.
  n = format_failure(line, sizeof(line), Transport::TlsCa, Reason::None, 0, 0, false);
  CHECK(n == 0 && line[0] == '\0', "no-failure → empty");

  // Bounded: a tiny buffer is NUL-terminated, never overrun.
  char tiny[16];
  std::memset(tiny, 'X', sizeof(tiny));
  n = format_failure(tiny, sizeof(tiny), Transport::TlsCa, Reason::None, kMbedX509VerifyFailed, 0, false);
  CHECK(n == sizeof(tiny) - 1 && tiny[sizeof(tiny) - 1] == '\0', "tiny buffer bounded (n=%zu)", n);
  CHECK(format_failure(nullptr, 0, Transport::Refused, Reason::CaMissing, 0, 0, false) == 0, "null out → 0");

  // Every recognized mbedTLS code has text; the table is not allowed to rot
  // into "-0xNNNN" for the cases a person will actually hit.
  const int codes[] = {kMbedNetConnectFailed, kMbedNetRecvFailed, kMbedNetConnReset,
                       kMbedX509VerifyFailed, kMbedX509BadFormat, kMbedSslBadInput,
                       kMbedSslInvalidRecord, kMbedSslConnEof, kMbedSslFatalAlert,
                       kMbedSslPeerClose, kMbedSslHandshakeFail};
  for (int c : codes) CHECK(handshake_error_text(c) != nullptr, "code -0x%04X has text", (unsigned)(-c));
  CHECK(handshake_error_text(0) == nullptr && handshake_error_text(-1) == nullptr, "unknown codes → null");
  CHECK(verify_flags_text(0) == nullptr, "no flags → null");

  // JSON-safety: canary-wap splices these texts into its /api/mqtt/config and
  // /api/mqtt/test bodies with %s and no escaper, on the promise that none
  // of them carries a double quote or a backslash. Hold the promise here.
  auto json_plain = [](const char* t) {
    return t != nullptr && std::strchr(t, '"') == nullptr && std::strchr(t, '\\') == nullptr;
  };
  for (int c : codes) CHECK(json_plain(handshake_error_text(c)), "handshake text -0x%04X is JSON-plain", (unsigned)(-c));
  const uint32_t flags[] = {kX509NotTrusted, kX509CnMismatch, kX509Expired, kX509Future, kX509Revoked, 0x8000u};
  for (uint32_t f : flags) CHECK(json_plain(verify_flags_text(f)), "verify text 0x%x is JSON-plain", (unsigned)f);
  const Reason all_reasons[] = {Reason::ModeUnknown, Reason::CaMissing, Reason::CaMalformed,
                                Reason::FingerprintMissing, Reason::FingerprintMalformed,
                                Reason::FingerprintUnsupported, Reason::InsecureUnsupported};
  for (Reason r : all_reasons) CHECK(json_plain(reason_text(r)), "reason %d is JSON-plain", (int)r);
  CHECK(json_plain(insecure_warning()), "insecure warning is JSON-plain");
  const Transport all_t[] = {Transport::Plain, Transport::TlsCa, Transport::TlsFingerprint,
                             Transport::TlsInsecure, Transport::Refused};
  for (Transport t : all_t) CHECK(json_plain(transport_name(t)), "transport name is JSON-plain");
  n = format_failure(line, sizeof(line), Transport::TlsFingerprint, Reason::None, kMbedSslFatalAlert, 0, true);
  CHECK(n > 0 && json_plain(line), "a full failure line is JSON-plain: %s", line);
}

int main() {
  plain_is_the_default_and_stays_plain();
  unknown_mode_byte_fails_closed();
  ca_mode_needs_a_real_pem();
  fingerprint_spellings_normalize_to_one_form();
  fingerprint_mode_needs_a_valid_pin_and_a_capable_transport();
  insecure_is_opt_in_by_name_only_and_always_warns();
  every_name_and_reason_has_text_and_none_carries_a_secret();
  failure_lines_name_the_tls_reason_without_the_secret();
  if (g_failures) {
    std::printf("test_mqtt_transport_logic: %d FAILED\n", g_failures);
    return 1;
  }
  std::puts("test_mqtt_transport_logic: all passed");
  return 0;
}
