/* test_csi_event_log_load.cpp — csi_event_log::load_into_ring() over a fake
 * SD card.
 *
 * Compiles the REAL csi_event_log.cpp (the sketch's SD event log) with the
 * REAL canonical CSI chokepoint (csi_event.cpp / csi_module.cpp /
 * csi_bundler.cpp) over stubs/sd_fake — a card that lives in RAM — and
 * reboots into a log the way the device does: privacy ceiling and id floor
 * restored first (arm_load()), then load_into_ring(). What it pins:
 *
 *   - before arm_load() (csi_integration::init has not restored the floor,
 *     or never ran because the AP failed), a mounted card is not read and
 *     nothing latches, so the call after init still loads;
 *   - no card, and a card that is a canary base's (owner file): nothing is
 *     read, and nothing latches, so the call after a mount still loads;
 *   - only the last LOAD_TAIL_BYTES are read, and the fragment the tail
 *     window cuts is dropped, not parsed; a window that starts exactly on a
 *     line boundary keeps that whole first line;
 *   - the walk feeds the task watchdog and lets the idle task run;
 *   - a live row already in the ring (a late card): nothing is read;
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
#include <Arduino.h>        // stub_task_delays()
#include <esp_task_wdt.h>  // stub_wdt_feeds()

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

  // Expected: the loader reads from size - LOAD_TAIL_BYTES and drops bytes
  // up to and including the first '\n' from there, unless the window starts
  // on a line boundary (the byte before it is a '\n').
  CHECK(log.size() > csi_event_log::LOAD_TAIL_BYTES, "the log is longer than the tail window");
  const size_t start = log.size() - csi_event_log::LOAD_TAIL_BYTES;
  CHECK(log[start - 1] != '\n', "this log's window starts mid-line (test sanity)");
  const size_t first_whole = log.find('\n', start) + 1;
  size_t expected = 0;
  for (const auto& s : starts) {
    if (s.first >= first_whole && s.second) expected++;
  }
  CHECK(expected > 300 && expected < 1000, "the window cuts the good run (test sanity)");

  // ── Not armed: a card is there, but csi_integration::init has not restored
  //    the id floor (or never ran: the AP failed and start_http_server() with
  //    it). Nothing read, and no latch. ───────────────────────────────────
  SD.present = true;
  SD.dirs.insert("/EVENTS");
  SD.files["/EVENTS/today.ndjson"] = log;
  SD.writes = 0;
  {
    const size_t read0 = fake_sd_bytes_read();
    CHECK(csi_event_log::load_into_ring() == 0, "not armed: nothing restored");
    CHECK(fake_sd_bytes_read() == read0 && SD.writes == 0, "not armed: the card is not even read");
  }
  csi_event_log::arm_load();   // csi_integration::init, floor and ceiling restored

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
  const unsigned feeds0 = stub_wdt_feeds();
  const unsigned delays0 = stub_task_delays();
  const size_t restored = csi_event_log::load_into_ring();
  printf("     restored=%zu expected=%zu\n", restored, expected);
  CHECK(restored == expected, "exactly the good lines inside the tail window are restored");
  CHECK(SD.writes == 0, "the load writes nothing to the card");
  CHECK(SD.files["/EVENTS/today.ndjson"] == log, "the log is byte-for-byte unchanged");
  CHECK(g_committed == 0, "no witness write or commit hook fired");
  {
    // Two passes over a 128 KB tail, a feed every 16 chunks of 256 bytes.
    const unsigned per_pass = (unsigned)(csi_event_log::LOAD_TAIL_BYTES / (256u * 16u));
    CHECK(stub_wdt_feeds() - feeds0 >= 2 * per_pass - 2,
          "the walk feeds the task watchdog every 4 KB");
    CHECK(stub_task_delays() - delays0 == stub_wdt_feeds() - feeds0,
          "and lets the idle task run each time (vTaskDelay, not yield)");
  }

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

  // ── A window that starts exactly on a line boundary keeps its first line.
  //    6-digit ids make lines of length L, 7-digit ids of L + 1: a lines of
  //    one and b of the other sum to exactly LOAD_TAIL_BYTES. ─────────────
  {
    csi_event_test_reset();                  // reboot
    csi_event_set_event_id_floor(2000000);
    csi_event_log::test_rearm_load();
    csi_event_log::arm_load();
    const size_t L = line_for(100000, "presence_changed").size();
    CHECK(line_for(1000000, "presence_changed").size() == L + 1, "7-digit ids are one byte longer");
    const size_t n = csi_event_log::LOAD_TAIL_BYTES / L;
    const size_t b = csi_event_log::LOAD_TAIL_BYTES - n * L;
    CHECK(b <= n, "the split exists (test sanity)");
    const size_t a = n - b;
    std::string exact;
    for (uint32_t id = 900000; id < 900005; ++id) exact += line_for(id, "presence_changed");
    const size_t head = exact.size();
    for (uint32_t id = 999999 - (uint32_t)a + 1; id <= 999999; ++id) {
      exact += line_for(id, "presence_changed");
    }
    for (uint32_t id = 1000000; id < 1000000 + (uint32_t)b; ++id) {
      exact += line_for(id, "presence_changed");
    }
    CHECK(exact.size() - head == csi_event_log::LOAD_TAIL_BYTES &&
          exact[exact.size() - csi_event_log::LOAD_TAIL_BYTES - 1] == '\n',
          "the window starts exactly on a line (test sanity)");
    SD.files["/EVENTS/today.ndjson"] = exact;
    const size_t got = csi_event_log::load_into_ring();
    printf("     boundary: restored=%zu of %zu\n", got, n);
    CHECK(got == n, "every line of a boundary-aligned window is restored, the first included");
    csi_event_record_t head_row;
    CHECK(!csi_event_find(900004, &head_row), "the line before the window is not read");
  }

  // ── A late card after a live commit: nothing read, and latched. ─────────
  {
    csi_event_test_reset();                  // reboot
    csi_event_set_event_id_floor(200000);
    csi_event_log::test_rearm_load();
    csi_event_log::arm_load();
    SD.files["/EVENTS/today.ndjson"] = log;
    std::string row = line_for(150000, "presence_changed");
    row.pop_back();   // parse() takes the line without its '\n'
    csi_event_record_t live;
    CHECK(csi_event_log_line::parse(row.c_str(), &live) && csi_event_inject(&live),
          "a row in the ring before the card's load (a live commit's stand-in)");
    const size_t read0 = fake_sd_bytes_read();
    CHECK(csi_event_log::load_into_ring() == 0, "a ring with a row in it: nothing restored");
    CHECK(fake_sd_bytes_read() == read0, "and the card's 128 KB tail is not read at all");
    CHECK(csi_event_log::load_into_ring() == 0 && fake_sd_bytes_read() == read0,
          "and it latched: the next mount does not read it either");
  }

  if (g_fail == 0) {
    printf("ALL csi_event_log load tests PASSED\n");
    return 0;
  }
  printf("%d FAILED\n", g_fail);
  return 1;
}
