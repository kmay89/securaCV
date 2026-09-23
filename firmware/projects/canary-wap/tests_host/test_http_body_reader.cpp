// test_http_body_reader.cpp — http_body::read_all (http_body_reader.h), and
// the pin that every Beacon route reads its body through it.
//
// What this guards: a handler that parses after ONE httpd_req_recv() call
// rejects a valid body the client sent in two TCP segments, and silently
// truncates one longer than its buffer. For the Beacon routes (an alert, an
// all-clear) the body must be read completely or refused by name.

#include "http_body_reader.h"
#include "beacon_source_scan.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define EXPECT(cond, msg)                                   \
  do {                                                      \
    if (!(cond)) {                                          \
      std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      ++g_fail;                                             \
    }                                                       \
  } while (0)

static const int TIMEOUT = -3;  // HTTPD_SOCK_ERR_TIMEOUT's value in esp_http_server

// A scripted socket: each entry is how many bytes the next read delivers
// (capped at what was asked), or a negative code / 0 returned verbatim.
struct Script {
  std::string body;
  std::vector<int> steps;
  size_t pos = 0, step = 0, calls = 0, max_ask = 0, asked_past_end = 0;
  int operator()(char* dst, size_t n) {
    ++calls;
    if (n > max_ask) max_ask = n;
    if (pos + n > body.size()) ++asked_past_end;
    const int s = step < steps.size() ? steps[step++] : static_cast<int>(n);
    if (s <= 0) return s;
    size_t k = static_cast<size_t>(s);
    if (k > n) k = n;
    if (k > body.size() - pos) k = body.size() - pos;
    std::memcpy(dst, body.data() + pos, k);
    pos += k;
    return static_cast<int>(k);
  }
};

static http_body::Result run(Script& s, size_t cap, std::string* out, int max_timeouts = 3) {
  std::vector<char> buf(cap ? cap : 1, 'X');
  size_t len = 999;
  const auto r = http_body::read_all([&s](char* p, size_t n) { return s(p, n); },
                                     s.body.size(), buf.data(), cap, TIMEOUT, max_timeouts, &len);
  if (r == http_body::OK) {
    EXPECT(len == s.body.size(), "out_len is the body length");
    EXPECT(buf[len] == '\0', "the body is NUL-terminated");
    *out = std::string(buf.data(), len);
  } else {
    EXPECT(len == 0, "a refused read reports no length");
  }
  return r;
}

static void test_reads() {
  std::string out;
  {
    Script s{"{\"reason\":\"resolved\"}", {}};
    EXPECT(run(s, 192, &out) == http_body::OK && out == s.body, "one read delivers the whole body");
  }
  {
    Script s{"{\"reason\":\"resolved\",\"certainty\":\"observed\"}", {7, 1, 30}};
    EXPECT(run(s, 192, &out) == http_body::OK && out == s.body,
           "a body split across three reads is reassembled, not parsed from the first fragment");
    EXPECT(s.asked_past_end == 0, "never asks for bytes past content_len (cannot read into a next request)");
  }
  {
    Script s{"", {}};
    EXPECT(run(s, 192, &out) == http_body::OK && out.empty() && s.calls == 0,
           "an empty body reads nothing and is an empty string");
  }
  {
    Script s{std::string(191, 'a'), {}};
    EXPECT(run(s, 192, &out) == http_body::OK && out.size() == 191, "a body that fits with its terminator is read");
  }
  {
    Script s{std::string(192, 'a'), {}};
    EXPECT(run(s, 192, &out) == http_body::TOO_LARGE && s.calls == 0,
           "a body that cannot fit its terminator is refused before any read (never truncated)");
  }
  {
    Script s{std::string(4000, 'a'), {}};
    EXPECT(run(s, 192, &out) == http_body::TOO_LARGE, "an oversized body is refused by name");
  }
  {
    Script s{"x", {}};
    EXPECT(run(s, 0, &out) == http_body::TOO_LARGE, "a zero-capacity buffer holds nothing");
  }
}

static void test_failures() {
  std::string out;
  {
    Script s{"{\"a\":1}", {3, 0}};
    EXPECT(run(s, 64, &out) == http_body::READ_FAILED, "the peer closing mid-body is a failed read");
  }
  {
    Script s{"{\"a\":1}", {-1}};
    EXPECT(run(s, 64, &out) == http_body::READ_FAILED, "a socket error is a failed read");
  }
  {
    Script s{"{\"a\":1}", {TIMEOUT, TIMEOUT, TIMEOUT, 7}};
    EXPECT(run(s, 64, &out, 3) == http_body::OK && out == s.body, "up to max_timeouts timeouts are retried");
  }
  {
    Script s{"{\"a\":1}", {TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, 7}};
    EXPECT(run(s, 64, &out, 3) == http_body::READ_FAILED, "one timeout too many fails the read");
  }
}

// Every Beacon route reads through read_body(), and read_body() through
// read_all(): the one httpd_req_recv() left in beacon_api.h is inside it.
static void test_beacon_routes_read_whole_bodies() {
  using namespace beacon_source_scan;
  bool ok = false;
  const std::string src = read_source(BEACON_API_H, &ok);
  EXPECT(ok, "beacon_api.h is readable (source pin fails closed)");
  if (!ok) return;
  const std::string code = strip_comments(src);
  size_t n = 0;
  for (size_t at = code.find("httpd_req_recv("); at != std::string::npos; at = code.find("httpd_req_recv(", at + 1)) ++n;
  const std::string rb = squeeze(function_body(code, "read_body"));
  EXPECT(n == 1, "exactly one httpd_req_recv call remains in beacon_api.h");
  EXPECT(rb.find("httpd_req_recv(") != std::string::npos && rb.find("http_body::read_all(") != std::string::npos,
         "and it is inside read_body, which loops through http_body::read_all");
  EXPECT(rb.find("req->content_len") != std::string::npos, "read_body reads exactly the declared content length");
  for (const char* h : {"handle_revoke", "handle_originate", "handle_originate_solo", "handle_cosign",
                        "read_cancel_request"}) {
    const std::string body = squeeze(function_body(code, h));
    EXPECT(!body.empty() && body.find("read_body(req,body,sizeof(body),&len)") != std::string::npos,
           (std::string(h) + " reads its body through read_body").c_str());
  }
}

int main() {
  test_reads();
  test_failures();
  test_beacon_routes_read_whole_bodies();
  if (g_fail) {
    std::printf("%d http_body_reader test(s) FAILED\n", g_fail);
    return 1;
  }
  std::printf("ALL http_body_reader TESTS PASSED\n");
  return 0;
}
