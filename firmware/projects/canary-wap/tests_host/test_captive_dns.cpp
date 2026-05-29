// Host-side unit tests for the captive-portal DNS response builder
// (arduino/canary_wap/captive_dns.h). Pure logic, no Arduino glue: feed
// crafted query packets and assert the response bytes.
//
// What's pinned here:
//   - A queries get an A answer pointing at the AP IP (the redirect).
//   - AAAA / HTTPS / other QTYPEs get NOERROR + ANCOUNT=0 (NODATA), so a
//     client falls straight back to its IPv4 lookup instead of stalling on a
//     malformed answer (the Android Chrome failure mode).
//   - Malformed / non-query / oversized inputs are dropped (return 0).
//
// Build/run via tests_host/Makefile. Exits non-zero on any failure.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../arduino/canary_wap/captive_dns.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

// DNS record-type constants used in the tests.
static constexpr uint16_t QTYPE_A    = 0x0001;
static constexpr uint16_t QTYPE_AAAA = 0x001C;  // 28
static constexpr uint16_t QTYPE_HTTPS = 0x0041; // 65 (SVCB/HTTPS)

// Build a minimal DNS query: 12-byte header + QNAME (labels) + QTYPE + QCLASS.
static std::vector<uint8_t> make_query(const std::vector<std::string>& labels,
                                       uint16_t qtype, uint8_t flags_hi = 0x01,
                                       uint16_t qdcount = 1) {
  std::vector<uint8_t> p;
  p.push_back(0x12); p.push_back(0x34);                       // ID
  p.push_back(flags_hi); p.push_back(0x00);                   // flags (QR=0)
  p.push_back(qdcount >> 8); p.push_back(qdcount & 0xFF);     // QDCOUNT
  p.push_back(0); p.push_back(0);                             // ANCOUNT
  p.push_back(0); p.push_back(0);                             // NSCOUNT
  p.push_back(0); p.push_back(0);                             // ARCOUNT
  for (const auto& l : labels) {
    p.push_back((uint8_t)l.size());
    for (char c : l) p.push_back((uint8_t)c);
  }
  p.push_back(0x00);                                          // root label
  p.push_back(qtype >> 8); p.push_back(qtype & 0xFF);         // QTYPE
  p.push_back(0x00); p.push_back(0x01);                       // QCLASS IN
  return p;
}

static const uint8_t AP_IP[4] = {192, 168, 4, 1};

static void test_a_query_gets_redirect() {
  std::printf("test_a_query_gets_redirect\n");
  auto q = make_query({"canary", "local"}, QTYPE_A);
  uint8_t out[512];
  size_t n = captive_dns::build_response(q.data(), q.size(), AP_IP, out, sizeof(out));

  CHECK(n == q.size() + 16, "A response is query + 16-byte answer");
  CHECK((out[2] & 0x80) != 0, "QR bit set (is a response)");
  CHECK((out[2] & 0x04) != 0, "AA bit set (authoritative)");
  CHECK((out[2] & 0x01) == 1, "RD bit preserved from query");
  CHECK(out[3] == 0x00, "RCODE = NOERROR");
  CHECK(out[4] == 0x00 && out[5] == 0x01, "QDCOUNT preserved = 1");
  CHECK(out[6] == 0x00 && out[7] == 0x01, "ANCOUNT = 1");

  size_t a = q.size();  // answer begins where the query ended
  CHECK(out[a] == 0xC0 && out[a + 1] == 0x0C, "answer NAME is pointer to 0x0C");
  CHECK(out[a + 2] == 0x00 && out[a + 3] == 0x01, "answer TYPE = A");
  CHECK(out[a + 4] == 0x00 && out[a + 5] == 0x01, "answer CLASS = IN");
  CHECK(out[a + 10] == 0x00 && out[a + 11] == 0x04, "answer RDLENGTH = 4");
  CHECK(out[a + 12] == 192 && out[a + 13] == 168 &&
        out[a + 14] == 4 && out[a + 15] == 1, "answer RDATA = AP IP");
}

static void test_bare_hostname_a_query() {
  std::printf("test_bare_hostname_a_query\n");
  // Single-label QNAME ("canary") exercises the label walk with one label.
  auto q = make_query({"canary"}, QTYPE_A);
  uint8_t out[512];
  size_t n = captive_dns::build_response(q.data(), q.size(), AP_IP, out, sizeof(out));
  CHECK(n == q.size() + 16, "single-label A query still resolves");
  CHECK(out[7] == 0x01, "ANCOUNT = 1 for bare hostname");
  CHECK(out[n - 4] == 192 && out[n - 1] == 1, "trailing RDATA is AP IP");
}

static void test_aaaa_query_gets_nodata() {
  std::printf("test_aaaa_query_gets_nodata\n");
  auto q = make_query({"canary", "local"}, QTYPE_AAAA);
  uint8_t out[512];
  size_t n = captive_dns::build_response(q.data(), q.size(), AP_IP, out, sizeof(out));
  CHECK(n == q.size(), "AAAA response has no appended answer");
  CHECK((out[2] & 0x80) != 0, "AAAA response still marked QR");
  CHECK(out[3] == 0x00, "AAAA RCODE = NOERROR (not NXDOMAIN)");
  CHECK(out[6] == 0x00 && out[7] == 0x00, "AAAA ANCOUNT = 0 (NODATA)");
}

static void test_https_query_gets_nodata() {
  std::printf("test_https_query_gets_nodata\n");
  auto q = make_query({"connectivitycheck", "gstatic", "com"}, QTYPE_HTTPS);
  uint8_t out[512];
  size_t n = captive_dns::build_response(q.data(), q.size(), AP_IP, out, sizeof(out));
  CHECK(n == q.size(), "HTTPS/SVCB response has no appended answer");
  CHECK(out[6] == 0x00 && out[7] == 0x00, "HTTPS ANCOUNT = 0 (NODATA)");
  CHECK(out[3] == 0x00, "HTTPS RCODE = NOERROR");
}

static void test_rd_bit_cleared_is_preserved() {
  std::printf("test_rd_bit_cleared_is_preserved\n");
  auto q = make_query({"canary"}, QTYPE_A, /*flags_hi=*/0x00);  // RD not set
  uint8_t out[512];
  size_t n = captive_dns::build_response(q.data(), q.size(), AP_IP, out, sizeof(out));
  CHECK(n > 0, "RD-clear query still answered");
  CHECK((out[2] & 0x01) == 0, "RD stays clear in response");
  CHECK(out[2] == 0x84, "flags = QR|AA only when RD clear");
}

static void test_dropped_inputs() {
  std::printf("test_dropped_inputs\n");
  uint8_t out[512];

  // Too short to be a DNS header.
  uint8_t tiny[5] = {0, 1, 2, 3, 4};
  CHECK(captive_dns::build_response(tiny, sizeof(tiny), AP_IP, out, sizeof(out)) == 0,
        "sub-header packet dropped");

  // Already a response (QR=1).
  auto resp = make_query({"canary"}, QTYPE_A, /*flags_hi=*/0x80);
  CHECK(captive_dns::build_response(resp.data(), resp.size(), AP_IP, out, sizeof(out)) == 0,
        "QR=1 packet dropped (don't answer responses)");

  // No question (QDCOUNT = 0).
  auto noq = make_query({"canary"}, QTYPE_A, /*flags_hi=*/0x01, /*qdcount=*/0);
  CHECK(captive_dns::build_response(noq.data(), noq.size(), AP_IP, out, sizeof(out)) == 0,
        "QDCOUNT=0 packet dropped");

  // Output buffer too small for query + 16-byte answer.
  auto q = make_query({"canary"}, QTYPE_A);
  CHECK(captive_dns::build_response(q.data(), q.size(), AP_IP, out, q.size() + 10) == 0,
        "undersized out buffer → drop, no overflow");
}

int main() {
  std::printf("=== captive_dns host tests ===\n");
  test_a_query_gets_redirect();
  test_bare_hostname_a_query();
  test_aaaa_query_gets_nodata();
  test_https_query_gets_nodata();
  test_rd_bit_cleared_is_preserved();
  test_dropped_inputs();

  if (g_failures == 0) {
    std::printf("ALL CAPTIVE-DNS TESTS PASSED\n");
    return 0;
  }
  std::printf("CAPTIVE-DNS TESTS FAILED: %d\n", g_failures);
  return 1;
}
