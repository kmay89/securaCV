// Host tests for the one event-id space (backlog F46), on the REAL CSI
// library: common/csi/src/csi_event.cpp (the chokepoint and its one
// allocator), csi_bundler.cpp (open bundles, which commit through it) and
// csi_module.cpp, linked as both firmware trees link them.
//
// Before F46 a row that went through the bundler (presence, the
// system.integrity tampers) took its id from the bundler's own counter
// (0x80000000 upward, restarted every boot, committed in bundle-close
// order), while direct rows took the chokepoint's (1 upward, NVS floor).
// Home Assistant's replay gate (custom_components/securacv/sensor.py
// `_replay_gate`: a verified events body whose event_id is below the last
// one verified is refused) therefore refused the chokepoint rows after any
// bundle, and every bundle after a reboot. Now every row takes its id when
// it COMMITS, from the one allocator, which starts at kIdSpaceBase.
//
// The test is the host's side of a device: the commit hooks are the host's
// (csi_event_on_committed feeds a model of HA's gate; csi_event_on_id_advance
// writes the floor with csi_event_id_floor.h's policy, as both trees do),
// and a reboot is csi_event_test_reset() (RAM gone) plus the host's restore,
// csi_event_set_event_id_floor(boot_floor(NVS floor, delivery ceiling)).
//
// Each test here fails on the library before F46: its ids fall in commit
// order across bundled and direct rows, and HA's model refuses rows.
//
// What it cannot show: the commit lock. The host build is single-threaded
// (the FreeRTOS mutex compiles out), so the ordering between the loop task
// and the NimBLE host task is held by firmware/scripts/
// check_csi_commit_order.py instead (run by check_csi_sync.sh).
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "csi_bundler.h"
#include "csi_event.h"
#include "csi_event_id_floor.h"
#include "csi_module.h"

using csi_event_id_floor::boot_floor;
using csi_event_id_floor::kHandleBase;
using csi_event_id_floor::kIdSpaceBase;
using csi_event_id_floor::kStride;

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

// ── The device's NVS and Home Assistant: what survives a reboot ─────────
struct Nvs {
  uint32_t floor = 0;     // csi.evid / ev.next (0 = never written)
  uint32_t ceiling = 0;   // csi.evsent, the delivery ceiling (0 = none)
  bool floor_ok = true;   // false: floor writes fail
  uint32_t floor_writes = 0;
};
static Nvs g_nvs;
static uint32_t g_floor_stored = 0;   // the host's copy (RAM)

struct Ha {
  bool has_mark = false;
  uint32_t mark = 0;
  std::vector<uint32_t> accepted;
  std::vector<uint32_t> refused;
  void receive(uint32_t id) {
    if (has_mark && id < mark) {
      refused.push_back(id);
      return;
    }
    mark = id;
    has_mark = true;
    accepted.push_back(id);
  }
};
static Ha g_ha;

// Every commit the hooks saw, in order.
struct Commit {
  uint32_t id;
  bool bundled;   // carried a bundled_count (the bundler set it)
  char state[CSI_EVENT_NAME_MAX];
};
static std::vector<Commit> g_commits;
static std::vector<uint32_t> g_witnessed;

extern "C" {

// csi_event_egress.cpp / csi_integration.cpp's override, over the model.
void csi_event_on_id_advance(uint32_t new_id) {
  if (!csi_event_id_floor::must_persist(g_floor_stored, new_id)) return;
  if (!g_nvs.floor_ok) return;   // retried on the next id
  g_nvs.floor = csi_event_id_floor::floor_for(new_id);
  g_floor_stored = g_nvs.floor;
  g_nvs.floor_writes++;
}

bool csi_event_commit_witness(uint32_t event_id, const char*, const char*,
                              csi_event_category_t, const csi_event_values_t*) {
  g_witnessed.push_back(event_id);
  return true;
}

// Every committed row is handed to the broker at once (the live path; the
// backfill planner's own rules are test_csi_event_backfill.cpp's), with the
// delivery ceiling written first, as the planner does.
void csi_event_on_committed(uint32_t event_id, const char*, const char*,
                            csi_event_category_t, csi_privacy_class_t,
                            const csi_event_values_t* v) {
  Commit c;
  c.id = event_id;
  c.bundled = (v->present_fields & CSI_FIELD_BUNDLED_COUNT) != 0;
  std::memset(c.state, 0, sizeof(c.state));
  std::strncpy(c.state, v->state_name, sizeof(c.state) - 1);
  g_commits.push_back(c);
  if (csi_event_id_floor::must_persist(g_nvs.ceiling, event_id)) {
    g_nvs.ceiling = csi_event_id_floor::floor_for(event_id);
  }
  g_ha.receive(event_id);
}

}  // extern "C"

// ── A module: one state-bearing type (bundled) and one stateless use ────
static const csi_event_decl_t kEvents[] = {
  {
    /* type_name */                "presence",
    /* allowed_fields */            CSI_FIELD_STATE_NAME | CSI_FIELD_MOTION_SCORE
                                  | CSI_FIELD_BUNDLED_COUNT | CSI_FIELD_DURATION_SEC,
    /* privacy */                  CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  0,   // no cap: the test emits many rows
  },
};
static void tick(const csi_features_t*) {}
static const csi_module_t kModule = {
  "test.idspace", CSI_PRIVACY_P0, kEvents, 1, nullptr, tick, nullptr, nullptr,
};

// A reboot: RAM goes (the library's state, the host's copy of the floor),
// NVS and Home Assistant stay; the host restores the floor.
static void boot() {
  csi_event_test_reset();
  (void)csi_module_register(&kModule);   // registered once; the registry is ROM-like
  g_floor_stored = g_nvs.floor;
  csi_event_set_event_id_floor(boot_floor(g_nvs.floor, g_nvs.ceiling));
}

static void power_on_fresh() {
  g_nvs = Nvs{};
  g_ha = Ha{};
  g_commits.clear();
  g_witnessed.clear();
  boot();
}

// A state-bearing emit: opens a bundle (or rolls into one). Returns emit()'s value.
static uint32_t bundled(const char* state) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_MOTION_SCORE;
  std::strncpy(v.state_name, state, sizeof(v.state_name) - 1);
  v.motion_score = 40;
  return csi_event_emit("test.idspace", "presence", &v);
}

// A stateless emit: the chokepoint commits it at once.
static uint32_t direct() {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_MOTION_SCORE;
  v.motion_score = 7;
  return csi_event_emit("test.idspace", "presence", &v);
}

static void close_bundle(const char* state) {
  csi_bundler_flush_key("test.idspace", "presence", state);
}

static bool strictly_rising(const std::vector<Commit>& cs) {
  for (size_t i = 1; i < cs.size(); ++i) {
    if (cs[i].id <= cs[i - 1].id) return false;
  }
  return true;
}

// ── The tests ────────────────────────────────────────────────────────────

static int test_interleaved_rows_rise_in_commit_order() {
  power_on_fresh();
  // Open three bundles, commit direct rows between them, close the bundles
  // in a different order than they opened, and force one close by running
  // out of slots (CSI_BUNDLER_SLOTS = 8).
  CHECK(bundled("a") != 0);
  CHECK(direct() != 0);
  CHECK(bundled("b") != 0);
  CHECK(bundled("c") != 0);
  CHECK(direct() != 0);
  close_bundle("c");            // opened last, closes first
  CHECK(direct() != 0);
  close_bundle("a");
  bundled("a");          // a again: a fresh bundle
  for (const char* s : {"d", "e", "f", "g", "h", "i", "j"}) bundled(s);  // evicts the oldest open
  CHECK(direct() != 0);
  csi_event_flush_bundles();

  CHECK(g_commits.size() >= 12);
  CHECK(strictly_rising(g_commits));          // rising in commit order, bundles or not
  CHECK(g_ha.refused.empty());                // so HA's gate refuses nothing
  size_t nbundled = 0;
  for (const Commit& c : g_commits) {
    CHECK(c.id >= kIdSpaceBase);              // one space, above every older id
    nbundled += c.bundled ? 1 : 0;
  }
  CHECK(nbundled >= 8);
  CHECK(g_ha.accepted.size() == g_commits.size());
  // The witness chain saw the same ids as the hooks, in the same order.
  CHECK(g_witnessed.size() == g_commits.size());
  for (size_t i = 0; i < g_commits.size(); ++i) CHECK(g_witnessed[i] == g_commits[i].id);
  return 0;
}

static int test_reboots_keep_one_rising_space() {
  power_on_fresh();
  // Boots of different lengths; each ends with bundles still open (lost
  // with RAM, as a power cut loses them) or flushed.
  const int rows_per_boot[] = {3, 1, 12, 4, 0, 7, 2};
  int b = 0;
  for (int n : rows_per_boot) {
    if (b++ > 0) boot();
    for (int i = 0; i < n; ++i) {
      if (i % 3 == 0) {
        direct();
      } else {
        char s[8];
        std::snprintf(s, sizeof(s), "s%d", i % 5);
        bundled(s);
        if (i % 2 == 0) close_bundle(s);
      }
    }
    if (b % 2 == 0) csi_event_flush_bundles();
  }
  CHECK(!g_commits.empty());
  CHECK(strictly_rising(g_commits));
  CHECK(g_ha.refused.empty());
  for (const Commit& c : g_commits) CHECK(c.id < g_nvs.floor);   // NVS is past every id
  return 0;
}

static int test_one_space_across_reboots_mid_bundle() {
  // The case that broke most often before: a bundle commits after a
  // reboot. Its old id restarted at 0x80000000 and HA (whose mark was the
  // previous boot's last bundle) refused it.
  power_on_fresh();
  bundled("x");
  close_bundle("x");
  direct();
  bundled("y");
  close_bundle("y");
  const uint32_t last_before = g_commits.back().id;
  boot();
  bundled("x");
  close_bundle("x");
  CHECK(g_commits.back().id > last_before);
  CHECK(g_ha.refused.empty());
  return 0;
}

static int test_open_rows_carry_handles_never_event_ids() {
  power_on_fresh();
  const uint32_t h = bundled("open");
  CHECK(h >= kHandleBase && h < kIdSpaceBase);        // emit's return: the handle
  CHECK(bundled("open") == h);                         // stable while open
  csi_event_record_t rows[8];
  CHECK(csi_bundler_snapshot_open(rows, 8) == 1);
  CHECK(rows[0].event_id == h);                        // the open row's id
  CHECK(g_commits.empty());                            // no id taken while open
  const uint32_t next_before = csi_event_get_next_event_id();
  close_bundle("open");
  CHECK(g_commits.size() == 1);
  CHECK(g_commits[0].id == next_before);               // the id is taken at commit
  CHECK(g_commits[0].id != h);
  // Many bundles later, still no handle in the event-id space.
  for (int i = 0; i < 50; ++i) {
    char s[8];
    std::snprintf(s, sizeof(s), "o%d", i);
    const uint32_t hi = bundled(s);
    CHECK(hi >= kHandleBase && hi < kIdSpaceBase);
    close_bundle(s);
  }
  for (const Commit& c : g_commits) CHECK(c.id >= kIdSpaceBase);
  return 0;
}

static int test_upgrade_from_older_ids_is_accepted_without_a_reset() {
  // A device upgraded from firmware before F46: its NVS floor held a
  // chokepoint id, its delivery ceiling (F37 / #1754) sat in the old
  // bundler's space, and Home Assistant's mark is wherever the old firmware
  // left it: an old bundler id. Nothing is reset, on the device or in HA.
  power_on_fresh();
  g_nvs.floor = 500;
  g_nvs.ceiling = 0x80000003u + kStride;
  g_ha.receive(480);
  g_ha.receive(0x80000003u);
  const size_t old_accepted = g_ha.accepted.size();
  boot();
  direct();
  bundled("p");
  close_bundle("p");
  CHECK(g_commits.size() == 2);
  CHECK(g_commits[0].id == kIdSpaceBase);
  CHECK(g_ha.refused.empty());                          // the next events: accepted
  CHECK(g_ha.accepted.size() == old_accepted + 2);
  // And a replay of an old message is still refused.
  g_ha.receive(0x80000003u);
  g_ha.receive(480);
  CHECK(g_ha.refused.size() == 2);
  return 0;
}

static int test_failed_floor_writes_are_covered_by_the_ceiling() {
  // The floor's NVS writes fail for a while; the delivery ceiling's do
  // not (both trees hold boot_floor at the ceiling). The next boot does
  // not reissue an id Home Assistant already has.
  power_on_fresh();
  direct();
  g_nvs.floor_ok = false;
  for (int i = 0; i < 25; ++i) {
    if (i % 2) {
      direct();
    } else {
      bundled("f");
      close_bundle("f");
    }
  }
  CHECK(g_commits.back().id >= g_nvs.floor);   // ids passed the stuck floor
  g_nvs.floor_ok = true;
  boot();
  direct();
  CHECK(g_ha.refused.empty());
  CHECK(strictly_rising(g_commits));
  return 0;
}

int main() {
  if (test_interleaved_rows_rise_in_commit_order()) return 1;
  if (test_reboots_keep_one_rising_space()) return 1;
  if (test_one_space_across_reboots_mid_bundle()) return 1;
  if (test_open_rows_carry_handles_never_event_ids()) return 1;
  if (test_upgrade_from_older_ids_is_accepted_without_a_reset()) return 1;
  if (test_failed_floor_writes_are_covered_by_the_ceiling()) return 1;
  std::printf("test_csi_event_id_space: %d checks passed\n", g_checks);
  return 0;
}
