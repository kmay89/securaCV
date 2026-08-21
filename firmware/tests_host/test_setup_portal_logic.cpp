// Host tests for the shared headless setup portal's pure halves:
//
//   common/network/setup_portal_logic.h — the timing choreography (AP linger
//   after success, the quiet background retry of saved credentials, the
//   stuck-phone hint, scan-cache freshness). Each rule was paid for on a real
//   phone: instant AP teardown makes every successful provision look failed,
//   and a background begin() under an associated phone reads as "the setup
//   network kicked me off".
//
//   common/network/provision_core.h — the canonical copy of the onboarding
//   byte-math (A-only captive DNS, per-OS probe classification, JSON escaping
//   for hostile SSIDs, the unbiased password alphabet). The display carries a
//   byte-identical copy until it migrates to the shared portal
//   (firmware/scripts/check_provision_core_sync.sh pins the pair); these
//   tests pin the BEHAVIOR of the common copy the sense/vision portal links.

#include "../common/network/provision_core.h"
#include "../common/network/setup_portal_logic.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace canary::net;

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

// ── teardown linger ─────────────────────────────────────────────────────────

static void success_holds_the_ap_until_the_phone_acks() {
  SetupPortalTiming t;
  const uint32_t success = 1000;
  // No ack yet, cap not reached: hold.
  CHECK(!portal_teardown_due(success + 5000, success, false, 0, t),
        "AP must linger while the phone has not seen the verdict");
  // Acked, but the ack beat has not played out: hold.
  const uint32_t acked = success + 5000;
  CHECK(!portal_teardown_due(acked + t.ap_linger_ack_ms - 1, success, true,
                             acked, t),
        "the ack beat must complete before teardown");
  // Acked + beat: tear down.
  CHECK(portal_teardown_due(acked + t.ap_linger_ack_ms + 1, success, true,
                            acked, t),
        "acked + beat means teardown");
  // Never acked: the cap bounds the wait (a phone that stopped polling).
  CHECK(portal_teardown_due(success + t.ap_linger_max_ms + 1, success, false,
                            0, t),
        "the linger cap must fire without an ack");
}

static void teardown_math_survives_millis_wrap() {
  SetupPortalTiming t;
  const uint32_t success = 0xFFFFF000u;  // shortly before wrap
  const uint32_t now = success + t.ap_linger_max_ms + 10;  // wrapped
  CHECK(portal_teardown_due(now, success, false, 0, t),
        "cap must fire across the millis() wrap");
  CHECK(!portal_teardown_due(success + 100, success, false, 0, t),
        "cap must not fire early near the wrap");
}

// ── the quiet background retry ─────────────────────────────────────────────

static void background_retry_needs_saved_credentials_and_an_empty_ap() {
  SetupPortalTiming t;
  const uint32_t due = t.background_retry_ms + 1;
  CHECK(portal_background_retry_due(due, true, 0, false, 0, t),
        "recovery portal must quietly retry the saved network");
  CHECK(!portal_background_retry_due(due, false, 0, false, 0, t),
        "first boot has nothing saved to retry");
  CHECK(!portal_background_retry_due(due, true, 1, false, 0, t),
        "never yank the radio while a phone is associated");
  CHECK(!portal_background_retry_due(due, true, 0, true, 0, t),
        "never retry underneath an in-flight wizard join");
  CHECK(!portal_background_retry_due(due - 2, true, 0, false, 0, t),
        "the cadence gate must hold before it is due");
}

// ── the stuck-phone hint ────────────────────────────────────────────────────

static void stuck_hint_fires_once_and_only_for_a_never_used_ap() {
  SetupPortalTiming t;
  const uint32_t raised = 500;
  const uint32_t late = raised + t.stuck_hint_ms + 1;
  CHECK(portal_stuck_hint_due(late, raised, false, false, t),
        "a long-empty AP earns the forget-this-network hint");
  CHECK(!portal_stuck_hint_due(late, raised, true, false, t),
        "a phone that associated once disproves the stale-password theory");
  CHECK(!portal_stuck_hint_due(late, raised, false, true, t),
        "the hint is logged once, not every pass");
  CHECK(!portal_stuck_hint_due(raised + 1000, raised, false, false, t),
        "the hint must not fire early");
}

// ── scan cache freshness ────────────────────────────────────────────────────

static void scan_cache_staleness() {
  SetupPortalTiming t;
  CHECK(portal_scan_stale(123456, 0, t), "never-swept means stale");
  CHECK(!portal_scan_stale(10000, 9000, t), "a fresh sweep serves from cache");
  CHECK(portal_scan_stale(9000 + t.scan_ttl_ms + 1, 9000, t),
        "an old sweep must trigger a fresh one");
}

// ── captive DNS (common copy) ───────────────────────────────────────────────

// Minimal query builder: header + one QNAME ("a.b") + QTYPE/QCLASS.
static size_t build_query(uint8_t* q, uint16_t qtype) {
  std::memset(q, 0, 64);
  q[0] = 0x12; q[1] = 0x34;   // id
  q[2] = 0x01;                // RD
  q[5] = 0x01;                // QDCOUNT=1
  size_t o = 12;
  q[o++] = 1; q[o++] = 'a';
  q[o++] = 1; q[o++] = 'b';
  q[o++] = 0;
  q[o++] = (uint8_t)(qtype >> 8); q[o++] = (uint8_t)(qtype & 0xFF);
  q[o++] = 0x00; q[o++] = 0x01;  // IN
  return o;
}

static void captive_dns_answers_a_only() {
  const uint8_t ip[4] = {192, 168, 4, 1};
  uint8_t q[64], out[560];

  // A query: one answer carrying the AP IP.
  size_t qlen = build_query(q, 0x0001);
  size_t n = dns_build_response(q, qlen, ip, out, sizeof(out));
  CHECK(n > qlen, "A query must gain an answer record");
  CHECK(out[7] == 0x01, "ANCOUNT must be 1 for A");
  CHECK(out[n - 4] == 192 && out[n - 1] == 1, "answer must carry the AP IP");
  CHECK((out[2] & 0x80) != 0, "QR must flip to response");

  // AAAA: NODATA — answering it with an A record is the malformed reply that
  // stalls Android Chrome instead of falling back to IPv4.
  qlen = build_query(q, 0x001C);
  n = dns_build_response(q, qlen, ip, out, sizeof(out));
  CHECK(n > 0 && out[7] == 0x00, "AAAA must get NODATA, not an A record");

  // HTTPS/SVCB (type 65): same NODATA rule.
  qlen = build_query(q, 0x0041);
  n = dns_build_response(q, qlen, ip, out, sizeof(out));
  CHECK(n > 0 && out[7] == 0x00, "HTTPS query must get NODATA");
}

static void captive_dns_drops_garbage() {
  const uint8_t ip[4] = {192, 168, 4, 1};
  uint8_t q[64], out[560];
  size_t qlen = build_query(q, 0x0001);

  CHECK(dns_build_response(q, 11, ip, out, sizeof(out)) == 0,
        "truncated header must be dropped");
  q[2] |= 0x80;  // QR=1: already a response
  CHECK(dns_build_response(q, qlen, ip, out, sizeof(out)) == 0,
        "a response packet must be dropped, not echoed");
  q[2] &= ~0x80;
  q[5] = 0;      // QDCOUNT=0
  CHECK(dns_build_response(q, qlen, ip, out, sizeof(out)) == 0,
        "zero questions must be dropped");
}

// ── per-OS probe classification (common copy) ───────────────────────────────

static void probe_classification_table() {
  CHECK(classify_probe("/hotspot-detect.html") == Probe::ApplePortal,
        "Apple probe");
  CHECK(classify_probe("/library/test/success.html") == Probe::ApplePortal,
        "legacy Apple probe");
  CHECK(classify_probe("/generate_204") == Probe::Android204, "Android probe");
  CHECK(classify_probe("/gen_204") == Probe::Android204, "short Android probe");
  CHECK(classify_probe("/generate_204?cache=1") == Probe::Android204,
        "cache-busting query must still classify");
  CHECK(classify_probe("/ncsi.txt") == Probe::WindowsNcsi, "Windows NCSI");
  CHECK(classify_probe("/connecttest.txt") == Probe::WindowsConnect,
        "Windows connect test");
  CHECK(classify_probe("/") == Probe::None, "the portal page is not a probe");
  CHECK(classify_probe("/generate_204x") == Probe::None,
        "a longer path must not prefix-match");
}

// ── JSON escaping for hostile SSIDs (common copy) ───────────────────────────

static void hostile_ssid_stays_inert() {
  char out[256];
  CHECK(json_escape("Bob's \"Cafe\" \\ <script>", out, sizeof(out)) > 0,
        "escape must fit");
  CHECK(std::strstr(out, "\\\"Cafe\\\"") != nullptr, "quotes must escape");
  CHECK(std::strstr(out, "\\\\") != nullptr, "backslash must escape");
  const char ctrl[] = {'x', 0x07, '\0'};
  CHECK(json_escape(ctrl, out, sizeof(out)) > 0 &&
            std::strstr(out, "\\u0007") != nullptr,
        "control bytes must become \\u00XX");
  char tiny[4];
  CHECK(json_escape("aaaaaaaa", tiny, sizeof(tiny)) == 0,
        "a string that cannot fit must refuse, not truncate");
}

// ── AP password rendering (common copy) ─────────────────────────────────────

static void password_uses_the_unambiguous_alphabet() {
  uint8_t rnd[16];
  for (size_t i = 0; i < sizeof(rnd); i++) rnd[i] = (uint8_t)(i * 17 + 3);
  char pass[9];
  CHECK(render_password(rnd, sizeof(rnd), pass, sizeof(pass)),
        "render must succeed");
  CHECK(std::strlen(pass) == 8, "must fill the full length");
  CHECK(std::strpbrk(pass, "0O1IlLoi") == nullptr,
        "ambiguous glyphs are banned from human-read passwords");
}

int main() {
  success_holds_the_ap_until_the_phone_acks();
  teardown_math_survives_millis_wrap();
  background_retry_needs_saved_credentials_and_an_empty_ap();
  stuck_hint_fires_once_and_only_for_a_never_used_ap();
  scan_cache_staleness();
  captive_dns_answers_a_only();
  captive_dns_drops_garbage();
  probe_classification_table();
  hostile_ssid_stays_inert();
  password_uses_the_unambiguous_alphabet();

  if (g_failures == 0) {
    std::printf("test_setup_portal_logic: all tests passed\n");
    return 0;
  }
  std::printf("test_setup_portal_logic: %d FAILURE(S)\n", g_failures);
  return 1;
}
