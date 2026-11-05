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
// http_send_error(), handle_logs(), handle_mesh_status(),
// handle_mesh_pair_start() and handle_mesh_name(). Under them: stand-ins for
// esp_http_server's request (its body, query and answer), the sketch's
// g_health and g_device counters, and arduinojson_tree.h (ArduinoJson's
// writer: the seven escapes, every other byte as it is). check_wap_json_answers.py
// rule J12 holds the ring's text fields to log_health()'s two writes.
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
struct httpd_req_t {
  std::string body;            // what the client sent, read once
  std::string query;           // after the '?'; "" is none
  std::string status = "200 OK";
  std::string type;
  std::string resp;
  int sends = 0;
};
static int httpd_req_recv(httpd_req_t* r, char* buf, size_t len) {
  const size_t n = r->body.size() < len ? r->body.size() : len;
  memcpy(buf, r->body.data(), n);
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
  mn::compute_opera_id(secret, mn::g_opera_config.opera_id);
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
httpd_req_t request(Device& d, H handler, const std::string& body, const std::string& query = "") {
  become(d);
  httpd_req_t req;
  req.body = body;
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
  // A body the handler's 128-byte buffer cuts is not JSON there: refused,
  // where it founded an opera under the default name.
  cases.push_back({"a 120-byte name (the body does not fit the buffer)", body_with("name", std::string(120, 'n')),
                   "invalid_json"});
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
  // side confirmed (sweep F359), the one line of the mesh layer written as
  // message and detail: each reaches GET /api/logs whole, under the fields'
  // 79 and 47 bytes (the F359 review: one longer line reached the owner cut
  // before its reason).
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

}  // namespace opera_name

int main() {
  using namespace opera_name;
  make_device(A, "A", 0xA1);
  reset_log();
  test_the_rule_and_its_words();
  test_a_rename_keeps_a_name_the_rule_keeps_whole();
  test_a_rename_refuses_a_name_the_rule_refuses_in_its_words();
  test_a_pairing_start_founds_its_opera_under_a_name_the_rule_keeps();
  test_a_pairing_start_refuses_a_name_and_founds_nothing();
  test_the_setters_hold_the_name_to_the_rule();
  test_a_name_stored_before_the_rule_reads_back_clean();
  test_every_log_line_reaches_the_ring_clean();
  if (g_failures != 0) {
    std::printf("test_opera_name_wap: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL opera name and health log text checks PASSED (%d checks)\n", g_checks);
  return 0;
}
