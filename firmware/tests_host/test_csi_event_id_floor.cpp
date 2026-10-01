// Host tests for common/csi/src/csi_event_id_floor.h — when the event-id
// allocator's floor is written to NVS. Each boot below is modeled the way
// both firmware trees run it: restore the floor from NVS, then allocate
// ids through csi_event.cpp's allocator, calling the policy from the
// csi_event_on_id_advance hook. The property Home Assistant's replay gate
// needs: across any sequence of reboots, every id handed out is above
// every id handed out before it.
//
// Since backlog F46 the allocator starts at kIdSpaceBase (the one event-id
// space, above every id an older firmware handed out) and the host restores
// boot_floor(persisted floor, delivery ceiling). The model below does the
// same; the F46 tests at the end pin the base, the hold above the delivery
// ceiling, and what a boot loop costs.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdint>
#include <cstdio>
#include <vector>

#include "../common/csi/src/csi_event_id_floor.h"

using csi_event_id_floor::boot_floor;
using csi_event_id_floor::floor_for;
using csi_event_id_floor::kHoldLimit;
using csi_event_id_floor::kIdSpaceBase;
using csi_event_id_floor::kStride;
using csi_event_id_floor::must_persist;

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

// One device: NVS survives reboots, everything else does not.
struct Device {
  uint32_t nvs = 0;            // the persisted floor (0 = never written)
  bool     nvs_ok = true;      // false: every write fails
  uint32_t nvs_writes = 0;
  uint32_t ceiling = 0;        // the delivery ceiling, csi.evsent (0 = none)
  // RAM, reset on every boot:
  uint32_t next_id = kIdSpaceBase;  // csi_event.cpp's g_next_event_id
  uint32_t stored = 0;         // the host's copy of what NVS holds
};

// The boot half: csi_event.cpp starts at kIdSpaceBase, then the host
// restores boot_floor() (set_event_id_floor only ever moves it up).
static void boot(Device* d) {
  d->next_id = kIdSpaceBase;
  d->stored = d->nvs;
  const uint32_t f = boot_floor(d->nvs, d->ceiling);
  if (f > d->next_id) d->next_id = f;
}

// allocate_event_id() with the host's strong csi_event_on_id_advance.
static uint32_t allocate(Device* d) {
  uint32_t id = d->next_id++;
  if (id == 0) id = d->next_id++;
  if (must_persist(d->stored, id) && d->nvs_ok) {
    d->nvs = floor_for(id);
    d->stored = d->nvs;
    d->nvs_writes++;
  }
  return id;
}

// The scheme both trees shipped before this header, kept here so the test
// proves its scenarios can tell the two apart.
static uint32_t allocate_old(Device* d, uint32_t* persisted_at) {
  uint32_t id = d->next_id++;
  if (id == 0) id = d->next_id++;
  if (id >= *persisted_at + kStride) {
    d->nvs = id + kStride;
    *persisted_at = id;
  }
  return id;
}

// Run `per_boot[i]` allocations in boot i; returns every id in order.
static std::vector<uint32_t> run(Device* d, const std::vector<int>& per_boot) {
  std::vector<uint32_t> ids;
  for (int n : per_boot) {
    boot(d);
    for (int i = 0; i < n; ++i) ids.push_back(allocate(d));
  }
  return ids;
}

static bool strictly_increasing(const std::vector<uint32_t>& ids) {
  for (size_t i = 1; i < ids.size(); ++i) {
    if (ids[i] <= ids[i - 1]) return false;
  }
  return true;
}

static int test_reboot_before_a_full_stride_reuses_nothing() {
  // The review's schedule: short boots between a long one.
  Device d;
  const std::vector<uint32_t> ids = run(&d, {3, 3, 12, 4, 4});
  CHECK(ids.size() == 26);
  CHECK(strictly_increasing(ids));
  CHECK(ids.front() == kIdSpaceBase);
  return 0;
}

static int test_the_old_scheme_fails_the_same_schedule() {
  // Guard on the test itself: the schedule above must be one the replaced
  // scheme gets wrong, or it proves nothing.
  Device d;
  std::vector<uint32_t> ids;
  for (int n : {3, 3, 12, 4, 4}) {
    boot(&d);
    uint32_t persisted_at = d.nvs;  // the old restore
    for (int i = 0; i < n; ++i) ids.push_back(allocate_old(&d, &persisted_at));
  }
  CHECK(!strictly_increasing(ids));
  return 0;
}

static int test_every_boot_writes_before_its_first_id_leaves() {
  Device d;
  boot(&d);
  CHECK(allocate(&d) == kIdSpaceBase);
  CHECK(d.nvs == kIdSpaceBase + kStride);  // written by the first allocation
  boot(&d);
  const uint32_t first = allocate(&d);
  CHECK(first == kIdSpaceBase + kStride);
  CHECK(d.nvs > first);          // NVS is past it before it is used
  return 0;
}

static int test_nvs_always_holds_a_value_above_every_id() {
  Device d;
  boot(&d);
  for (int i = 0; i < 1000; ++i) {
    const uint32_t id = allocate(&d);
    CHECK(d.nvs > id);
  }
  return 0;
}

static int test_writes_stay_bounded() {
  // One write per boot that allocates, plus one per kStride ids.
  Device d;
  run(&d, {100});
  CHECK(d.nvs_writes == 100 / kStride);
  Device e;
  run(&e, {1, 1, 1, 1, 1});
  CHECK(e.nvs_writes == 5);
  Device f;
  run(&f, {0, 0, 0});  // a boot that allocates nothing writes nothing
  CHECK(f.nvs_writes == 0);
  return 0;
}

static int test_a_reboot_skips_at_most_a_stride() {
  Device d;
  std::vector<uint32_t> last_of_boot;
  std::vector<uint32_t> first_of_boot;
  for (int n : {7, 1, 23, 10, 9, 11, 2}) {
    boot(&d);
    for (int i = 0; i < n; ++i) {
      const uint32_t id = allocate(&d);
      if (i == 0) first_of_boot.push_back(id);
      if (i == n - 1) last_of_boot.push_back(id);
    }
  }
  for (size_t b = 1; b < first_of_boot.size(); ++b) {
    CHECK(first_of_boot[b] > last_of_boot[b - 1]);
    CHECK(first_of_boot[b] - last_of_boot[b - 1] <= kStride);
  }
  return 0;
}

static int test_random_reboot_schedules() {
  // Deterministic LCG; 500 devices, each rebooted 40 times after 0..24 ids.
  uint32_t seed = 0x5eed1234u;
  for (int dev = 0; dev < 500; ++dev) {
    Device d;
    std::vector<int> schedule;
    for (int b = 0; b < 40; ++b) {
      seed = seed * 1664525u + 1013904223u;
      schedule.push_back((int)((seed >> 16) % 25));
    }
    CHECK(strictly_increasing(run(&d, schedule)));
  }
  return 0;
}

static int test_a_failed_write_is_retried_on_the_next_id() {
  Device d;
  boot(&d);
  d.nvs_ok = false;
  allocate(&d);                  // write fails: stored stays 0
  CHECK(d.stored == 0);
  CHECK(d.nvs == 0);
  d.nvs_ok = true;
  const uint32_t id = allocate(&d);  // tries again
  CHECK(d.nvs == id + kStride);
  return 0;
}

static int test_pure_helpers() {
  CHECK(must_persist(0, 1));
  CHECK(must_persist(11, 11));
  CHECK(!must_persist(11, 10));
  CHECK(floor_for(5) == 5 + kStride);
  CHECK(floor_for(UINT32_MAX - kStride) == UINT32_MAX);
  CHECK(floor_for(UINT32_MAX - 3) == UINT32_MAX);  // saturates, never wraps
  CHECK(floor_for(UINT32_MAX) == UINT32_MAX);
  CHECK(kStride == 10);
  return 0;
}

// ── Backlog F46: one id space ───────────────────────────────────────────

static int test_the_space_starts_above_every_older_id() {
  // A fresh device, and devices upgraded from firmware whose floor held a
  // chokepoint id (they counted from 1) or whose bundler handed out ids
  // from 0x80000000: every first id is kIdSpaceBase.
  for (uint32_t old_floor : {0u, 1u, 500u, 0x80000005u, kIdSpaceBase - 1}) {
    Device d;
    d.nvs = old_floor;
    boot(&d);
    CHECK(allocate(&d) == kIdSpaceBase);
    CHECK(d.nvs == kIdSpaceBase + kStride);  // and NVS is past it at once
  }
  // A floor this firmware wrote keeps counting from where it was.
  Device e;
  e.nvs = kIdSpaceBase + 1234;
  boot(&e);
  CHECK(allocate(&e) == kIdSpaceBase + 1234);
  return 0;
}

static int test_boot_floor_rules() {
  CHECK(boot_floor(0, 0) == kIdSpaceBase);
  CHECK(boot_floor(500, 0) == kIdSpaceBase);
  // An old bundler-space delivery ceiling sits below the space: no effect.
  CHECK(boot_floor(500, 0x8000000Du) == kIdSpaceBase);
  // Held at or above the delivery ceiling...
  CHECK(boot_floor(kIdSpaceBase + 20, kIdSpaceBase + 35) == kIdSpaceBase + 35);
  CHECK(boot_floor(kIdSpaceBase + 40, kIdSpaceBase + 35) == kIdSpaceBase + 40);
  CHECK(boot_floor(0, kHoldLimit) == kHoldLimit);
  // ...but never raised past kHoldLimit (a corrupt ceiling, or one an older
  // firmware wrote for a forged card line): the allocator stays 2^28 ids
  // short of the wrap.
  CHECK(boot_floor(0, kHoldLimit + 1) == kIdSpaceBase);
  CHECK(boot_floor(kIdSpaceBase + 7, UINT32_MAX) == kIdSpaceBase + 7);
  CHECK(0xFFFFFFFFu - kHoldLimit >= (1u << 28) - 1);
  return 0;
}

static int test_the_ceiling_covers_a_failed_floor_write() {
  // A boot whose floor writes all fail while the backfill's delivery
  // ceiling is written (csi_event_backfill.h: before an id is handed over,
  // must_persist / floor_for over the ceiling the planner holds). Those ids
  // reached Home Assistant. The next boot must start above them: boot_floor
  // holds the floor at the ceiling.
  Device d;
  boot(&d);
  for (int i = 0; i < 5; ++i) allocate(&d);   // floor written at the first
  const uint32_t nvs_floor = d.nvs;
  d.nvs_ok = false;                            // floor writes fail from here
  uint32_t handed_max = 0;
  uint32_t ceiling_stored = 0;
  for (int i = 0; i < 30; ++i) {
    const uint32_t id = allocate(&d);
    if (must_persist(ceiling_stored, id)) {
      d.ceiling = floor_for(id);
      ceiling_stored = d.ceiling;
    }
    handed_max = id;
  }
  CHECK(d.nvs == nvs_floor);                   // the floor stood still
  CHECK(handed_max >= nvs_floor);              // ...while ids passed it
  d.nvs_ok = true;
  // Without the hold (the floor alone, as restored before F46), the boot
  // would reissue ids Home Assistant has already verified.
  const uint32_t floor_only = (d.nvs > kIdSpaceBase) ? d.nvs : kIdSpaceBase;
  CHECK(floor_only <= handed_max);
  boot(&d);
  CHECK(allocate(&d) > handed_max);            // held above the ceiling
  return 0;
}

static int test_boot_loop_cost_and_headroom() {
  // A boot loop that commits one row per boot: one floor write per boot
  // (the hold adds none), kStride ids per boot.
  Device d;
  d.ceiling = kIdSpaceBase + 3;                // a ceiling to hold above
  const int boots = 1000;
  uint32_t first = 0;
  uint32_t last = 0;
  for (int b = 0; b < boots; ++b) {
    boot(&d);
    const uint32_t id = allocate(&d);
    if (b == 0) first = id;
    last = id;
  }
  CHECK(d.nvs_writes == (uint32_t)boots);
  CHECK(last - first == (uint32_t)(boots - 1) * kStride);
  // The space: 2^30 ids. A boot every 5 s (17,280 a day) at kStride ids a
  // boot lasts about 17 years; the manifests' ceiling at its most (16
  // modules x 255/hour, 97,920 rows a day) about 30.
  const uint64_t space = 0x100000000ull - kIdSpaceBase;
  CHECK(space == (1ull << 30));
  CHECK(space / (17280ull * kStride) / 365 >= 17);
  CHECK(space / 97920ull / 365 >= 30);
  return 0;
}

int main() {
  if (test_reboot_before_a_full_stride_reuses_nothing()) return 1;
  if (test_the_old_scheme_fails_the_same_schedule()) return 1;
  if (test_every_boot_writes_before_its_first_id_leaves()) return 1;
  if (test_nvs_always_holds_a_value_above_every_id()) return 1;
  if (test_writes_stay_bounded()) return 1;
  if (test_a_reboot_skips_at_most_a_stride()) return 1;
  if (test_random_reboot_schedules()) return 1;
  if (test_a_failed_write_is_retried_on_the_next_id()) return 1;
  if (test_pure_helpers()) return 1;
  if (test_the_space_starts_above_every_older_id()) return 1;
  if (test_boot_floor_rules()) return 1;
  if (test_the_ceiling_covers_a_failed_floor_write()) return 1;
  if (test_boot_loop_cost_and_headroom()) return 1;
  std::printf("test_csi_event_id_floor: %d checks passed\n", g_checks);
  return 0;
}
