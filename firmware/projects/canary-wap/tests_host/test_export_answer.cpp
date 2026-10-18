// POST /api/export on canary-wap answers the bundle it exports (sweep F278).
//
// The handler wrote the bundle header to the card and answered
// {"ok":true,"download_url":"/api/download?path=/EXPORT/bundle_<ms>.json"},
// a route nothing registers, so the dashboard's Export failed after every
// good export, and with no card it answered ok:false and exported nothing.
// The bundle is a fixed set of short fields, so it is now answered inline
// beside ok:true (as the PlatformIO canary's handle_export answers its own),
// and the card copy is kept where a card takes it, named in card_path.
//
// This runs handle_export() cut verbatim out of canary_wap.ino by
// firmware/tests_host/cut_functions.awk, over the fake SD card in
// stubs/sd_fake and stand-ins for what else it calls (a flat ArduinoJson that
// writes JSON the way ArduinoJson does for these fields, the health log, the
// SD state hooks, http_send_json). What it pins:
//   - with a card: the answer parses under JSON.parse's rules and carries
//     ok:true and every bundle field (the public key as 64 hex); it names no
//     URL to fetch (no download_url, no /api/ path anywhere); card_path names
//     the file now on the card, and that file is the same header, field for
//     field, without the answer's ok, sd_available and card_path;
//   - with no card: the same bundle, ok:true, sd_available:false, no
//     card_path, nothing written to the card;
//   - a card that takes nothing (every write fails), or takes only part of
//     the write: the export still answers the bundle, names no copy, leaves
//     no torn file, and counts the SD failure;
//   - a card mounted but gone when the sketch verifies it: the bundle,
//     sd_available:false, no card_path, no write tried;
//   - the longest answer (a 31-character device id, every counter at its
//     uint32 maximum, a card path) stays a few hundred bytes, which is what
//     lets the handler hold it whole.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-export-answer

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Arduino.h"   // stubs/sd_fake: Serial, yield
#include "SD.h"        // stubs/sd_fake: the fake card, File, FILE_WRITE
#include "json_strict.h"

// ── Arduino String: the calls the handler (and the one it replaced) make ──
class String {
 public:
  String() = default;
  String(const char* s) : s_(s ? s : "") {}  // NOLINT: implicit, as Arduino's
  String operator+(const char* t) const { String r(*this); r.s_ += t ? t : ""; return r; }
  const char* c_str() const { return s_.c_str(); }
  std::string s_;
};

// ── A flat ArduinoJson stand-in: string, unsigned and bool members, in order ──
struct JVal {
  enum Kind { STR, UINT, BOOL } kind = STR;
  std::string s;
  uint64_t u = 0;
  bool b = false;
};

class JsonDocument {
 public:
  class Ref {
   public:
    Ref(JsonDocument* d, std::string k) : d_(d), k_(std::move(k)) {}
    Ref& operator=(const char* v) { JVal& x = d_->slot(k_); x.kind = JVal::STR; x.s = v ? v : ""; return *this; }
    Ref& operator=(const String& v) { return *this = v.c_str(); }
    Ref& operator=(bool v) { JVal& x = d_->slot(k_); x.kind = JVal::BOOL; x.b = v; return *this; }
    template <class T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value, int>::type = 0>
    Ref& operator=(T v) { JVal& x = d_->slot(k_); x.kind = JVal::UINT; x.u = (uint64_t)v; return *this; }
   private:
    JsonDocument* d_;
    std::string k_;
  };
  Ref operator[](const char* k) { return Ref(this, k); }
  JVal& slot(const std::string& k) {
    for (auto& m : members) if (m.first == k) return m.second;
    members.emplace_back(k, JVal());
    return members.back().second;
  }
  std::vector<std::pair<std::string, JVal>> members;
};

inline std::string json_text(const JsonDocument& doc) {
  std::string o = "{";
  bool first = true;
  for (const auto& m : doc.members) {
    if (!first) o += ',';
    first = false;
    o += '"' + m.first + "\":";
    const JVal& v = m.second;
    if (v.kind == JVal::BOOL) {
      o += v.b ? "true" : "false";
    } else if (v.kind == JVal::UINT) {
      o += std::to_string(v.u);
    } else {
      o += '"';
      for (unsigned char c : v.s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); o += e; }
        else o += (char)c;
      }
      o += '"';
    }
  }
  return o + "}";
}
inline size_t measureJson(const JsonDocument& doc) { return json_text(doc).size(); }
inline size_t serializeJson(const JsonDocument& doc, String& out) { out.s_ = json_text(doc); return out.s_.size(); }
inline size_t serializeJson(const JsonDocument& doc, File& f) {
  const std::string t = json_text(doc);
  return f.write((const uint8_t*)t.data(), t.size());
}

// ── esp_http_server and the sketch's globals ─────────────────────────────
typedef int esp_err_t;
static const esp_err_t ESP_OK = 0;
struct httpd_req_t {
  std::string json;
  int sends = 0;
};
inline esp_err_t http_send_json(httpd_req_t* req, const char* json) {
  req->json = json;
  req->sends++;
  return ESP_OK;
}

static const char* FIRMWARE_VERSION = "2.4.16-wap";
static const char* RULESET_ID = "securacv:canary:v1.0";
static const char* PROTOCOL_VERSION = "pwk:v0.3.0";

struct {
  char device_id[32];
  uint32_t seq;
  uint8_t pubkey[32];
} g_device;
struct {
  uint32_t http_requests;
  uint32_t records_created;
} g_health;
[[maybe_unused]] static struct { int sd_state; } g_hw;   // the replaced handler named it in its refusals

static uint32_t g_now = 1234567;
inline uint32_t millis() { return g_now; }

inline void hex_to_str(char* out, const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
  out[2 * n] = '\0';
}

// The card's state as the sketch's hooks see it: sd_is_available() is the
// mount (the fake card's own flag), sd_verify_present() the probe the sketch
// runs before it trusts the mount, which a card pulled since it was mounted
// fails (g_sd_verify_ok); the counters record what was said.
static int g_sd_success = 0, g_sd_failure = 0;
static bool g_sd_verify_ok = true;
inline bool sd_is_available() { return SD.present; }
inline bool sd_verify_present() { return SD.present && g_sd_verify_ok; }
inline void sd_op_success() { g_sd_success++; }
inline void sd_op_failure() { g_sd_failure++; }
inline const char* sd_state_name(int) { return "stub"; }

enum { SCV_LOG_INFO = 1, SCV_LOG_NOTICE = 2, SCV_LOG_WARNING = 3 };
enum { SCV_CAT_USER = 1, SCV_CAT_STORAGE = 2 };
static std::vector<std::string> g_log;
inline void log_health(int, int, const char* msg, const char*) { g_log.push_back(msg); }

#include "test_export_answer.inc"

// ── the suite ────────────────────────────────────────────────────────────
static int failures = 0;
#define CHECK(cond, what)                                                  \
  do {                                                                     \
    if (!(cond)) { std::printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
  } while (0)

static void reset(bool card, const char* id = "canary-s3-4dC2") {
  SD.present = card;
  SD.files.clear();
  SD.dirs.clear();
  SD.writes = 0;
  SD.fail_writes = false;
  SD.short_write_next = 0;
  g_sd_verify_ok = true;
  g_sd_success = g_sd_failure = 0;
  g_log.clear();
  memset(&g_device, 0, sizeof g_device);
  snprintf(g_device.device_id, sizeof g_device.device_id, "%s", id);
  g_device.seq = 41;
  for (int i = 0; i < 32; i++) g_device.pubkey[i] = (uint8_t)(0xa0 + i);
  g_health.records_created = 40;
}

static const char* const BUNDLE[] = {"version", "device_id", "firmware", "ruleset",
                                     "export_time_ms", "chain_seq", "records_total", "pubkey"};

static json_strict::Value answer_of(const httpd_req_t& req) {
  json_strict::Value v;
  CHECK(req.sends == 1, "one answer sent");
  CHECK(json_strict::parse(req.json, &v), "the answer parses under JSON.parse's rules");
  return v;
}

static void check_bundle(const json_strict::Value& v) {
  for (const char* k : BUNDLE) CHECK(v.get(k) != nullptr, (std::string("the bundle carries ") + k).c_str());
  const json_strict::Value* id = v.get("device_id");
  CHECK(id && id->text == g_device.device_id, "device_id is the device's");
  const json_strict::Value* pk = v.get("pubkey");
  CHECK(pk && pk->text.size() == 64 && pk->text.compare(0, 4, "a0a1") == 0, "pubkey is the key as 64 hex");
  const json_strict::Value* seq = v.get("chain_seq");
  CHECK(seq && seq->text == std::to_string(g_device.seq), "chain_seq is the device's");
  const json_strict::Value* ok = v.get("ok");
  CHECK(ok && ok->kind == json_strict::Value::Bool && ok->b, "ok:true");
}

static void names_no_url(const httpd_req_t& req, const json_strict::Value& v) {
  CHECK(v.get("download_url") == nullptr, "no download_url");
  CHECK(req.json.find("/api/") == std::string::npos, "no /api/ path anywhere in the answer");
}

static void test_card_in() {
  std::printf("export with a card in\n");
  reset(true);
  httpd_req_t req;
  handle_export(&req);
  const json_strict::Value v = answer_of(req);
  check_bundle(v);
  names_no_url(req, v);
  const json_strict::Value* sd = v.get("sd_available");
  CHECK(sd && sd->kind == json_strict::Value::Bool && sd->b, "sd_available:true");
  const json_strict::Value* path = v.get("card_path");
  CHECK(path && path->text == "/EXPORT/bundle_1234567.json", "card_path names the copy");
  CHECK(path && SD.files.count(path->text) == 1, "the copy is on the card");
  if (path && SD.files.count(path->text)) {
    json_strict::Value file;
    CHECK(json_strict::parse(SD.files[path->text], &file), "the copy parses");
    CHECK(file.keys.size() == sizeof BUNDLE / sizeof BUNDLE[0], "the copy is the header alone");
    for (const char* k : BUNDLE) {
      const json_strict::Value* a = v.get(k);
      const json_strict::Value* b = file.get(k);
      CHECK(a && b && a->text == b->text, (std::string("the copy's ") + k + " is the answer's").c_str());
    }
  }
  CHECK(g_sd_success == 1 && g_sd_failure == 0, "one SD success counted");
}

static void test_no_card() {
  std::printf("export with no card\n");
  reset(false);
  httpd_req_t req;
  handle_export(&req);
  const json_strict::Value v = answer_of(req);
  check_bundle(v);
  names_no_url(req, v);
  const json_strict::Value* sd = v.get("sd_available");
  CHECK(sd && sd->kind == json_strict::Value::Bool && !sd->b, "sd_available:false");
  CHECK(v.get("card_path") == nullptr, "no card_path without a card");
  CHECK(SD.writes == 0 && SD.files.empty(), "nothing written");
  CHECK(g_sd_success == 0 && g_sd_failure == 0, "no SD operation counted");
}

static void test_card_refuses() {
  std::printf("export with a card that takes nothing\n");
  reset(true);
  SD.fail_writes = true;
  httpd_req_t req;
  handle_export(&req);
  const json_strict::Value v = answer_of(req);
  check_bundle(v);
  names_no_url(req, v);
  CHECK(v.get("card_path") == nullptr, "no copy is named");
  CHECK(SD.files.empty(), "no torn file is left on the card");
  CHECK(g_sd_failure == 1 && g_sd_success == 0, "the SD failure is counted");
}

static void test_card_cuts_the_write_short() {
  std::printf("export with a card that takes part of the write\n");
  reset(true);
  SD.short_write_next = 10;   // the copy stops ten bytes in
  httpd_req_t req;
  handle_export(&req);
  const json_strict::Value v = answer_of(req);
  check_bundle(v);
  names_no_url(req, v);
  CHECK(v.get("card_path") == nullptr, "a torn copy is not named");
  CHECK(SD.files.empty(), "and is not left on the card");
  CHECK(g_sd_failure == 1 && g_sd_success == 0, "the SD failure is counted");
}

static void test_card_gone_at_verify() {
  std::printf("export with a card mounted but gone when verified\n");
  reset(true);
  g_sd_verify_ok = false;
  httpd_req_t req;
  handle_export(&req);
  const json_strict::Value v = answer_of(req);
  check_bundle(v);
  names_no_url(req, v);
  const json_strict::Value* sd = v.get("sd_available");
  CHECK(sd && sd->kind == json_strict::Value::Bool && !sd->b, "sd_available:false");
  CHECK(v.get("card_path") == nullptr, "no card_path");
  CHECK(SD.writes == 0 && SD.files.empty(), "no write is tried");
  CHECK(g_sd_success == 0 && g_sd_failure == 0, "no SD operation counted");
}

static void test_longest_answer() {
  std::printf("the longest answer\n");
  reset(true, "abcdefghijklmnopqrstuvwxyz01234");   // 31 characters, the buffer's most
  g_device.seq = 0xFFFFFFFFu;
  g_health.records_created = 0xFFFFFFFFu;
  g_now = 0xFFFFFFFFu;
  httpd_req_t req;
  handle_export(&req);
  const json_strict::Value v = answer_of(req);
  check_bundle(v);
  CHECK(v.get("card_path") != nullptr, "the copy is named");
  std::printf("  %zu bytes\n", req.json.size());
  CHECK(req.json.size() < 512, "a few hundred bytes: the handler holds it whole");
  g_now = 1234567;
}

int main() {
  (void)Serial;   // stubs/sd_fake's; the handler logs through log_health
  test_card_in();
  test_no_card();
  test_card_refuses();
  test_card_cuts_the_write_short();
  test_card_gone_at_verify();
  test_longest_answer();
  if (failures) {
    std::printf("%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("ALL export answer tests PASSED\n");
  return 0;
}
