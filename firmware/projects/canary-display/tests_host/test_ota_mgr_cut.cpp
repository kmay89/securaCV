// Host test for the OTA glue of the display, Sense, Sentinel and Vision that
// no host build could reach: each product's src/net/ota_mgr.cpp needs the
// Arduino stack. Two functions are cut verbatim out of each of the four
// files by firmware/tests_host/cut_functions.awk (the Makefile's
// test_ota_mgr_cut_<product>.inc) and driven over a stand-in radio, clock
// and engine:
//
//   copy_json_safe()    the release summary published to Home Assistant's
//                       update entity. It was cut at a raw byte count, so a
//                       multibyte character on the boundary left half a UTF-8
//                       sequence and Home Assistant refused the whole
//                       retained payload for that release. The cut must land
//                       between characters.
//
//   ota_boot_validate() the boot probe of a freshly installed image. It ran
//                       in setup() right after the single bounded boot join,
//                       so one missed association rolled a good image back.
//                       It runs from loop() now, once, at a bounded healthy
//                       gate: the first pass with the station link up, or
//                       90 s after the first pass at the latest.
//
// Each product's main.cpp (paths passed in) is read too, for what the cut
// cannot show: the one call of ota_boot_validate() sits in loop(), ahead of
// the health tick, and no longer in setup().
//
// Run: make -C firmware/projects/canary-display/tests_host run-ota-mgr-cut

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "securacv_ota.h"  // the real securacv_selftest_t (host subset)

static int g_fail = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      g_fail++;                                                              \
    }                                                                        \
  } while (0)

// ── Stand-ins for what the cut functions name ──────────────────────────────
static uint32_t g_now_ms = 0;
static bool g_link_up = false;

static uint32_t millis() { return g_now_ms; }
enum { WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
static struct {
  int status() const { return g_link_up ? WL_CONNECTED : WL_DISCONNECTED; }
} WiFi;

// The engine, as far as the probe goes: keep what was registered, run it
// when asked, and count a failed required probe as the rollback it is.
static const securacv_selftest_t* g_tests[8];
static int g_num_tests = 0;
static int g_probe_runs = 0;
static int g_rollbacks = 0;

esp_err_t securacv_ota_register_selftest(const securacv_selftest_t* test) {
  if (g_num_tests < 8) g_tests[g_num_tests++] = test;
  return ESP_OK;
}

esp_err_t securacv_ota_boot_self_test(void) {
  g_probe_runs++;
  for (int i = 0; i < g_num_tests; i++) {
    if (!g_tests[i]->fn(g_tests[i]->name) && g_tests[i]->required) g_rollbacks++;
  }
  return ESP_OK;
}

static void engine_reset() {
  g_num_tests = 0;
  g_probe_runs = 0;
  g_rollbacks = 0;
}

// ── The cuts. Members of a class template so each scenario gets its own
// copy of ota_boot_validate()'s function-local state (one instantiation per
// scenario number). ─────────────────────────────────────────────────────────
namespace display {
template <int Scenario> struct Cut {
#include OTACUT_INC_DISPLAY
};
}  // namespace display
namespace sense {
template <int Scenario> struct Cut {
#include OTACUT_INC_SENSE
};
}  // namespace sense
namespace sentinel {
template <int Scenario> struct Cut {
#include OTACUT_INC_SENTINEL
};
}  // namespace sentinel
namespace vision {
template <int Scenario> struct Cut {
#include OTACUT_INC_VISION
};
}  // namespace vision

// ── copy_json_safe ─────────────────────────────────────────────────────────

// Strict UTF-8: every lead byte followed by exactly its continuation bytes.
static bool valid_utf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = (unsigned char)s[i];
    size_t n;
    if (c < 0x80) n = 1;
    else if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) n = 3;
    else if (c >= 0xF0 && c <= 0xF4) n = 4;
    else return false;  // a stray continuation byte, or an invalid lead
    if (i + n > s.size()) return false;
    for (size_t k = 1; k < n; k++) {
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
    }
    i += n;
  }
  return true;
}

static size_t char_len(unsigned char lead) {
  return lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
}

template <template <int> class Cut>
static void test_summary_copy(const char* product) {
  const int f0 = g_fail;
  Cut<0> cut;

  // One character of each width across every byte offset of the boundary,
  // at a small cap and at the real one (release_summary is char[160]).
  const char* const kChars[] = {"\xC3\xA9" /* e-acute */, "\xE2\x80\x94" /* em dash */,
                                "\xF0\x9F\x98\x80" /* an emoji */};
  const size_t kCaps[] = {8, 160};
  for (const char* ch : kChars) {
    for (size_t cap : kCaps) {
      for (size_t pad = 0; pad <= cap + 4; pad++) {
        const std::string src = std::string(pad, 'a') + ch + "tail";
        // A guard byte after the cap: the copy never writes past cap.
        std::string buf(cap + 1, '\x7F');
        cut.copy_json_safe(&buf[0], cap, src.c_str());
        CHECK(buf[cap] == '\x7F');
        const std::string out(buf.c_str());
        CHECK(out.size() <= cap - 1);
        CHECK(valid_utf8(out));                          // the defect: half a character
        CHECK(src.compare(0, out.size(), out) == 0);     // a prefix of what was given
        if (out.size() < src.size()) {
          // Cut — and at the last character boundary that fits, not earlier.
          CHECK(out.size() + char_len((unsigned char)src[out.size()]) > cap - 1);
        }
      }
    }
  }

  // The reported case: an em dash across byte 160 of the summary.
  {
    char summary[160];
    const std::string fits = std::string(156, 'a') + "\xE2\x80\x94" + "b";
    cut.copy_json_safe(summary, sizeof(summary), fits.c_str());
    CHECK(std::string(summary) == std::string(156, 'a') + "\xE2\x80\x94");
    const std::string straddles = std::string(157, 'a') + "\xE2\x80\x94" + "b";
    cut.copy_json_safe(summary, sizeof(summary), straddles.c_str());
    CHECK(std::string(summary) == std::string(157, 'a'));   // was 157 a's + E2 80
  }

  // What it always did: JSON-breaking characters become spaces, the edges hold.
  {
    char out[32];
    cut.copy_json_safe(out, sizeof(out), "a\"b\\c\nd\te");
    CHECK(std::strcmp(out, "a b c d e") == 0);
    cut.copy_json_safe(out, sizeof(out), "plain \xE2\x80\x94 text");
    CHECK(std::strcmp(out, "plain \xE2\x80\x94 text") == 0);
    std::strcpy(out, "kept");
    cut.copy_json_safe(out, sizeof(out), nullptr);
    CHECK(out[0] == '\0');
    std::strcpy(out, "kept");
    cut.copy_json_safe(out, 1, "abc");
    CHECK(out[0] == '\0');
    std::strcpy(out, "kept");
    cut.copy_json_safe(out, 0, "abc");
    CHECK(std::strcmp(out, "kept") == 0);
    cut.copy_json_safe(nullptr, 8, "abc");                  // no write, no crash
  }
  std::printf("%s %s: summary_copy\n", g_fail == f0 ? "PASS" : "FAIL", product);
}

// ── ota_boot_validate ──────────────────────────────────────────────────────

static const uint32_t kGate = 90000;  // the gate the docs and the changelog name

template <template <int> class Cut>
static void test_boot_probe_gate(const char* product) {
  const int f0 = g_fail;

  // 1. The link is up on the first pass: the probe runs there, once, passes.
  {
    Cut<1> cut;
    engine_reset();
    g_now_ms = 31000; g_link_up = true;
    cut.ota_boot_validate();
    CHECK(g_probe_runs == 1 && g_num_tests == 1 && g_rollbacks == 0);
    CHECK(g_tests[0]->required);                            // the fleet rule: Wi-Fi associated
    for (int i = 1; i <= 1000; i++) { g_now_ms += 250; g_link_up = (i % 3) != 0; cut.ota_boot_validate(); }
    CHECK(g_probe_runs == 1 && g_num_tests == 1 && g_rollbacks == 0);   // never again
  }

  // 2. The boot join missed: no verdict on the first pass (the defect — a
  //    good image rolled back there); the probe runs on the first pass with
  //    the link up, and passes.
  {
    Cut<2> cut;
    engine_reset();
    g_now_ms = 31000; g_link_up = false;
    cut.ota_boot_validate();
    CHECK(g_probe_runs == 0 && g_rollbacks == 0);
    for (g_now_ms = 31010; g_now_ms < 31000 + 20000; g_now_ms += 10) cut.ota_boot_validate();
    CHECK(g_probe_runs == 0 && g_rollbacks == 0);
    g_link_up = true;                                       // loop()'s backoff got one in
    cut.ota_boot_validate();
    CHECK(g_probe_runs == 1 && g_rollbacks == 0);
    g_link_up = false; g_now_ms += 200000;
    cut.ota_boot_validate();
    CHECK(g_probe_runs == 1 && g_rollbacks == 0);           // a later drop is the dwell's business
  }

  // 3. The link never comes up: the gate is bounded. Nothing at 89.999 s
  //    after the first pass, the probe at 90 s — and it fails, required.
  {
    Cut<3> cut;
    engine_reset();
    g_now_ms = 45000; g_link_up = false;                    // the first pass, whenever setup() ended
    cut.ota_boot_validate();
    g_now_ms = 45000 + kGate - 1;
    cut.ota_boot_validate();
    CHECK(g_probe_runs == 0 && g_rollbacks == 0);
    g_now_ms = 45000 + kGate;
    cut.ota_boot_validate();
    CHECK(g_probe_runs == 1 && g_rollbacks == 1);
    g_now_ms += 1000;
    cut.ota_boot_validate();
    CHECK(g_probe_runs == 1 && g_rollbacks == 1);
  }

  // 4. The gate survives the millis() wrap.
  {
    Cut<4> cut;
    engine_reset();
    g_now_ms = 0xFFFFF000u; g_link_up = false;
    cut.ota_boot_validate();
    g_now_ms = 0xFFFFF000u + (kGate - 1);                   // wrapped past zero
    cut.ota_boot_validate();
    CHECK(g_probe_runs == 0);
    g_now_ms = 0xFFFFF000u + kGate;
    cut.ota_boot_validate();
    CHECK(g_probe_runs == 1 && g_rollbacks == 1);
  }
  g_now_ms = 0; g_link_up = false;
  std::printf("%s %s: boot_probe_gate\n", g_fail == f0 ? "PASS" : "FAIL", product);
}

// ── main.cpp: where the one call sits ──────────────────────────────────────

static void test_call_site(const char* product, const char* main_cpp) {
  const int f0 = g_fail;
  std::ifstream f(main_cpp);
  if (!f) {
    std::fprintf(stderr, "FAIL: cannot open %s\n", main_cpp);
    g_fail++;
    return;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string src = ss.str();
  const std::string call = "canary::net::ota_boot_validate();";
  const size_t at = src.find(call);
  CHECK(at != std::string::npos);
  CHECK(src.find(call, at + 1) == std::string::npos);       // one caller
  const size_t loop_at = src.find("\nvoid loop() {");
  const size_t tick_at = src.find("canary::net::ota_health_tick(");
  CHECK(loop_at != std::string::npos && tick_at != std::string::npos);
  CHECK(src.find("\nvoid setup() {") < loop_at);            // setup() first, as these files are laid out
  CHECK(at != std::string::npos && at > loop_at);           // in loop(), not setup()
  CHECK(at != std::string::npos && at < tick_at);           // ahead of the health tick
  std::printf("%s %s: call_site\n", g_fail == f0 ? "PASS" : "FAIL", product);
}

int main() {
  test_summary_copy<display::Cut>("display");
  test_summary_copy<sense::Cut>("sense");
  test_summary_copy<sentinel::Cut>("sentinel");
  test_summary_copy<vision::Cut>("vision");
  test_boot_probe_gate<display::Cut>("display");
  test_boot_probe_gate<sense::Cut>("sense");
  test_boot_probe_gate<sentinel::Cut>("sentinel");
  test_boot_probe_gate<vision::Cut>("vision");
  test_call_site("display", OTACUT_MAIN_DISPLAY);
  test_call_site("sense", OTACUT_MAIN_SENSE);
  test_call_site("sentinel", OTACUT_MAIN_SENTINEL);
  test_call_site("vision", OTACUT_MAIN_VISION);
  if (g_fail) {
    std::fprintf(stderr, "\n%d OTA_MGR_CUT CHECK(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("\nALL OTA_MGR_CUT TESTS PASSED\n");
  return 0;
}
