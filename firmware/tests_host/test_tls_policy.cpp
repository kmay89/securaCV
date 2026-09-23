// Host tests for firmware/common/network/tls_policy.h — the decisions behind
// the canary's self-signed HTTPS surface (F15).
//
// The rows that matter most:
//   * decide(): HTTPS only when the build, the core, the certificate and the
//     wizard all allow it, with the reason naming the FIRST fact that did not;
//   * the port-80 Location builder refuses truncation, foreign hosts, a
//     non-origin-form target and control bytes, drops ":port", and keeps the
//     query string;
//   * the six connectivity probes are always exempt from the redirect, / and
//     /setup only while the wizard runs;
//   * fingerprint_hex is 64 lowercase hex chars.

#include "../common/network/tls_policy.h"

#include <cstdio>
#include <cstring>

using namespace canary::net::tls_policy;

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

// ── decide() truth table ────────────────────────────────────────────────────

static void decide_every_row() {
  // All 16 combinations: HTTPS only for (feature, lib, cert, !setup).
  for (int bits = 0; bits < 16; ++bits) {
    const bool feature = bits & 1, lib = bits & 2, cert = bits & 4, setup = bits & 8;
    const char* why = nullptr;
    const Mode m = decide(feature, lib, cert, setup, &why);
    const bool want_https = feature && lib && cert && !setup;
    CHECK((m == Mode::HTTPS_REDIRECT) == want_https,
          "bits=%d: feature=%d lib=%d cert=%d setup=%d", bits, feature, lib, cert, setup);
    CHECK(why != nullptr && why[0] != '\0', "bits=%d: a reason is always given", bits);
  }
}

static void decide_names_the_first_blocker() {
  const char* why = nullptr;
  decide(false, false, false, true, &why);
  CHECK(std::strstr(why, "FEATURE_HTTPS=0"), "build first: %s", why);
  decide(true, false, false, true, &why);
  CHECK(std::strstr(why, "esp_https_server"), "then the core: %s", why);
  decide(true, true, false, true, &why);
  CHECK(std::strstr(why, "setup wizard"), "then the wizard: %s", why);
  decide(true, true, false, false, &why);
  CHECK(std::strstr(why, "certificate"), "then the certificate: %s", why);
  decide(true, true, true, false, &why);
  CHECK(std::strstr(why, "443"), "HTTPS reason names the port: %s", why);
  CHECK(decide(true, true, true, false, nullptr) == Mode::HTTPS_REDIRECT, "reason pointer optional");
}

// ── Location builder ────────────────────────────────────────────────────────

static void redirect_uses_host_header_and_keeps_query() {
  char out[128];
  CHECK(build_redirect_location(out, sizeof(out), "canary.local", "192.168.4.1", "/api/status?x=1"),
        "plain host accepted");
  CHECK(std::strcmp(out, "https://canary.local/api/status?x=1") == 0, "got %s", out);
  CHECK(build_redirect_location(out, sizeof(out), "192.168.1.40:80", "192.168.4.1", "/"),
        "host with port accepted");
  CHECK(std::strcmp(out, "https://192.168.1.40/") == 0, ":80 dropped, got %s", out);
  CHECK(build_redirect_location(out, sizeof(out), "[fe80::1]:80", "192.168.4.1", "/x"),
        "bracketed IPv6 accepted");
  CHECK(std::strcmp(out, "https://[fe80::1]/x") == 0, "got %s", out);
}

static void redirect_falls_back_on_a_foreign_or_missing_host() {
  char out[128];
  CHECK(build_redirect_location(out, sizeof(out), nullptr, "192.168.4.1", "/setup"), "null host");
  CHECK(std::strcmp(out, "https://192.168.4.1/setup") == 0, "got %s", out);
  CHECK(build_redirect_location(out, sizeof(out), "", "192.168.4.1", "/"), "empty host");
  CHECK(std::strcmp(out, "https://192.168.4.1/") == 0, "got %s", out);
  CHECK(build_redirect_location(out, sizeof(out), "evil.example@canary.local", "192.168.4.1", "/"),
        "userinfo host falls back");
  CHECK(std::strcmp(out, "https://192.168.4.1/") == 0, "no '@' ever reaches Location: %s", out);
  CHECK(build_redirect_location(out, sizeof(out), "a/b", "192.168.4.1", "/"), "path in host");
  CHECK(std::strcmp(out, "https://192.168.4.1/") == 0, "got %s", out);
  CHECK(!build_redirect_location(out, sizeof(out), "a b", "no good", "/"),
        "neither host usable → refused");
  CHECK(out[0] == '\0', "refused output is emptied");
}

static void redirect_refuses_truncation_and_bad_targets() {
  char small[24];
  // "https://" (8) + "canary.local" (12) + "/api/status" (11) + NUL = 32 > 24.
  CHECK(!build_redirect_location(small, sizeof(small), "canary.local", "192.168.4.1", "/api/status"),
        "truncation refused, never a cut-off redirect");
  CHECK(small[0] == '\0', "refused output is emptied");
  char exact[8 + 12 + 1 + 1];  // "https://canary.local/" + NUL
  CHECK(build_redirect_location(exact, sizeof(exact), "canary.local", nullptr, "/"),
        "an exactly-sized buffer is enough");
  char out[128];
  CHECK(!build_redirect_location(out, sizeof(out), "canary.local", nullptr, "api/status"),
        "a target that is not origin-form is refused");
  CHECK(!build_redirect_location(out, sizeof(out), "canary.local", nullptr, "/a\r\nSet-Cookie: x=1"),
        "control bytes in the target are refused (no header splitting)");
  CHECK(!build_redirect_location(out, sizeof(out), "canary.local", nullptr, nullptr), "null uri");
  CHECK(!build_redirect_location(nullptr, 10, "canary.local", nullptr, "/"), "null out");
  CHECK(!build_redirect_location(out, 0, "canary.local", nullptr, "/"), "zero cap");
}

// ── plain-HTTP exemptions ───────────────────────────────────────────────────

static void probes_always_exempt() {
  const char* probes[] = {"/hotspot-detect.html", "/library/test/success.html", "/generate_204",
                          "/gen_204", "/connecttest.txt", "/ncsi.txt"};
  for (const char* p : probes) {
    CHECK(plain_http_exempt(p, false), "%s exempt after setup", p);
    CHECK(plain_http_exempt(p, true), "%s exempt during setup", p);
  }
  CHECK(plain_http_exempt("/generate_204?cb=123", false), "a cache-busting query still matches");
  CHECK(!plain_http_exempt("/generate_2045", false), "a longer path is not the probe");
  CHECK(!plain_http_exempt("/gen", false), "a prefix is not the probe");
}

static void pages_exempt_only_during_setup() {
  CHECK(plain_http_exempt("/", true), "/ stays plain while the wizard runs");
  CHECK(plain_http_exempt("/setup", true), "/setup stays plain while the wizard runs");
  CHECK(plain_http_exempt("/setup?step=2", true), "with a query too");
  CHECK(!plain_http_exempt("/", false), "/ redirects after setup");
  CHECK(!plain_http_exempt("/setup", false), "/setup redirects after setup");
  CHECK(!plain_http_exempt("/api/status", true), "the API is never exempt");
  CHECK(!plain_http_exempt("/api/provisioning-receipt", true), "the receipt is never exempt");
  CHECK(!plain_http_exempt(nullptr, true), "null uri is not exempt");
}

// ── fingerprint ─────────────────────────────────────────────────────────────

static void fingerprint_is_64_lowercase_hex() {
  uint8_t sha[32];
  for (int i = 0; i < 32; ++i) sha[i] = (uint8_t)(i * 8 + 7);
  char out[65];
  std::memset(out, 'Z', sizeof(out));
  fingerprint_hex(sha, out);
  CHECK(std::strlen(out) == 64, "64 chars, got %zu", std::strlen(out));
  CHECK(std::strncmp(out, "070f171f", 8) == 0, "byte order and lowercase: %.8s", out);
  for (int i = 0; i < 64; ++i) {
    const char c = out[i];
    CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "char %d is lowercase hex: %c", i, c);
  }
}

int main() {
  decide_every_row();
  decide_names_the_first_blocker();
  redirect_uses_host_header_and_keeps_query();
  redirect_falls_back_on_a_foreign_or_missing_host();
  redirect_refuses_truncation_and_bad_targets();
  probes_always_exempt();
  pages_exempt_only_during_setup();
  fingerprint_is_64_lowercase_hex();

  if (g_failures) {
    std::printf("test_tls_policy: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_tls_policy: all checks passed\n");
  return 0;
}
