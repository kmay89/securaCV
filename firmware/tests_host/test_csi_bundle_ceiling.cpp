// Host tests for the hourly ceiling against the bundler (sweep F80), on the
// REAL CSI library: common/csi/src/csi_event.cpp (the chokepoint and its
// per-module ceiling), csi_bundler.cpp and csi_module.cpp, linked as both
// firmware trees link them, under a fake monotonic clock.
//
// The ceiling caps the ROWS a module commits per hour. A bundle opening is a
// future row and spends a slot; an emit that merges into a bundle already
// open makes no row and gives its slot back. Before F80 the chokepoint
// decided that refund BEFORE the admit, by asking csi_bundler_has_open()
// whether the key was open. But the admit first expires an overdue bundle
// (a quiet gap of CSI_BUNDLER_MAX_GAP_MS, or the 10-minute window) and then
// opens a new one for the same key, so a refunded emit could open a bundle,
// and commit a row, that the ceiling never counted. The canary hid it by
// flushing every bundle every CSI window (F81); the canary-wap ticks.
//
// Each test drives a module whose manifest ceiling is 6 an hour and checks
// the rows committed against what the ceiling allows: 6 in each of the 24
// hours the counter's six 10-minute buckets slide across. The fake clock
// starts where the counter is created, so the day is exactly 24 of its
// windows. Each test fails on the library before F80 (the numbers in the
// comments are what that library committed).
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "csi_bundler.h"
#include "csi_event.h"
#include "csi_module.h"

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

// ── The fake monotonic clock ────────────────────────────────────────────
// The library's host build reads CLOCK_MONOTONIC (csi_event.cpp and
// csi_bundler.cpp, `#ifndef ARDUINO`); this definition takes the place of
// the C library's in this binary, so the test owns time.
static uint64_t g_now_ms = 1000000;
extern "C" int clock_gettime(clockid_t, struct timespec* ts) noexcept {
  ts->tv_sec = (time_t)(g_now_ms / 1000);
  ts->tv_nsec = (long)((g_now_ms % 1000) * 1000000);
  return 0;
}

// ── The host's side: every committed row ────────────────────────────────
static uint32_t g_rows = 0;
static uint32_t g_rows_by_state_a = 0;

extern "C" void csi_event_on_committed(uint32_t, const char*, const char*,
                                       csi_event_category_t, csi_privacy_class_t,
                                       const csi_event_values_t* v) {
  ++g_rows;
  if (v && std::strcmp(v->state_name, "a") == 0) ++g_rows_by_state_a;
}

// A module with one state-bearing event type and the shipped presence
// ceiling (core.presence: 6 an hour).
static const csi_event_decl_t kEvents[] = {
    {"st", CSI_FIELD_STATE_NAME | CSI_FIELD_DURATION_SEC | CSI_FIELD_BUNDLED_COUNT,
     CSI_PRIVACY_P0, 6},
};
static void noop_tick(const csi_features_t*) {}
static const csi_module_t kModule = {
    "probe.ceiling", CSI_PRIVACY_P0, kEvents, 1, nullptr, noop_tick, nullptr, nullptr,
};
static const uint32_t kCeiling = 6;
static const uint64_t kHourMs = 3600ull * 1000;

// A fresh device whose counter is created now (the day starts here).
static void fresh() {
  csi_event_test_reset();
  csi_module_register(&kModule);
  g_rows = 0;
  g_rows_by_state_a = 0;
  // csi_event_set_module_ceiling creates the counter, anchored at now,
  // without emitting (an override of 0 keeps the manifest's ceiling).
  csi_event_set_module_ceiling("probe.ceiling", 0);
}

static uint32_t emit(const char* state) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME;
  std::snprintf(v.state_name, sizeof(v.state_name), "%s", state);
  return csi_event_emit("probe.ceiling", "st", &v);
}

// The F80 probe: one state-bearing emit every 121 s for a day, nothing
// else ticking the bundler. Each emit finds its key's bundle open, 121 s
// old, so the admit closes it (gap >= 120 s) and opens a new one. Every
// one of those openings is a row, and the ceiling allows 6 an hour. The
// library before F80 refunded each of them and committed 714 rows.
static int test_a_gap_reopen_spends_the_ceiling() {
  fresh();
  const uint64_t start = g_now_ms;
  while (g_now_ms + 121000 <= start + 24 * kHourMs) {
    (void)emit("a");
    g_now_ms += 121000;
  }
  csi_event_flush_bundles();   // the last bundle commits too
  std::printf("  gap reopen: %u rows in a day (ceiling allows %u)\n",
              (unsigned)g_rows, (unsigned)(kCeiling * 24));
  CHECK(g_rows <= kCeiling * 24);
  // Not starved either: every hour still gets its six.
  CHECK(g_rows == kCeiling * 24);
  return 0;
}

// The window path: state "a" refreshed every 60 s, so its bundle never
// reaches the quiet gap and closes on its 10-minute window, at a refresh,
// inside that refresh's admit (nothing else ticks). The reopened bundle is
// a row like any other. Beside it the module tries one new state every
// 10 minutes. Six rows an hour in all: the library before F80 refunded
// every window reopen of "a" and let the new states have the ceiling to
// themselves, 286 rows a day (143 of them "a").
static int test_a_window_reopen_spends_the_ceiling() {
  fresh();
  const uint64_t start = g_now_ms;
  int k = 0;
  for (uint64_t t = 0; t + 60000 <= 24 * kHourMs; t += 60000) {
    g_now_ms = start + t;
    (void)emit("a");
    if (t % 600000 == 0) {
      char s[16];
      std::snprintf(s, sizeof(s), "s%d", k++);
      (void)emit(s);
    }
  }
  g_now_ms = start + 24 * kHourMs - 1;
  csi_event_flush_bundles();
  std::printf("  window reopen: %u rows in a day (%u of state a; ceiling allows %u)\n",
              (unsigned)g_rows, (unsigned)g_rows_by_state_a, (unsigned)(kCeiling * 24));
  CHECK(g_rows <= kCeiling * 24);
  return 0;
}

// What F80 must not break: a refresh that merges into its open bundle
// still gives its slot back. Thirty refreshes of "a" inside one window,
// then five new states: all five open (one slot spent of six), and the
// whole lot commits as six rows.
static int test_a_merge_still_refunds() {
  fresh();
  CHECK(emit("a") != 0);
  for (int i = 1; i <= 30; ++i) {
    g_now_ms += 15000;          // 7.5 minutes, gaps of 15 s: one bundle
    CHECK(emit("a") != 0);
  }
  int opened = 0;
  for (int i = 0; i < 5; ++i) {
    char s[16];
    std::snprintf(s, sizeof(s), "t%d", i);
    if (emit(s) != 0) ++opened;
  }
  CHECK(opened == 5);
  CHECK(emit("u") == 0);        // the sixth new state is over the ceiling
  csi_event_flush_bundles();
  CHECK(g_rows == 6);
  CHECK(g_rows_by_state_a == 1);
  return 0;
}

int main() {
  if (test_a_gap_reopen_spends_the_ceiling()) return 1;
  if (test_a_window_reopen_spends_the_ceiling()) return 1;
  if (test_a_merge_still_refunds()) return 1;
  std::printf("test_csi_bundle_ceiling: %d checks passed\n", g_checks);
  return 0;
}
