// The fleet scan's cache (sweep F211), through the REAL fleet_scan_cache.h the
// worker task writes it with, at the sketch's own FLEET_SCAN_CACHE_SIZE (read
// from canary_wap.ino, its path passed in, so the read fails closed).
//
// Before, fleet_scan_task() serialized every advert it browsed with
// ArduinoJson's sized form into the 2560-byte cache and, when they did not
// fit, terminated the cut text by hand: handle_fleet_scan()'s deserializeJson()
// failed on it and GET /api/fleet/scan answered an empty `canaries` list. Each
// advert carries seven TXT values of up to 255 bytes that the advertising
// device chose, and `"` or `\` cost two bytes each, so eight adverts with long
// values never fit. This suite feeds such adverts and holds the cache to what
// a reader must get: a document a strict parser (JSON.parse's rules) reads,
// the adverts that fit kept whole and in browse order, every value exactly as
// advertised, and an advert that does not fit skipped without hiding the ones
// after it.
//
// The expected rows come from an oracle written here (std::string, the JSON
// spec's escapes), not from the header: an advert is kept exactly when its
// row fits in what is left of the cache when its turn comes.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "fleet_scan_cache.h"
#include "json_strict.h"

#ifndef CANARY_WAP_INO
#error "build with -DCANARY_WAP_INO=\"<path to canary_wap.ino>\""
#endif

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

// The sketch's cache size, as canary_wap.ino declares it.
size_t sketch_cache_size() {
  std::ifstream f(CANARY_WAP_INO);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string src = ss.str();
  std::smatch m;
  static const std::regex re("static const size_t\\s+FLEET_SCAN_CACHE_SIZE\\s*=\\s*(\\d+)\\s*;");
  if (!std::regex_search(src, m, re)) return 0;
  return (size_t)std::stoul(m[1].str());
}

// One advert's values, owned (the header takes const char*).
struct Ad {
  std::string device_id, name, mdns_host, fw, model, dt, role, ip;
  uint16_t port = 80;

  fleet_scan_cache::Advert view() const {
    return {device_id.c_str(), name.c_str(), mdns_host.c_str(), fw.c_str(),
            model.c_str(), dt.c_str(), role.c_str(), ip.c_str(), port};
  }
  std::string* field(int k) {
    std::string* f[] = {&device_id, &name, &mdns_host, &fw, &model, &dt, &role};
    return f[k % 7];
  }
};

Ad ordinary(int i) {
  Ad a;
  a.device_id = "canary-wap-" + std::to_string(1000 + i);
  a.name = "room " + std::to_string(i);
  a.mdns_host = "canary-room" + std::to_string(i);
  a.fw = "2.4.1";
  a.model = "XIAO ESP32S3";
  a.dt = "canary-wap";
  a.role = "witness";
  a.ip = "192.168.1." + std::to_string(20 + i);
  a.port = 80;
  return a;
}

// 255 bytes: `quotes` bytes alternating `"` and `\`, then letters.
std::string long_value(size_t quotes, char fill = 'a') {
  std::string v;
  for (size_t i = 0; i < 255; ++i) v.push_back(i < quotes ? (i % 2 ? '\\' : '"') : fill);
  return v;
}

// ── The oracle ──────────────────────────────────────────────────────────
std::string oracle_str(const std::string& s) {
  std::string o = "\"";
  char buf[8];
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\b': o += "\\b"; break;
      case '\f': o += "\\f"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o.push_back((char)c);
        }
    }
  }
  return o + "\"";
}

std::string oracle_row(const Ad& a) {
  return "{\"device_id\":" + oracle_str(a.device_id) + ",\"name\":" + oracle_str(a.name) +
         ",\"mdns_host\":" + oracle_str(a.mdns_host) + ",\"fw\":" + oracle_str(a.fw) +
         ",\"model\":" + oracle_str(a.model) + ",\"dt\":" + oracle_str(a.dt) +
         ",\"role\":" + oracle_str(a.role) + ",\"ip\":" + oracle_str(a.ip) +
         ",\"port\":" + std::to_string(a.port) + "}";
}

// The cache a reader must get for `ads` in `cap` bytes, and which were kept.
std::string oracle_cache(const std::vector<Ad>& ads, size_t cap, std::vector<size_t>* kept) {
  std::string rows;
  size_t n = 0;
  for (size_t i = 0; i < ads.size() && n < fleet_scan_cache::kMaxAdverts; ++i) {
    const std::string row = (n ? "," : "") + oracle_row(ads[i]);
    const std::string doc = "{\"canaries\":[" + rows + row + "]}";
    if (doc.size() + 1 <= cap) {   // the text and its NUL
      rows += row;
      ++n;
      if (kept) kept->push_back(i);
    }
  }
  return "{\"canaries\":[" + rows + "]}";
}

// ── Running the header ──────────────────────────────────────────────────
struct Built {
  std::string text;
  size_t kept = 0, skipped = 0;
  bool guard_intact = true;   // nothing written past cap
};

Built build(const std::vector<Ad>& ads, size_t cap) {
  std::vector<char> buf(cap + 16, '\x7f');
  fleet_scan_cache::Cache c;
  fleet_scan_cache::begin(c, buf.data(), cap);
  for (const Ad& a : ads) {
    if (fleet_scan_cache::full(c)) break;   // the task's loop condition
    fleet_scan_cache::add(c, a.view());
  }
  Built b;
  for (size_t i = cap; i < buf.size(); ++i) b.guard_intact = b.guard_intact && buf[i] == '\x7f';
  if (cap > 0) b.text = std::string(buf.data(), strnlen(buf.data(), cap));
  b.kept = c.kept;
  b.skipped = c.skipped;
  return b;
}

// The rows a strict reader gets, each compared with the advert it came from.
bool rows_match(const std::string& text, const std::vector<Ad>& ads, const std::vector<size_t>& want) {
  json_strict::Value v;
  if (!json_strict::parse(text, &v)) return false;
  const json_strict::Value* list = v.get("canaries");
  if (v.kind != json_strict::Value::Object || v.keys.size() != 1 || !list ||
      list->kind != json_strict::Value::Array || list->items.size() != want.size())
    return false;
  for (size_t r = 0; r < want.size(); ++r) {
    const Ad& a = ads[want[r]];
    const json_strict::Value& row = list->items[r];
    const char* names[] = {"device_id", "name", "mdns_host", "fw", "model", "dt", "role", "ip"};
    const std::string* vals[] = {&a.device_id, &a.name, &a.mdns_host, &a.fw, &a.model, &a.dt, &a.role, &a.ip};
    if (row.keys.size() != 9) return false;
    for (int k = 0; k < 8; ++k) {
      const json_strict::Value* f = row.get(names[k]);
      if (!f || f->kind != json_strict::Value::String || f->text != *vals[k]) return false;
    }
    const json_strict::Value* port = row.get("port");
    if (!port || port->kind != json_strict::Value::Number || port->text != std::to_string(a.port)) return false;
  }
  return true;
}

const size_t kCap = sketch_cache_size();

// The item's case: eight adverts whose 255-byte values hold `"` and `\`.
// Each carries one such value (a different TXT key each time) with a
// different share of escaping bytes, so the cache fills part way, an advert
// too long for what is left is skipped, and a shorter one after it still fits.
void test_eight_adverts_with_long_escaping_values() {
  const size_t quotes[8] = {255, 200, 160, 255, 40, 255, 10, 120};
  std::vector<Ad> ads;
  for (int i = 0; i < 8; ++i) {
    Ad a = ordinary(i);
    *a.field(i) = long_value(quotes[i]);
    ads.push_back(a);
  }
  std::vector<size_t> want;
  const std::string expect = oracle_cache(ads, kCap, &want);
  // The case discriminates: some kept, some skipped, and one kept after a skip.
  CHECK(!want.empty() && want.size() < ads.size());
  bool kept_after_skip = false;
  for (size_t r = 1; r < want.size(); ++r) kept_after_skip = kept_after_skip || want[r] != want[r - 1] + 1;
  CHECK(kept_after_skip);
  // As the oracle computes it at 2560 bytes: the first three rows (670, 624
  // and 578 bytes), not the fourth (681 with its comma, 670 left), the fifth
  // (458), and none after it (211 bytes left).
  CHECK((want == std::vector<size_t>{0, 1, 2, 4}));

  const Built b = build(ads, kCap);
  CHECK(b.guard_intact);
  CHECK(b.text == expect);
  CHECK(rows_match(b.text, ads, want));
  CHECK(b.kept == want.size());
  CHECK(b.skipped == ads.size() - want.size());
  CHECK(b.text.size() < kCap);
}

// Eight adverts whose seven values are all 255 bytes of `"` and `\`: no row
// fits even alone (about 3.7 KB each). The cache is the empty list, not a cut
// one, and an ordinary advert after them is still kept.
void test_none_fits_alone_and_the_next_one_is_kept() {
  std::vector<Ad> ads;
  for (int i = 0; i < 8; ++i) {
    Ad a = ordinary(i);
    for (int k = 0; k < 7; ++k) *a.field(k) = long_value(255);
    ads.push_back(a);
  }
  CHECK(oracle_row(ads[0]).size() > kCap);
  Built b = build(ads, kCap);
  CHECK(b.text == "{\"canaries\":[]}");
  CHECK(rows_match(b.text, ads, {}));
  CHECK(b.kept == 0 && b.skipped == 8);

  ads.push_back(ordinary(8));
  b = build(ads, kCap);
  CHECK(rows_match(b.text, ads, {8}));
  CHECK(b.kept == 1 && b.skipped == 8);
}

// The cache is a complete document after begin() and after every add(),
// kept or skipped: the task can stop anywhere and the handler still reads it.
void test_a_complete_document_after_every_add() {
  std::vector<Ad> ads;
  for (int i = 0; i < 10; ++i) {
    Ad a = ordinary(i);
    if (i % 3 == 1) *a.field(1) = long_value(255);
    if (i % 4 == 2) for (int k = 0; k < 7; ++k) *a.field(k) = long_value(255);
    ads.push_back(a);
  }
  std::vector<char> buf(kCap, '\0');
  fleet_scan_cache::Cache c;
  fleet_scan_cache::begin(c, buf.data(), kCap);
  json_strict::Value v;
  CHECK(json_strict::parse(buf.data(), &v));
  std::vector<size_t> kept;
  for (size_t i = 0; i < ads.size() && !fleet_scan_cache::full(c); ++i) {
    if (fleet_scan_cache::add(c, ads[i].view())) kept.push_back(i);
    CHECK(rows_match(buf.data(), ads, kept));
  }
  std::vector<size_t> want;
  oracle_cache(ads, kCap, &want);
  CHECK(kept == want);
}

// The boundary, to the byte: a row that leaves the document exactly one byte
// short of the cache (its NUL in the last byte) is kept; one byte longer is
// skipped.
void test_the_last_byte() {
  const std::vector<Ad> base = {ordinary(0)};
  const size_t used = oracle_cache(base, kCap, nullptr).size();
  Ad probe = ordinary(1);
  probe.name.clear();
  std::vector<Ad> ads = {ordinary(0), probe};
  const size_t row = 1 + oracle_row(probe).size();       // "," and the row
  const size_t room = kCap - 1 - used;                   // bytes left before the NUL
  CHECK(room > row);
  probe.name = std::string(room - row, 'n');
  ads[1] = probe;
  Built b = build(ads, kCap);
  CHECK(b.text.size() == kCap - 1);
  CHECK(rows_match(b.text, ads, {0, 1}));

  probe.name.push_back('n');
  ads[1] = probe;
  b = build(ads, kCap);
  CHECK(rows_match(b.text, ads, {0}));
  CHECK(b.skipped == 1);

  probe.name.back() = '"';   // the same length, but the quote costs two
  probe.name.erase(0, 1);
  ads[1] = probe;
  b = build(ads, kCap);
  CHECK(rows_match(b.text, ads, {0}));
}

// At most eight are kept, the first eight that fit, as the browse cap was.
void test_at_most_eight() {
  std::vector<Ad> ads;
  for (int i = 0; i < 12; ++i) ads.push_back(ordinary(i));
  const Built b = build(ads, kCap);
  CHECK(rows_match(b.text, ads, {0, 1, 2, 3, 4, 5, 6, 7}));
  CHECK(b.kept == 8 && b.skipped == 0);

  std::vector<char> buf(kCap, '\0');
  fleet_scan_cache::Cache c;
  fleet_scan_cache::begin(c, buf.data(), kCap);
  for (int i = 0; i < 8; ++i) CHECK(fleet_scan_cache::add(c, ads[i].view()));
  CHECK(fleet_scan_cache::full(c));
  const std::string before = buf.data();
  CHECK(!fleet_scan_cache::add(c, ads[8].view()));
  CHECK(before == buf.data());
}

// Control bytes go as \u00XX (ArduinoJson would write them raw, which a
// strict parser refuses), the two-byte escapes as themselves, and bytes from
// 0x7f up as they are; a missing value reads as "".
void test_every_byte_a_reader_can_get_back() {
  Ad a = ordinary(0);
  a.name = std::string("a\x01" "b\x1f" "c\n\t\r\b\f\x7f", 11) + "\xc3\xa9" "/\"\\";
  a.model = std::string(1, '\x02');
  std::vector<Ad> ads = {a};
  const Built b = build(ads, kCap);
  CHECK(rows_match(b.text, ads, {0}));
  CHECK(b.text.find("a\\u0001b\\u001fc\\n\\t\\r\\b\\f\x7f\xc3\xa9/\\\"\\\\") != std::string::npos);

  std::vector<char> buf(kCap, '\0');
  fleet_scan_cache::Cache c;
  fleet_scan_cache::begin(c, buf.data(), kCap);
  fleet_scan_cache::Advert none = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0};
  CHECK(fleet_scan_cache::add(c, none));
  CHECK(std::string(buf.data()) ==
        "{\"canaries\":[{\"device_id\":\"\",\"name\":\"\",\"mdns_host\":\"\",\"fw\":\"\",\"model\":\"\","
        "\"dt\":\"\",\"role\":\"\",\"ip\":\"\",\"port\":0}]}");
}

// A buffer too small for the empty list holds "" and nothing past its end is
// written; the smallest that fits holds the empty list.
void test_small_buffers() {
  const std::vector<Ad> ads = {ordinary(0)};
  for (size_t cap = 0; cap < 16; ++cap) {
    const Built b = build(ads, cap);
    CHECK(b.guard_intact);
    CHECK(b.text.empty());
    CHECK(b.kept == 0);
  }
  const Built b = build(ads, 16);
  CHECK(b.guard_intact);
  CHECK(b.text == "{\"canaries\":[]}");
  CHECK(b.skipped == 1);
}

}  // namespace

int main() {
  std::printf("fleet scan cache (F211) at FLEET_SCAN_CACHE_SIZE = %zu\n", kCap);
  CHECK(kCap == 2560);   // the size the cases above were laid out for
  if (kCap < 64) {
    std::printf("FAIL: FLEET_SCAN_CACHE_SIZE not found in %s\n", CANARY_WAP_INO);
    return 1;
  }
  test_eight_adverts_with_long_escaping_values();
  test_none_fits_alone_and_the_next_one_is_kept();
  test_a_complete_document_after_every_add();
  test_the_last_byte();
  test_at_most_eight();
  test_every_byte_a_reader_can_get_back();
  test_small_buffers();
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL fleet_scan_cache TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
