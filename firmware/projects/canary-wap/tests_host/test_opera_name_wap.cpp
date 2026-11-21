// Host test (sweep F303): the Opera name holds no byte a strict JSON reader
// refuses, and neither does any line of the health log.
//
// POST /api/mesh/name (handle_mesh_name()) tested only the name's length,
// POST /api/mesh/pair/start (handle_mesh_pair_start()) nothing, cutting a
// longer name at 32 bytes (inside a character if one stood there), and
// mesh_network's set_opera_name(), start_pairing_initiator() and NVS load
// took what they were handed. GET /api/mesh (handle_mesh_status()) writes
// the name, and GET /api/logs (handle_logs()) every log line's message and
// detail, through ArduinoJson 7.4.1, which writes a control byte raw and
// every other byte as it is; log_health() copied message and detail into
// the ring with strncpy(), raw and cut at a byte count. So a name holding
// 0x01 made both answers ones JSON.parse refuses, and a name cut inside a
// character, or a log line cut inside one, made them text that is not
// UTF-8.
//
// Now:
//   - both routes refuse a name mesh_network::opera_name_storable() refuses
//     (1 to 32 bytes, none below 0x20, well-formed UTF-8) with a 400 whose
//     error is opera_name_refusal(), the rule in words; pair/start keeps the
//     default for no name and refuses a body that is not JSON (one cut at
//     the handler's buffer is not), founding no opera either way;
//   - set_opera_name() and start_pairing_initiator() hold the name to the
//     same test, and a name stored before the rule is read back clean;
//   - log_health() stores message and detail through wap_json::shown_text():
//     each control byte JSON forbids raw and each piece that is not whole
//     UTF-8 as U+FFFD, cut on a character to the ring's field.
// Every answer here is read with json_strict.h (stricter than JSON.parse:
// no raw control byte) and each text in it held to browser_utf8() (the
// WHATWG decoder: well-formed UTF-8 reads back as its own bytes).
//
// This runs the REAL code: mesh_network.cpp #included (on stubs/mesh_net and
// stubs/pair_crypto, mesh_net_sim.h's device, as test_mesh_commands_wap.cpp
// does, the loop task given its turn in the handler's wait), and cut verbatim
// out of canary_wap.ino (firmware/tests_host/cut_functions.awk, the Makefile
// cuts them): the health log's ring and staging declarations (by line
// range), log_health(), log_health_ssid(), health_log(), http_send_json(),
// http_send_error(), http_body_refusal() (over the REAL http_body_reader.h,
// sweep F300), handle_logs(), handle_mesh_status(),
// handle_mesh_pair_start() and handle_mesh_name(). Under them: stand-ins for
// esp_http_server's request (its body, query and answer), the sketch's
// g_health and g_device counters, and arduinojson_tree.h (ArduinoJson's
// writer: the seven escapes, every other byte as it is). check_wap_json_answers.py
// rule J12 holds the ring's text fields to log_health()'s two writes.
//
// And sweep F445: three pairing lines of mesh_network.cpp were longer than
// the ring's 79-byte message field, so GET /api/logs carried them cut. Two
// are now message and detail with no word changed; the guard here reads the
// file's every health_log()/log_health() call, joins each argument's
// literals as the compiler does, and fails a line over its field that is
// not on its one-entry record (the CONFIRM-mismatch line, whose words are
// the maintainer's call), then logs each through the real log_health() to
// show the measure is the ring's.
//
// Host-tested only; the Arduino compile of the sketch is CI's.
//
// Run: ./test_opera_name_wap

#ifndef MESH_NETWORK_CPP
#error "MESH_NETWORK_CPP (absolute path to mesh_network.cpp) must be defined"
#endif
#ifndef OPERA_NAME_RING_INC
#error "OPERA_NAME_RING_INC (the cut of canary_wap.ino's health log ring) must be defined"
#endif
#ifndef OPERA_NAME_INC
#error "OPERA_NAME_INC (the cut of canary_wap.ino's functions) must be defined"
#endif
// The firmware file is held to the device build's warnings, not to this
// Makefile's -Wextra -Wpedantic -Werror (see test_mesh_address_wap.cpp);
// this test's own code keeps all of them.
#pragma GCC diagnostic push
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunknown-warning-option"   // -Wstringop-truncation is GCC's
#endif
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#include MESH_NETWORK_CPP
#pragma GCC diagnostic pop
#include "mesh_net_sim.h"

#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "arduinojson_tree.h"     // ArduinoJson's model, as the cut handlers use it
#include "browser_utf8.h"
#include "health_store_logic.h"   // health_store::line_build(), the ring's durable tier
#include "http_status_line.h"     // the status line http_send_error() sends
#include "json_strict.h"

namespace chirp_channel {
void dispatch_espnow_message(const uint8_t*, const uint8_t*, int, int8_t) {}
}

// ── esp_http_server, as the cut handlers use it ──────────────────────────
#define HTTPD_RESP_USE_STRLEN -1
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif
#define HTTPD_SOCK_ERR_TIMEOUT -3   // what httpd_req_recv() returns for a read that timed out
struct httpd_req_t {
  size_t content_len = 0;      // the request's Content-Length, as IDF fills it
  std::string body;            // what the client sent
  size_t body_off = 0;         // how much of it the handler has read
  // How the socket hands the body over (sweep F300), as stubs/mqtt's
  // request does: each entry caps the next httpd_req_recv() call, as one
  // TCP segment would (a short read); a negative one is returned as it is
  // and reads nothing. Once the plan is spent each call returns all it may.
  std::vector<int> recv_plan;
  size_t recv_calls = 0;
  std::string query;           // after the '?'; "" is none
  std::string status = "200 OK";
  std::string type;
  std::string resp;
  int sends = 0;
};
// As IDF's: at most `len` bytes, never past content_len, as the plan allows.
static int httpd_req_recv(httpd_req_t* r, char* buf, size_t len) {
  const size_t call = r->recv_calls++;
  size_t cap = len;
  if (call < r->recv_plan.size()) {
    const int step = r->recv_plan[call];
    if (step <= 0) return step;
    if ((size_t)step < cap) cap = (size_t)step;
  }
  const size_t owed = r->content_len > r->body_off ? r->content_len - r->body_off : 0;
  if (owed < cap) cap = owed;
  const size_t left = r->body.size() - r->body_off;
  const size_t n = left < cap ? left : cap;
  memcpy(buf, r->body.data() + r->body_off, n);
  r->body_off += n;
  return (int)n;
}
static esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* s) {
  r->status = s;
  return ESP_OK;
}
static esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* t) {
  r->type = t;
  return ESP_OK;
}
static esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
static esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t len) {
  r->resp.assign(buf ? buf : "", buf ? (len < 0 ? strlen(buf) : (size_t)len) : 0);
  ++r->sends;
  return ESP_OK;
}
static esp_err_t httpd_req_get_url_query_str(httpd_req_t* r, char* buf, size_t len) {
  if (r->query.empty() || len == 0) return ESP_ERR_NOT_FOUND;
  snprintf(buf, len, "%s", r->query.c_str());
  return ESP_OK;
}

// ── the sketch's globals the cut functions name ──────────────────────────
static struct {
  uint32_t http_requests;
  uint32_t logs_stored;
  uint32_t logs_unacked;
} g_health;
static struct {
  uint32_t log_seq;
} g_device;
static uint8_t g_log_min_level = SCV_LOG_INFO;

#include "http_body_reader.h"   // the handlers' body reader, http_body::read_request() (sweep F300)
#include OPERA_NAME_RING_INC
void log_health(LogLevel level, LogCategory category, const char* message, const char* detail = nullptr);
#include OPERA_NAME_INC

// ── the suite ────────────────────────────────────────────────────────────
namespace opera_name {

int g_checks = 0;
int g_failures = 0;
#define CHECK_MSG(c, msg)                                                    \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(c)) {                                                              \
      ++g_failures;                                                          \
      std::printf("FAIL %s:%d: %s -- %s\n", __FILE__, __LINE__, #c,          \
                  std::string(msg).c_str());                                 \
    }                                                                        \
  } while (0)
#define CHECK(c) CHECK_MSG(c, "")

using namespace mesh_net_sim;
namespace mn = mesh_network;

Device A;
HealthLogRingEntry g_ring[HEALTH_LOG_RING_SIZE];
HealthPendingLine g_pending[health_store::HS_PENDING_SLOTS];

// The bytes a strict reader takes as text: well-formed UTF-8 (the WHATWG
// decoder reads them back as themselves) with no byte below 0x20.
bool clean_text(const std::string& s) {
  for (unsigned char c : s) {
    if (c < 0x20) return false;
  }
  return browser_utf8(s) == s;
}

// Printable: each byte that is not printable ASCII as \xNN.
std::string shown(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c >= 0x20 && c < 0x7f) {
      out += (char)c;
    } else {
      char e[8];
      snprintf(e, sizeof e, "\\x%02X", c);
      out += e;
    }
  }
  return out;
}

// A JSON body with `key` holding `bytes`, as JSON.stringify() spells a
// string (each byte below 0x20 as \u00XX, `"` and `\` escaped), every other
// byte as it is: a byte that is not UTF-8 too, as a client that is not a
// browser can send.
std::string body_with(const char* key, const std::string& bytes) {
  std::string out = std::string("{\"") + key + "\":\"";
  for (unsigned char c : bytes) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c < 0x20) {
      char e[8];
      snprintf(e, sizeof e, "\\u%04x", c);
      out += e;
    } else {
      out += (char)c;
    }
  }
  return out + "\"}";
}

void reset_log() {
  memset(g_ring, 0, sizeof g_ring);
  memset(g_pending, 0, sizeof g_pending);
  g_health_log_ring = g_ring;
  g_health_log_ring_head = 0;
  g_health_log_ring_count = 0;
  g_health_pending = g_pending;
  g_health_pending_head = 0;
  g_health_pending_count = 0;
  g_health_pending_dropped = 0;
  g_device.log_seq = 0;
  g_health.logs_stored = 0;
  g_health.logs_unacked = 0;
}

// A device that holds an opera named `name` (written as the pairing wrote
// it before the rule, straight into RAM and NVS), booted.
void opera_named(Device& d, const char* name) {
  d.nvs.clear();
  d.espnow = host_sim::EspNow();
  g_cur = nullptr;
  boot(d);
  uint8_t secret[mn::OPERA_SECRET_SIZE];
  host_sim::fill_random(secret, sizeof secret);
  memcpy(mn::g_opera_config.opera_secret, secret, sizeof secret);
  CHECK(mn::compute_opera_id(secret, mn::g_opera_config.opera_id));
  mn::g_opera_config.configured = true;
  mn::g_opera_config.enabled = true;
  snprintf(mn::g_opera_config.opera_name, sizeof mn::g_opera_config.opera_name, "%s", name);
  CHECK(mn::persist_opera_config());
  boot(d);
}

// A device with no opera, booted.
void no_opera(Device& d) {
  d.nvs.clear();
  d.espnow = host_sim::EspNow();
  g_cur = nullptr;
  boot(d);
}

// One request to `handler` on `d`, as the HTTP server's task makes it: the
// loop task gets one update() pass at the handler's first wait.
template <typename H>
httpd_req_t request(Device& d, H handler, const std::string& body, const std::string& query = "",
                    std::vector<int> recv_plan = {}) {
  become(d);
  httpd_req_t req;
  req.content_len = body.size();
  req.body = body;
  req.recv_plan = std::move(recv_plan);
  req.query = query;
  host_sim::on_task_delay = [](uint32_t ms) {
    host_sim::now_ms += ms;
    host_sim::on_httpd_task = false;   // the loop task's turn
    mn::update();
    host_sim::on_httpd_task = true;
  };
  host_sim::on_httpd_task = true;
  handler(&req);
  host_sim::on_httpd_task = false;
  host_sim::on_task_delay = nullptr;
  CHECK_MSG(req.sends == 1, "one answer");
  return req;
}

json_strict::Value parsed(const httpd_req_t& req, const std::string& label) {
  json_strict::Value v;
  CHECK_MSG(json_strict::parse(req.resp, &v), label + ": the answer parses strictly: " + shown(req.resp));
  return v;
}

bool ok_true(const json_strict::Value& v) {
  const json_strict::Value* ok = v.get("ok");
  return ok != nullptr && ok->kind == json_strict::Value::Bool && ok->b;
}

std::string error_of(const json_strict::Value& v) {
  const json_strict::Value* e = v.get("error");
  return e != nullptr && e->kind == json_strict::Value::String ? e->text : "";
}

// GET /api/mesh on `d`: it parses strictly, and its opera_name is clean
// text; returns the name.
std::string mesh_name(Device& d, const std::string& label) {
  const httpd_req_t req = request(d, handle_mesh_status, "");
  const json_strict::Value v = parsed(req, label + ", GET /api/mesh");
  const json_strict::Value* n = v.get("opera_name");
  CHECK_MSG(n != nullptr && n->kind == json_strict::Value::String, label + ": opera_name is a string");
  const std::string name = n != nullptr ? n->text : "";
  CHECK_MSG(clean_text(name), label + ": GET /api/mesh's opera_name is well-formed UTF-8: " + shown(name));
  return name;
}

std::string nvs_name(Device& d) {
  become(d);
  mn::g_prefs.begin(mn::NVS_NS, true);
  const String s = mn::g_prefs.getString(mn::NVS_FLEET_NAME, "");
  mn::g_prefs.end();
  return s.c_str();
}

// One line of GET /api/logs.
struct LogLine {
  std::string message;
  std::string detail;
  bool has_detail = false;
};

// GET /api/logs: it parses strictly, and every message and detail in it is
// clean text, cut on a character to the ring's field; returns the lines,
// most recent first.
std::vector<LogLine> logs(Device& d, const std::string& label) {
  const httpd_req_t req = request(d, handle_logs, "");
  const json_strict::Value v = parsed(req, label + ", GET /api/logs");
  std::vector<LogLine> out;
  const json_strict::Value* list = v.get("logs");
  CHECK_MSG(list != nullptr && list->kind == json_strict::Value::Array, label + ": logs is a list");
  if (list == nullptr) return out;
  for (const json_strict::Value& e : list->items) {
    LogLine line;
    const json_strict::Value* m = e.get("message");
    const json_strict::Value* det = e.get("detail");
    line.message = m != nullptr ? m->text : "";
    line.has_detail = det != nullptr;
    line.detail = det != nullptr ? det->text : "";
    CHECK_MSG(clean_text(line.message), label + ": a log message is well-formed UTF-8: " + shown(line.message));
    CHECK_MSG(clean_text(line.detail), label + ": a log detail is well-formed UTF-8: " + shown(line.detail));
    CHECK_MSG(line.message.size() < sizeof(HealthLogRingEntry::message) &&
                  line.detail.size() < sizeof(HealthLogRingEntry::detail),
              label + ": each text fits its field");
    out.push_back(line);
  }
  return out;
}

bool logged_message(const std::vector<LogLine>& lines, const std::string& message) {
  for (const LogLine& l : lines) {
    if (l.message == message) return true;
  }
  return false;
}

// ── the rule itself ──────────────────────────────────────────────────────

// Names the rule keeps and refuses, as bytes, and why.
struct Name {
  const char* why;
  std::string bytes;
};
const std::string k31(31, 'a');
const std::string k32(32, 'a');
const std::vector<Name> kKept = {
    {"an ASCII name", "Kitchen"},
    {"DEL, which JSON carries raw", std::string("a\x7f") + "b"},
    {"32 bytes of ASCII, the cap", k32},
    {"16 two-byte characters, the cap", [] { std::string s; for (int i = 0; i < 16; ++i) s += "\xC3\xA9"; return s; }()},
    {"three-byte characters to the cap", std::string(2, 'a') + [] { std::string s; for (int i = 0; i < 10; ++i) s += "\xE6\x97\xA5"; return s; }()},
    {"eight four-byte characters, the cap", [] { std::string s; for (int i = 0; i < 8; ++i) s += "\xF0\x9F\x98\x80"; return s; }()},
    {"U+FFFD itself", "a\xEF\xBF\xBD" "b"},
};
const std::vector<Name> kRefused = {
    {"0x01", std::string("Ki\x01") + "tchen"},
    {"a newline", "two\nlines"},
    {"0x1f at the end", "attic\x1f"},
    {"empty", ""},
    {"33 bytes of ASCII", std::string(33, 'a')},
    {"31 bytes and a two-byte character (33 bytes: cut at 32, a lone lead byte)", k31 + "\xC3\xA9"},
    {"30 bytes and a three-byte character (33 bytes)", std::string(30, 'a') + "\xE6\x97\xA5"},
    {"a lone 0xC3", "a\xC3" "b"},
    {"a lead byte at the end", "abc\xC3"},
    {"a stray continuation byte", "a\x80" "b"},
    {"an overlong '/'", "a\xC0\xAF" "b"},
    {"a surrogate", "a\xED\xA0\x80" "b"},
    {"0xFF", "a\xFF" "b"},
};

void test_the_rule_and_its_words() {
  for (const Name& n : kKept) CHECK_MSG(mn::opera_name_storable(n.bytes.c_str()), std::string("kept: ") + n.why);
  for (const Name& n : kRefused) {
    CHECK_MSG(!mn::opera_name_storable(n.bytes.c_str()), std::string("refused: ") + n.why);
  }
  CHECK(!mn::opera_name_storable(nullptr));
  // The words state the rule's bound, and fit http_send_error()'s answer
  // whole, with nothing in them JSON escapes.
  const std::string words = mn::opera_name_refusal();
  CHECK(words.find("1 to 32 bytes") != std::string::npos);
  CHECK(words.find('"') == std::string::npos && words.find('\\') == std::string::npos);
  CHECK(std::string("{\"ok\":false,\"error\":\"").size() + words.size() + 2 < 128);
  std::printf("PASS the_rule_and_its_words\n");
}

// ── POST /api/mesh/name ──────────────────────────────────────────────────

void test_a_rename_keeps_a_name_the_rule_keeps_whole() {
  for (const Name& n : kKept) {
    const std::string label = std::string("rename to ") + n.why;
    opera_named(A, "test");
    reset_log();
    const httpd_req_t req = request(A, handle_mesh_name, body_with("name", n.bytes));
    const json_strict::Value v = parsed(req, label);
    CHECK_MSG(req.status == "200 OK" && ok_true(v), label + ": answered " + req.status + " " + shown(req.resp));
    become(A);
    CHECK_MSG(std::string(mn::g_opera_config.opera_name) == n.bytes, label + ": in RAM, byte for byte");
    CHECK_MSG(nvs_name(A) == n.bytes, label + ": in NVS, byte for byte");
    CHECK_MSG(mesh_name(A, label) == n.bytes, label + ": GET /api/mesh shows it");
    const std::vector<LogLine> lines = logs(A, label);
    CHECK_MSG(!lines.empty() && lines[0].message == "Opera name changed" && lines[0].detail == n.bytes,
              label + ": logged with the name as its detail");
    boot(A);
    CHECK_MSG(mesh_name(A, label + ", after a reboot") == n.bytes, label + ": and after a reboot");
  }
  std::printf("PASS a_rename_keeps_a_name_the_rule_keeps_whole\n");
}

void test_a_rename_refuses_a_name_the_rule_refuses_in_its_words() {
  for (const Name& n : kRefused) {
    const std::string label = std::string("rename to ") + n.why;
    opera_named(A, "test");
    reset_log();
    const httpd_req_t req = request(A, handle_mesh_name, body_with("name", n.bytes));
    const json_strict::Value v = parsed(req, label);
    CHECK_MSG(req.status == "400 Bad Request", label + ": " + req.status);
    CHECK_MSG(!ok_true(v) && error_of(v) == mn::opera_name_refusal(), label + ": the rule's words: " + shown(req.resp));
    become(A);
    CHECK_MSG(std::string(mn::g_opera_config.opera_name) == "test" && nvs_name(A) == "test",
              label + ": the name is as it was, in RAM and NVS");
    CHECK_MSG(mesh_name(A, label) == "test", label + ": GET /api/mesh shows the old name");
    CHECK_MSG(!logged_message(logs(A, label), "Opera name changed"), label + ": no rename is logged");
  }
  std::printf("PASS a_rename_refuses_a_name_the_rule_refuses_in_its_words\n");
}

// The rename's body, as the socket hands it over (sweep F300). The handler
// parsed what ONE httpd_req_recv() returned: a body that came in short
// reads was parsed from its first segment (cut, so refused as invalid_json
// while the client had sent a good rename), and one past the 128-byte
// buffer was cut there. Now it reads Content-Length bytes through
// http_body::read_request(): whole across short reads and a timeout, refused by
// name when it does not fit or stops arriving, renaming nothing.
void test_a_rename_reads_its_body_whole_or_refuses_it_by_name() {
  const std::string name = "Back garden shed";
  const std::string body = body_with("name", name);
  {
    const std::string label = "rename, the body in three short reads and a timeout";
    opera_named(A, "test");
    reset_log();
    const httpd_req_t req = request(A, handle_mesh_name, body, "", {9, HTTPD_SOCK_ERR_TIMEOUT, 4});
    const json_strict::Value v = parsed(req, label);
    CHECK_MSG(req.status == "200 OK" && ok_true(v), label + ": answered " + req.status + " " + shown(req.resp));
    become(A);
    CHECK_MSG(std::string(mn::g_opera_config.opera_name) == name && nvs_name(A) == name,
              label + ": renamed to the whole name, not parsed from the first segment");
    CHECK_MSG(req.body_off == body.size(), label + ": every byte of the body was read");
  }
  struct Refused {
    const char* why;
    std::string body;
    size_t content_len;
    std::vector<int> plan;
    const char* error;
  };
  const std::string big = body_with("name", std::string(117, 'n'));   // 128 bytes: no room for its NUL
  const std::string fits = body_with("name", std::string(116, 'n'));   // 127 bytes and its NUL
  const std::vector<Refused> refused = {
      // Read whole and parsed: the rule refuses a 116-byte name in its words.
      {"a body that just fits the buffer", fits, fits.size(), {40, 40}, mn::opera_name_refusal()},
      {"one byte past the buffer", big, big.size(), {}, "body_too_large"},
      {"four timeouts in a row", body, body.size(),
       {HTTPD_SOCK_ERR_TIMEOUT, HTTPD_SOCK_ERR_TIMEOUT, HTTPD_SOCK_ERR_TIMEOUT, HTTPD_SOCK_ERR_TIMEOUT},
       "body_read_failed"},
      {"the peer closing before Content-Length is in", body.substr(0, 12), body.size(), {}, "body_read_failed"},
      {"no body", "", 0, {}, "invalid_body"},
  };
  CHECK(big.size() == 128 && fits.size() == 127);
  for (const Refused& c : refused) {
    const std::string label = std::string("rename, ") + c.why;
    opera_named(A, "test");
    reset_log();
    become(A);
    httpd_req_t req;
    req.content_len = c.content_len;
    req.body = c.body;
    req.recv_plan = c.plan;
    host_sim::on_task_delay = [](uint32_t ms) { host_sim::now_ms += ms; };
    host_sim::on_httpd_task = true;
    handle_mesh_name(&req);
    host_sim::on_httpd_task = false;
    host_sim::on_task_delay = nullptr;
    const json_strict::Value v = parsed(req, label);
    CHECK_MSG(req.sends == 1 && req.status == "400 Bad Request" && !ok_true(v) && error_of(v) == c.error,
              label + ": " + req.status + " " + shown(req.resp));
    CHECK_MSG(std::string(mn::g_opera_config.opera_name) == "test" && nvs_name(A) == "test",
              label + ": the name is as it was");
    if (std::string(c.error) == "body_too_large") {
      CHECK_MSG(req.recv_calls == 0, label + ": refused before any of it was read");
    }
  }
  std::printf("PASS a_rename_reads_its_body_whole_or_refuses_it_by_name\n");
}

// ── POST /api/mesh/pair/start ────────────────────────────────────────────

void test_a_pairing_start_founds_its_opera_under_a_name_the_rule_keeps() {
  struct Case {
    const char* why;
    std::string body;
    std::string founded;
  };
  const std::vector<Case> cases = {
      {"no body", "", "My Canary Opera"},
      {"a body with no name", "{\"other\":1}", "My Canary Opera"},
      {"a null name", "{\"name\":null}", "My Canary Opera"},
      {"an ASCII name", body_with("name", "Den"), "Den"},
      {"two-byte characters to the cap", body_with("name", kKept[3].bytes), kKept[3].bytes},
      {"four-byte characters to the cap", body_with("name", kKept[5].bytes), kKept[5].bytes},
  };
  for (const Case& c : cases) {
    const std::string label = std::string("pair/start, ") + c.why;
    no_opera(A);
    reset_log();
    const httpd_req_t req = request(A, handle_mesh_pair_start, c.body);
    const json_strict::Value v = parsed(req, label);
    CHECK_MSG(req.status == "200 OK" && ok_true(v), label + ": answered " + req.status + " " + shown(req.resp));
    become(A);
    CHECK_MSG(mn::g_opera_config.configured && mn::g_mesh_state == mn::MESH_PAIRING_INIT, label + ": pairing");
    CHECK_MSG(std::string(mn::g_opera_config.opera_name) == c.founded && nvs_name(A) == c.founded,
              label + ": founded as " + shown(c.founded));
    CHECK_MSG(mesh_name(A, label) == c.founded, label + ": GET /api/mesh shows it");
  }
  std::printf("PASS a_pairing_start_founds_its_opera_under_a_name_the_rule_keeps\n");
}

void test_a_pairing_start_refuses_a_name_and_founds_nothing() {
  struct Case {
    const char* why;
    std::string body;
    std::string error;
  };
  std::vector<Case> cases;
  for (const Name& n : kRefused) {
    cases.push_back({n.why, body_with("name", n.bytes), mn::opera_name_refusal()});
  }
  // A body past the handler's 128-byte buffer: refused by name before any
  // of it is read (sweep F300). It was cut at the buffer, where it founded
  // an opera under the default name, and then (F303) refused as not JSON.
  cases.push_back({"a 120-byte name (the body does not fit the buffer)", body_with("name", std::string(120, 'n')),
                   "body_too_large"});
  cases.push_back({"a body that is not JSON", "name=Den", "invalid_json"});
  for (const Case& c : cases) {
    const std::string label = std::string("pair/start, ") + c.why;
    no_opera(A);
    reset_log();
    become(A);
    const mn::MeshState before = mn::g_mesh_state;
    const httpd_req_t req = request(A, handle_mesh_pair_start, c.body);
    const json_strict::Value v = parsed(req, label);
    CHECK_MSG(req.status == "400 Bad Request", label + ": " + req.status);
    CHECK_MSG(!ok_true(v) && error_of(v) == c.error, label + ": " + shown(req.resp));
    become(A);
    CHECK_MSG(!mn::g_opera_config.configured && !mn::is_pairing() && mn::g_mesh_state == before,
              label + ": no opera founded, no pairing started");
    CHECK_MSG(A.nvs.find("mesh/opera_id") == A.nvs.end() && nvs_name(A).empty(), label + ": nothing stored");
    CHECK_MSG(!logged_message(logs(A, label), "Pairing started (initiator)"), label + ": no start is logged");
  }
  std::printf("PASS a_pairing_start_refuses_a_name_and_founds_nothing\n");
}

void test_the_setters_hold_the_name_to_the_rule() {
  // The loop task's own paths (no route in front of them): a rename
  // command and a pairing start whose name the rule refuses change nothing.
  for (const Name& n : kRefused) {
    if (n.bytes.size() > mn::MAX_OPERA_NAME_LEN) continue;   // a Command carries 32 bytes
    const std::string label = std::string("the setters, ") + n.why;
    opera_named(A, "test");
    become(A);
    CHECK_MSG(!mn::set_opera_name(n.bytes.c_str()), label + ": set_opera_name() refuses it");
    CHECK_MSG(std::string(mn::g_opera_config.opera_name) == "test" && nvs_name(A) == "test", label + ": kept");
    no_opera(A);
    become(A);
    const mn::MeshState before = mn::g_mesh_state;
    CHECK_MSG(!mn::start_pairing_initiator(n.bytes.c_str()), label + ": start_pairing_initiator() refuses it");
    CHECK_MSG(!mn::g_opera_config.configured && !mn::is_pairing() && mn::g_mesh_state == before,
              label + ": nothing founded");
  }
  std::printf("PASS the_setters_hold_the_name_to_the_rule\n");
}

void test_a_name_stored_before_the_rule_reads_back_clean() {
  struct Case {
    const char* why;
    std::string stored;
    std::string loaded;
  };
  const std::vector<Case> cases = {
      {"a name the rule keeps", "Kitchen", "Kitchen"},
      {"a control byte, as a space", std::string("ab\x01") + "cd", "ab cd"},
      {"a newline, as a space", "two\nlines", "two lines"},
      {"the old pair/start's cut: 31 bytes and a lone lead byte", k31 + "\xC3", k31},
      {"a lone 0xC3 inside, as U+FFFD", "a\xC3" "b", "a\xEF\xBF\xBD" "b"},
      {"32 bytes ending in a cut three-byte character", std::string(30, 'a') + "\xE6\x97", std::string(30, 'a')},
  };
  for (const Case& c : cases) {
    const std::string label = std::string("stored before the rule, ") + c.why;
    opera_named(A, "test");
    A.nvs["mesh/opera_name"] = std::vector<uint8_t>(c.stored.begin(), c.stored.end());
    g_cur = nullptr;
    boot(A);
    become(A);
    const std::string loaded = mn::g_opera_config.opera_name;
    CHECK_MSG(clean_text(loaded) && mn::opera_name_storable(loaded.c_str()),
              label + ": loaded clean: " + shown(loaded));
    CHECK_MSG(loaded == c.loaded, label + ": loaded as " + shown(loaded));
    CHECK_MSG(mesh_name(A, label) == c.loaded, label + ": GET /api/mesh shows what was loaded");
  }
  std::printf("PASS a_name_stored_before_the_rule_reads_back_clean\n");
}

// ── the health log's ring ────────────────────────────────────────────────

void test_every_log_line_reaches_the_ring_clean() {
  // log_health() is the one writer of the ring's text (J12): whatever a
  // caller hands it, GET /api/logs parses strictly and holds clean text,
  // cut on a character to the field.
  const size_t msg_cap = sizeof(HealthLogRingEntry::message) - 1;   // 79
  const size_t det_cap = sizeof(HealthLogRingEntry::detail) - 1;    // 47
  struct Case {
    const char* why;
    std::string message;
    std::string detail;
    std::string message_kept;
    std::string detail_kept;
  };
  const std::string rep = "\xEF\xBF\xBD";
  const std::vector<Case> cases = {
      {"0x01", std::string("a\x01") + "b", std::string("c\x01") + "d", "a" + rep + "b", "c" + rep + "d"},
      {"a lone 0xC3", "a\xC3" "b", "c\xC3" "d", "a" + rep + "b", "c" + rep + "d"},
      {"0x7F, kept", std::string("a\x7f") + "b", std::string("c\x7f") + "d", std::string("a\x7f") + "b",
       std::string("c\x7f") + "d"},
      {"a two-byte character across the message's cap", std::string(msg_cap - 1, 'm') + "\xC3\xA9", "",
       std::string(msg_cap - 1, 'm'), ""},
      {"a three-byte character across the detail's cap", "msg", std::string(det_cap - 2, 'd') + "\xE6\x97\xA5",
       "msg", std::string(det_cap - 2, 'd')},
      {"a four-byte character across the detail's cap", "msg", std::string(det_cap - 1, 'd') + "\xF0\x9F\x98\x80",
       "msg", std::string(det_cap - 1, 'd')},
      {"the caps exactly, in ASCII", std::string(msg_cap, 'm'), std::string(det_cap, 'd'), std::string(msg_cap, 'm'),
       std::string(det_cap, 'd')},
      {"past the caps, in ASCII", std::string(msg_cap + 9, 'm'), std::string(det_cap + 9, 'd'),
       std::string(msg_cap, 'm'), std::string(det_cap, 'd')},
      {"a control byte where a replacement does not fit", std::string(msg_cap - 1, 'm') + "\x02",
       std::string(det_cap - 2, 'd') + "\x03", std::string(msg_cap - 1, 'm'), std::string(det_cap - 2, 'd')},
  };
  for (const Case& c : cases) {
    const std::string label = std::string("log_health(), ") + c.why;
    no_opera(A);
    reset_log();
    log_health(SCV_LOG_WARNING, SCV_CAT_MESH, c.message.c_str(), c.detail.empty() ? nullptr : c.detail.c_str());
    const std::vector<LogLine> lines = logs(A, label);
    CHECK_MSG(lines.size() == 1, label + ": one line");
    if (lines.size() != 1) continue;
    CHECK_MSG(lines[0].message == c.message_kept, label + ": message " + shown(lines[0].message));
    CHECK_MSG(lines[0].detail == c.detail_kept && lines[0].has_detail == !c.detail_kept.empty(),
              label + ": detail " + shown(lines[0].detail));
  }

  // The other two ways in: health_log() (every module's) and
  // log_health_ssid(), whose own replaced_text() pass before log_health()'s
  // leaves well-formed text as log_health() alone would, and for this SSID
  // the same bytes (not for every one: at the detail's cap its pass can
  // drop a final ill-formed sequence whole where shown_text() alone keeps
  // a U+FFFD for its first byte; both are well-formed).
  no_opera(A);
  reset_log();
  health_log(SCV_LOG_WARNING, SCV_CAT_MESH, "opera\x01 line\xC3");
  const std::string ssid = std::string("Cafe\x01Net\xC3");
  log_health_ssid(SCV_LOG_INFO, SCV_CAT_NETWORK, "WiFi connected", ssid.c_str());
  log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "WiFi connected", ssid.c_str());
  const std::vector<LogLine> lines = logs(A, "health_log() and log_health_ssid()");
  CHECK(lines.size() == 3);
  if (lines.size() == 3) {
    CHECK_MSG(lines[2].message == "opera" + rep + " line" + rep, shown(lines[2].message));
    CHECK_MSG(lines[1].detail == "Cafe" + rep + "Net" + rep, shown(lines[1].detail));
    CHECK_MSG(lines[0].detail == lines[1].detail,
              "log_health_ssid() stores what log_health() alone stores for this SSID");
  }

  // And the line mesh_network.cpp logs when a pairing times out after this
  // side confirmed (sweep F359), the first line of the mesh layer written as
  // message and detail: each reaches GET /api/logs whole, under the fields'
  // 79 and 47 bytes (the F359 review: one longer line reached the owner cut
  // before its reason). every_mesh_health_line_fits_the_ring below holds
  // every line of the file to the fields (sweep F445).
  opera_named(A, "test");
  reset_log();
  become(A);
  CHECK(mn::start_pairing_initiator(nullptr));
  become(A);
  mn::g_pairing.code_displayed = true;
  mn::g_pairing.code_confirmed = true;
  mn::g_mesh_state = mn::MESH_PAIRING_CONFIRM;
  host_sim::now_ms += mn::PAIRING_TIMEOUT_MS + 1000;
  mn::update();
  CHECK(!mn::is_pairing());
  const std::vector<LogLine> timeout_lines = logs(A, "the pairing timeout's line");
  bool timeout_whole = false;
  for (const LogLine& l : timeout_lines) {
    if (l.message == "opera: pairing timed out; code confirmed here, no answer from the other Canary") {
      timeout_whole = l.has_detail && l.detail == "check its log; older firmware? update both";
    }
  }
  CHECK_MSG(timeout_whole, "the pairing timeout's message and detail reach GET /api/logs whole");
  std::printf("PASS every_log_line_reaches_the_ring_clean\n");
}

// ── mesh_network.cpp's health-log lines fit the ring (sweep F445) ────────
//
// The ring keeps 79 bytes of a message and 47 of a detail
// (HealthLogRingEntry), and GET /api/logs, the dashboard and the BLE log
// read it, so a longer line reached the owner cut on a character, before
// its reason; Serial and /HEALTH carry it whole. Three pairing lines did
// (80, 84 and 121 bytes) until two were split into message and detail
// with no word changed. This reads mesh_network.cpp itself: every
// health_log() and log_health() call, its message and detail arguments
// joined as the compiler joins adjacent literals (a line split across
// source lines is one line), each arm of a ?: on its own. A line over its
// field fails unless kOverCap records it, a record that no longer matches
// an over-long line fails, and so does an argument this reader cannot
// measure (anything but literals, a ?: of them, or a detail's nullptr), so
// no line reaches the ring through a variable the guard never saw. Each
// line it measured is then logged through the REAL log_health() and read
// back from GET /api/logs: whole exactly when the measure says it fits.

struct LogLiteral {
  int line = 0;          // where the call starts, 1-based
  std::string fn;        // health_log or log_health
  bool detail = false;   // the detail argument, not the message
  std::string text;      // the bytes the compiler emits for it
};
struct LogScan {
  size_t calls = 0;
  std::vector<LogLiteral> literals;
  std::vector<std::string> unreadable;   // "line N: why"
};

bool ident_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The end (one past the closing quote) of the string or character literal
// at `i`, raw strings included, or npos if it does not close.
size_t literal_end(const std::string& s, size_t i) {
  if (s[i] == 'R' && i + 1 < s.size() && s[i + 1] == '"') {   // R"delim( ... )delim"
    const size_t open = s.find('(', i + 2);
    if (open == std::string::npos) return std::string::npos;
    const std::string close = ")" + s.substr(i + 2, open - (i + 2)) + "\"";
    const size_t end = s.find(close, open + 1);
    return end == std::string::npos ? end : end + close.size();
  }
  const char q = s[i];
  for (size_t j = i + 1; j < s.size(); ++j) {
    if (s[j] == '\\') {
      ++j;
    } else if (s[j] == q) {
      return j + 1;
    } else if (s[j] == '\n') {
      return std::string::npos;
    }
  }
  return std::string::npos;
}

// Whether a literal starts at `i`: a quote, or R" not inside an identifier.
bool literal_at(const std::string& s, size_t i) {
  if (s[i] == '"' || s[i] == '\'') return true;
  return s[i] == 'R' && i + 1 < s.size() && s[i + 1] == '"' && (i == 0 || !ident_char(s[i - 1]));
}

// `src` with every comment blanked to spaces (newlines kept, so offsets and
// line numbers hold), literals left as they are.
std::string blank_comments(const std::string& src) {
  std::string s = src;
  for (size_t i = 0; i < s.size();) {
    if (literal_at(s, i)) {
      const size_t e = literal_end(s, i);
      i = e == std::string::npos ? s.size() : e;
    } else if (s.compare(i, 2, "//") == 0) {
      while (i < s.size() && s[i] != '\n') s[i++] = ' ';
    } else if (s.compare(i, 2, "/*") == 0) {
      const size_t e = s.find("*/", i + 2);
      const size_t stop = e == std::string::npos ? s.size() : e + 2;
      for (; i < stop; ++i) {
        if (s[i] != '\n') s[i] = ' ';
      }
    } else {
      ++i;
    }
  }
  return s;
}

int line_at(const std::string& s, size_t off) {
  return 1 + (int)std::count(s.begin(), s.begin() + (std::ptrdiff_t)off, '\n');
}

std::string trimmed(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// The bytes one plain "..." literal emits (C escapes decoded; source bytes,
// UTF-8 included, as they are). False for any other literal.
bool decode_literal(const std::string& lit, std::string* out) {
  if (lit.size() < 2 || lit.front() != '"' || lit.back() != '"') return false;
  for (size_t i = 1; i + 1 < lit.size(); ++i) {
    const char c = lit[i];
    if (c != '\\') {
      *out += c;
      continue;
    }
    const char e = lit[++i];
    switch (e) {
      case 'n': *out += '\n'; break;
      case 't': *out += '\t'; break;
      case 'r': *out += '\r'; break;
      case 'a': *out += '\a'; break;
      case 'b': *out += '\b'; break;
      case 'f': *out += '\f'; break;
      case 'v': *out += '\v'; break;
      case 'x': {
        unsigned v = 0;
        size_t n = 0;
        while (i + 1 < lit.size() - 1 && std::isxdigit((unsigned char)lit[i + 1])) {
          const char h = (char)std::tolower((unsigned char)lit[++i]);
          v = v * 16 + (unsigned)(h <= '9' ? h - '0' : h - 'a' + 10);
          ++n;
        }
        if (n == 0) return false;
        *out += (char)(v & 0xff);
        break;
      }
      default:
        if (e >= '0' && e <= '7') {
          unsigned v = (unsigned)(e - '0');
          for (int k = 0; k < 2 && i + 1 < lit.size() - 1 && lit[i + 1] >= '0' && lit[i + 1] <= '7'; ++k) {
            v = v * 8 + (unsigned)(lit[++i] - '0');
          }
          *out += (char)(v & 0xff);
        } else if (e == '\\' || e == '"' || e == '\'' || e == '?') {
          *out += e;
        } else {
          return false;   // \u, \U and the rest: not in this file; refuse rather than guess
        }
    }
  }
  return true;
}

// The ')' that closes the '(' at `open`, or npos.
size_t close_of(const std::string& s, size_t open) {
  int depth = 0;
  for (size_t i = open; i < s.size(); ++i) {
    if (literal_at(s, i)) {
      const size_t e = literal_end(s, i);
      if (e == std::string::npos) return std::string::npos;
      i = e - 1;
    } else if (s[i] == '(') {
      ++depth;
    } else if (s[i] == ')' && --depth == 0) {
      return i;
    }
  }
  return std::string::npos;
}

// The first `sep` in `expr` from `from` outside brackets and literals, or
// npos. A ':' that is half of "::" is not one.
size_t top_level(const std::string& expr, size_t from, char sep) {
  int depth = 0;
  for (size_t i = from; i < expr.size(); ++i) {
    if (literal_at(expr, i)) {
      const size_t e = literal_end(expr, i);
      if (e == std::string::npos) return std::string::npos;
      i = e - 1;
      continue;
    }
    const char c = expr[i];
    if (c == '(' || c == '[' || c == '{') ++depth;
    else if (c == ')' || c == ']' || c == '}') --depth;
    else if (depth == 0 && c == sep) {
      if (sep == ':' && ((i + 1 < expr.size() && expr[i + 1] == ':') || (i > 0 && expr[i - 1] == ':'))) continue;
      return i;
    }
  }
  return std::string::npos;
}

// The texts `expr` can be: each arm of a ?:, each arm joined from its
// adjacent literals. nullptr counts as no text when `null_ok`. False when
// any arm is something else.
bool arm_texts(std::string expr, bool null_ok, std::vector<std::string>* out, std::string* why) {
  expr = trimmed(expr);
  while (expr.size() >= 2 && expr.front() == '(' && close_of(expr, 0) == expr.size() - 1) {
    expr = trimmed(expr.substr(1, expr.size() - 2));
  }
  const size_t q = top_level(expr, 0, '?');
  if (q != std::string::npos) {
    // The ':' that closes this '?': past every nested ?: in the true arm.
    int pending = 1;
    size_t colon = std::string::npos;
    for (size_t i = q + 1; i < expr.size();) {
      const size_t nq = top_level(expr, i, '?');
      const size_t nc = top_level(expr, i, ':');
      if (nc == std::string::npos) break;
      if (nq != std::string::npos && nq < nc) {
        ++pending;
        i = nq + 1;
      } else {
        if (--pending == 0) {
          colon = nc;
          break;
        }
        i = nc + 1;
      }
    }
    if (colon == std::string::npos) {
      *why = "a ?: without its ':'";
      return false;
    }
    return arm_texts(expr.substr(q + 1, colon - q - 1), null_ok, out, why) &&
           arm_texts(expr.substr(colon + 1), null_ok, out, why);
  }
  if (expr == "nullptr" && null_ok) return true;
  std::string text;
  bool any = false;
  for (size_t i = 0; i < expr.size();) {
    if (std::isspace((unsigned char)expr[i])) {
      ++i;
      continue;
    }
    if (expr[i] != '"') {
      *why = "not a literal: " + expr.substr(0, 60);
      return false;
    }
    const size_t e = literal_end(expr, i);
    if (e == std::string::npos || !decode_literal(expr.substr(i, e - i), &text)) {
      *why = "a literal this reader cannot decode: " + expr.substr(i, 60);
      return false;
    }
    any = true;
    i = e;
  }
  if (!any) {
    *why = "an empty argument";
    return false;
  }
  out->push_back(text);
  return true;
}

// Every health_log()/log_health() call in `src` and the texts its message
// and detail can be.
LogScan scan_log_calls(const std::string& src) {
  LogScan scan;
  const std::string s = blank_comments(src);
  std::string prev_ident;
  for (size_t i = 0; i < s.size();) {
    if (literal_at(s, i)) {
      const size_t e = literal_end(s, i);
      i = e == std::string::npos ? s.size() : e;
      prev_ident.clear();
      continue;
    }
    if (!ident_char(s[i]) || (i > 0 && ident_char(s[i - 1]))) {
      if (!std::isspace((unsigned char)s[i])) prev_ident.clear();
      ++i;
      continue;
    }
    size_t j = i;
    while (j < s.size() && ident_char(s[j])) ++j;
    const std::string id = s.substr(i, j - i);
    size_t k = j;
    while (k < s.size() && std::isspace((unsigned char)s[k])) ++k;
    const bool decl = prev_ident == "void" || prev_ident == "extern" || prev_ident == "static" ||
                      prev_ident == "inline";
    if ((id == "health_log" || id == "log_health") && k < s.size() && s[k] == '(' && !decl) {
      ++scan.calls;
      const int line = line_at(s, i);
      // The arguments, split at top-level commas, up to the matching ')'.
      std::vector<std::string> args;
      int depth = 0;
      size_t start = k + 1, end = std::string::npos;
      for (size_t m = k + 1; m < s.size(); ++m) {
        if (literal_at(s, m)) {
          const size_t e = literal_end(s, m);
          if (e == std::string::npos) break;
          m = e - 1;
          continue;
        }
        const char c = s[m];
        if (c == '(' || c == '[' || c == '{') {
          ++depth;
        } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
          --depth;
        } else if (c == ')') {
          args.push_back(s.substr(start, m - start));
          end = m;
          break;
        } else if (c == ',' && depth == 0) {
          args.push_back(s.substr(start, m - start));
          start = m + 1;
        }
      }
      const size_t want = id == "health_log" ? 3 : 4;
      if (end == std::string::npos || args.size() != want) {
        scan.unreadable.push_back("line " + std::to_string(line) + ": " + id + "() with " +
                                  std::to_string(args.size()) + " arguments, not " + std::to_string(want));
        i = j;
        continue;
      }
      for (size_t a = 2; a < want; ++a) {
        std::vector<std::string> texts;
        std::string why;
        if (!arm_texts(args[a], a == 3, &texts, &why)) {
          scan.unreadable.push_back("line " + std::to_string(line) + ": " + id + "()'s " +
                                    (a == 3 ? "detail" : "message") + ": " + why);
          continue;
        }
        for (const std::string& t : texts) scan.literals.push_back({line, id, a == 3, t});
      }
      i = end + 1;
      prev_ident.clear();
      continue;
    }
    prev_ident = id;
    i = j;
  }
  return scan;
}

// The lines kept over their field, each with why: the guard's one recorded
// exception. Its words are the maintainer's call (sweep F445): split as
// the other two were, its message would end at "did not match (a" and its
// detail start inside the parenthesis, so it fits only in new words.
struct OverCap {
  const char* text;
  const char* why;
};
const OverCap kOverCap[] = {
    {"opera: pairing failed: the partner's CONFIRM did not match (a different code, or the partner runs older "
     "pairing firmware)",
     "121 bytes; it fits the ring only reworded (F445 leaves the words to the maintainer)"},
};

const OverCap* over_cap_record(const std::string& text) {
  for (const OverCap& o : kOverCap) {
    if (text == o.text) return &o;
  }
  return nullptr;
}

// The lines of `scan` over their field and not recorded, as "line N: ...".
std::vector<std::string> unrecorded_over_cap(const LogScan& scan, size_t msg_cap, size_t det_cap) {
  std::vector<std::string> out;
  for (const LogLiteral& l : scan.literals) {
    const size_t cap = l.detail ? det_cap : msg_cap;
    if (l.text.size() > cap && over_cap_record(l.text) == nullptr) {
      out.push_back("line " + std::to_string(l.line) + ": " + l.fn + "()'s " + (l.detail ? "detail" : "message") +
                    " is " + std::to_string(l.text.size()) + " bytes, over " + std::to_string(cap) + ": " + l.text);
    }
  }
  return out;
}

void test_the_guard_reads_every_shape_a_line_is_written_in() {
  // The reader itself, on sources written for it: a line split across
  // source lines is measured joined, each arm of a ?: on its own, a comment
  // or a string is not a call, escapes count as the bytes they emit, and
  // what it cannot measure it says.
  const size_t msg_cap = sizeof(HealthLogRingEntry::message) - 1;
  const size_t det_cap = sizeof(HealthLogRingEntry::detail) - 1;
  const std::string fits(msg_cap, 'm'), over(msg_cap + 1, 'm');
  const std::string half1(msg_cap / 2, 'a'), half2(msg_cap + 1 - msg_cap / 2, 'b');
  struct Case {
    const char* why;
    std::string src;
    size_t over;        // lines over their field
    size_t unreadable;  // arguments it cannot measure
    size_t calls;
  };
  const std::vector<Case> cases = {
      {"a message at the field's size", "health_log(L, C, \"" + fits + "\");", 0, 0, 1},
      {"a planted message one byte over", "health_log(L, C, \"" + over + "\");", 1, 0, 1},
      {"one over, split across two source lines",
       "health_log(L, C,\n    \"" + half1 + "\"\n    \"" + half2 + "\");", 1, 0, 1},
      {"one over in a ?:'s second arm", "health_log(L, C, x ? \"short\" : \"" + over + "\");", 1, 0, 1},
      {"one over in a nested ?:", "health_log(L, C, x ? (y ? \"" + over + "\" : \"b\") : \"c\");", 1, 0, 1},
      {"a detail one byte over", "log_health(L, C, \"m\", \"" + std::string(det_cap + 1, 'd') + "\");", 1, 0, 1},
      {"a detail at its size, and nullptr", "log_health(L, C, \"m\", x ? \"" + std::string(det_cap, 'd') +
                                                "\" : nullptr);", 0, 0, 1},
      {"commented out, both ways", "// health_log(L, C, \"" + over + "\");\n/* log_health(L, C, \"" + over +
                                       "\", nullptr); */", 0, 0, 0},
      {"named inside a string", "puts(\"health_log(L, C, \\\"" + over + "\\\")\");", 0, 0, 0},
      {"a message from a variable", "health_log(L, C, buf);", 0, 1, 1},
      {"a nullptr message", "log_health(L, C, nullptr, \"d\");", 0, 1, 1},
      {"the wrong argument count", "log_health(L, C, \"m\");", 0, 1, 1},
      {"log_health_ssid() is another function", "log_health_ssid(L, C, \"" + over + "\", ssid);", 0, 0, 0},
      {"a declaration is not a call", "void log_health(LogLevel l, LogCategory c, const char* m, const char* d);",
       0, 0, 0},
      {"\\x41 is one byte, \\\" one", "health_log(L, C, \"" + std::string(msg_cap - 2, 'm') + "\\x41\\\"\");", 0, 0, 1},
      {"\\x41 one over", "health_log(L, C, \"" + std::string(msg_cap - 1, 'm') + "\\x41\\\"\");", 1, 0, 1},
      {"a three-byte character counts three", "health_log(L, C, \"" + std::string(msg_cap - 2, 'm') +
                                                  "\xE2\x80\x94\");", 1, 0, 1},
      {"two calls on one line", "health_log(L, C, \"a\"); health_log(L, C, \"" + over + "\");", 1, 0, 2},
  };
  for (const Case& c : cases) {
    const LogScan scan = scan_log_calls(c.src);
    CHECK_MSG(scan.calls == c.calls, std::string(c.why) + ": calls " + std::to_string(scan.calls));
    CHECK_MSG(unrecorded_over_cap(scan, msg_cap, det_cap).size() == c.over, std::string(c.why) + ": over");
    CHECK_MSG(scan.unreadable.size() == c.unreadable, std::string(c.why) + ": unreadable");
  }
  // The joined measure is the compiler's: the split line is one text.
  const LogScan split = scan_log_calls("health_log(L, C,\n  \"" + half1 + "\"\n  \"" + half2 + "\");");
  CHECK(split.literals.size() == 1 && split.literals[0].text == half1 + half2 && split.literals[0].line == 1);
  std::printf("PASS the_guard_reads_every_shape_a_line_is_written_in\n");
}

void test_every_mesh_health_line_fits_the_ring() {
  const size_t msg_cap = sizeof(HealthLogRingEntry::message) - 1;   // 79
  const size_t det_cap = sizeof(HealthLogRingEntry::detail) - 1;    // 47
  std::ifstream in(MESH_NETWORK_CPP, std::ios::binary);
  CHECK_MSG(in.good(), std::string("read ") + MESH_NETWORK_CPP);
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string src = buf.str();
  const LogScan scan = scan_log_calls(src);

  // Every call the file holds was read: a plain search of the code with
  // its comments blanked (each name as a whole word, then '(') finds as
  // many. It does not skip literals as the reader does, so a name inside a
  // string would show here as a call the reader did not count.
  const std::string code = blank_comments(src);
  size_t searched = 0;
  for (const char* name : {"health_log", "log_health"}) {
    const size_t len = strlen(name);
    for (size_t at = code.find(name); at != std::string::npos; at = code.find(name, at + 1)) {
      if (at > 0 && ident_char(code[at - 1])) continue;
      size_t k = at + len;
      if (k < code.size() && ident_char(code[k])) continue;
      while (k < code.size() && std::isspace((unsigned char)code[k])) ++k;
      searched += k < code.size() && code[k] == '(' ? 1 : 0;
    }
  }
  CHECK_MSG(scan.calls == searched && searched > 0,
            "calls read " + std::to_string(scan.calls) + ", found " + std::to_string(searched));
  for (const std::string& u : scan.unreadable) CHECK_MSG(false, "mesh_network.cpp " + u);
  for (const std::string& o : unrecorded_over_cap(scan, msg_cap, det_cap)) {
    CHECK_MSG(false, "mesh_network.cpp " + o + " (the ring keeps " + std::to_string(msg_cap) + " of a message, " +
                         std::to_string(det_cap) + " of a detail: split it into message and detail, F445)");
  }
  // Each record still names a line in the file, and one over its field.
  for (const OverCap& o : kOverCap) {
    bool found = false;
    for (const LogLiteral& l : scan.literals) {
      found |= l.text == o.text && l.text.size() > (l.detail ? det_cap : msg_cap);
    }
    CHECK_MSG(found, std::string("kOverCap names no over-long line of mesh_network.cpp: ") + o.text);
  }

  // The measure is the ring's: each text logged through the REAL
  // log_health() reaches GET /api/logs whole exactly when it fits.
  for (const LogLiteral& l : scan.literals) {
    const std::string label = "mesh_network.cpp:" + std::to_string(l.line) + " " + (l.detail ? "detail" : "message");
    no_opera(A);
    reset_log();
    if (l.detail) {
      log_health(SCV_LOG_WARNING, SCV_CAT_MESH, "m", l.text.c_str());
    } else {
      log_health(SCV_LOG_WARNING, SCV_CAT_MESH, l.text.c_str(), nullptr);
    }
    const std::vector<LogLine> lines = logs(A, label);
    CHECK_MSG(lines.size() == 1, label + ": one line");
    if (lines.size() != 1) continue;
    const std::string& kept = l.detail ? lines[0].detail : lines[0].message;
    const bool fits_field = l.text.size() <= (l.detail ? det_cap : msg_cap);
    CHECK_MSG((kept == l.text) == fits_field, label + ": kept " + std::to_string(kept.size()) + " of " +
                                                  std::to_string(l.text.size()) + " bytes");
  }
  std::printf("PASS every_mesh_health_line_fits_the_ring (%zu calls, %zu texts, %zu recorded over)\n", scan.calls,
              scan.literals.size(), sizeof kOverCap / sizeof kOverCap[0]);
}

}  // namespace opera_name

int main() {
  using namespace opera_name;
  make_device(A, "A", 0xA1);
  reset_log();
  test_the_rule_and_its_words();
  test_a_rename_keeps_a_name_the_rule_keeps_whole();
  test_a_rename_refuses_a_name_the_rule_refuses_in_its_words();
  test_a_rename_reads_its_body_whole_or_refuses_it_by_name();
  test_a_pairing_start_founds_its_opera_under_a_name_the_rule_keeps();
  test_a_pairing_start_refuses_a_name_and_founds_nothing();
  test_the_setters_hold_the_name_to_the_rule();
  test_a_name_stored_before_the_rule_reads_back_clean();
  test_every_log_line_reaches_the_ring_clean();
  test_the_guard_reads_every_shape_a_line_is_written_in();
  test_every_mesh_health_line_fits_the_ring();
  if (g_failures != 0) {
    std::printf("test_opera_name_wap: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL opera name and health log text checks PASSED (%d checks)\n", g_checks);
  return 0;
}
