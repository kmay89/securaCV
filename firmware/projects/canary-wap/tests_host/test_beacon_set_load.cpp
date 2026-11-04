// Host test (sweep F408): the Beacon set as it loads from NVS and as GET
// /api/beacon/set answers it.
//
// load_beacon_set() read each set entry with Preferences::getBytes() and
// dropped its answer, so an entry kept whatever the buffer then held: the
// bytes of a row from another layout that was shorter, the copy IDF had made
// before its data check refused the read (it answers 0 then), or a short
// read's bytes with the rest left as they were. And it never terminated the
// entry's 25-byte name, which handle_set_list() wrote into the answer as it
// was, so ArduinoJson read the name to the first zero byte past its field,
// and wrote a control byte raw and ill-formed UTF-8 as it is. Nothing in this
// firmware fills a member's name yet (start_pair_init() and
// start_pair_join() only set the state, "deferred to v0.3"), but v0.3's
// pairing will, from a peer's frame.
//
// Now an entry is loaded only as NVS returns it whole (any other answer
// leaves it zeroed, valid false, and the health log counts them once), each
// loaded name ends at its last byte, and the answer writes each name through
// wap_json::shown_text(). This suite runs the REAL code:
// load_beacon_set(), get_beacon_set_size() and get_beacon_set_entry() cut
// verbatim out of beacon_channel.cpp, and handle_set_list() with the helpers
// it calls (send_json(), send_reply() with its REPLY_BUF, send_error(),
// fp_to_hex()) cut verbatim out of beacon_api.h, both by
// firmware/tests_host/cut_functions.awk (the Makefile cuts them), over the
// REAL beacon_wire.h (the entry's layout) and wap_json_writer.h, and stubs/bt's
// ArduinoJson and esp_http_server stand-ins. beacon_channel.cpp needs the
// whole radio stack, so no suite compiles it whole; the cut functions name
// only what this file declares for them below (the set, its NVS keys, the
// flash-encryption gate, the health log). Built with AddressSanitizer: the
// old load's unterminated name, read past the set's last entry, stops there.
//
// Host-tested only. Beacon is off in every stock build (FEATURE_BEACON_CHANNEL
// is 0, and CI's validation build compiles it), the set loads only with flash
// encryption on, and nothing here ran on a device.
//
// Run: ./test_beacon_set_load

#ifndef BEACON_SET_CHANNEL_INC
#error "BEACON_SET_CHANNEL_INC (the cut of beacon_channel.cpp) must be defined"
#endif
#ifndef BEACON_SET_API_INC
#error "BEACON_SET_API_INC (the cut of beacon_api.h) must be defined"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"            // stubs/bt
#include "ArduinoJson.h"        // stubs/bt
#include "esp_http_server.h"    // stubs/bt
#include "beacon_wire.h"
#include "log_level.h"
#include "wap_json_writer.h"
#include "browser_utf8.h"
#include "json_strict.h"

// The health log the cut code writes (health_log.h's health_log()).
std::vector<std::string> g_health;
void health_log(LogLevel, LogCategory, const char* message) { g_health.push_back(message ? message : ""); }

// Arduino-ESP32 3.3.8's Preferences, as load_beacon_set() calls it, over one
// in-memory namespace. getBytes() answers as Preferences::getBytes() does:
// the row's length (0) for a missing or empty row, 0 with nothing read for a
// row longer than the buffer, and otherwise what nvs_get_blob() answers,
// which `fault` changes for the key `fault_key`: kCopies copies the row and
// answers 0 (IDF copies before its data check refuses the read), kShort
// copies all but its last byte and answers that count.
struct HostPrefs {
  enum class Read { kOk, kCopies, kShort };
  std::map<std::string, std::vector<uint8_t>> rows;
  std::string fault_key;
  Read fault = Read::kOk;
  int begins = 0;
  int ends = 0;
  bool begin(const char*, bool) {
    ++begins;
    return true;
  }
  void end() { ++ends; }
  uint8_t getUChar(const char* key, uint8_t d) {
    auto it = rows.find(key);
    return it != rows.end() && it->second.size() == 1 ? it->second[0] : d;
  }
  size_t getBytes(const char* key, void* buf, size_t max_len) {
    auto it = rows.find(key);
    if (it == rows.end()) return 0;
    const size_t len = it->second.size();
    if (len == 0 || buf == nullptr || max_len == 0) return len;
    if (len > max_len) return 0;
    const Read read = fault_key == key ? fault : Read::kOk;
    const size_t copied = read == Read::kShort ? len - 1 : len;
    memcpy(buf, it->second.data(), copied);
    return read == Read::kCopies ? 0 : copied;
  }
};

// What the cut of beacon_channel.cpp names, as the file declares it.
namespace beacon_channel {
static BeaconSetEntry g_beacon_set[MAX_BEACON_SET];
static uint8_t g_beacon_set_count = 0;
static HostPrefs g_prefs;
static const char* NVS_NS = "beacon";
static const char* NVS_SET_COUNT = "set_count";
static const char* NVS_SET_PREFIX = "set_";
static bool g_flash_encrypted = true;
static bool flash_encryption_enabled() { return g_flash_encrypted; }
#include BEACON_SET_CHANNEL_INC
}  // namespace beacon_channel

namespace beacon_api {
#include BEACON_SET_API_INC
}  // namespace beacon_api

namespace {

int g_checks = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

namespace bc = beacon_channel;
using bc::BeaconSetEntry;

const char* const kRefusedLog = "set entries NVS did not return whole were left out";

// An entry with no zero byte anywhere, padding included (a 25-byte name of
// `fill` with no NUL, a fingerprint of `id`), valid, a gateway's.
BeaconSetEntry entry(uint8_t id, char fill) {
  BeaconSetEntry e;
  memset(&e, 0x5A, sizeof e);
  memset(e.fingerprint, id, sizeof e.fingerprint);
  memset(e.name, fill, sizeof e.name);
  e.trust_level = bc::BCN_TRUST_GATEWAY;
  e.valid = true;
  e.has_x25519_pubkey = true;
  return e;
}
std::vector<uint8_t> bytes_of(const BeaconSetEntry& e) {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&e);
  return std::vector<uint8_t>(b, b + sizeof e);
}
std::string key_of(size_t i) { return "set_" + std::to_string(i); }

// A boot: RAM gone, NVS kept; `n` saved entries.
void boot(size_t n) {
  memset(bc::g_beacon_set, 0, sizeof bc::g_beacon_set);
  bc::g_beacon_set_count = 0;
  bc::g_prefs = HostPrefs();
  bc::g_prefs.rows["set_count"] = {(uint8_t)n};
  bc::g_flash_encrypted = true;
  g_health.clear();
}
size_t refused_lines() {
  size_t n = 0;
  for (const std::string& m : g_health) n += m.find(kRefusedLog) != std::string::npos;
  return n;
}

// The text a page shows for `raw` (wap_json_writer.h's shown_text(), read
// back the way a browser decodes it: each control byte JSON forbids raw and
// each ill-formed sequence as U+FFFD).
std::string shown(const std::string& raw) {
  std::string pre;
  for (unsigned char c : raw) {
    const bool control = c < 0x20 && c != '\b' && c != '\f' && c != '\n' && c != '\r' && c != '\t';
    if (control) pre += "\xEF\xBF\xBD";
    else pre += (char)c;
  }
  return browser_utf8(pre);
}

// GET /api/beacon/set: strict JSON (no raw control byte, well-formed UTF-8,
// one value), and its members' names in order.
std::vector<std::string> set_answer_names() {
  httpd_req_t req;
  CHECK(beacon_api::handle_set_list(&req) == ESP_OK);
  CHECK(req.sends == 1);
  bool strict = browser_utf8(req.resp) == req.resp;
  for (unsigned char c : req.resp) strict = strict && c >= 0x20;
  json_strict::Value v;
  strict = strict && json_strict::parse(req.resp, &v);
  if (!strict) std::fprintf(stderr, "GET /api/beacon/set: %s\n", req.resp.c_str());
  CHECK(strict);
  const json_strict::Value* members = v.get("members");
  CHECK(members != nullptr && members->kind == json_strict::Value::Array);
  std::vector<std::string> names;
  for (const json_strict::Value& m : members->items) {
    const json_strict::Value* n = m.get("name");
    CHECK(n != nullptr && n->kind == json_strict::Value::String);
    names.push_back(n->text);
  }
  return names;
}

// A full set whose every entry holds no zero byte but in its `valid` flag
// (the first half are stored holes, valid false; the answer lists the
// other half, and the set's last entry is one of them): each name is cut at
// its field's last byte as it loads, so each is BEACON_NAME_LEN bytes, and
// the answer lists every valid member with that name, whole. (Before, the
// last entry's name was read past the end of the set: AddressSanitizer
// stops there; the others carried the bytes after their field.) Half,
// because the answer refuses a list over 4096 bytes, which a full set of
// full-length names exceeds, before this change and after it.
void test_a_name_without_a_nul_ends_in_its_field() {
  const size_t holes = bc::MAX_BEACON_SET / 2;
  boot(bc::MAX_BEACON_SET);
  for (size_t i = 0; i < bc::MAX_BEACON_SET; ++i) {
    BeaconSetEntry e = entry((uint8_t)(i + 1), 'B');
    e.valid = i >= holes;
    bc::g_prefs.rows[key_of(i)] = bytes_of(e);
  }
  CHECK(bc::load_beacon_set());
  CHECK(bc::get_beacon_set_size() == bc::MAX_BEACON_SET);
  CHECK(bc::g_prefs.begins == 1 && bc::g_prefs.ends == 1);
  const std::string kept(bc::BEACON_NAME_LEN, 'B');
  const std::vector<std::string> names = set_answer_names();   // the answer first: it reads each name to its NUL
  CHECK(names.size() == bc::MAX_BEACON_SET - holes);
  for (const std::string& n : names) CHECK(n == kept);
  for (size_t i = 0; i < bc::MAX_BEACON_SET; ++i) {
    const BeaconSetEntry* e = bc::get_beacon_set_entry((uint8_t)i);
    CHECK(e != nullptr && e->valid == (i >= holes));
    CHECK(e->name[bc::BEACON_NAME_LEN] == '\0' && std::string(e->name) == kept);
  }
  CHECK(refused_lines() == 0);
  std::printf("PASS a_name_without_a_nul_ends_in_its_field\n");
}

// An entry NVS did not return whole is left out: a row a byte shorter than
// the entry (a short read of it), one a byte longer (Preferences reads
// nothing into a buffer it does not fit), a read that copied the row and
// answered 0, and a short read of a whole row are each left zeroed (valid
// false) and unlisted, the entries around them load, the count stays the
// saved count, and the health log says how many once. Every byte length
// around the entry's, each alone. (Before, a short row's and a copied
// read's entry was listed from whatever the read left in it.)
void test_an_entry_nvs_did_not_return_whole_is_left_out() {
  const size_t n = 6;
  boot(n);
  for (size_t i = 0; i < n; ++i) bc::g_prefs.rows[key_of(i)] = bytes_of(entry((uint8_t)(i + 1), (char)('a' + i)));
  bc::g_prefs.rows[key_of(1)].pop_back();                    // a byte short
  bc::g_prefs.rows[key_of(2)].push_back(0x5A);               // a byte long
  bc::g_prefs.fault_key = key_of(3);
  bc::g_prefs.fault = HostPrefs::Read::kCopies;              // copied, then refused
  CHECK(bc::load_beacon_set());
  CHECK(bc::get_beacon_set_size() == n);
  for (size_t i : {(size_t)1, (size_t)2, (size_t)3}) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(bc::get_beacon_set_entry((uint8_t)i));
    CHECK(std::all_of(b, b + sizeof(BeaconSetEntry), [](uint8_t x) { return x == 0; }));
    CHECK(!bc::get_beacon_set_entry((uint8_t)i)->valid);
  }
  const std::vector<std::string> names = set_answer_names();
  CHECK((names == std::vector<std::string>{std::string(bc::BEACON_NAME_LEN, 'a'), std::string(bc::BEACON_NAME_LEN, 'e'),
                                           std::string(bc::BEACON_NAME_LEN, 'f')}));
  CHECK(refused_lines() == 1 && g_health[0] == "beacon: 3 " + std::string(kRefusedLog));

  // A short read of a whole row.
  boot(1);
  bc::g_prefs.rows[key_of(0)] = bytes_of(entry(1, 'q'));
  bc::g_prefs.fault_key = key_of(0);
  bc::g_prefs.fault = HostPrefs::Read::kShort;
  CHECK(bc::load_beacon_set());
  CHECK(bc::get_beacon_set_size() == 1 && !bc::get_beacon_set_entry(0)->valid);
  CHECK(set_answer_names().empty() && refused_lines() == 1);

  // Every row length from empty to two entries, alone: only the entry's own loads.
  for (size_t len = 0; len <= 2 * sizeof(BeaconSetEntry); ++len) {
    boot(1);
    std::vector<uint8_t> row = bytes_of(entry(7, 'z'));
    row.resize(len, 0x5A);
    bc::g_prefs.rows[key_of(0)] = row;
    CHECK(bc::load_beacon_set());
    const bool whole = len == sizeof(BeaconSetEntry);
    CHECK(bc::get_beacon_set_entry(0)->valid == whole);
    CHECK(set_answer_names().size() == (whole ? 1u : 0u));
    CHECK(refused_lines() == (whole ? 0u : 1u));
  }
  std::printf("PASS an_entry_nvs_did_not_return_whole_is_left_out\n");
}

// What v0.3's pairing will put in a member's name comes from a peer's frame:
// a control byte, a quote, a backslash, a cut UTF-8 sequence, an overlong
// form. The answer is strict JSON and carries the text a page shows for
// each, whole (wap_json::shown_text()). (Before, ArduinoJson wrote the
// control byte raw and the ill-formed bytes as they were.)
void test_a_member_name_reaches_the_answer_as_shown() {
  const std::string names[] = {
      std::string("Porch\x01"),          std::string("a\"b\\c"),           std::string("cut \xC3"),
      std::string("over\xC0\xAF" "long"), std::string("\x1f\x1f\x1f"),     std::string("caf\xC3\xA9"),
      std::string(bc::BEACON_NAME_LEN, '\x01'),
  };
  boot(sizeof names / sizeof names[0]);
  for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
    BeaconSetEntry e = entry((uint8_t)(i + 1), 'x');
    memset(e.name, 0, sizeof e.name);
    memcpy(e.name, names[i].data(), names[i].size());
    bc::g_prefs.rows[key_of(i)] = bytes_of(e);
  }
  CHECK(bc::load_beacon_set());
  const std::vector<std::string> got = set_answer_names();
  CHECK(got.size() == sizeof names / sizeof names[0]);
  for (size_t i = 0; i < got.size(); ++i) {
    if (got[i] != shown(names[i])) std::fprintf(stderr, "member %zu: [%s]\n", i, got[i].c_str());
    CHECK(got[i] == shown(names[i]));
  }
  // The widest: 24 bytes of 0x01 are 72 bytes of text, and the buffer held them.
  CHECK(got.back().size() == 3 * bc::BEACON_NAME_LEN);
  std::printf("PASS a_member_name_reaches_the_answer_as_shown\n");
}

// Without flash encryption the set is not read at all, as before: empty,
// zeroed, and the load says so.
void test_without_flash_encryption_nothing_loads() {
  boot(2);
  bc::g_prefs.rows[key_of(0)] = bytes_of(entry(1, 'a'));
  bc::g_prefs.rows[key_of(1)] = bytes_of(entry(2, 'b'));
  bc::g_flash_encrypted = false;
  memset(bc::g_beacon_set, 0x5A, sizeof bc::g_beacon_set);
  CHECK(!bc::load_beacon_set());
  CHECK(bc::get_beacon_set_size() == 0 && bc::g_prefs.begins == 0);
  const uint8_t* b = reinterpret_cast<const uint8_t*>(bc::g_beacon_set);
  CHECK(std::all_of(b, b + sizeof bc::g_beacon_set, [](uint8_t x) { return x == 0; }));
  CHECK(set_answer_names().empty());
  // A count past the set's room is held to it.
  boot(bc::MAX_BEACON_SET + 8);
  CHECK(bc::load_beacon_set() && bc::get_beacon_set_size() == bc::MAX_BEACON_SET);
  std::printf("PASS without_flash_encryption_nothing_loads\n");
}

}  // namespace

int main() {
  test_a_name_without_a_nul_ends_in_its_field();
  test_an_entry_nvs_did_not_return_whole_is_left_out();
  test_a_member_name_reaches_the_answer_as_shown();
  test_without_flash_encryption_nothing_loads();
  std::printf("ALL %d Beacon set load checks PASSED\n", g_checks);
  return 0;
}
