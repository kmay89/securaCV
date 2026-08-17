// Host-side unit tests for the glass help-QR verdict (canary/ui/help_verdict.h).
// Pure header, no LVGL, no Arduino.
//
// Build/run:
//   g++ -std=c++17 -O2 -Wall -Wextra -I ../include -o test_help_verdict \
//       test_help_verdict.cpp && ./test_help_verdict
//
// What's pinned here:
//   - Worst-first precedence: a verification failure beats a down hub beats
//     a quiet witness; a display with no hub configured (or all-well) gets
//     the bare Help Desk, never a wrong-but-plausible anchor.
//   - Compose refusal: overflow yields 0 with out NUL'd — never a truncated
//     URL (half a URL in a QR scans to a 404 on the exact glass someone is
//     asking for help). Exact-fit boundary included.
//   - Contract charset: every anchor survives the website's #s-[a-z0-9-]
//     hash regex (securacv_website js/help.js openFromHash) — an anchor the
//     page rejects is a QR that scans to nothing.

#include <cstdio>
#include <cstring>

#include "canary/ui/help_verdict.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

using namespace canary::ui::help_verdict;

static const char* BASE = "https://securacv.com/help";

static void test_precedence() {
  std::printf("precedence:\n");
  CHECK(std::strcmp(anchor(true, true, true), "s-not-verified") == 0,
        "verification failure beats everything");
  CHECK(std::strcmp(anchor(false, true, true), "s-hub-unreachable") == 0,
        "hub down beats quiet witnesses");
  CHECK(std::strcmp(anchor(false, false, true), "s-stale-witness") == 0,
        "quiet witness when the hub is fine");
  CHECK(std::strcmp(anchor(false, false, false), "") == 0,
        "all well (or no hub configured) = bare Help Desk");
}

static void test_compose() {
  std::printf("compose:\n");
  char out[128];
  CHECK(compose(out, sizeof(out), BASE, false, true, false) > 0 &&
        std::strcmp(out, "https://securacv.com/help#s-hub-unreachable") == 0,
        "hub-down URL composes whole");
  CHECK(compose(out, sizeof(out), BASE, false, false, false) > 0 &&
        std::strcmp(out, BASE) == 0,
        "all-well composes the bare base");

  char tiny[8];
  CHECK(compose(tiny, sizeof(tiny), BASE, true, false, false) == 0 &&
        tiny[0] == '\0',
        "overflow is a refusal with out NUL'd, never a half URL");
  char fit[sizeof("https://securacv.com/help")];
  CHECK(compose(fit, sizeof(fit), BASE, false, false, false) ==
            std::strlen(BASE),
        "exact-fit capacity composes cleanly");
  char short1[sizeof("https://securacv.com/help") - 1];
  CHECK(compose(short1, sizeof(short1), BASE, false, false, false) == 0 &&
        short1[0] == '\0',
        "one byte short refuses");
  CHECK(compose(nullptr, 16, BASE, false, false, false) == 0,
        "nullptr out is safe");
  char out2[64];
  CHECK(compose(out2, sizeof(out2), nullptr, false, false, false) == 0 &&
        out2[0] == '\0',
        "nullptr base refuses");
  CHECK(compose(out2, sizeof(out2), "", false, false, false) == 0,
        "empty base refuses");
}

static void test_contract_charset() {
  std::printf("contract charset:\n");
  // The website's #s- handler accepts [a-z0-9-] after the prefix.
  const char* anchors[] = { anchor(true, false, false),
                            anchor(false, true, false),
                            anchor(false, false, true) };
  for (const char* a : anchors) {
    CHECK(std::strncmp(a, "s-", 2) == 0, "every anchor is an #s- symptom id");
    bool ok = a[2] != '\0';
    for (const char* c = a + 2; *c; ++c) {
      if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-'))
        ok = false;
    }
    CHECK(ok, a);
  }
}

int main() {
  test_precedence();
  test_compose();
  test_contract_charset();
  if (g_fail) {
    std::printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("ALL HELP-VERDICT TESTS PASSED\n");
  return 0;
}
