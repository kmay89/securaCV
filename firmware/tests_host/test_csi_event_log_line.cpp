// Host tests for common/csi/src/csi_event_log_line.h — the line format of
// the SD event log (/EVENTS/today.ndjson) that the canary-wap sketch and the
// canary PIO tree both write, so a tool reads either card the same way.
// Byte-exact goldens: the format moved here unchanged from the canary-wap's
// csi_event_log.cpp (backlog F37), so these strings are what every card
// written since PR #395 holds.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdio>
#include <cstring>

#include "csi_event_log_line.h"
// Both trees' SD adapters, for their paths and caps: the canary-wap's
// declarations (no SD in the header) and the canary's (pure over
// csi_event_backfill.h). The functions are only declared here.
#include "../projects/canary-wap/arduino/canary_wap/csi_event_log.h"
#include "../canary/src/csi_event_log.h"

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
  return 0;
}

int main() {
  if (test_presence_golden()) return 1;
  if (test_integrity_golden_and_empty_strings()) return 1;
  if (test_round_trip()) return 1;
  if (test_widest_record_fits_and_overflow_is_zero()) return 1;
  if (test_parse_refuses_what_is_not_one_record()) return 1;
  if (test_both_trees_write_the_same_file()) return 1;
  std::printf("test_csi_event_log_line: %d checks passed\n", g_checks);
  return 0;
}
