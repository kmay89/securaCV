// Host-side unit tests for the help-QR verdict → Help Desk URL composer
// (arduino/canary_wap/help_qr_logic.h). Pure logic, no Arduino glue.
//
// What's pinned here:
//   - The probe-namespace bridge: WAP probe ids map to real website anchors
//     ("wifi" → probe-wifi_ok, "sd" → probe-sd_card, "bluetooth" →
//     s-ble-not-working); unmapped/unknown/nullptr map to "" (bare Help
//     Desk), never to a wrong-but-plausible fix.
//   - Worst-first precedence: safe mode beats a down hub beats failing
//     probes; hub beats probes; the first MAPPED failing probe wins (an
//     unmapped one ahead of it is skipped, not a dead end).
//   - All-clear (or unmapped-only failures) composes the bare base URL.
//   - Overflow safety: a too-small buffer yields 0 with out NUL'd — never
//     a truncated URL (half a URL in a QR scans to a 404 on the exact
//     device that needs help). Exact-fit boundary included.
//   - Contract charset: every anchor this header can emit survives the
//     website's hash regexes (#s-[a-z0-9-] / #probe-[a-z0-9_]) — an anchor
//     the page would reject is a QR that scans to nothing.
//
// Build/run via tests_host/Makefile. Exits non-zero on any failure.

#include <cstdio>
#include <cstring>

#include "../arduino/canary_wap/help_qr_logic.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

static const char* BASE = "https://securacv.com/help";

static void test_anchor_mapping() {
  using help_qr_logic::anchor_for_probe;
  CHECK(std::strcmp(anchor_for_probe("wifi"), "probe-wifi_ok") == 0,
        "wifi maps to the kernel wifi probe anchor");
  CHECK(std::strcmp(anchor_for_probe("sd"), "probe-sd_card") == 0,
        "sd maps to the kernel sd probe anchor");
  CHECK(std::strcmp(anchor_for_probe("bluetooth"), "s-ble-not-working") == 0,
        "bluetooth maps to the BLE symptom card");
  CHECK(std::strcmp(anchor_for_probe("camera"), "") == 0,
        "camera has no anchor yet — bare Help Desk, not a wrong fix");
  CHECK(std::strcmp(anchor_for_probe("gpio"), "") == 0, "gpio unmapped");
  CHECK(std::strcmp(anchor_for_probe("power"), "") == 0,
        "optional peripherals never fail, and map to nothing anyway");
  CHECK(std::strcmp(anchor_for_probe("zzz"), "") == 0, "unknown id unmapped");
  CHECK(std::strcmp(anchor_for_probe(nullptr), "") == 0, "nullptr is safe");
}

static void test_precedence() {
  using help_qr_logic::compose_help_url;
  char out[128];
  const char* failing_sd[] = { "sd" };

  // Safe mode wins over everything — it explains the wall of SKIP rows.
  size_t n = compose_help_url(out, sizeof(out), BASE,
                              /*safe_mode=*/true, /*hub_down=*/true,
                              failing_sd, 1);
  CHECK(n > 0 && std::strcmp(out, "https://securacv.com/help#s-safe-mode") == 0,
        "safe mode beats hub and probes");

  // Hub down beats failing probes.
  n = compose_help_url(out, sizeof(out), BASE, false, true, failing_sd, 1);
  CHECK(n > 0 &&
        std::strcmp(out, "https://securacv.com/help#s-hub-unreachable") == 0,
        "hub down beats failing probes");

  // First MAPPED failing probe wins; an unmapped one ahead is skipped.
  const char* failing_mixed[] = { "camera", "sd", "wifi" };
  n = compose_help_url(out, sizeof(out), BASE, false, false, failing_mixed, 3);
  CHECK(n > 0 &&
        std::strcmp(out, "https://securacv.com/help#probe-sd_card") == 0,
        "first mapped failing probe wins (camera skipped, sd taken)");

  // All clear → bare Help Desk.
  n = compose_help_url(out, sizeof(out), BASE, false, false, nullptr, 0);
  CHECK(n > 0 && std::strcmp(out, BASE) == 0, "all-clear composes the bare base");

  // Unmapped-only failures → bare Help Desk, not a bogus anchor.
  const char* failing_unmapped[] = { "camera", "gpio" };
  n = compose_help_url(out, sizeof(out), BASE, false, false, failing_unmapped, 2);
  CHECK(n > 0 && std::strcmp(out, BASE) == 0,
        "unmapped-only failures fall back to the bare Help Desk");
}

static void test_overflow_safety() {
  using help_qr_logic::compose_help_url;
  char tiny[8];
  size_t n = compose_help_url(tiny, sizeof(tiny), BASE, true, false, nullptr, 0);
  CHECK(n == 0 && tiny[0] == '\0',
        "a too-small buffer yields 0 with out NUL'd — never a half URL");

  // Exact fit: strlen(base) chars need strlen+1 capacity.
  char fit[sizeof("https://securacv.com/help")];
  n = compose_help_url(fit, sizeof(fit), BASE, false, false, nullptr, 0);
  CHECK(n == std::strlen(BASE) && std::strcmp(fit, BASE) == 0,
        "exact-fit capacity composes cleanly");
  char short1[sizeof("https://securacv.com/help") - 1];
  n = compose_help_url(short1, sizeof(short1), BASE, false, false, nullptr, 0);
  CHECK(n == 0 && short1[0] == '\0', "one byte short is a refusal, not a trim");

  CHECK(compose_help_url(nullptr, 16, BASE, false, false, nullptr, 0) == 0,
        "nullptr out is safe");
  char out[64];
  CHECK(compose_help_url(out, sizeof(out), nullptr, false, false, nullptr, 0) == 0 &&
        out[0] == '\0', "nullptr base is a refusal");
  CHECK(compose_help_url(out, sizeof(out), "", false, false, nullptr, 0) == 0,
        "empty base is a refusal");
}

static void test_contract_charset() {
  // The website's handlers accept #s-[a-z0-9-]+ and #probe-[a-z0-9_]+
  // (securacv_website js/help.js openFromHash). Every anchor this header
  // can emit must survive those regexes, or the QR scans to nothing.
  using help_qr_logic::anchor_for_probe;
  const char* wap_probes[] = { "wifi", "camera", "bluetooth", "gps", "sd",
                               "power", "microphone", "buzzer", "tamper",
                               "gpio" };
  const char* fixed[] = { "s-safe-mode", "s-hub-unreachable" };
  auto ok_for_contract = [](const char* a) -> bool {
    if (a[0] == '\0') return true;  // bare Help Desk — no anchor to check
    const char* body;
    bool underscore_ok;
    if (std::strncmp(a, "probe-", 6) == 0) { body = a + 6; underscore_ok = true; }
    else if (std::strncmp(a, "s-", 2) == 0) { body = a + 2; underscore_ok = false; }
    else return false;
    if (body[0] == '\0') return false;
    for (const char* c = body; *c; ++c) {
      const bool az = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9');
      if (az) continue;
      if (*c == '_' && underscore_ok) continue;
      if (*c == '-' && !underscore_ok) continue;
      return false;
    }
    return true;
  };
  for (const char* p : wap_probes) {
    CHECK(ok_for_contract(anchor_for_probe(p)), p);
  }
  for (const char* a : fixed) {
    CHECK(ok_for_contract(a), a);
  }
}

int main() {
  test_anchor_mapping();
  test_precedence();
  test_overflow_safety();
  test_contract_charset();
  if (g_failures > 0) {
    std::printf("test_help_qr_logic: %d FAILURE(S)\n", g_failures);
    return 1;
  }
  std::printf("test_help_qr_logic: ALL help-QR tests PASSED\n");
  return 0;
}
