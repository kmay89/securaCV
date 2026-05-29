// Host-side unit tests for the captive-portal connectivity-probe policy
// (arduino/canary_wap/captive_probe.h). Pure logic, no Arduino glue: feed
// request paths and assert the classification + response descriptor.
//
// What's pinned here:
//   - Each OS's exact probe paths classify to that platform; unknown paths and
//     nullptr classify to None.
//   - respond() yields the right (kind, content_type, body) per platform:
//     Apple → 200 HTML page (body served by firmware), Android → 204 / no body,
//     Windows → text/plain + the exact NCSI body.
//   - The Windows NCSI body branch picks "Microsoft NCSI" for ncsi.txt and
//     "Microsoft Connect Test" for connecttest.txt (the strstr branch).
//   - Unknown paths fall back to the Apple instruction page (the
//     connection-preserving safe default), never a 404/None response.
//
// Build/run via tests_host/Makefile. Exits non-zero on any failure.

#include <cstdio>
#include <cstring>

#include "../arduino/canary_wap/captive_probe.h"

using captive_probe::ProbeKind;
using captive_probe::ProbeResponse;

static int g_failures = 0;

#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

// Two strings are equal (both non-null and matching).
static bool streq(const char* a, const char* b) {
  return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

static void test_classify_apple() {
  std::printf("test_classify_apple\n");
  CHECK(captive_probe::classify("/hotspot-detect.html") == ProbeKind::AppleInstructionPage,
        "/hotspot-detect.html → Apple");
  CHECK(captive_probe::classify("/library/test/success.html") == ProbeKind::AppleInstructionPage,
        "/library/test/success.html → Apple");
}

static void test_classify_android() {
  std::printf("test_classify_android\n");
  CHECK(captive_probe::classify("/generate_204") == ProbeKind::AndroidNoContent,
        "/generate_204 → Android");
  CHECK(captive_probe::classify("/gen_204") == ProbeKind::AndroidNoContent,
        "/gen_204 → Android");
}

static void test_classify_windows() {
  std::printf("test_classify_windows\n");
  CHECK(captive_probe::classify("/connecttest.txt") == ProbeKind::WindowsNcsiBody,
        "/connecttest.txt → Windows");
  CHECK(captive_probe::classify("/ncsi.txt") == ProbeKind::WindowsNcsiBody,
        "/ncsi.txt → Windows");
}

static void test_classify_unknown_and_null() {
  std::printf("test_classify_unknown_and_null\n");
  CHECK(captive_probe::classify("/") == ProbeKind::None, "/ → None");
  CHECK(captive_probe::classify("/api/status") == ProbeKind::None, "/api/status → None");
  CHECK(captive_probe::classify("/generate_204x") == ProbeKind::None,
        "near-miss path is exact-matched, not substring → None");
  CHECK(captive_probe::classify(nullptr) == ProbeKind::None, "nullptr path → None (no deref)");
}

static void test_windows_ncsi_body() {
  std::printf("test_windows_ncsi_body\n");
  CHECK(streq(captive_probe::windows_ncsi_body("/ncsi.txt"), "Microsoft NCSI"),
        "ncsi.txt → 'Microsoft NCSI'");
  CHECK(streq(captive_probe::windows_ncsi_body("/connecttest.txt"), "Microsoft Connect Test"),
        "connecttest.txt → 'Microsoft Connect Test'");
  // The branch keys off the substring "ncsi", matching the firmware's strstr.
  CHECK(streq(captive_probe::windows_ncsi_body("/redir/ncsi.txt"), "Microsoft NCSI"),
        "any path containing 'ncsi' → 'Microsoft NCSI'");
}

static void test_respond_apple() {
  std::printf("test_respond_apple\n");
  ProbeResponse r = captive_probe::respond("/hotspot-detect.html");
  CHECK(r.kind == ProbeKind::AppleInstructionPage, "Apple respond kind");
  CHECK(streq(r.content_type, "text/html; charset=utf-8"), "Apple content type is HTML");
  CHECK(r.body == nullptr, "Apple body is null (firmware serves CAPTIVE_PORTAL_HTML)");
}

static void test_respond_android() {
  std::printf("test_respond_android\n");
  ProbeResponse r = captive_probe::respond("/generate_204");
  CHECK(r.kind == ProbeKind::AndroidNoContent, "Android respond kind");
  CHECK(r.content_type == nullptr, "Android 204 carries no content type");
  CHECK(r.body == nullptr, "Android 204 has no body");
}

static void test_respond_windows() {
  std::printf("test_respond_windows\n");
  ProbeResponse rc = captive_probe::respond("/connecttest.txt");
  CHECK(rc.kind == ProbeKind::WindowsNcsiBody, "Windows respond kind (connecttest)");
  CHECK(streq(rc.content_type, "text/plain"), "Windows content type is text/plain");
  CHECK(streq(rc.body, "Microsoft Connect Test"), "connecttest body");

  ProbeResponse rn = captive_probe::respond("/ncsi.txt");
  CHECK(streq(rn.body, "Microsoft NCSI"), "ncsi body");
  CHECK(streq(rn.content_type, "text/plain"), "ncsi content type is text/plain");
}

static void test_respond_unknown_falls_back_to_apple() {
  std::printf("test_respond_unknown_falls_back_to_apple\n");
  // An unmatched path must not yield ProbeKind::None at the response layer —
  // the firmware would otherwise have no body to send and could 404 a probe,
  // tripping the very disconnect this whole feature prevents.
  ProbeResponse r = captive_probe::respond("/something-unexpected");
  CHECK(r.kind == ProbeKind::AppleInstructionPage, "unknown path → Apple page fallback");
  CHECK(streq(r.content_type, "text/html; charset=utf-8"), "fallback content type is HTML");
  CHECK(r.body == nullptr, "fallback body is null (firmware serves the page)");

  ProbeResponse rnull = captive_probe::respond(nullptr);
  CHECK(rnull.kind == ProbeKind::AppleInstructionPage, "nullptr path → Apple page fallback");
}

int main() {
  std::printf("=== captive_probe host tests ===\n");
  test_classify_apple();
  test_classify_android();
  test_classify_windows();
  test_classify_unknown_and_null();
  test_windows_ncsi_body();
  test_respond_apple();
  test_respond_android();
  test_respond_windows();
  test_respond_unknown_falls_back_to_apple();

  if (g_failures == 0) {
    std::printf("ALL CAPTIVE-PROBE TESTS PASSED\n");
    return 0;
  }
  std::printf("CAPTIVE-PROBE TESTS FAILED: %d\n", g_failures);
  return 1;
}
