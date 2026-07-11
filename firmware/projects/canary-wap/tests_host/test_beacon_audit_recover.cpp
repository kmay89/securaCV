/* Host tests for beacon_audit_recover.h — the pure chain-linkage guard the
 * beacon-audit SD recovery uses before adopting a head from the file tail.
 * Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_beacon_audit_recover.cpp \
 *       -o /tmp/test_beacon_audit_recover && /tmp/test_beacon_audit_recover
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "beacon_audit_recover.h"

using namespace beacon_audit_recover;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static const std::string ZEROS(64, '0');
static const std::string HA(64, 'a');
static const std::string HB(64, 'b');
static const std::string HC(64, 'c');

// A minimal audit line carrying just the two fields the guard inspects. The
// on-disk lines carry many more, but the parser is bounded to these markers.
static std::string line(const std::string& prev, const std::string& head) {
  return "{\"prev\":\"" + prev + "\",\"head\":\"" + head + "\"}\n";
}

// Convenience: run recover_head and compare the adopted head to `want_head`.
static bool recovers_to(const std::string& buf, bool from_start,
                        const std::string& want_head) {
  uint8_t out[32];
  if (!recover_head(buf.data(), buf.size(), from_start, out)) return false;
  uint8_t want[32];
  if (!parse_hex32(want_head.c_str(), want)) return false;
  return memcmp(out, want, 32) == 0;
}

static bool refuses(const std::string& buf, bool from_start) {
  uint8_t out[32];
  return !recover_head(buf.data(), buf.size(), from_start, out);
}

static void test_linkage() {
  // Two chain-linked lines: newest.prev == predecessor.head → adopt newest.
  CHECK(recovers_to(line(ZEROS, HA) + line(HA, HB), true, HB));

  // Predecessor confirmed by a leading fragment even when not from file start.
  CHECK(recovers_to("partial-no-nl\n" + line(ZEROS, HA) + line(HA, HB), false, HB));

  // Not linked: newest.prev != predecessor.head → refuse (keep NVS head).
  CHECK(refuses(line(ZEROS, HA) + line(HB, HC), true));
}

static void test_first_record() {
  // A genuine first record: no predecessor, prev == genesis (all-zero) → adopt.
  CHECK(recovers_to(line(ZEROS, HA), true, HA));

  // Single line whose prev is NOT genesis and with no verifiable predecessor:
  // linkage cannot be checked → refuse.
  CHECK(refuses(line(HB, HA), true));

  // Same single non-genesis line, but the window began mid-file: the leading
  // fragment is not a trustworthy predecessor → refuse.
  CHECK(refuses("frag-no-nl\n" + line(HB, HA), false));
}

static void test_torn_and_junk() {
  // A torn final write (no trailing '\n') is ignored; the newest COMPLETE line
  // still wins and must chain-link.
  const std::string torn = "{\"prev\":\"" + HB + "\",\"head\":\"" + HC;  // no close/newline
  CHECK(recovers_to(line(ZEROS, HA) + line(HA, HB) + torn, true, HB));

  // Newest complete line missing the fields → refuse (junk can't be adopted).
  CHECK(refuses(line(ZEROS, HA) + "{\"foo\":\"bar\"}\n", true));

  // Empty / unterminated buffers → refuse.
  CHECK(refuses("", true));
  CHECK(refuses("{\"prev\":\"" + ZEROS + "\",\"head\":\"" + HA + "\"}", true));  // no '\n'
}

static void test_field_bounds() {
  // A `head` field belonging to the PREDECESSOR must not be read as the
  // newest line's head: fields are bounded to their own line. Here the newest
  // line has its own (linked) head HC; the predecessor's head is HB.
  CHECK(recovers_to(line(ZEROS, HB) + line(HB, HC), true, HC));

  // Short hex (truncated head value) → parse fails → refuse.
  const std::string short_head =
      "{\"prev\":\"" + ZEROS + "\",\"head\":\"abcd\"}\n";
  CHECK(refuses(short_head, true));
}

int main() {
  test_linkage();
  test_first_record();
  test_torn_and_junk();
  test_field_bounds();
  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL beacon_audit_recover tests PASSED\n");
  return 0;
}
