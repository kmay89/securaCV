/* test_csi_event_log_load.cpp — csi_event_log::load_into_ring() over a fake
 * SD card.
 *
 * Compiles the REAL csi_event_log.cpp (the sketch's SD event log) with the
 * REAL canonical CSI chokepoint (csi_event.cpp / csi_module.cpp /
 * csi_bundler.cpp) over stubs/sd_fake — a card that lives in RAM — and
 * reboots into a log the way the device does: privacy ceiling and id floor
 * restored first, then load_into_ring(). What it pins:
 *
 *   - no card, and a card that is a canary base's (owner file): nothing is
 *     read, and nothing latches, so the call after a mount still loads;
 *   - only the last LOAD_TAIL_BYTES are read, and the fragment the tail
 *     window cuts is dropped, not parsed;
 *   - every line csi_event_inject refuses (an id at or above the floor, an
 *     unregistered type, a type above the ceiling, a duplicate, a torn or
 *     glued line, an over-long line) is refused; the rest land newest-first;
 *   - a last line without its '\n' still loads when whole;
 *   - the load writes nothing to the card and fires no commit hook;
 *   - it runs once per boot.
 *
 * What it does not pin: the real SD driver, the SPI bus, the mount worker.
 * Those are bench territory (docs/CHANGELOG.md says which is which). */

#include "csi_event_log.h"
#include "csi_event_log_line.h"
#include "csi_module.h"

#include <SD.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

/* The sketch TU defines this (hardware_state.h's mount worker); the fake
 * card never has a mount in flight. */
bool sd_mount_in_flight() { return false; }

static int g_fail = 0;
static size_t g_committed = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } \
  else { printf("ok   %s\n", msg); } } while (0)

extern "C" {
bool csi_event_commit_witness(uint32_t, const char*, const char*,
                              csi_event_category_t, const csi_event_values_t*) {
  g_committed++;
  return true;
}
void csi_event_on_committed(uint32_t, const char*, const char*, csi_event_category_t,
                            csi_privacy_class_t, const csi_event_values_t*) {
  g_committed++;
}
}

static void noop_init(const csi_module_settings_t*) {}
static void noop_tick(const csi_features_t*) {}

static const csi_event_decl_t EVENTS[] = {
  { "presence_changed",
    CSI_FIELD_STATE_NAME | CSI_FIELD_CONFIDENCE | CSI_FIELD_MOTION_SCORE | CSI_FIELD_TIME_BUCKET,
    CSI_PRIVACY_P0, 6 },
  { "breathing_rate", CSI_FIELD_BREATHING_RATE, CSI_PRIVACY_P1, 4 },
};
static const csi_module_t MODULE = {
  "core.presence", CSI_PRIVACY_P0, EVENTS, sizeof(EVENTS) / sizeof(EVENTS[0]),
  noop_init, noop_tick, nullptr, nullptr,
};

static std::string line_for(uint32_t id, const char* type) {
  csi_event_record_t r;
  memset(&r, 0, sizeof(r));
  r.event_id = id;
  r.category = CSI_CATEGORY_EVENT;
  r.privacy = CSI_PRIVACY_P0;
  r.bundled_count = 1;
  strncpy(r.module_id, "core.presence", CSI_EVENT_NAME_MAX - 1);
  strncpy(r.type_name, type, CSI_EVENT_NAME_MAX - 1);
  strncpy(r.values.state_name, "active", sizeof(r.values.state_name) - 1);
  r.values.motion_score = 50;
  r.values.time_bucket = 60;
  char buf[csi_event_log_line::kLineMax];
  const size_t n = csi_event_log_line::marshal(&r, buf, sizeof(buf));
  return std::string(buf, n);
}

int main() {
  csi_event_test_reset();
  csi_module_register(&MODULE);
  csi_event_set_event_id_floor(200000);   // what NVS restores before the load

  // ── Build the card's log. ────────────────────────────────────────────────
  // 1000 same-length good lines (ids 100000..100999), so the tail window
  // falls mid-line; then the lines inject must refuse; then two good lines,
  // the last without its '\n' (whole, only the newline lost).
  std::string log;
  std::vector<std::pair<size_t, bool>> starts;   // (offset, restorable)
  for (uint32_t id = 100000; id < 101000; ++id) {
    starts.push_back({log.size(), true});
    log += line_for(id, "presence_changed");
  }
  const char* refused_lines[] = {nullptr};
  (void)refused_lines;
  std::vector<std::string> bad;
  bad.push_back(line_for(200000, "presence_changed"));   // at the floor: this boot's id
  bad.push_back(line_for(250000, "presence_changed"));   // above the floor
  bad.push_back(line_for(101500, "no_such_type"));       // unregistered type
  bad.push_back(line_for(101501, "breathing_rate"));     // P1 under the P0 ceiling
  bad.push_back(line_for(100999, "presence_changed"));   // duplicate of a restored id
  {
    std::string torn = line_for(101502, "presence_changed");
    torn = torn.substr(0, torn.size() / 2);              // power cut mid-line ...
    bad.push_back(torn + line_for(101503, "presence_changed"));  // ... glued to the next
  }
  bad.push_back(std::string(700, 'x') + "\n");           // longer than any record
  for (const std::string& b : bad) {
    starts.push_back({log.size(), false});
    log += b;
  }
  starts.push_back({log.size(), true});
  log += line_for(101600, "presence_changed");
  starts.push_back({log.size(), true});
  std::string last = line_for(101601, "presence_changed");
  last.pop_back();                                       // no trailing '\n'
  log += last;

  // Expected: the loader seeks to size - LOAD_TAIL_BYTES and drops bytes up
  // to and including the first '\n' from there (a whole line, if the window
  // starts on a line boundary).
  CHECK(log.size() > csi_event_log::LOAD_TAIL_BYTES, "the log is longer than the tail window");
  const size_t start = log.size() - csi_event_log::LOAD_TAIL_BYTES;
  const size_t first_whole = log.find('\n', start) + 1;
  size_t expected = 0;
  for (const auto& s : starts) {
    if (s.first >= first_whole && s.second) expected++;
  }
  CHECK(expected > 300 && expected < 1000, "the window cuts the good run (test sanity)");

  // ── No card: nothing, and no latch. ──────────────────────────────────────
  SD.present = false;
  CHECK(csi_event_log::load_into_ring() == 0, "no card: nothing restored");

  // ── A canary base's card: left alone, and no latch. ─────────────────────
  SD.present = true;
  SD.dirs.insert("/EVENTS");
  SD.files["/EVENTS/today.ndjson"] = log;
  SD.files["/EVENTS/owner"] = "fp\n";
  SD.writes = 0;
  CHECK(csi_event_log::load_into_ring() == 0, "a canary base's card: nothing restored");
  CHECK(SD.writes == 0, "a canary base's card: nothing written");
  SD.files.erase("/EVENTS/owner");

  // ── This device's card. ──────────────────────────────────────────────────
  SD.writes = 0;
  const size_t restored = csi_event_log::load_into_ring();
  printf("     restored=%zu expected=%zu\n", restored, expected);
  CHECK(restored == expected, "exactly the good lines inside the tail window are restored");
  CHECK(SD.writes == 0, "the load writes nothing to the card");
  CHECK(SD.files["/EVENTS/today.ndjson"] == log, "the log is byte-for-byte unchanged");
  CHECK(g_committed == 0, "no witness write or commit hook fired");

  csi_event_record_t out[4];
  const size_t n = csi_event_recent(out, 4);
  CHECK(n == 4 && out[0].event_id == 101601, "the unterminated last line is newest");
  CHECK(n == 4 && out[1].event_id == 101600, "then the line before it");
  CHECK(n == 4 && out[2].event_id == 100999, "then the good run (refused lines skipped)");
  csi_event_record_t probe;
  CHECK(!csi_event_find(200000, &probe) && !csi_event_find(250000, &probe),
        "ids this boot can allocate are not in the ring");
  CHECK(!csi_event_find(101500, &probe), "an unregistered type is not in the ring");
  CHECK(!csi_event_find(101501, &probe), "a type above the ceiling is not in the ring");
  CHECK(!csi_event_find(101502, &probe) && !csi_event_find(101503, &probe),
        "a torn line glued to the next is not in the ring");
  CHECK(!csi_event_find(100000, &probe), "a line before the tail window is not read");
  CHECK(csi_event_find(100999, &probe) && strcmp(probe.values.state_name, "active") == 0
        && probe.values.motion_score == 50 && probe.first_seen_ms == 0,
        "a restored row carries its fields, not the earlier boot's clock");

  // ── Once per boot. ───────────────────────────────────────────────────────
  CHECK(csi_event_log::load_into_ring() == 0, "a second call restores nothing");
  CHECK(csi_event_get_next_event_id() == 200000, "the load allocated no id");

  if (g_fail == 0) {
    printf("ALL csi_event_log load tests PASSED\n");
    return 0;
  }
  printf("%d FAILED\n", g_fail);
  return 1;
}
