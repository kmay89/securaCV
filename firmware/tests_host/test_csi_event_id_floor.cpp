// Host tests for common/csi/src/csi_event_id_floor.h — when the event-id
// allocator's floor is written to NVS. Each boot below is modeled the way
// both firmware trees run it: restore the floor from NVS, then allocate
// ids through csi_event.cpp's allocator, calling the policy from the
// csi_event_on_id_advance hook. The property Home Assistant's replay gate
// needs: across any sequence of reboots, every id handed out is above
// every id handed out before it.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdint>
#include <cstdio>
#include <vector>

#include "../common/csi/src/csi_event_id_floor.h"

using csi_event_id_floor::floor_for;
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
  // RAM, reset on every boot:
  uint32_t next_id = 1;        // csi_event.cpp's g_next_event_id
  uint32_t stored = 0;         // the host's copy of what NVS holds
};

// The boot half: csi_event.cpp starts at 1, then the host restores.
static void boot(Device* d) {
  d->next_id = 1;
  d->stored = 0;
  if (d->nvs > 0) {
    if (d->nvs > d->next_id) d->next_id = d->nvs;  // set_event_id_floor
    d->stored = d->nvs;
  }
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
  CHECK(ids.front() == 1);
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
  CHECK(allocate(&d) == 1);
  CHECK(d.nvs == 1 + kStride);  // written by the first allocation
  boot(&d);
  const uint32_t first = allocate(&d);
  CHECK(first == 1 + kStride);
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
  std::printf("test_csi_event_id_floor: %d checks passed\n", g_checks);
  return 0;
}
