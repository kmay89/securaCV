// wap_json_writer.h's bounds (sweep F230), through the REAL header and the
// REAL identity_json.h and fleet_scan_cache.h built on it.
//
// escaped() measured its string, checked the room for that length, and then
// copied up to the string's NUL: a string another task grew between the
// measure and the copy was written past the room checked for it, and an
// identity answer's buffer is sized to exactly the measured length (the
// loop task writes g_wifi_status.ap_auth while the HTTP task builds GET
// /api/device-info). room() computed `cap - 1 - reserve - len`, which wraps
// once len passes it, so after one such overrun every later write read as
// fitting. This suite grows strings at the two places a live global can
// change under a build, through test hooks the header compiles only here:
// inside a write, between escaped()'s measure and its copy (and between
// str()'s measure and escaped()'s), and between identity_json.h's measuring
// pass and its writing pass. Every buffer is a heap block of exactly the
// size the builder was given, and the suite runs under AddressSanitizer
// (the Makefile builds it with -fsanitize=address), so a byte written past
// it aborts the run; each buffer is also checked against a guard copy, so a
// build without the sanitizer still fails on an overrun.
//
// Against the writer before F230 the escaped() cases abort under ASan with a
// heap-buffer-overflow (without ASan, the guard checks fail), the room() case
// reads a length past the cap as room, and the identity build answers a
// refused write pass with its measured length instead of a refusal.
//
// And replaced_text() (sweep F262), the replacement the fleet scan cache's
// writer spells \ufffd written into a plain string for ArduinoJson or a
// printf: every byte value at three places, the 32-byte SSID of 0x01 that
// needs 96 bytes, 400 seeded SSIDs, and every buffer size for strings of
// mixed UTF-8, each an exact heap block. Taking every byte below 0x20 (not
// raw_control()) fails 245 checks, cutting by bytes rather than characters
// 20, and testing the room one byte too late aborts under ASan with a
// heap-buffer-overflow.
//
// And shows_as() and listed_by_shown(), that text read backwards (the F262
// review): what a page posts back for a listed SSID, after a browser decoded
// the answer (browser_utf8(), the WHATWG decoder, checked against Node's
// TextDecoder), names the listed network, which is joined by its own bytes.
// Reading the post byte for byte, as replaced_text() alone writes it, fails
// 1846 checks (every name a browser shows a byte of as U+FFFD), and each of
// the decoder's four range bounds dropped fails 41 to 85.

// Arduino.h's global names first, as in the sketch's translation unit.
#include "arduino_globals.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The hooks: each write calls this after it measured `s` and before it
// copies it; identity_json::build() calls the second between its passes.
static void test_after_measure(const char* s);
static void test_between_passes();
#define WAP_JSON_TEST_HOOK_AFTER_MEASURE(s) test_after_measure(s)
#define IDENTITY_JSON_TEST_HOOK_BETWEEN_PASSES() test_between_passes()

#include "fleet_scan_cache.h"
#include "identity_json.h"
#include "json_strict.h"
#include "wap_json_writer.h"

namespace {

int g_checks = 0;
int g_failures = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      ++g_failures;                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
    }                                                                     \
  } while (0)

// A string a "loop task" rewrites: the hook copies `to` over it on the
// `fire`-th time a write calls the hook with it, once. A str() calls it
// after its own measure (1) and then its escaped() after that one's (2); an
// escaped() called directly calls it once (1). A measuring writer never
// calls it.
struct Grow {
  char*       target = nullptr;
  const char* to = nullptr;
  int         fire = 0;
  int         seen = 0;
  bool        fired = false;
};
Grow g_after;     // in a write, after its measure
Grow g_between;   // between identity_json's passes

void arm(Grow& g, char* target, const char* to, int fire = 1) {
  g.target = target;
  g.to = to;
  g.fire = fire;
  g.seen = 0;
  g.fired = false;
}

void disarm() {
  g_after = Grow();
  g_between = Grow();
}

// A heap block of exactly `cap` bytes, for the writer, beside a guard copy
// of what the block's neighbors would be: an overrun is an ASan abort, and
// without ASan the bytes past `cap` in `shadow` (written the same way) show it.
struct Exact {
  char* buf;
  size_t cap;
  explicit Exact(size_t n) : buf(static_cast<char*>(std::malloc(n))), cap(n) {
    std::memset(buf, 0x7f, n);
  }
  ~Exact() { std::free(buf); }
  Exact(const Exact&) = delete;
  Exact& operator=(const Exact&) = delete;
};

// The same writes into a block with 64 guard bytes behind `cap`: true when
// none of them changed.
const unsigned char kGuard = 0xA5;
struct Guarded {
  char block[512];
  size_t cap;
  explicit Guarded(size_t n) : cap(n) { std::memset(block, kGuard, sizeof(block)); }
  bool intact() const {
    for (size_t i = cap; i < sizeof(block); ++i)
      if ((unsigned char)block[i] != kGuard) return false;
    return true;
  }
};

}  // namespace

static void fire_if(Grow& g, const char* s) {
  if (g.target == nullptr || g.fired || s != g.target) return;
  if (++g.seen < g.fire) return;
  std::strcpy(g.target, g.to);
  g.fired = true;
}

static void test_after_measure(const char* s) { fire_if(g_after, s); }

static void test_between_passes() {
  if (g_between.target == nullptr || g_between.fired) return;
  std::strcpy(g_between.target, g_between.to);
  g_between.fired = true;
}

namespace {

// room() reads a length past what it may use as no room: it never wraps.
void test_room_never_wraps() {
  char buf[16];
  wap_json::Writer w;
  wap_json::begin(w, buf, sizeof(buf));
  // Exactly full: 15 bytes and the NUL.
  w.len = 15;
  CHECK(wap_json::room(w, 0));
  CHECK(!wap_json::room(w, 1));
  // A length past the cap (what an overrun left behind): no room at all.
  // `cap - 1 - reserve - len` wraps to SIZE_MAX here and read as room.
  w.len = 20;
  CHECK(!wap_json::room(w, 0));
  CHECK(!wap_json::room(w, 1));
  CHECK(!wap_json::room(w, 1000));
  // The same past the reserve: 16 - 1 - 2 = 13 bytes may be used.
  wap_json::begin(w, buf, sizeof(buf), 2);
  w.len = 13;
  CHECK(wap_json::room(w, 0));
  CHECK(!wap_json::room(w, 1));
  w.len = 14;
  CHECK(!wap_json::room(w, 0));
  CHECK(!wap_json::room(w, SIZE_MAX));
  // A reserve that leaves nothing, and a reserve past the cap.
  wap_json::begin(w, buf, sizeof(buf), 15);
  CHECK(wap_json::room(w, 0));
  CHECK(!wap_json::room(w, 1));
  wap_json::begin(w, buf, sizeof(buf), 40);
  CHECK(!wap_json::room(w, 0));
  // A measuring writer has room for anything that adds up.
  wap_json::begin_measure(w);
  w.len = SIZE_MAX - 10;
  CHECK(wap_json::room(w, 9));
  CHECK(!wap_json::room(w, 10));
  CHECK(!wap_json::room(w, SIZE_MAX));
}

// A string that grows between escaped()'s measure and its copy, written into
// exactly the room measured for it: refused whole, nothing past the buffer.
void test_escaped_stops_at_the_room_it_checked() {
  struct Case {
    const char* before;
    const char* after;
  };
  // Longer, the same length but escaping wider (a quote: 1 -> 2 bytes; a
  // control byte: 1 -> 6), and much longer than the buffer.
  const Case cases[] = {
      {"WPA2", "WPA2_WPA3_PSK_MIXED_ENTERPRISE"},
      {"abcd", "ab\"d"},
      {"abcd", "ab\x01" "d"},
      {"open", "\x01\x02\x03\x04"},
      {"x", "this label grew a hundred bytes longer than the one measured, past every byte of room"},
  };
  for (const Case& c : cases) {
    // The room escaped() needs for `before`, its NUL, and nothing more.
    const size_t n = wap_json::string_len(c.before) - 2;
    {
      char target[160];
      std::strcpy(target, c.before);
      Exact e(n + 1);
      wap_json::Writer w;
      wap_json::begin(w, e.buf, e.cap);
      arm(g_after, target, c.after);
      const bool wrote = wap_json::escaped(w, target);
      CHECK(g_after.fired);
      CHECK(!wrote);
      CHECK(!wap_json::ok(w));
      CHECK(w.len == 0);
      CHECK(e.buf[0] == '\0');
      disarm();
    }
    {
      char target[160];
      std::strcpy(target, c.before);
      Guarded g(n + 1);
      wap_json::Writer w;
      wap_json::begin(w, g.block, g.cap);
      arm(g_after, target, c.after);
      CHECK(!wap_json::escaped(w, target));
      CHECK(g.intact());
      CHECK(g.block[0] == '\0');
      disarm();
    }
  }
  // A string that shrinks, or keeps its bytes, is written whole: the bound
  // refuses only what was not measured.
  {
    char target[32];
    std::strcpy(target, "WPA2_WPA3");
    Exact e(wap_json::string_len(target) - 2 + 1);
    wap_json::Writer w;
    wap_json::begin(w, e.buf, e.cap);
    arm(g_after, target, "WPA2");
    CHECK(wap_json::escaped(w, target));
    CHECK(wap_json::ok(w));
    CHECK(std::strcmp(e.buf, "WPA2") == 0);
    CHECK(w.len == 4);
    disarm();
  }
  {
    char target[32];
    std::strcpy(target, "a\"b\x01");
    const size_t n = wap_json::string_len(target) - 2;
    Exact e(n + 1);
    wap_json::Writer w;
    wap_json::begin(w, e.buf, e.cap);
    CHECK(wap_json::escaped(w, target));
    CHECK(std::strcmp(e.buf, "a\\\"b\\u0001") == 0);
    CHECK(w.len == n);
  }
  // After a refused copy every later write is refused too (overflow sticks),
  // and the text before it is kept, terminated.
  {
    char target[64];
    std::strcpy(target, "ab");
    Exact e(8);
    wap_json::Writer w;
    wap_json::begin(w, e.buf, e.cap);
    CHECK(wap_json::raw(w, "{\""));
    arm(g_after, target, "abcdefghijklmnop");
    CHECK(!wap_json::escaped(w, target));
    CHECK(std::strcmp(e.buf, "{\"") == 0);
    CHECK(w.len == 2);
    CHECK(!wap_json::raw(w, "x"));
    CHECK(std::strcmp(e.buf, "{\"") == 0);
    disarm();
  }
}

// str() is whole or nothing: a string that grows between str()'s measure and
// escaped()'s, or inside escaped(), leaves neither quote behind.
void test_str_is_whole_or_nothing() {
  for (int fire = 1; fire <= 2; ++fire) {   // 1: before escaped()'s measure, 2: after it
    char target[64];
    std::strcpy(target, "wpa2");
    const size_t prefix = 3;                 // `{"a` written before it
    Exact e(prefix + wap_json::string_len(target) + 1);
    wap_json::Writer w;
    wap_json::begin(w, e.buf, e.cap);
    CHECK(wap_json::raw(w, "{\"a"));
    arm(g_after, target, "wpa2-wpa3-mixed", fire);
    CHECK(!wap_json::str(w, target));
    CHECK(g_after.fired);
    CHECK(w.len == prefix);
    CHECK(std::strcmp(e.buf, "{\"a") == 0);
    CHECK(!wap_json::ok(w));
    disarm();
  }
}

identity_json::DeviceInfo info_with(const char* ap_auth) {
  identity_json::DeviceInfo in = {};
  in.device_id = "canary-s3-4dC2";
  in.device_name = "kitchen";
  in.mdns_host = "canary-kitchen";
  in.firmware = "2.4.1";
  in.pubkey_fp = "a1b2c3d4e5f60718";
  in.hw_token = "0f1e2d3c4b5a6978";
  in.uptime_ms = 4242;
  in.chain_length = 7;
  in.born_day = 20345;
  in.born_exact = true;
  in.tls_enabled = false;
  in.ap_auth = ap_auth;
  return in;
}

// What handle_device_info() does, with g_wifi_status.ap_auth growing under
// it: measured, an exactly sized heap buffer, built. The build is refused
// (a return of the buffer's size or more, an empty text) and nothing is
// written past the buffer, whether the label grows between the two passes or
// inside the write of it.
void test_device_info_refuses_a_label_that_grew() {
  const char* grown = "WPA2_WPA3_PSK_MIXED_\"ENTERPRISE\"_\x01";
  // 0: between the passes; 1: in the write pass, between str()'s measure and
  // escaped()'s; 2: inside escaped(), between its measure and its copy (the
  // case the writer before F230 wrote past the buffer).
  for (int where = 0; where < 3; ++where) {
    char ap_auth[64];
    std::strcpy(ap_auth, "WPA2");
    const identity_json::DeviceInfo in = info_with(ap_auth);
    const size_t need = identity_json::device_info(in, nullptr, 0) + 1;
    Exact e(need);
    if (where == 0) {
      arm(g_between, ap_auth, grown);
    } else {
      arm(g_after, ap_auth, grown, where);
    }
    const size_t n = identity_json::device_info(in, e.buf, need);
    CHECK(where == 0 ? g_between.fired : g_after.fired);
    CHECK(n >= need);                // refused: the handler answers 503
    CHECK(e.buf[0] == '\0');
    disarm();
    // With the label as it is now, the same route answers it whole.
    const size_t need2 = identity_json::device_info(in, nullptr, 0) + 1;
    CHECK(need2 > need);
    Exact e2(need2);
    CHECK(identity_json::device_info(in, e2.buf, need2) == need2 - 1);
    json_strict::Value v;
    CHECK(json_strict::parse(std::string(e2.buf), &v));
    const json_strict::Value* f = v.get("ap_auth");
    CHECK(f != nullptr && f->text == grown);
  }
  // A label that shrinks between the passes is answered, shorter.
  {
    char ap_auth[64];
    std::strcpy(ap_auth, "WPA2_WPA3_PSK");
    const identity_json::DeviceInfo in = info_with(ap_auth);
    const size_t need = identity_json::device_info(in, nullptr, 0) + 1;
    Exact e(need);
    arm(g_between, ap_auth, "WPA2");
    const size_t n = identity_json::device_info(in, e.buf, need);
    CHECK(n < need);
    CHECK(n == std::strlen(e.buf));
    json_strict::Value v;
    CHECK(json_strict::parse(std::string(e.buf), &v));
    const json_strict::Value* f = v.get("ap_auth");
    CHECK(f != nullptr && f->text == "WPA2");
    disarm();
  }
}

// The receipt's base_url writes the AP address with escaped() directly, not
// through str(): the same bound holds it.
void test_receipt_refuses_an_address_that_grew() {
  char ap_ip[64];
  std::strcpy(ap_ip, "192.168.4.1");
  identity_json::Receipt in = {};
  in.device_id = "canary-s3-4dC2";
  in.tls_enabled = true;
  in.ap_ip = ap_ip;
  in.token = "cv_0123456789abcdefABCDEF0123456789";
  in.pubkey_fp = "a1b2c3d4e5f60718";
  in.firmware = "2.4.1";
  in.hw_token = "0f1e2d3c4b5a6978";
  in.ap_ssid = "SecuraCV-4dC2";
  in.ap_password = "cv-abcdefghjkmn";
  in.tls_cert_fp = "";
  in.boot_count = 42;
  const size_t need = identity_json::provisioning_receipt(in, nullptr, 0) + 1;
  // tls_cert_fp is the last string: grow the address inside its own write.
  Exact e(need);
  arm(g_after, ap_ip, "192.168.104.123:8080/extra/path");
  const size_t n = identity_json::provisioning_receipt(in, e.buf, need);
  CHECK(g_after.fired);
  CHECK(n >= need);
  CHECK(e.buf[0] == '\0');
  disarm();
}

// The fleet scan's cache measures every advert, then writes the ones that
// fit: an advert value that grows inside its row's write costs that row
// only, and the cache stays a complete document within its buffer.
void test_fleet_cache_drops_a_row_that_grew() {
  char name[300];
  std::strcpy(name, "porch");
  fleet_scan_cache::Advert ads[3] = {
      {"canary-wap-1000", "hall", "canary-hall", "2.4.1", "XIAO ESP32S3", "canary-wap", "witness", "192.168.1.20", 80},
      {"canary-wap-1001", name, "canary-porch", "2.4.1", "XIAO ESP32S3", "canary-wap", "witness", "192.168.1.21", 80},
      {"canary-wap-1002", "den", "canary-den", "2.4.1", "XIAO ESP32S3", "canary-wap", "witness", "192.168.1.22", 80},
  };
  // A cache with room for the three rows as measured, and not much more.
  size_t rows = 0;
  for (const auto& a : ads) rows += fleet_scan_cache::row_len(a);
  const size_t cap = std::strlen("{\"canaries\":[") + rows + 2 + 2 + 1;
  Exact e(cap);
  fleet_scan_cache::Cache c;
  fleet_scan_cache::begin(c, e.buf, cap);
  std::string grown(250, 'q');
  grown[10] = '\x02';
  // fill() measures every row first (a measuring writer, no hook), then
  // writes the kept ones: the name's str() calls the hook once after its
  // measure and its escaped() once more, between its measure and its copy.
  // The second is where the writer before F230 copied past the cache.
  arm(g_after, name, grown.c_str(), 2);
  const size_t kept = fleet_scan_cache::fill(c, 3, [&](int i) { return ads[i]; });
  CHECK(g_after.fired);
  CHECK(kept == 2);
  CHECK(c.skipped == 1);
  json_strict::Value v;
  CHECK(json_strict::parse(std::string(e.buf), &v));
  const json_strict::Value* list = v.get("canaries");
  CHECK(list != nullptr && list->items.size() == 2);
  if (list != nullptr && list->items.size() == 2) {
    const json_strict::Value* n0 = list->items[0].get("name");
    const json_strict::Value* n1 = list->items[1].get("name");
    CHECK(n0 != nullptr && n0->text == "hall");
    CHECK(n1 != nullptr && n1->text == "den");
  }
  disarm();
}

// ── replaced_text(): an SSID's control bytes as U+FFFD (sweep F262) ─────

// ArduinoJson 7.4.1's spelling of a string value (TextFormatter::writeChar
// over EscapeSequence's table): `"` `\` \b \f \n \r \t escaped, every other
// byte as it is, a control byte included.
std::string arduinojson_spelling(const std::string& v) {
  std::string out = "\"";
  for (char c : v) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out + "\"";
}

// True when `{"ssid":<value>}`, as ArduinoJson writes it, parses as a
// browser's JSON.parse reads it, and gives `want` back.
bool answer_parses_as(const std::string& value, const std::string& want) {
  json_strict::Value v;
  if (!json_strict::parse("{\"ssid\":" + arduinojson_spelling(value) + "}", &v)) return false;
  const json_strict::Value* got = v.get("ssid");
  return got != nullptr && got->kind == json_strict::Value::String && got->text == want;
}

// The test's own reading of `s` (independent of the header): its
// characters, each with the text replaced_text() owes for it.
struct Piece {
  std::string text;
};
std::vector<Piece> pieces_of(const std::string& s) {
  std::vector<Piece> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    size_t want = 1;
    if (c >= 0xC0 && c <= 0xDF) want = 2;
    else if (c >= 0xE0 && c <= 0xEF) want = 3;
    else if (c >= 0xF0 && c <= 0xF7) want = 4;
    size_t n = 1;
    while (n < want && i + n < s.size() && ((unsigned char)s[i + n] & 0xC0) == 0x80) ++n;
    const bool control = c < 0x20 && c != '\b' && c != '\f' && c != '\n' && c != '\r' && c != '\t';
    out.push_back({control ? std::string("\xEF\xBF\xBD") : s.substr(i, n)});
    i += n;
  }
  return out;
}
std::string expected_text(const std::string& s) {
  std::string out;
  for (const Piece& p : pieces_of(s)) out += p.text;
  return out;
}

// The predicate all three spellings read: a control byte other than \b \f
// \n \r \t, and the F211 writer (replace_controls) writes \ufffd for exactly
// those bytes.
void test_raw_control_is_the_writers_own_test() {
  for (int b = 1; b < 256; ++b) {
    const unsigned char c = (unsigned char)b;
    const bool control = c < 0x20 && c != '\b' && c != '\f' && c != '\n' && c != '\r' && c != '\t';
    CHECK(wap_json::raw_control(c) == control);
    char one[2] = {(char)c, 0};
    char out[16];
    wap_json::Writer w;
    wap_json::begin(w, out, sizeof(out));
    w.replace_controls = true;
    CHECK(wap_json::str(w, one));
    CHECK((std::strcmp(out, "\"\\ufffd\"") == 0) == control);
  }
}

// Every byte value at three places in an SSID: each raw control byte comes
// back as U+FFFD and every other byte as it is, whole in replaced_cap() of
// the input's length, and the answer ArduinoJson writes from it parses with
// that text; the input itself, written raw, does not parse wherever it held
// a raw control byte (so the parse is not vacuous).
void test_replaced_text_writes_each_control_byte_as_u_fffd() {
  for (int b = 1; b < 256; ++b) {
    for (int at = 0; at < 3; ++at) {
      std::string s = "Shed";
      if (at == 0) s.insert(s.begin(), (char)b);
      else if (at == 1) s.insert(2, 1, (char)b);
      else s.push_back((char)b);
      const std::string want = expected_text(s);
      Exact e(wap_json::replaced_cap(s.size()));
      CHECK(wap_json::replaced_text(e.buf, e.cap, s.c_str()));
      CHECK(std::string(e.buf) == want);
      CHECK(answer_parses_as(e.buf, want));
      CHECK(answer_parses_as(s, s) == !wap_json::raw_control((unsigned char)b));
    }
  }
}

// The longest SSID an access point can broadcast, 32 bytes of 0x01: 96
// bytes of text, which replaced_cap(32) holds whole; one byte less cuts it
// to 31 whole replacements (never two bytes of the 32nd). And 400 seeded
// SSIDs of 1 to 32 arbitrary bytes, each whole in replaced_cap(32).
void test_replaced_text_keeps_a_32_byte_ssid_whole() {
  const std::string ssid(32, '\x01');
  CHECK(wap_json::replaced_cap(32) == 97);
  {
    Exact e(wap_json::replaced_cap(32));
    CHECK(wap_json::replaced_text(e.buf, e.cap, ssid.c_str()));
    CHECK(std::strlen(e.buf) == 96 && std::string(e.buf) == expected_text(ssid));
  }
  {
    Exact e(96);
    CHECK(!wap_json::replaced_text(e.buf, e.cap, ssid.c_str()));
    CHECK(std::strlen(e.buf) == 93 && std::string(e.buf) == expected_text(std::string(31, '\x01')));
  }
  uint32_t seed = 0x5eed1234u;
  for (int k = 0; k < 400; ++k) {
    seed = seed * 1103515245u + 12345u;
    std::string s;
    const size_t n = 1 + (seed >> 16) % 32;
    for (size_t i = 0; i < n; ++i) {
      seed = seed * 1103515245u + 12345u;
      const char c = (char)(1 + (seed >> 16) % 255);
      s.push_back(c);
    }
    Exact e(wap_json::replaced_cap(32));
    CHECK(wap_json::replaced_text(e.buf, e.cap, s.c_str()));
    CHECK(std::string(e.buf) == expected_text(s));
    CHECK(answer_parses_as(e.buf, expected_text(s)));
  }
}

// A text that does not fit is cut before the first character that does not
// fit whole: for strings mixing ASCII, raw control bytes, \t, two-, three-
// and four-byte UTF-8, a stray continuation byte, a lead byte past 0xF7 and
// a sequence cut short in the input, at every buffer size from 1 to one past
// whole (each an exact heap block, under AddressSanitizer), the text is the
// longest run of whole characters that fits, terminated, and the answer is
// true only when nothing was left out.
void test_replaced_text_cuts_on_a_character_boundary() {
  const std::string inputs[] = {
      std::string("caf\xC3\xA9\x01" "x\xE2\x82\xAC" "\x02\xF0\x9F\x90\xA6" "\t!"),
      std::string("\x80\x01\xC3\xA9\xF8\xFF\x1F"),
      std::string("ab\xE2\x82" "c\x01\xF0\x9F\x90" "d"),
      std::string(31, '\x01') + "\xC3\xA9",
      std::string("\xF0\x9F\x90\xA6\xF0\x9F\x90\xA6\x01\x01\xF0\x9F\x90\xA6"),
  };
  for (const std::string& s : inputs) {
    const std::vector<Piece> pieces = pieces_of(s);
    const std::string whole = expected_text(s);
    for (size_t cap = 1; cap <= whole.size() + 2; ++cap) {
      std::string fits;
      for (const Piece& p : pieces) {
        if (fits.size() + p.text.size() > cap - 1) break;
        fits += p.text;
      }
      Exact e(cap);
      const bool all = wap_json::replaced_text(e.buf, e.cap, s.c_str());
      CHECK(all == (whole.size() <= cap - 1));
      CHECK(std::string(e.buf) == fits);
    }
  }
  // Nothing to write into, and no string at all.
  char one[1] = {'x'};
  CHECK(!wap_json::replaced_text(one, 0, "a") && one[0] == 'x');
  CHECK(!wap_json::replaced_text(nullptr, 8, "a"));
  CHECK(!wap_json::replaced_text(one, 1, "a") && one[0] == '\0');
  char four[4] = {'x', 'x', 'x', 'x'};
  CHECK(wap_json::replaced_text(four, sizeof(four), nullptr) && four[0] == '\0');
}

// The list's text read backwards (the F262 review). GET /api/wifi/scan,
// GET /api/wifi and the BLE provisioning list show a network's control
// bytes as U+FFFD, a browser shows any byte that is not UTF-8 the same way,
// and the wizard, the dashboard and the phone post back the text they were
// shown (the answer decoded, JSON.parse, JSON.stringify of the same string,
// encoded as UTF-8 again): that text names the listed network, which is
// joined by its own bytes.
//
// What a browser's UTF-8 decoder reads (the WHATWG Encoding Standard's
// "UTF-8 decoder", which fetch().json() and TextDecoder run), written back
// out as UTF-8: each ill-formed sequence's maximal subpart as U+FFFD.
std::string browser_utf8(const std::string& in) {
  std::string out;
  uint32_t cp = 0;
  int needed = 0, seen = 0;
  unsigned lower = 0x80, upper = 0xBF;
  auto put = [&](uint32_t c) {
    if (c < 0x80) {
      out.push_back((char)c);
    } else if (c < 0x800) {
      out.push_back((char)(0xC0 | (c >> 6)));
      out.push_back((char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out.push_back((char)(0xE0 | (c >> 12)));
      out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (c & 0x3F)));
    } else {
      out.push_back((char)(0xF0 | (c >> 18)));
      out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
      out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (c & 0x3F)));
    }
  };
  for (size_t i = 0; i < in.size();) {
    const unsigned b = (unsigned char)in[i];
    if (needed == 0) {
      ++i;
      if (b <= 0x7F) { put(b); }
      else if (b >= 0xC2 && b <= 0xDF) { needed = 1; cp = b & 0x1F; }
      else if (b >= 0xE0 && b <= 0xEF) {
        if (b == 0xE0) lower = 0xA0;
        if (b == 0xED) upper = 0x9F;
        needed = 2; cp = b & 0xF;
      } else if (b >= 0xF0 && b <= 0xF4) {
        if (b == 0xF0) lower = 0x90;
        if (b == 0xF4) upper = 0x8F;
        needed = 3; cp = b & 0x7;
      } else { put(0xFFFD); }
      continue;
    }
    if (b < lower || b > upper) {        // the byte is read again (not consumed)
      cp = 0; needed = 0; seen = 0; lower = 0x80; upper = 0xBF;
      put(0xFFFD);
      continue;
    }
    ++i;
    lower = 0x80; upper = 0xBF;
    cp = (cp << 6) | (b & 0x3F);
    if (++seen == needed) {
      put(cp);
      cp = 0; needed = 0; seen = 0;
    }
  }
  if (needed != 0) put(0xFFFD);          // the stream ended inside a sequence
  return out;
}

// What a page posts back for the SSID `raw` it was shown: replaced_text()'s
// whole text, written by ArduinoJson into `{"ssid":...}`, the answer's
// bytes decoded as a browser decodes them (fetch().json(), a TextDecoder),
// JSON.parse, and the string encoded as UTF-8 again (JSON.stringify writes
// \b \f \n \r \t as escapes ArduinoJson reads back to the bytes, and every
// other character as it is). browser_utf8() above was checked against
// Node 22's TextDecoder on 200 000 seeded byte strings, every one equal.
std::string posted_back(const std::string& raw) {
  Exact e(wap_json::replaced_cap(raw.size()));
  CHECK(wap_json::replaced_text(e.buf, e.cap, raw.c_str()));
  json_strict::Value v;
  if (!json_strict::parse(browser_utf8("{\"ssid\":" + arduinojson_spelling(e.buf) + "}"), &v)) {
    return std::string("\x01unparsed");
  }
  const json_strict::Value* got = v.get("ssid");
  return got != nullptr && got->kind == json_strict::Value::String ? got->text : std::string("\x01none");
}

// True when no byte of `s` is a character a browser reads as U+FFFD or
// replaced_text() writes as one: the post is then the name's own bytes.
bool posts_itself(const std::string& s) {
  return browser_utf8(expected_text(s)) == s;
}

// Every byte value at three places in a name comes back to the network's
// bytes: a control byte replaced_text() wrote as U+FFFD, a byte a browser
// reads as U+FFFD (0x80 to 0xFF alone are never UTF-8), and every other byte
// as it is; the 32-byte name of 0x01 (96 bytes of text, past the 32 a join
// takes) comes back to its 32 bytes; and 400 seeded names of 1 to 32
// arbitrary bytes, and 400 more drawn from the bytes UTF-8 turns on (lead
// bytes, continuation bytes, the overlong, surrogate and past-U+10FFFF
// bounds), are each found among the others. Without listed_by_shown() the
// post is the text itself, which no listed network has.
void test_a_listed_name_is_read_back_to_its_own_bytes() {
  for (int b = 1; b < 256; ++b) {
    for (int at = 0; at < 3; ++at) {
      std::string raw = "kitchen";
      if (at == 0) raw.insert(raw.begin(), (char)b);
      else if (at == 1) raw.insert(3, 1, (char)b);
      else raw.push_back((char)b);
      const std::vector<std::string> list = {"Shed", raw, "Attic"};
      const std::string posted = posted_back(raw);
      const int i = wap_json::listed_by_shown(posted.c_str(), (int)list.size(),
                                              [&](int k) { return list[k].c_str(); });
      CHECK(i == 1);
      CHECK(wap_json::shows_as(raw.c_str(), posted.c_str()));
      // The post differs from the network's bytes exactly when the list or
      // the browser replaced one.
      CHECK((posted != raw) == (wap_json::raw_control((unsigned char)b) || b >= 0x80));
      CHECK((posted != raw) == !posts_itself(raw));
    }
  }
  const std::string ones(32, '\x01');
  const std::vector<std::string> list = {ones};
  const std::string posted = posted_back(ones);
  CHECK(posted.size() == 96);
  CHECK(wap_json::listed_by_shown(posted.c_str(), 1, [&](int k) { return list[k].c_str(); }) == 0);
  // A name in Latin-1 (a browser shows its 0xE9 as U+FFFD) and one with a
  // lead byte cut short of its sequence, each with a control byte too.
  for (const std::string& raw : {std::string("Caf\xE9\x01"), std::string("\xE2\x82\x01" "x"),
                                std::string("\xF0\x9F\x98\x80\x1F"), std::string("\xED\xA0\x80")}) {
    const std::vector<std::string> one = {"Shed", raw};
    const std::string p = posted_back(raw);
    CHECK(p != raw);
    CHECK(wap_json::listed_by_shown(p.c_str(), 2, [&](int k) { return one[k].c_str(); }) == 1);
  }
  const unsigned char pool[] = {0x01, 0x1F, 0x09, 0x0A, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF,
                                0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1,
                                0xF3, 0xF4, 0xF5, 0xFF, 0xBD, 0x85};
  uint32_t seed = 0x0dd5eedu;
  for (int set = 0; set < 2; ++set) {
    std::vector<std::string> names;
    for (int k = 0; k < 400; ++k) {
      std::string s;
      seed = seed * 1103515245u + 12345u;
      const size_t n = 1 + (seed >> 16) % 32;
      for (size_t i = 0; i < n; ++i) {
        seed = seed * 1103515245u + 12345u;
        s.push_back(set == 0 ? (char)(1 + (seed >> 16) % 255) : (char)pool[(seed >> 16) % sizeof(pool)]);
      }
      names.push_back(s);
    }
    for (size_t k = 0; k < names.size(); ++k) {
      const std::string p = posted_back(names[k]);
      CHECK(wap_json::shows_as(names[k].c_str(), p.c_str()));
      const int i = wap_json::listed_by_shown(p.c_str(), (int)names.size(),
                                              [&](int j) { return names[j].c_str(); });
      // The first listed name a page shows as the same text: this one, unless
      // an earlier one shows the same (two names that differ only in bytes a
      // page shows as U+FFFD), or one is the text itself.
      CHECK(i >= 0 && (names[i] == p || (i <= (int)k && posted_back(names[i]) == p)));
    }
  }
}

// What a post names when the list holds more than one candidate: a name
// whose own bytes are the posted text wins (a network that holds U+FFFD
// itself), then the first listed that shows as it; a text no listed name
// shows as names none (the caller keeps it as posted), and so does a text
// that is a listed name's shown text cut short or run on.
void test_listed_by_shown_prefers_a_name_that_is_its_own_text() {
  const std::string shown = "kit\xEF\xBF\xBD" "chen";
  {
    const std::vector<std::string> list = {"kit\x01" "chen", shown, "kit\x02" "chen"};
    CHECK(wap_json::listed_by_shown(shown.c_str(), 3, [&](int k) { return list[k].c_str(); }) == 1);
  }
  {
    const std::vector<std::string> list = {"Shed", "kit\x02" "chen", "kit\x01" "chen", "kit\xFF" "chen"};
    CHECK(wap_json::listed_by_shown(shown.c_str(), 4, [&](int k) { return list[k].c_str(); }) == 1);
  }
  {
    const std::vector<std::string> list = {"kit\x01" "chen"};
    auto at = [&](int k) { return list[k].c_str(); };
    CHECK(wap_json::listed_by_shown("kitchen", 1, at) == -1);
    CHECK(wap_json::listed_by_shown("kit\xEF\xBF\xBD" "che", 1, at) == -1);
    CHECK(wap_json::listed_by_shown("kit\xEF\xBF\xBD" "chens", 1, at) == -1);
    CHECK(wap_json::listed_by_shown("kit\xEF\xBF" "chen", 1, at) == -1);
    CHECK(wap_json::listed_by_shown("kit\x01" "chen\x01", 1, at) == -1);
    CHECK(wap_json::listed_by_shown("", 1, at) == -1);
    CHECK(wap_json::listed_by_shown(nullptr, 1, at) == -1);
    CHECK(wap_json::listed_by_shown(shown.c_str(), 0, at) == -1);
  }
  // A NULL row is skipped; \t, \n and the like are not replaced, so a name
  // holding one is its own text; a valid sequence is its own text too.
  const char* rows[] = {nullptr, "a\tb", "Caf\xC3\xA9"};
  CHECK(wap_json::listed_by_shown("a\tb", 3, [&](int k) { return rows[k]; }) == 1);
  CHECK(wap_json::listed_by_shown("Caf\xC3\xA9", 3, [&](int k) { return rows[k]; }) == 2);
  CHECK(wap_json::listed_by_shown("Caf\xEF\xBF\xBD", 3, [&](int k) { return rows[k]; }) == -1);
  CHECK(wap_json::shows_as("", "") && !wap_json::shows_as("", "x") && !wap_json::shows_as("x", ""));
  CHECK(wap_json::shows_as(nullptr, nullptr) && wap_json::shows_as("\x01", "\xEF\xBF\xBD"));
  // One U+FFFD per maximal subpart, as the WHATWG decoder reads it: E2 82
  // cut short is one, F0 alone before ASCII one, ED A0 (a surrogate's start)
  // two, and C0 AF (an overlong form) two.
  CHECK(wap_json::shows_as("\xE2\x82" "A", "\xEF\xBF\xBD" "A"));
  CHECK(!wap_json::shows_as("\xE2\x82" "A", "\xEF\xBF\xBD\xEF\xBF\xBD" "A"));
  CHECK(wap_json::shows_as("\xF0" "A", "\xEF\xBF\xBD" "A"));
  CHECK(wap_json::shows_as("\xED\xA0", "\xEF\xBF\xBD\xEF\xBF\xBD"));
  CHECK(wap_json::shows_as("\xC0\xAF", "\xEF\xBF\xBD\xEF\xBF\xBD"));
  CHECK(wap_json::shows_as("\xF4\x8F\xBF\xBF", "\xF4\x8F\xBF\xBF"));
  CHECK(wap_json::shows_as("\xF4\x90\x80\x80", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));
}

}  // namespace

int main() {
  test_room_never_wraps();
  test_escaped_stops_at_the_room_it_checked();
  test_str_is_whole_or_nothing();
  test_device_info_refuses_a_label_that_grew();
  test_receipt_refuses_an_address_that_grew();
  test_fleet_cache_drops_a_row_that_grew();
  test_raw_control_is_the_writers_own_test();
  test_replaced_text_writes_each_control_byte_as_u_fffd();
  test_replaced_text_keeps_a_32_byte_ssid_whole();
  test_replaced_text_cuts_on_a_character_boundary();
  test_a_listed_name_is_read_back_to_its_own_bytes();
  test_listed_by_shown_prefers_a_name_that_is_its_own_text();
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d wap_json_writer bound checks PASSED\n", g_checks);
  return 0;
}
