// The fleet scan's cache (sweep F211), through the REAL fleet_scan_cache.h the
// worker task writes it with (fleet_scan_cache::fill(), the task's whole
// browse), at the sketch's own FLEET_SCAN_CACHE_SIZE (read from
// canary_wap.ino, its path passed in, so the read fails closed).
//
// Before, fleet_scan_task() serialized the first eight adverts it browsed
// with ArduinoJson's sized form into the 2560-byte cache and, when they did
// not fit, terminated the cut text by hand: handle_fleet_scan()'s
// deserializeJson() failed on it and GET /api/fleet/scan answered an empty
// `canaries` list (the Fleet sheet showed this device and no other Canary).
// Each advert carries seven TXT values of up to 255 bytes that the
// advertising device chose, and `"` or `\` cost two bytes each, so eight
// adverts with long values never fit. This suite feeds such adverts and holds
// the cache to what a reader must get: a document a strict parser
// (JSON.parse's rules) reads, every value exactly as advertised, and the
// shortest adverts that fit kept whole, in browse order, so a long advert
// never costs a shorter one its row, wherever it falls in the browse.
//
// The expected rows come from oracles written here (std::string, the JSON
// spec's escapes), not from the header: the shortest rows taken while they
// fit together (at most eight; of two equally long, the one browsed first),
// and, independently, a brute force over every subset for the most adverts
// that can fit at all.

// Arduino.h's global names first, as the sketch has them in front of every
// header it includes: a header that collides with one (the first
// identity_json.h's `boolean(...)` under a using-directive) fails here, not
// in CI's ESP32 compiles.
#include "arduino_globals.h"

#include <algorithm>
#include <cstdint>
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

// The bytes a cache of `cap` bytes has for rows and the commas between them.
size_t rows_room(size_t cap) {
  const size_t frame = std::string("{\"canaries\":[]}").size() + 1;   // and its NUL
  return cap >= frame ? cap - frame : 0;
}

// The cache a reader must get for `ads` in `cap` bytes, and which were kept:
// the shortest rows (of two equally long, the one browsed first) taken while
// they fit together, at most eight, written in browse order.
std::string oracle_cache(const std::vector<Ad>& ads, size_t cap, std::vector<size_t>* kept) {
  std::vector<size_t> order(ads.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return oracle_row(ads[a]).size() < oracle_row(ads[b]).size();
  });
  std::vector<size_t> pick;
  size_t used = 0;
  for (size_t i : order) {
    if (pick.size() == fleet_scan_cache::kMaxAdverts) break;
    const size_t need = used + (pick.empty() ? 0 : 1) + oracle_row(ads[i]).size();
    if (need > rows_room(cap)) break;   // every row after it is as long or longer
    used = need;
    pick.push_back(i);
  }
  std::sort(pick.begin(), pick.end());
  std::string rows;
  for (size_t r = 0; r < pick.size(); ++r) rows += (r ? "," : "") + oracle_row(ads[pick[r]]);
  if (kept) *kept = pick;
  return "{\"canaries\":[" + rows + "]}";
}

// add()'s own rule, one advert after another: kept exactly when its row fits
// in what is left when its turn comes.
std::vector<size_t> oracle_add_in_order(const std::vector<Ad>& ads, size_t cap) {
  std::vector<size_t> kept;
  size_t used = 0;
  for (size_t i = 0; i < ads.size() && kept.size() < fleet_scan_cache::kMaxAdverts; ++i) {
    const size_t need = used + (kept.empty() ? 0 : 1) + oracle_row(ads[i]).size();
    if (need <= rows_room(cap)) {
      used = need;
      kept.push_back(i);
    }
  }
  return kept;
}

// The most adverts that fit in `cap` together, by trying every subset of
// rows `len` bytes long.
size_t brute_force_most(const std::vector<size_t>& len, size_t cap) {
  size_t best = 0;
  for (uint32_t m = 0; m < (1u << len.size()); ++m) {
    size_t count = 0, used = 0;
    for (size_t i = 0; i < len.size(); ++i) {
      if (!(m & (1u << i))) continue;
      used += (count ? 1 : 0) + len[i];
      ++count;
    }
    if (count <= fleet_scan_cache::kMaxAdverts && used <= rows_room(cap) && count > best) best = count;
  }
  return best;
}

std::vector<size_t> row_lens(const std::vector<Ad>& ads) {
  std::vector<size_t> len;
  for (const Ad& a : ads) len.push_back(oracle_row(a).size());
  return len;
}

// ── Running the header ──────────────────────────────────────────────────
struct Built {
  std::string text;
  size_t kept = 0, skipped = 0;
  bool guard_intact = true;   // nothing written past cap
};

// The task's browse over `ads`, through fill(); each read returns the
// advert's view, as the sketch's reader returns MDNS.txt()'s strings.
Built build(const std::vector<Ad>& ads, size_t cap, size_t* reads = nullptr) {
  std::vector<char> buf(cap + 16, '\x7f');
  fleet_scan_cache::Cache c;
  fleet_scan_cache::begin(c, buf.data(), cap);
  size_t n_reads = 0;
  auto read = [&](int i) {
    ++n_reads;
    return ads[(size_t)i].view();
  };
  fleet_scan_cache::fill(c, (int)ads.size(), read);
  Built b;
  for (size_t i = cap; i < buf.size(); ++i) b.guard_intact = b.guard_intact && buf[i] == '\x7f';
  if (cap > 0) b.text = std::string(buf.data(), strnlen(buf.data(), cap));
  b.kept = c.kept;
  b.skipped = c.skipped;
  if (reads) *reads = n_reads;
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
// different share of escaping bytes, so not every advert fits: the shortest
// rows are kept, in browse order, and the longest are the ones left out.
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
  // As the oracle computes it at 2560 bytes (2544 for rows and commas): the
  // rows are 670, 624, 578, 680, 458, 675, 433 and 535 bytes, and the four
  // shortest (433, 458, 535 and 578: 2007 with their commas) fit where a
  // fifth (624) does not. Kept in browse order instead, the first three and
  // the fifth would have filled it, the same count with longer rows.
  CHECK((want == std::vector<size_t>{2, 4, 6, 7}));
  CHECK((oracle_add_in_order(ads, kCap) == std::vector<size_t>{0, 1, 2, 4}));
  CHECK(brute_force_most(row_lens(ads), kCap) == want.size());

  size_t reads = 0;
  const Built b = build(ads, kCap, &reads);
  CHECK(b.guard_intact);
  CHECK(b.text == expect);
  CHECK(rows_match(b.text, ads, want));
  CHECK(b.kept == want.size());
  CHECK(b.skipped == ads.size() - want.size());
  CHECK(b.text.size() < kCap);
  CHECK(reads == ads.size() + want.size());   // each measured once, each kept one read again to write
}

// One long advert and seven ordinary Canaries: wherever the long one falls in
// the browse, the seven are kept and the long one is the advert left out. In
// browse order, a long advert answered first used to crowd out the rows
// after it (the review's case: six 200-byte values of `"` kept it alone; six
// 255-byte plain values kept it and five of the seven).
void test_one_long_advert_costs_only_its_own_row() {
  for (int kind = 0; kind < 2; ++kind) {
    Ad long_ad = ordinary(100);
    for (int k = 1; k < 7; ++k) *long_ad.field(k) = kind == 0 ? std::string(200, '"') : std::string(255, 'p');
    for (size_t at = 0; at < 8; ++at) {
      std::vector<Ad> ads;
      std::vector<size_t> canaries;
      for (int i = 0, o = 0; i < 8; ++i) {
        if ((size_t)i == at) {
          ads.push_back(long_ad);
        } else {
          ads.push_back(ordinary(o++));
          canaries.push_back((size_t)i);
        }
      }
      if (at == 0) {   // the case discriminates: in browse order the Canaries lost rows
        const std::vector<size_t> in_order = oracle_add_in_order(ads, kCap);
        CHECK(in_order.size() == (kind == 0 ? 1u : 6u) && in_order[0] == 0);
      }
      std::vector<size_t> want;
      const std::string expect = oracle_cache(ads, kCap, &want);
      CHECK(want == canaries);
      const Built b = build(ads, kCap);
      CHECK(b.text == expect);
      CHECK(rows_match(b.text, ads, canaries));
      CHECK(b.kept == 7 && b.skipped == 1);
    }
  }
}

// The browse is read to its end: results after the eighth are offered too,
// and an advert skipped for size does not hide a shorter one after it.
void test_past_the_eighth_result() {
  std::vector<Ad> ads;
  for (int i = 0; i < 8; ++i) {
    Ad a = ordinary(i);
    for (int k = 0; k < 7; ++k) *a.field(k) = long_value(255);   // too long even alone
    ads.push_back(a);
  }
  for (int i = 8; i < 12; ++i) ads.push_back(ordinary(i));
  Built b = build(ads, kCap);
  CHECK(rows_match(b.text, ads, {8, 9, 10, 11}));
  CHECK(b.kept == 4 && b.skipped == 8);

  // Twenty results, the eight shortest at the end: those eight are kept.
  ads.clear();
  for (int i = 0; i < 20; ++i) {
    Ad a = ordinary(i);
    if (i < 12) a.model = std::string(40, 'm');
    ads.push_back(a);
  }
  b = build(ads, kCap);
  CHECK(rows_match(b.text, ads, {12, 13, 14, 15, 16, 17, 18, 19}));
  CHECK(b.kept == 8 && b.skipped == 12);
}

// Of two equally long rows, the one browsed first is kept: when a shorter
// row arrives after eight equal ones it displaces the last of them, and when
// equal rows compete for the room the earliest win.
void test_equal_rows_keep_the_first_browsed() {
  std::vector<Ad> ads;
  for (int i = 0; i < 8; ++i) ads.push_back(ordinary(i));   // eight rows of one length
  Ad shorter = ordinary(8);
  shorter.model.clear();
  ads.push_back(shorter);
  Built b = build(ads, kCap);
  CHECK(rows_match(b.text, ads, {0, 1, 2, 3, 4, 5, 6, 8}));

  ads.clear();
  for (int i = 0; i < 3; ++i) {   // three rows of one length, two fit
    Ad a = ordinary(i);
    a.model = std::string(1000, 'm');
    ads.push_back(a);
  }
  CHECK(2 * oracle_row(ads[0]).size() + 1 <= rows_room(kCap));
  CHECK(3 * oracle_row(ads[0]).size() + 2 > rows_room(kCap));
  b = build(ads, kCap);
  CHECK(rows_match(b.text, ads, {0, 1}));
}

// Many browses, made up from a fixed seed: the answer parses, holds exactly
// the oracle's rows, as many as could fit at all (a brute force over every
// subset), and no advert left out has a shorter row than one kept (of two
// equally long, the one browsed first is kept).
void test_the_most_that_fit_and_never_a_longer_one() {
  uint32_t seed = 0x5eed2110u;
  auto next = [&](uint32_t bound) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % bound;
  };
  const char pool[] = {'a', 'b', '"', '\\', 'z', '\xc3', '\xa9', '-', '"', '\\'};
  int cases = 0, left_out = 0;
  for (int round = 0; round < 400; ++round) {
    const size_t n = 1 + next(12);
    std::vector<Ad> ads;
    for (size_t i = 0; i < n; ++i) {
      Ad a = ordinary((int)i);
      for (int k = 0; k < 7; ++k) {
        if (next(3) != 0) continue;
        const size_t len = next(256);
        std::string v;
        for (size_t j = 0; j < len; ++j) v.push_back(pool[next(sizeof(pool))]);
        *a.field(k) = v;
      }
      ads.push_back(a);
    }
    std::vector<size_t> want;
    const std::string expect = oracle_cache(ads, kCap, &want);
    const std::vector<size_t> len = row_lens(ads);
    size_t reads = 0;
    const Built b = build(ads, kCap, &reads);
    bool ok = b.guard_intact && b.text == expect && rows_match(b.text, ads, want) &&
              b.kept == want.size() && b.kept + b.skipped == n && reads == n + want.size() &&
              want.size() == brute_force_most(len, kCap);
    for (size_t i = 0; i < n && ok; ++i) {
      if (std::find(want.begin(), want.end(), i) != want.end()) continue;
      for (size_t k : want) ok = ok && (len[i] > len[k] || (len[i] == len[k] && i > k));
    }
    CHECK(ok);
    cases += ok ? 1 : 0;
    left_out += want.size() < n ? 1 : 0;
  }
  // The seed makes browses that do not all fit: the rule is exercised.
  CHECK(left_out >= 100);
  std::printf("  %d made-up browses held to the oracle and the brute force (%d left adverts out)\n",
              cases, left_out);
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
  CHECK(kept == oracle_add_in_order(ads, kCap));
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

// At most eight are kept, as the browse cap was: of twelve rows of nearly one
// length, the eight shortest (the first ten are equally long, the last two
// two bytes longer), the eight browsed first among equals.
void test_at_most_eight() {
  std::vector<Ad> ads;
  for (int i = 0; i < 12; ++i) ads.push_back(ordinary(i));
  const Built b = build(ads, kCap);
  CHECK(rows_match(b.text, ads, {0, 1, 2, 3, 4, 5, 6, 7}));
  CHECK(b.kept == 8 && b.skipped == 4);

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
  test_one_long_advert_costs_only_its_own_row();
  test_past_the_eighth_result();
  test_equal_rows_keep_the_first_browsed();
  test_the_most_that_fit_and_never_a_longer_one();
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
