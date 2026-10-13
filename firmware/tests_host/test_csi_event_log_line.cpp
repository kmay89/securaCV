// Host tests for common/csi/src/csi_event_log_line.h — the line format of
// the SD event log (/EVENTS/today.ndjson) that the canary-wap sketch and the
// canary PIO tree both write, so a tool reads either card the same way.
// Byte-exact goldens: the format moved here unchanged from the canary-wap's
// csi_event_log.cpp (backlog F37), so these strings are what every card
// written since PR #395 holds.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdlib.h>  // before the shim: libstdc++'s wrapper re-exports strtol

// The ESP32's `long` is 32 bits; this host's is 64. A parser that reads a
// uint32 field through strtol is right here and wrong on the device: every
// id at or above 0x80000000 and every millisecond mark past 24.8 days comes
// back as 2147483647 (backlog F46). So the header is compiled against a
// strtol with the device's range, and the uint32 tests below fail on any
// parser that still goes through `long`.
static long strtol_ilp32(const char* s, char** end, int base) {
  const long long v = std::strtoll(s, end, base);
  if (v > INT32_MAX) { errno = ERANGE; return INT32_MAX; }
  if (v < INT32_MIN) { errno = ERANGE; return INT32_MIN; }
  return (long)v;
}
#define strtol strtol_ilp32
#include "csi_event_log_line.h"
#undef strtol
// Both trees' SD adapters, for their paths and caps: the canary-wap's
// declarations (no SD in the header) and the canary's (pure over
// csi_event_backfill.h). The functions are only declared here.
#include "../projects/canary-wap/arduino/canary_wap/csi_event_log.h"
#include "../canary/src/csi_event_log.h"
// The two writers that spell a parsed record with %s (sweep F264): the
// events body the backfill publishes, and a strict JSON reader (the
// canary-wap host tests', JSON.parse()'s rules) to hold what they write.
#include "csi_event_wire.h"
#include "../projects/canary-wap/tests_host/json_strict.h"

#include <string>

using csi_event_log_line::kLineMax;
using csi_event_log_line::marshal;
using csi_event_log_line::parse;
using csi_event_log_line::well_formed;

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

#define CHECK_STR(got, want)                                            \
  do {                                                                  \
    if (std::strcmp((got), (want)) != 0) {                              \
      std::fprintf(stderr, "FAIL %s:%d:\n  got  %s\n  want %s\n",        \
                   __FILE__, __LINE__, (got), (want));                  \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

static csi_event_record_t presence_row() {
  csi_event_record_t r;
  std::memset(&r, 0, sizeof(r));
  r.event_id = 12345;
  r.first_seen_ms = 600000;
  r.last_seen_ms = 607000;
  r.category = CSI_CATEGORY_EVENT;
  r.privacy = CSI_PRIVACY_P1;
  r.bundled_count = 3;
  std::strcpy(r.module_id, "core.presence");
  std::strcpy(r.type_name, "presence_changed");
  std::strcpy(r.values.state_name, "active");
  std::strcpy(r.values.confidence, "observed");
  std::strcpy(r.values.dominant_signal, "motion");
  r.values.motion_score = 62;
  r.values.breathing_score = 18;
  r.values.breathing_rate_bpm = 14;
  r.values.duration_sec = 7;
  r.values.time_bucket = 54;
  r.values.dismissed = 0;
  return r;
}

static int test_presence_golden() {
  const csi_event_record_t r = presence_row();
  char line[kLineMax];
  const size_t n = marshal(&r, line, sizeof(line));
  CHECK(n == std::strlen(line));
  CHECK_STR(line,
      "{\"id\":12345,\"first\":600000,\"last\":607000,\"cat\":\"event\",\"priv\":\"p1\","
      "\"module\":\"core.presence\",\"type\":\"presence_changed\",\"bundled\":3,"
      "\"state\":\"active\",\"conf\":\"observed\",\"motion\":62,\"breathing\":18,"
      "\"bpm\":14,\"dur\":7,\"tb\":54,\"dom\":\"motion\",\"dismissed\":0}\n");
  return 0;
}

static int test_integrity_golden_and_empty_strings() {
  // The system.integrity row: anomaly, p0, empty confidence/dominant
  // strings written as "", not dropped.
  csi_event_record_t r;
  std::memset(&r, 0, sizeof(r));
  r.event_id = 7;
  r.first_seen_ms = 5000;
  r.last_seen_ms = 5000;
  r.category = CSI_CATEGORY_ANOMALY;
  r.privacy = CSI_PRIVACY_P0;
  r.bundled_count = 1;
  std::strcpy(r.module_id, "system.integrity");
  std::strcpy(r.type_name, "tamper");
  std::strcpy(r.values.state_name, "sd_remove");
  char line[kLineMax];
  CHECK(marshal(&r, line, sizeof(line)) > 0);
  CHECK_STR(line,
      "{\"id\":7,\"first\":5000,\"last\":5000,\"cat\":\"anomaly\",\"priv\":\"p0\","
      "\"module\":\"system.integrity\",\"type\":\"tamper\",\"bundled\":1,"
      "\"state\":\"sd_remove\",\"conf\":\"\",\"motion\":0,\"breathing\":0,"
      "\"bpm\":0,\"dur\":0,\"tb\":0,\"dom\":\"\",\"dismissed\":0}\n");
  r.category = CSI_CATEGORY_AMBIENT;
  r.privacy = CSI_PRIVACY_P2;
  CHECK(marshal(&r, line, sizeof(line)) > 0);
  CHECK(std::strstr(line, "\"cat\":\"ambient\",\"priv\":\"p2\"") != nullptr);
  return 0;
}

static int test_round_trip() {
  const csi_event_record_t r = presence_row();
  char line[kLineMax];
  const size_t n = marshal(&r, line, sizeof(line));
  line[n - 1] = '\0';  // readers hand parse() the line without its '\n'
  csi_event_record_t back;
  CHECK(parse(line, &back));
  CHECK(back.event_id == r.event_id);
  CHECK(back.first_seen_ms == r.first_seen_ms);
  CHECK(back.last_seen_ms == r.last_seen_ms);
  CHECK(back.category == r.category);
  CHECK(back.privacy == r.privacy);
  CHECK(back.bundled_count == r.bundled_count);
  CHECK_STR(back.module_id, r.module_id);
  CHECK_STR(back.type_name, r.type_name);
  CHECK_STR(back.values.state_name, r.values.state_name);
  CHECK_STR(back.values.confidence, r.values.confidence);
  CHECK_STR(back.values.dominant_signal, r.values.dominant_signal);
  CHECK(back.values.motion_score == r.values.motion_score);
  CHECK(back.values.breathing_score == r.values.breathing_score);
  CHECK(back.values.breathing_rate_bpm == r.values.breathing_rate_bpm);
  CHECK(back.values.duration_sec == r.values.duration_sec);
  CHECK(back.values.time_bucket == r.values.time_bucket);
  CHECK(back.values.dismissed == r.values.dismissed);
  return 0;
}

static int test_widest_record_fits_and_overflow_is_zero() {
  csi_event_record_t r;
  std::memset(&r, 0, sizeof(r));
  r.event_id = 0xFFFFFFFFu;
  r.first_seen_ms = 0xFFFFFFFFu;
  r.last_seen_ms = 0xFFFFFFFFu;
  r.category = CSI_CATEGORY_ANOMALY;
  r.privacy = CSI_PRIVACY_P2;
  r.bundled_count = 65535;
  std::memset(r.module_id, 'm', CSI_EVENT_NAME_MAX - 1);
  std::memset(r.type_name, 't', CSI_EVENT_NAME_MAX - 1);
  std::memset(r.values.state_name, 's', CSI_EVENT_NAME_MAX - 1);
  std::memset(r.values.confidence, 'c', CSI_EVENT_CONFIDENCE_MAX - 1);
  std::memset(r.values.dominant_signal, 'd', CSI_EVENT_NAME_MAX - 1);
  r.values.motion_score = 255;
  r.values.breathing_score = 255;
  r.values.breathing_rate_bpm = 255;
  r.values.duration_sec = 65535;
  r.values.time_bucket = 255;
  r.values.dismissed = 255;
  char line[kLineMax];
  const size_t n = marshal(&r, line, sizeof(line));
  CHECK(n > 0);
  std::printf("test_csi_event_log_line: widest line = %zu bytes\n", n);
  CHECK(n < 400);  // the header's claim; kLineMax (512) has headroom
  // Overflow: 0, never a clipped line; and a null record or tiny buffer.
  CHECK(marshal(&r, line, n) == 0);
  CHECK(marshal(&r, line, n + 1) == n);
  CHECK(marshal(&r, line, 31) == 0);
  CHECK(marshal(nullptr, line, sizeof(line)) == 0);
  CHECK(marshal(&r, nullptr, sizeof(line)) == 0);
  return 0;
}

static int test_parse_refuses_what_is_not_one_record() {
  csi_event_record_t rec;
  const char* good =
      "{\"id\":9,\"first\":1,\"last\":1,\"cat\":\"event\",\"priv\":\"p0\","
      "\"module\":\"core.presence\",\"type\":\"presence_changed\",\"bundled\":1,"
      "\"state\":\"idle\",\"conf\":\"\",\"motion\":0,\"breathing\":0,"
      "\"bpm\":0,\"dur\":0,\"tb\":0,\"dom\":\"\",\"dismissed\":0}";
  CHECK(well_formed(good));
  CHECK(parse(good, &rec) && rec.event_id == 9);
  // A torn last line (power cut mid-write).
  CHECK(!parse("{\"id\":12,\"first\":60", &rec));
  // The torn line with the next append glued on: two records, one line.
  char glued[kLineMax * 2];
  std::snprintf(glued, sizeof(glued), "{\"id\":12,\"fi%s", good);
  CHECK(!well_formed(glued));
  CHECK(!parse(glued, &rec));
  // No such event, not a record, empty, trailing junk after the record.
  CHECK(!parse("{\"id\":0,\"first\":1}", &rec));
  CHECK(!parse("\"id\":9}", &rec));
  CHECK(!parse("", &rec));
  CHECK(!parse("{\"id\":9} ", &rec));
  CHECK(!parse(nullptr, &rec));
  CHECK(!parse(good, nullptr));
  // A record the scanner can read but with fields missing keeps the
  // canary-wap's defaults: bundled 1, everything else zero / empty.
  CHECK(parse("{\"id\":44}", &rec));
  CHECK(rec.event_id == 44 && rec.bundled_count == 1);
  CHECK(rec.category == CSI_CATEGORY_EVENT && rec.privacy == CSI_PRIVACY_P0);
  CHECK(rec.module_id[0] == '\0');
  return 0;
}

static int test_both_trees_write_the_same_file() {
  // The canary-wap's adapter and the canary's name the same path, the
  // canary-wap's through the shared constant's value, and cap the file at
  // the same size, so one tool and one retention story cover both cards.
  CHECK_STR(csi_event_log::LOG_PATH, csi_event_log_line::kLogPath);
  CHECK(csi_event_log::MAX_BYTES == csi_event_log::kMaxBytes);
  // The owner file's name is on cards already: renaming it would orphan
  // every canary's claim, and each would then refuse its own log as
  // foreign. It lives in the log's directory.
  CHECK_STR(csi_event_log_line::kOwnerPath, "/EVENTS/owner");
  CHECK(std::strncmp(csi_event_log_line::kOwnerPath, csi_event_log_line::kDirPath,
                     std::strlen(csi_event_log_line::kDirPath)) == 0);
  return 0;
}

static int test_uint32_fields_survive_a_32_bit_long() {
  // Backlog F46: the one event-id space starts at 0xC0000000, the old
  // bundler's ids at 0x80000000, and a millisecond mark passes 2^31 after
  // 24.8 days of uptime. All three must read back as written on a device
  // whose long is 32 bits (the strtol shim above).
  const uint32_t ids[] = {0x80000000u, 0xC0000000u, 0xC0000007u, 0xFFFFFFFFu};
  for (uint32_t id : ids) {
    csi_event_record_t r = presence_row();
    r.event_id = id;
    r.first_seen_ms = 0x80000001u;   // 24.8 days and a bit
    r.last_seen_ms = 0xFFFFFFFEu;
    char line[kLineMax];
    const size_t n = marshal(&r, line, sizeof(line));
    CHECK(n > 0);
    line[n - 1] = '\0';
    csi_event_record_t back;
    CHECK(parse(line, &back));
    CHECK(back.event_id == id);
    CHECK(back.first_seen_ms == 0x80000001u);
    CHECK(back.last_seen_ms == 0xFFFFFFFEu);
  }
  // The field reader on its own, at the edges of the range.
  uint32_t v = 7;
  CHECK(csi_event_log_line::json_u32("{\"id\":4294967295}", "id", &v) ==
        csi_event_log_line::U32::kOk && v == 0xFFFFFFFFu);
  CHECK(csi_event_log_line::json_u32("{\"id\":2147483648}", "id", &v) ==
        csi_event_log_line::U32::kOk && v == 0x80000000u);
  CHECK(csi_event_log_line::json_u32("{\"id\":3}", "last", &v) ==
        csi_event_log_line::U32::kMissing && v == 0x80000000u);
  return 0;
}

static int test_signed_and_overflowing_numbers_are_refused() {
  // strtol took a sign and wrapped past 2^32 when cast: "-5" read back as
  // 0xFFFFFFFB and 4294967297 as 1. A card line is input; neither is an id
  // this format writes, and a forged one near the top of the space would
  // reach the backfill (F46). Refused, not reinterpreted.
  csi_event_record_t rec;
  CHECK(!parse("{\"id\":-5,\"first\":1,\"last\":1}", &rec));
  CHECK(rec.event_id == 0);
  CHECK(!parse("{\"id\":4294967296,\"first\":1,\"last\":1}", &rec));
  CHECK(!parse("{\"id\":4294967297,\"first\":1,\"last\":1}", &rec));
  CHECK(!parse("{\"id\":99999999999999999999999}", &rec));
  CHECK(!parse("{\"id\":,\"first\":1}", &rec));
  CHECK(!parse("{\"id\":12,\"first\":-1,\"last\":1}", &rec));
  CHECK(rec.event_id == 0 && rec.first_seen_ms == 0);
  CHECK(!parse("{\"id\":12,\"first\":1,\"last\":4294967296}", &rec));
  // A missing mark still reads 0, as it always did.
  CHECK(parse("{\"id\":12}", &rec) && rec.first_seen_ms == 0 && rec.last_seen_ms == 0);
  return 0;
}

// Sweep F264: a string field read back from the card kept a backslash or
// a control byte (json_str() stopped only at a quote), and the two writers
// that spell a parsed record with %s, marshal() (a dismissal rewrites a
// restored row on the card) and csi_event_wire::build_event_body() (the
// backfill's MQTT body), then wrote a line and a body a strict JSON reader
// refuses: the item's probe, a state of `occ\` or one holding 0x01. Now
// parse() refuses a line whose string field holds a byte no writer of the
// format puts in a name (a backslash, or one outside printable ASCII), and
// every line it takes writes back as JSON both ways.
static std::string field_line(const char* key, const std::string& value) {
  const csi_event_record_t r = presence_row();
  char buf[kLineMax];
  const size_t n = marshal(&r, buf, sizeof(buf));
  std::string line(buf, n - 1);   // without its '\n', as readers hand it over
  const std::string needle = std::string("\"") + key + "\":\"";
  const size_t at = line.find(needle);
  const size_t from = at + needle.size();
  const size_t to = line.find('"', from);
  line.replace(from, to - from, value);
  return line;
}

// What the record writes back: the line marshal() rewrites (without its
// '\n') and the events body the backfill sends. Both must be JSON.
static bool writes_json(const csi_event_record_t& rec) {
  char line[kLineMax];
  const size_t n = marshal(&rec, line, sizeof(line));
  if (n == 0) return false;
  json_strict::Value v;
  if (!json_strict::parse(std::string(line, n - 1), &v)) return false;
  char body[768];
  const csi_event_wire::Signer unsigned_signer = {nullptr, 1, "", ""};
  const size_t b = csi_event_wire::build_event_body(
      body, sizeof(body), rec.event_id, rec.module_id, rec.type_name, rec.category, rec.privacy,
      &rec.values, rec.first_seen_ms, rec.bundled_count, /*is_replay=*/true, unsigned_signer);
  return b > 0 && json_strict::parse(std::string(body, b), &v);
}

static int test_a_string_holding_a_byte_no_writer_puts_there_is_refused() {
  csi_event_record_t rec;
  // The item's probe, both shapes, in the state field.
  CHECK(!parse(field_line("state", "occ\\").c_str(), &rec));
  CHECK(rec.event_id == 0 && rec.values.state_name[0] == '\0');   // nothing half-read
  CHECK(!parse(field_line("state", std::string("oc\x01" "c")).c_str(), &rec));
  // The unedited line parses and writes back as JSON.
  CHECK(parse(field_line("state", "active").c_str(), &rec));
  CHECK_STR(rec.values.state_name, "active");
  CHECK(writes_json(rec));

  // Every string field the parser reads, every byte value: taken exactly
  // when it is printable ASCII and not a backslash, and then the record
  // writes back as JSON. (0 ends the line and '"' ends the field, so those
  // two cannot be inside one; a '{' is a second record's start, which
  // well_formed() refuses whatever field holds it.)
  const char* const keys[] = {"cat", "priv", "module", "type", "state", "conf", "dom"};
  int taken = 0, refused = 0;
  for (const char* key : keys) {
    for (int c = 1; c < 256; ++c) {
      if (c == '"') continue;
      const std::string value = std::string("a") + (char)c + "b";
      const bool want = c >= 0x20 && c <= 0x7E && c != '\\' && c != '{';
      const bool got = parse(field_line(key, value).c_str(), &rec);
      if (got != want) {
        std::fprintf(stderr, "FAIL field %s byte 0x%02X: parse() %s\n", key, c,
                     got ? "took it" : "refused it");
        return 1;
      }
      ++g_checks;
      if (got) {
        ++taken;
        CHECK(writes_json(rec));
      } else {
        ++refused;
        CHECK(rec.event_id == 0);
      }
    }
  }
  CHECK(taken == 7 * 92 && refused == 7 * (254 - 92));   // 254 bytes tried per field

  // The whole field is read, not just what fits its member: a backslash
  // past the member's 23 bytes is refused too.
  CHECK(!parse(field_line("state", std::string(30, 's') + "\\").c_str(), &rec));
  // Printable punctuation, a space and a tilde are a writer's bytes.
  CHECK(parse(field_line("dom", "a b~/|}").c_str(), &rec));
  CHECK_STR(rec.values.dominant_signal, "a b~/|}");
  CHECK(writes_json(rec));
  // A key the parser does not read is not a field it writes back: a
  // backslash there changes nothing that reaches a writer.
  std::string extra = field_line("state", "active");
  extra.insert(extra.size() - 1, ",\"note\":\"x\\\\y\"");
  CHECK(parse(extra.c_str(), &rec));
  CHECK(writes_json(rec));
  return 0;
}

int main() {
  if (test_presence_golden()) return 1;
  if (test_integrity_golden_and_empty_strings()) return 1;
  if (test_round_trip()) return 1;
  if (test_widest_record_fits_and_overflow_is_zero()) return 1;
  if (test_parse_refuses_what_is_not_one_record()) return 1;
  if (test_both_trees_write_the_same_file()) return 1;
  if (test_uint32_fields_survive_a_32_bit_long()) return 1;
  if (test_signed_and_overflowing_numbers_are_refused()) return 1;
  if (test_a_string_holding_a_byte_no_writer_puts_there_is_refused()) return 1;
  std::printf("test_csi_event_log_line: %d checks passed\n", g_checks);
  return 0;
}
