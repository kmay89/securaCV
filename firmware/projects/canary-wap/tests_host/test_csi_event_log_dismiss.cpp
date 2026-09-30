/* test_csi_event_log_dismiss.cpp — a dismissal survives the reboot refill.
 *
 * Compiles the REAL csi_event_log.cpp with the REAL canonical CSI chokepoint
 * (csi_event.cpp / csi_module.cpp / csi_bundler.cpp) over stubs/sd_fake, like
 * test_csi_event_log_load.cpp, and pins:
 *
 *   - a dismissal the HTTP handler queues is written by flush_dismissals()
 *     as one more line in the usual format, with "dismissed":1, and nothing
 *     else reaches the card;
 *   - after a reboot, load_into_ring() restores that event dismissed (it
 *     used to come back undismissed, since only the ring was changed), leaves
 *     the others as they were, and does not turn the dismissal line into a
 *     row of its own; a dismissal whose record is outside the tail is ignored;
 *   - iterate_since() (MQTT backfill) does not replay the dismissal line;
 *   - the queue is bounded, refuses id 0, and a dismissal with no card, or of
 *     an event no longer in the ring, writes nothing.
 *
 * What it does not pin: the real SD driver, the HTTP handler, the loop task.
 * Bench territory (docs/CHANGELOG.md says which is which). */

#include "csi_event_log.h"
#include "csi_event_log_line.h"
#include "csi_module.h"

#include <SD.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

bool sd_mount_in_flight() { return false; }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } \
  else { printf("ok   %s\n", msg); } } while (0)

extern "C" {
bool csi_event_commit_witness(uint32_t, const char*, const char*,
                              csi_event_category_t, const csi_event_values_t*) {
  return true;
}
void csi_event_on_committed(uint32_t, const char*, const char*, csi_event_category_t,
                            csi_privacy_class_t, const csi_event_values_t*) {}
}

static void noop_init(const csi_module_settings_t*) {}
static void noop_tick(const csi_features_t*) {}

static const csi_event_decl_t EVENTS[] = {
  { "presence_changed",
    CSI_FIELD_STATE_NAME | CSI_FIELD_CONFIDENCE | CSI_FIELD_MOTION_SCORE | CSI_FIELD_TIME_BUCKET,
    CSI_PRIVACY_P0, 6 },
};
static const csi_module_t MODULE = {
  "core.presence", CSI_PRIVACY_P0, EVENTS, sizeof(EVENTS) / sizeof(EVENTS[0]),
  noop_init, noop_tick, nullptr, nullptr,
};

static std::string line_for(uint32_t id, bool dismissed) {
  csi_event_record_t r;
  memset(&r, 0, sizeof(r));
  r.event_id = id;
  r.category = CSI_CATEGORY_EVENT;
  r.privacy = CSI_PRIVACY_P0;
  r.bundled_count = 1;
  strncpy(r.module_id, "core.presence", CSI_EVENT_NAME_MAX - 1);
  strncpy(r.type_name, "presence_changed", CSI_EVENT_NAME_MAX - 1);
  strncpy(r.values.state_name, "active", sizeof(r.values.state_name) - 1);
  r.values.motion_score = 50;
  r.values.dismissed = dismissed ? 1 : 0;
  char buf[csi_event_log_line::kLineMax];
  const size_t n = csi_event_log_line::marshal(&r, buf, sizeof(buf));
  return std::string(buf, n);
}

static std::vector<std::string> card_lines() {
  std::vector<std::string> out;
  const std::string& log = SD.files["/EVENTS/today.ndjson"];
  size_t p = 0;
  while (p < log.size()) {
    size_t e = log.find('\n', p);
    if (e == std::string::npos) e = log.size();
    out.push_back(log.substr(p, e - p));
    p = e + 1;
  }
  return out;
}

static bool dismissed_in_ring(uint32_t id) {
  csi_event_record_t r;
  return csi_event_find(id, &r) && r.values.dismissed == 1;
}

static bool undismissed_in_ring(uint32_t id) {
  csi_event_record_t r;
  return csi_event_find(id, &r) && r.values.dismissed == 0;
}

static void reboot() {
  csi_event_test_reset();
  csi_event_set_event_id_floor(1000);   // what NVS restores before the load
  csi_event_log::test_rearm_load();
}

static size_t count_cb_calls = 0;
static bool count_cb(const csi_event_record_t* rec, void*) {
  count_cb_calls++;
  return rec->values.dismissed == 0;   // a dismissal line would stop the walk
}

int main() {
  csi_event_test_reset();
  csi_module_register(&MODULE);
  csi_event_set_event_id_floor(1000);

  // ── Boot 1: three events from an earlier boot on the card. ──────────────
  SD.present = true;
  SD.dirs.insert("/EVENTS");
  SD.files["/EVENTS/today.ndjson"] =
      line_for(10, false) + line_for(11, false) + line_for(12, false);
  CHECK(csi_event_log::load_into_ring() == 3, "three rows restored");
  CHECK(undismissed_in_ring(10) && undismissed_in_ring(11) && undismissed_in_ring(12),
        "none of them dismissed");

  // ── The user dismisses one. ─────────────────────────────────────────────
  CHECK(csi_event_dismiss(11), "the ring dismisses event 11");
  CHECK(csi_event_log::queue_dismissal(11), "the handler queues the dismissal");
  CHECK(csi_event_log::queue_dismissal(11), "queueing it twice is one entry");
  CHECK(!csi_event_log::queue_dismissal(0), "id 0 is no event");
  CHECK(card_lines().size() == 3, "queueing writes nothing (the loop task writes)");
  SD.writes = 0;
  CHECK(csi_event_log::flush_dismissals() == 1, "the flush writes one dismissal");
  CHECK(csi_event_log::flush_dismissals() == 0, "and nothing on the next pass");
  {
    const std::vector<std::string> lines = card_lines();
    csi_event_record_t r;
    CHECK(lines.size() == 4 && csi_event_log_line::parse(lines[3].c_str(), &r)
          && r.event_id == 11 && r.values.dismissed == 1
          && strcmp(r.values.state_name, "active") == 0 && r.values.motion_score == 50,
          "the dismissal is event 11's own record, in the usual format, dismissed");
    CHECK(lines[0] + "\n" == line_for(10, false) && lines[1] + "\n" == line_for(11, false)
          && lines[2] + "\n" == line_for(12, false), "the earlier lines are untouched");
  }

  // ── MQTT backfill does not replay the dismissal line. ───────────────────
  count_cb_calls = 0;
  CHECK(csi_event_log::iterate_since(0, count_cb, nullptr) == 3 && count_cb_calls == 3,
        "backfill replays the three records, not the dismissal");

  // ── Reboot: the dismissal holds. ────────────────────────────────────────
  reboot();
  SD.writes = 0;
  CHECK(csi_event_log::load_into_ring() == 3, "after the reboot, still three rows");
  CHECK(dismissed_in_ring(11), "event 11 comes back dismissed");
  CHECK(undismissed_in_ring(10) && undismissed_in_ring(12), "the others are as they were");
  CHECK(SD.writes == 0, "the load writes nothing");

  // ── A dismissal whose record is outside the tail is not a row. ──────────
  SD.files["/EVENTS/today.ndjson"] += line_for(13, true);
  reboot();
  CHECK(csi_event_log::load_into_ring() == 3, "a lone dismissal line restores nothing");
  csi_event_record_t probe;
  CHECK(!csi_event_find(13, &probe), "event 13 is not in the ring");
  CHECK(dismissed_in_ring(11), "and 11 is still dismissed");

  // ── Order in the tail does not matter. ──────────────────────────────────
  SD.files["/EVENTS/today.ndjson"] = line_for(20, true) + line_for(20, false);
  reboot();
  CHECK(csi_event_log::load_into_ring() == 1 && dismissed_in_ring(20),
        "the dismissal applies wherever it sits in the tail");

  // ── Bounds and no-card. ─────────────────────────────────────────────────
  for (uint32_t id = 100; id < 108; ++id) {
    CHECK(csi_event_log::queue_dismissal(id), "queue slot");
  }
  CHECK(!csi_event_log::queue_dismissal(108), "a ninth pending dismissal is refused");
  const size_t before = card_lines().size();
  CHECK(csi_event_log::flush_dismissals() == 0 && card_lines().size() == before,
        "a dismissal of an event not in the ring writes nothing");
  CHECK(csi_event_dismiss(20), "dismiss again");
  SD.present = false;
  CHECK(csi_event_log::queue_dismissal(20) && csi_event_log::flush_dismissals() == 0,
        "with no card, nothing is written");
  SD.present = true;
  CHECK(card_lines().size() == before, "and the card is unchanged");

  if (g_fail == 0) {
    printf("ALL csi_event_log dismiss tests PASSED\n");
    return 0;
  }
  printf("%d FAILED\n", g_fail);
  return 1;
}
