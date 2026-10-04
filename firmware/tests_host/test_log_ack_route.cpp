// POST /api/logs/<seq>/ack on both trees' own handlers (repo sweep F214).
//
// Both firmware trees registered the per-entry acknowledge as
// "/api/logs/*/ack", which esp_http_server's httpd_uri_match_wildcard never
// matches (a `*` counts only as a template's last character), so every
// per-entry Acknowledge answered 404. They now register "/api/logs/*" and
// the handler reads the rest of the path. This suite runs those handlers,
// cut verbatim out of the firmware by cut_functions.awk:
//   - log_ack_seq_from_uri() and handle_log_ack() from
//     canary/lib/securacv_network/src/securacv_network.cpp (PlatformIO), and
//   - the same two from projects/canary-wap/arduino/canary_wap/canary_wap.ino,
// over stand-ins for what they call (esp_http_server's request and error
// reply, the auth gates, the log ring's acknowledge, a flat ArduinoJson).
// What it pins, on both trees, from the same table:
//   - /api/logs/<digits>/ack acknowledges that entry, with a query or
//     fragment after it, a leading zero, the largest uint32_t, and the
//     absolute-form target an HTTP/1.1 server must accept;
//   - anything else the "/api/logs/*" route can be handed (no digits, a
//     sign, a number past uint32_t, another suffix, a trailing slash,
//     ack-all, rotate) answers 404 with httpd's own message and
//     acknowledges nothing. That strictness is new with the route, not a
//     fix of something a device did: the old template matched only its own
//     literal text, so no other path ever reached the old body, but behind
//     "/api/logs/*" that body's atoi() read would have acknowledged entry
//     42 for POST /api/logs/42 and entry 0 for POST /api/logs/x/ack;
//   - canary-wap reads the reason from the body only for a log-ack request;
//   - the PlatformIO handler's rate limit and bearer gate still come first.
// Which request reaches this handler at all is
// test_dashboard_route_match.test.js's question (registration order through
// a copy of the IDF matcher). That test also hands this binary the URL each
// dashboard's per-entry Acknowledge builds: run with --parse, it reads one
// target per line on stdin and prints, per line, what each tree's
// log_ack_seq_from_uri() reads from it ("<pio> <wap>", each the seq or "-"
// for a target it refuses).
//
// Build/run: make -C firmware/tests_host (the Makefile cuts the functions).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// ── esp_http_server stand-ins ────────────────────────────────────────────
typedef int esp_err_t;
static const esp_err_t ESP_OK = 0;
typedef enum { HTTPD_400_BAD_REQUEST = 400, HTTPD_404_NOT_FOUND = 404 } httpd_err_code_t;

struct httpd_req_t {
  const char* uri;
  std::string body;
  size_t body_read = 0;
  int recv_calls = 0;
  int err_status = 0;           // httpd_resp_send_err's code, 0 if none
  std::string err_msg;
  std::string json;             // http_send_json's body, empty if none
};

// httpd_resp_send_err with usr_msg NULL sends the default message; for 404
// that is "Nothing matches the given URI" (IDF httpd_txrx.c), the text httpd
// itself answers a request no route matches with.
static esp_err_t httpd_resp_send_err(httpd_req_t* req, httpd_err_code_t code, const char* usr_msg) {
  req->err_status = (int)code;
  req->err_msg = usr_msg ? usr_msg : (code == HTTPD_404_NOT_FOUND ? "Nothing matches the given URI" : "?");
  return ESP_OK;
}

static int httpd_req_recv(httpd_req_t* req, char* buf, size_t len) {
  req->recv_calls++;
  const size_t n = std::min(len, req->body.size() - req->body_read);
  memcpy(buf, req->body.data() + req->body_read, n);
  req->body_read += n;
  return (int)n;
}

static esp_err_t http_send_json(httpd_req_t* req, const char* json) {
  req->json = json;
  return ESP_OK;
}

// ── A flat ArduinoJson stand-in: string and bool members, in order ───────
class String {
 public:
  const char* c_str() const { return s_.c_str(); }
  std::string s_;
};

struct JsonValue {
  enum Kind { NONE, BOOL, STR } kind = NONE;
  bool b = false;
  std::string s;
};

class JsonDocument;
class JsonRef {
 public:
  JsonRef(JsonDocument* d, std::string k) : d_(d), k_(std::move(k)) {}
  JsonRef& operator=(bool v);
  JsonRef& operator=(const char* v);
  const char* operator|(const char* fallback) const;
 private:
  JsonDocument* d_;
  std::string k_;
};

class JsonDocument {
 public:
  JsonRef operator[](const char* key) { return JsonRef(this, key); }
  std::vector<std::pair<std::string, JsonValue>> members;
  JsonValue* find(const std::string& k) {
    for (auto& m : members) if (m.first == k) return &m.second;
    return nullptr;
  }
  JsonValue& slot(const std::string& k) {
    if (JsonValue* v = find(k)) return *v;
    members.emplace_back(k, JsonValue());
    return members.back().second;
  }
};

JsonRef& JsonRef::operator=(bool v) { JsonValue& s = d_->slot(k_); s.kind = JsonValue::BOOL; s.b = v; return *this; }
JsonRef& JsonRef::operator=(const char* v) { JsonValue& s = d_->slot(k_); s.kind = JsonValue::STR; s.s = v; return *this; }
const char* JsonRef::operator|(const char* fallback) const {
  JsonValue* v = d_->find(k_);
  return (v && v->kind == JsonValue::STR) ? v->s.c_str() : fallback;
}

struct DeserializationError {
  enum Code { Ok, InvalidInput } code;
  DeserializationError(Code c) : code(c) {}  // NOLINT: implicit, as ArduinoJson's
  bool operator==(Code c) const { return code == c; }
};

// {"key":"value", ...} with plain string values: all the tests send.
static DeserializationError deserializeJson(JsonDocument& doc, const char* in) {
  const char* p = in;
  while (*p == ' ') ++p;
  if (*p++ != '{') return DeserializationError::InvalidInput;
  for (;;) {
    while (*p == ' ' || *p == ',') ++p;
    if (*p == '}') return DeserializationError::Ok;
    if (*p++ != '"') return DeserializationError::InvalidInput;
    const char* k = p;
    while (*p && *p != '"') ++p;
    if (!*p) return DeserializationError::InvalidInput;
    std::string key(k, p++);
    while (*p == ' ' || *p == ':') ++p;
    if (*p++ != '"') return DeserializationError::InvalidInput;
    const char* v = p;
    while (*p && *p != '"') ++p;
    if (!*p) return DeserializationError::InvalidInput;
    doc[key.c_str()] = std::string(v, p++).c_str();
  }
}

static void serializeJson(JsonDocument& doc, String& out) {
  out.s_ = "{";
  bool first = true;
  for (auto& m : doc.members) {
    if (!first) out.s_ += ",";
    first = false;
    out.s_ += "\"" + m.first + "\":";
    out.s_ += m.second.kind == JsonValue::BOOL ? (m.second.b ? "true" : "false") : "\"" + m.second.s + "\"";
  }
  out.s_ += "}";
}

// ── The log ring both trees acknowledge into ─────────────────────────────
enum AckStatus { ACK_STATUS_UNREAD = 0, ACK_STATUS_ACKNOWLEDGED = 2 };
struct AckCall { uint32_t seq; AckStatus status; std::string reason; };
static std::vector<AckCall> g_acks;
static bool g_ack_result = true;
static bool acknowledge_log_entry(uint32_t seq, AckStatus status, const char* reason) {
  g_acks.push_back({seq, status, reason});
  return g_ack_result;
}

struct Health { uint32_t http_requests = 0; };

// ── The PlatformIO canary's handler ──────────────────────────────────────
namespace pio {
static Health health;
static Health& witness_get_health() { return health; }
static bool rate_limit_ok = true, auth_ok = true;
static int gate_calls = 0;
static bool rate_limit_check(httpd_req_t*, bool is_action) { gate_calls++; return is_action && rate_limit_ok; }
static bool auth_gate(httpd_req_t*) { gate_calls++; return auth_ok; }
#include "log_ack_pio.inc"
}  // namespace pio

// ── canary-wap's handler ─────────────────────────────────────────────────
namespace wap {
static Health g_health;
#include "log_ack_wap.inc"
}  // namespace wap

// ── Harness ──────────────────────────────────────────────────────────────
static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
  std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

typedef esp_err_t (*Handler)(httpd_req_t*);
struct Tree { const char* name; Handler handler; bool (*parse)(const char*, uint32_t*); };
static const Tree kTrees[] = {
  {"pio", pio::handle_log_ack, pio::log_ack_seq_from_uri},
  {"wap", wap::handle_log_ack, wap::log_ack_seq_from_uri},
};

static httpd_req_t run(const Tree& t, const char* uri, const char* body = "") {
  httpd_req_t req;
  req.uri = uri;
  req.body = body;
  g_acks.clear();
  t.handler(&req);
  return req;
}

// Targets "/api/logs/*" hands the handler that name an entry: the seq each
// acknowledges.
static const struct { const char* uri; uint32_t seq; } kAcks[] = {
  {"/api/logs/42/ack", 42},
  {"/api/logs/0/ack", 0},
  {"/api/logs/007/ack", 7},
  {"/api/logs/4294967295/ack", 4294967295u},
  {"/api/logs/42/ack?x=1", 42},
  {"/api/logs/42/ack#top", 42},
  {"http://canary.local/api/logs/9/ack", 9},   // absolute form (RFC 9112 3.2.2)
};

// Targets the route can be handed that name none: each answers 404.
static const char* const kNotAck[] = {
  "/api/logs/42",            // the old body's atoi() would read entry 42
  "/api/logs/x/ack",         // ... and entry 0
  "/api/logs/42/",
  "/api/logs/42/ackx",
  "/api/logs/42/ack/",
  "/api/logs/42/acknowledge",
  "/api/logs//ack",
  "/api/logs/-1/ack",        // atoi() would read 4294967295 into a uint32_t
  "/api/logs/+1/ack",
  "/api/logs/1a/ack",
  "/api/logs/4294967296/ack",
  "/api/logs/99999999999999999999/ack",
  "/api/logs/ack-all",       // registered before "/api/logs/*", never routed here
  "/api/logs/rotate",
  "/api/logs/unacked",
  "/api/logs/",
};

static void test_acks_name_their_entry() {
  for (const Tree& t : kTrees) {
    for (const auto& a : kAcks) {
      g_ack_result = true;
      httpd_req_t r = run(t, a.uri);
      CHECK(r.err_status == 0, "%s %s: answered %d", t.name, a.uri, r.err_status);
      CHECK(g_acks.size() == 1 && g_acks[0].seq == a.seq && g_acks[0].status == ACK_STATUS_ACKNOWLEDGED,
            "%s %s: acknowledged %zu entries (want seq %u)", t.name, a.uri, g_acks.size(), (unsigned)a.seq);
      CHECK(r.json == "{\"ok\":true}", "%s %s: body %s", t.name, a.uri, r.json.c_str());
      uint32_t seq = 12345;
      CHECK(t.parse(a.uri, &seq) && seq == a.seq, "%s parse %s", t.name, a.uri);
    }
    // An entry the ring does not hold: 200 with ok false, as before.
    g_ack_result = false;
    httpd_req_t r = run(t, "/api/logs/77/ack");
    CHECK(r.err_status == 0 && r.json == "{\"ok\":false,\"error\":\"Log entry not found\"}",
          "%s unknown entry: %d %s", t.name, r.err_status, r.json.c_str());
    g_ack_result = true;
  }
}

static void test_other_paths_answer_404_and_ack_nothing() {
  for (const Tree& t : kTrees) {
    for (const char* uri : kNotAck) {
      httpd_req_t r = run(t, uri, "{\"reason\":\"x\"}");
      CHECK(r.err_status == 404 && r.err_msg == "Nothing matches the given URI",
            "%s %s: answered %d (%s)", t.name, uri, r.err_status, r.err_msg.c_str());
      CHECK(g_acks.empty(), "%s %s: acknowledged entry %u", t.name, uri, g_acks.empty() ? 0u : (unsigned)g_acks[0].seq);
      CHECK(r.json.empty(), "%s %s: sent a JSON body too", t.name, uri);
      CHECK(r.recv_calls == 0, "%s %s: read the body of a request that is not its own", t.name, uri);
      uint32_t seq = 12345;
      CHECK(!t.parse(uri, &seq) && seq == 12345, "%s parse %s took it (or wrote seq)", t.name, uri);
    }
  }
}

static void test_wap_reads_the_reason() {
  const Tree& t = kTrees[1];
  httpd_req_t r = run(t, "/api/logs/5/ack", "{\"reason\":\"seen it\"}");
  CHECK(g_acks.size() == 1 && g_acks[0].seq == 5 && g_acks[0].reason == "seen it",
        "wap: reason %s", g_acks.empty() ? "(none)" : g_acks[0].reason.c_str());
  CHECK(r.recv_calls == 1, "wap: read the body %d times", r.recv_calls);
  r = run(t, "/api/logs/5/ack");
  CHECK(g_acks.size() == 1 && g_acks[0].reason.empty(), "wap: no body, empty reason");
  // The PlatformIO handler never read one.
  r = run(kTrees[0], "/api/logs/5/ack", "{\"reason\":\"seen it\"}");
  CHECK(g_acks.size() == 1 && g_acks[0].reason.empty() && r.recv_calls == 0, "pio: reason is empty");
}

static void test_pio_gates_come_first() {
  const Tree& t = kTrees[0];
  for (int which = 0; which < 2; ++which) {
    pio::rate_limit_ok = which != 0;
    pio::auth_ok = which != 1;
    pio::gate_calls = 0;
    const uint32_t before = pio::health.http_requests;
    for (const char* uri : {"/api/logs/42/ack", "/api/logs/42"}) {
      httpd_req_t r = run(t, uri);
      CHECK(r.err_status == 0 && r.json.empty() && g_acks.empty(),
            "pio %s refused by the %s: still answered %d %s", uri, which ? "bearer gate" : "rate limit",
            r.err_status, r.json.c_str());
    }
    CHECK(pio::health.http_requests == before, "pio: a refused request was counted");
  }
  pio::rate_limit_ok = pio::auth_ok = true;
  const uint32_t before = pio::health.http_requests;
  run(t, "/api/logs/42");
  CHECK(pio::health.http_requests == before + 1, "pio: a 404 after the gates is counted, as before");
}

// --parse: what each tree's parser reads from each target on stdin.
static int parse_stdin() {
  char line[512];
  while (std::fgets(line, sizeof line, stdin)) {
    line[std::strcspn(line, "\r\n")] = '\0';
    for (const Tree& t : kTrees) {
      uint32_t seq = 0;
      if (t.parse(line, &seq)) std::printf("%s%u", &t == kTrees ? "" : " ", (unsigned)seq);
      else std::printf("%s-", &t == kTrees ? "" : " ");
    }
    std::printf("\n");
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--parse") == 0) return parse_stdin();
  test_acks_name_their_entry();
  test_other_paths_answer_404_and_ack_nothing();
  test_wap_reads_the_reason();
  test_pio_gates_come_first();
  if (g_fail) {
    std::printf("test_log_ack_route: %d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("ALL test_log_ack_route TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
