// test_http_body_reader.cpp — http_body::read_all() and read_request()
// (http_body_reader.h), and the pin that every handler in canary_wap.ino and
// the sketch's *_api.h files reads its body through them (sweep F300; the
// Beacon routes since F266's reader).
//
// What this guards: a handler that parses after ONE httpd_req_recv() call
// rejects a valid body the client sent in two TCP segments, and silently
// truncates one longer than its buffer. A body must be read completely or
// refused by name.

#include "http_body_reader.h"
#include "beacon_source_scan.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifndef WAP_SKETCH_DIR
#error "WAP_SKETCH_DIR (the sketch directory's absolute path) must be defined"
#endif

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

// ── read_request(): read_all() over a request, as every handler calls it ──
//
// A stand-in request in the global namespace, as IDF's httpd_req_t is, with
// its httpd_req_recv() declared after http_body_reader.h was included: the
// header needs no IDF include, and read_request() finds the read where the
// request type is declared. Each read answers as the plan says (a positive
// entry caps it, as one TCP segment would; a negative one is returned as it
// is), never past content_len, and counts any ask for more than is owed.
struct FakeReq {
  size_t content_len = 0;
  std::string body;            // may hold more than content_len (a next request's bytes)
  std::vector<int> plan;
  size_t off = 0, calls = 0, asked_past_owed = 0;
};
static int httpd_req_recv(FakeReq* r, char* dst, size_t n) {
  const size_t call = r->calls++;
  const size_t owed = r->content_len - r->off;
  if (n > owed) ++r->asked_past_owed;
  size_t k = n;
  if (call < r->plan.size()) {
    const int step = r->plan[call];
    if (step <= 0) return step;
    if (static_cast<size_t>(step) < k) k = static_cast<size_t>(step);
  }
  if (k > owed) k = owed;
  if (k > r->body.size() - r->off) k = r->body.size() - r->off;
  if (k == 0) return 0;
  std::memcpy(dst, r->body.data() + r->off, k);
  r->off += k;
  return static_cast<int>(k);
}

static http_body::Result request(FakeReq& r, size_t cap, std::string* out) {
  std::vector<char> buf(cap ? cap : 1, 'X');
  size_t len = 999;
  const auto res = http_body::read_request(&r, buf.data(), cap, &len);
  if (res == http_body::OK) {
    EXPECT(buf[len] == '\0', "read_request NUL-terminates the body");
    *out = std::string(buf.data(), len);
  } else {
    EXPECT(len == 0, "a refused request reports no length");
  }
  return res;
}

static void test_read_request() {
  static_assert(http_body::REQUEST_TIMEOUT_CODE == TIMEOUT,
                "read_request() retries esp_http_server's HTTPD_SOCK_ERR_TIMEOUT (-3)");
  // No longer than /api/ota/config's own read_all() waits (the source pin
  // below holds that call to its 3).
  static_assert(http_body::REQUEST_MAX_TIMEOUTS == 3, "read_request() sits out three timeouts, as every reader here");
  std::string out;
  {
    const std::string b = "{\"name\":\"Back garden\",\"enabled\":true}";
    FakeReq r{b.size(), b, {5, TIMEOUT, 9}};
    EXPECT(request(r, 128, &out) == http_body::OK && out == b,
           "a body in three short reads and a timeout is read whole, not parsed from its first segment");
    EXPECT(r.asked_past_owed == 0, "read_request never asks for more than Content-Length still owes");
  }
  {
    FakeReq r{7, "{\"a\":1}POST /next", {}};
    EXPECT(request(r, 128, &out) == http_body::OK && out == "{\"a\":1}" && r.off == 7,
           "it reads exactly Content-Length bytes: a next request's bytes stay on the socket");
  }
  {
    FakeReq r{0, "", {}};
    EXPECT(request(r, 64, &out) == http_body::OK && out.empty() && r.calls == 0,
           "no body is OK with length 0 and no read (each route answers that its own way)");
  }
  {
    FakeReq r{63, std::string(63, 'a'), {}};
    EXPECT(request(r, 64, &out) == http_body::OK && out.size() == 63, "a body that fits its terminator is read");
  }
  {
    FakeReq r{64, std::string(64, 'a'), {}};
    EXPECT(request(r, 64, &out) == http_body::TOO_LARGE && r.calls == 0,
           "one byte past the buffer is refused before any read (never cut and parsed)");
  }
  {
    FakeReq r{7, "{\"a\":1}", {TIMEOUT, TIMEOUT, TIMEOUT, 7}};
    EXPECT(request(r, 64, &out) == http_body::OK && out == "{\"a\":1}", "three timeouts are sat out");
  }
  {
    FakeReq r{7, "{\"a\":1}", {TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, 7}};
    EXPECT(request(r, 64, &out) == http_body::READ_FAILED, "the fourth is a failed read");
  }
  {
    FakeReq r{20, "{\"a\":1}", {}};
    EXPECT(request(r, 64, &out) == http_body::READ_FAILED,
           "a peer that closes before Content-Length is in is a failed read, not a short body");
  }
  {
    FakeReq r{7, "{\"a\":1}", {-1}};
    EXPECT(request(r, 64, &out) == http_body::READ_FAILED, "a socket error is a failed read");
  }
}

// ── The source pin (sweep F300): every handler reads through it ──────────
//
// canary_wap.ino and every *_api.h of the sketch (found by listing the
// directory, so a new one is scanned without an edit here): each
// httpd_req_recv( in their code sits inside an http_body::read_all( call
// (/api/ota/config's, read for its Content-Length). In an *_api.h file
// every http_body::read_request( is the one in its read_body(), and every
// read_body( call reads into the buffer it names by that buffer's own size
// and answers the refusal it returns; in canary_wap.ino every
// http_body::read_request( call does that itself, answering through
// http_send_error(req, 400, http_body_refusal(read)) (a function of
// literals, which check_wap_json_answers.py reads). canary_wap.ino had 19
// handlers that parsed what ONE httpd_req_recv() returned, and the *_api.h
// files 20.
static std::string sketch_path(const char* name) { return std::string(WAP_SKETCH_DIR) + "/" + name; }

static size_t line_of(const std::string& code, size_t at) {
  size_t n = 1;
  for (size_t i = 0; i < at && i < code.size(); i++) n += code[i] == '\n';
  return n;
}

// Each `name(` in `code` not preceded by an identifier character: every
// call or definition of `name`, however it is qualified (`::httpd_req_recv(`
// counts), and never a longer name's tail (`xread_body(` is not a
// `read_body(`).
static std::vector<size_t> calls_of(const std::string& code, const std::string& name) {
  std::vector<size_t> out;
  const std::string needle = name + "(";
  for (size_t at = code.find(needle); at != std::string::npos; at = code.find(needle, at + 1)) {
    if (at > 0 && beacon_source_scan::ident_char(code[at - 1])) continue;
    out.push_back(at);
  }
  return out;
}

// The top-level comma-separated arguments of the call whose '(' is at `open`.
static std::vector<std::string> args_of(const std::string& code, size_t open, size_t* close_out) {
  std::vector<std::string> args;
  const size_t close = beacon_source_scan::match_close(code, open, '(', ')');
  *close_out = close;
  if (close == std::string::npos) return args;
  int depth = 0;
  std::string cur;
  for (size_t i = open + 1; i + 1 < close; i++) {
    const char c = code[i];
    if (c == '(' || c == '[' || c == '{') depth++;
    if (c == ')' || c == ']' || c == '}') depth--;
    if (c == ',' && depth == 0) { args.push_back(beacon_source_scan::squeeze(cur)); cur.clear(); continue; }
    cur += c;
  }
  args.push_back(beacon_source_scan::squeeze(cur));
  return args;
}

static void test_every_handler_reads_whole_bodies() {
  using namespace beacon_source_scan;
  std::vector<std::string> files;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(WAP_SKETCH_DIR, ec)) {
    const std::string n = e.path().filename().string();
    if (n == "canary_wap.ino" || (n.size() > 6 && n.compare(n.size() - 6, 6, "_api.h") == 0)) files.push_back(n);
  }
  std::sort(files.begin(), files.end());
  EXPECT(!ec, "the sketch directory lists (source pin fails closed)");
  for (const char* must : {"canary_wap.ino", "beacon_api.h", "bluetooth_api.h", "chirp_api.h", "household_api.h",
                           "audible_chirp_api.h", "rf_presence_api.h"}) {
    EXPECT(std::find(files.begin(), files.end(), must) != files.end(),
           (std::string(must) + " is among the files scanned").c_str());
  }

  std::vector<std::string> outside;   // httpd_req_recv( outside read_all(
  size_t inside = 0, wrapped = 0;
  for (const std::string& f : files) {
    bool ok = false;
    const std::string src = read_source(sketch_path(f.c_str()).c_str(), &ok);
    EXPECT(ok && !src.empty(), (f + " is readable (source pin fails closed)").c_str());
    if (!ok) continue;
    const std::string code = strip_comments(src);

    // The read_all( calls' spans: each reads for the request's Content-Length.
    std::vector<std::pair<size_t, size_t>> spans;
    for (size_t at : calls_of(code, "http_body::read_all")) {
      size_t close = 0;
      const auto a = args_of(code, at + std::strlen("http_body::read_all"), &close);
      spans.emplace_back(at, close);
      EXPECT(a.size() == 7 && a[1] == "req->content_len",
             (f + ":" + std::to_string(line_of(code, at)) + " read_all reads req->content_len bytes").c_str());
      if (f == "canary_wap.ino") {
        EXPECT(a.size() == 7 && a[4] == "HTTPD_SOCK_ERR_TIMEOUT" && a[5] == "3",
               "/api/ota/config's read_all sits out three timeouts (read_request's budget is no longer)");
      }
    }
    for (size_t at : calls_of(code, "httpd_req_recv")) {
      bool in = false;
      for (const auto& sp : spans) in = in || (at > sp.first && at < sp.second);
      if (in) ++inside;
      else outside.push_back(f + ":" + std::to_string(line_of(code, at)));
    }

    // The sketch's handlers: each read_request( call reads into the buffer it
    // names, by that buffer's size, and answers a refusal at once.
    if (f == "canary_wap.ino") {
      const std::string refusal = squeeze(function_body(code, "http_body_refusal"));
      EXPECT(refusal == "{returnr==http_body::TOO_LARGE?\"body_too_large\":\"body_read_failed\";}",
             "canary_wap.ino's http_body_refusal() names both refusals in http_send_error's words");
      for (size_t at : calls_of(code, "http_body::read_request")) {
        size_t close = 0;
        const auto a = args_of(code, at + std::strlen("http_body::read_request"), &close);
        if (close == std::string::npos) continue;
        ++wrapped;
        const std::string where = f + ":" + std::to_string(line_of(code, at));
        const std::string head = squeeze(code.substr(at >= 60 ? at - 60 : 0, at >= 60 ? 60 : at));
        const std::string decl = "consthttp_body::Resultread=";
        EXPECT(head.size() >= decl.size() && head.compare(head.size() - decl.size(), decl.size(), decl) == 0,
               (where + ": the result is kept as `const http_body::Result read`").c_str());
        EXPECT(a.size() == 4 && a[0] == "req" && a[2] == "sizeof(" + a[1] + ")" && a[3].compare(0, 1, "&") == 0,
               (where + ": reads into the buffer it names, by that buffer's size").c_str());
        const std::string tail = squeeze(code.substr(close, 160));
        const std::string answer = ";if(read!=http_body::OK)returnhttp_send_error(req,400,http_body_refusal(read));";
        EXPECT(tail.compare(0, answer.size(), answer) == 0,
               (where + ": a refusal is answered at once with http_send_error(req, 400, http_body_refusal(read))").c_str());
      }
      continue;
    }

    // An *_api.h file's wrapper and its callers.
    const std::string wrapper = "read_body";
    const std::string wbody = squeeze(function_body(code, wrapper));
    const size_t requests = calls_of(code, "http_body::read_request").size();
    if (wbody.empty()) {
      EXPECT(requests == 0, (f + " calls http_body::read_request only from its wrapper").c_str());
      continue;
    }
    EXPECT(wbody.find("http_body::read_request(req,buf,cap,len)") != std::string::npos && requests == 1,
           (f + "'s " + wrapper + "() is the one http_body::read_request() call, for its own buffer").c_str());
    EXPECT(wbody.find("http_body::TOO_LARGE") != std::string::npos && wbody.find("http_body::READ_FAILED") != std::string::npos,
           (f + "'s " + wrapper + "() names both refusals").c_str());
    for (size_t at : calls_of(code, wrapper)) {
      size_t close = 0;
      const auto a = args_of(code, at + wrapper.size(), &close);
      if (close == std::string::npos) continue;
      if (squeeze(code.substr(close, 64)).compare(0, 1, "{") == 0) continue;   // the definition
      ++wrapped;
      const std::string where = f + ":" + std::to_string(line_of(code, at));
      const std::string head = squeeze(code.substr(at >= 40 ? at - 40 : 0, at >= 40 ? 40 : at));
      EXPECT(head.size() >= 17 && head.compare(head.size() - 17, 17, "if(constchar*err=") == 0,
             (where + ": the wrapper's refusal is taken as `if (const char* err = ...)`").c_str());
      EXPECT(a.size() == 4 && a[0] == "req" && a[2] == "sizeof(" + a[1] + ")" && a[3].compare(0, 1, "&") == 0,
             (where + ": reads into the buffer it names, by that buffer's size").c_str());
      // The if's own statement: it answers (returns) with that refusal.
      const size_t stmt = close + 1;
      std::string rest = squeeze(code.substr(stmt, 240));
      const size_t stop = rest.compare(0, 1, "{") == 0 ? rest.find('}') : rest.find(';');
      rest = rest.substr(0, stop == std::string::npos ? rest.size() : stop + 1);
      const bool answers = rest.find(",err)") != std::string::npos || rest.find("=err;") != std::string::npos ||
                           rest.find("returnerr;") != std::string::npos;
      EXPECT(rest.find("return") != std::string::npos && answers,
             (where + ": answers the refusal it was handed and returns: " + rest).c_str());
    }
  }
  if (!outside.empty()) {
    std::printf("FAIL: %zu httpd_req_recv( call(s) in canary_wap.ino and the *_api.h files sit outside "
                "http_body::read_all(:\n", outside.size());
    for (const std::string& w : outside) std::printf("  %s\n", w.c_str());
  }
  EXPECT(outside.empty(), "every httpd_req_recv( in the handlers' files is inside an http_body::read_all( call");
  EXPECT(inside == 1, "and the one left there is /api/ota/config's read_all (read_request reads for the rest)");
  EXPECT(wrapped >= 44, "the 39 handler reads F300 converted and the Beacon routes' five go through a wrapper");
  std::printf("source pin: %zu files, %zu httpd_req_recv( inside read_all, %zu outside, %zu wrapper calls\n",
              files.size(), inside, outside.size(), wrapped);

  // read_request() itself: read_all over httpd_req_recv for the request's
  // Content-Length, with the timeout code and retries named above.
  bool ok = false;
  const std::string hdr = strip_comments(read_source(sketch_path("http_body_reader.h").c_str(), &ok));
  EXPECT(ok, "http_body_reader.h is readable (source pin fails closed)");
  const std::string rr = squeeze(function_body(hdr, "read_request"));
  EXPECT(rr.find("returnread_all([req](char*p,size_tn){returnhttpd_req_recv(req,p,n);},req->content_len,buf,cap,"
                 "REQUEST_TIMEOUT_CODE,REQUEST_MAX_TIMEOUTS,out_len);") != std::string::npos,
         "read_request is read_all over httpd_req_recv for req->content_len, with its timeout code and retries");
}

// Every Beacon route reads through read_body(), and read_body() through
// read_request(): no httpd_req_recv() is left in beacon_api.h.
static void test_beacon_routes_read_whole_bodies() {
  using namespace beacon_source_scan;
  bool ok = false;
  const std::string src = read_source(sketch_path("beacon_api.h").c_str(), &ok);
  EXPECT(ok, "beacon_api.h is readable (source pin fails closed)");
  if (!ok) return;
  const std::string code = strip_comments(src);
  const std::string rb = squeeze(function_body(code, "read_body"));
  EXPECT(count(code, "httpd_req_recv(") == 0, "no httpd_req_recv call is left in beacon_api.h");
  EXPECT(rb.find("http_body::read_request(req,buf,cap,len)") != std::string::npos,
         "read_body reads through http_body::read_request");
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
  test_read_request();
  test_every_handler_reads_whole_bodies();
  test_beacon_routes_read_whole_bodies();
  if (g_fail) {
    std::printf("%d http_body_reader test(s) FAILED\n", g_fail);
    return 1;
  }
  std::printf("ALL http_body_reader TESTS PASSED\n");
  return 0;
}
