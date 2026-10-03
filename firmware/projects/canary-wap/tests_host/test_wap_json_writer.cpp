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

// Arduino.h's global names first, as in the sketch's translation unit.
#include "arduino_globals.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

}  // namespace

int main() {
  test_room_never_wraps();
  test_escaped_stops_at_the_room_it_checked();
  test_str_is_whole_or_nothing();
  test_device_info_refuses_a_label_that_grew();
  test_receipt_refuses_an_address_that_grew();
  test_fleet_cache_drops_a_row_that_grew();
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d wap_json_writer bound checks PASSED\n", g_checks);
  return 0;
}
