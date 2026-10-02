// Host tests for the hourly ceiling against the bundler (backlog F80), on
// the REAL CSI library: common/csi/src/csi_event.cpp (the chokepoint and its
// per-module hourly ceiling), csi_bundler.cpp and csi_module.cpp, linked as
// both firmware trees link them, with the test owning the clock
// (CSI_TEST_CLOCK: csi_test_now_ms() below).
//
// The ceiling refunds a same-state REFRESH of an open bundle: it adds no row,
// and core.presence's refreshes would otherwise spend a 6/hour cap in
// minutes. Before F80 the chokepoint decided "refresh" by asking
// csi_bundler_has_open() BEFORE admitting. Admit then expires overdue
// bundles first (a quiet gap of CSI_BUNDLER_MAX_GAP_MS, or the window), so a
// key whose bundle had just gone quiet was refunded as a refresh and then
// OPENED a new bundle: a row the ceiling never counted. One state-bearing
// emit every 121 s committed 714 rows a day under a 6/hour ceiling (144
// allowed). The canary-wap ticks its bundler and was exposed; the canary
// hid it by flushing every bundle every window (F81, fixed with this).
//
// Now admit says which it was (CSI_BUNDLER_MERGED: no new row; BUFFERED: a
// new bundle), decided after its own expiry, and only a merge is refunded.
//
// The last three tests came with #1762, which fixed the same item in
// parallel (its own test_csi_bundle_ceiling.cpp, folded in here when #1763
// merged). They start the day where the module's counter is created, so a
// day is exactly 24 of its hours, and pin the exact count as well as the
// bound: the gap probe commits exactly 144 rows (every hour still gets its
// six, not fewer); the WINDOW reopen (a key refreshed every 60 s, whose
// bundle closes on its 10-minute window inside a refresh's admit, beside
// one new state every 10 minutes) stays within the ceiling (286 rows a day
// before F80, 143 of them the refreshed key); and thirty merges into one
// open bundle leave five of the six slots for new states.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdint>
#include <cstdio>
#include <cstring>

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

// ── The clock: the library reads it through csi_test_now_ms() ───────────
static uint32_t g_now_ms = 1000;
extern "C" uint32_t csi_test_now_ms(void) { return g_now_ms; }

static constexpr uint32_t kSecond = 1000;
static constexpr uint32_t kMinute = 60 * kSecond;
static constexpr uint32_t kHour = 60 * kMinute;
static constexpr uint8_t kCeiling = 6;   // core.presence's default per hour

// ── Every committed row, with the time it committed ─────────────────────
static uint32_t g_rows = 0;
static uint32_t g_rows_by_state_a = 0;
static uint32_t g_row_ms[4096];

extern "C" void csi_event_on_committed(uint32_t, const char*, const char*,
                                       csi_event_category_t, csi_privacy_class_t,
                                       const csi_event_values_t* v) {
  if (g_rows < sizeof(g_row_ms) / sizeof(g_row_ms[0])) g_row_ms[g_rows] = g_now_ms;
  ++g_rows;
  if (v && std::strcmp(v->state_name, "a") == 0) ++g_rows_by_state_a;
}

// ── A module: one state-bearing type under a 6/hour ceiling ─────────────
static const csi_event_decl_t kEvents[] = {
  {
    /* type_name */                "presence",
    /* allowed_fields */            CSI_FIELD_STATE_NAME | CSI_FIELD_MOTION_SCORE
                                  | CSI_FIELD_BUNDLED_COUNT | CSI_FIELD_DURATION_SEC,
    /* privacy */                  CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  kCeiling,
  },
};
static void tick(const csi_features_t*) {}
static const csi_module_t kModule = {
  "test.ceiling", CSI_PRIVACY_P0, kEvents, 1, nullptr, tick, nullptr, nullptr,
};

static void power_on() {
  g_now_ms = 1000;
  g_rows = 0;
  g_rows_by_state_a = 0;
  csi_event_test_reset();
  (void)csi_module_register(&kModule);
}

// power_on(), and the module's ceiling counter created now, without an
// emit (an override of 0 keeps the manifest's ceiling): the day starts
// here, so it is exactly 24 of the counter's hours.
static void power_on_counter_anchored() {
  power_on();
  csi_event_set_module_ceiling("test.ceiling", 0);
}

static csi_event_values_t presence(const char* state) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_MOTION_SCORE;
  std::strncpy(v.state_name, state, sizeof(v.state_name) - 1);
  v.motion_score = 40;
  return v;
}

static uint32_t emit(const char* state) {
  csi_event_values_t v = presence(state);
  return csi_event_emit("test.ceiling", "presence", &v);
}

// The most rows committed in any 60-minute span.
static uint32_t max_rows_in_an_hour() {
  uint32_t best = 0;
  const uint32_t n = g_rows < 4096 ? g_rows : 4096;
  for (uint32_t i = 0, j = 0; j < n; ++j) {
    while (g_row_ms[j] - g_row_ms[i] >= kHour) ++i;
    if (j - i + 1 > best) best = j - i + 1;
  }
  return best;
}

// ── The tests ────────────────────────────────────────────────────────────

// Admit's outcome names a merge only when the bundle is still open after its
// own expiry.
static int test_admit_reports_merge_after_its_own_expiry() {
  power_on();
  uint32_t h1 = 0, h2 = 0, h3 = 0;
  csi_event_values_t v = presence("present");
  CHECK(csi_bundler_admit("test.ceiling", "presence", CSI_PRIVACY_P0, &v, &h1)
        == CSI_BUNDLER_BUFFERED);                          // opened
  g_now_ms += CSI_BUNDLER_MAX_GAP_MS - kSecond;
  v = presence("present");
  CHECK(csi_bundler_admit("test.ceiling", "presence", CSI_PRIVACY_P0, &v, &h2)
        == CSI_BUNDLER_MERGED);                            // inside the gap
  CHECK(h2 == h1);
  g_now_ms += CSI_BUNDLER_MAX_GAP_MS;                      // quiet for the whole gap
  CHECK(csi_bundler_has_open("test.ceiling", "presence", "present"));   // not yet expired...
  v = presence("present");
  CHECK(csi_bundler_admit("test.ceiling", "presence", CSI_PRIVACY_P0, &v, &h3)
        == CSI_BUNDLER_BUFFERED);                          // ...but admit opens anew
  CHECK(h3 != h1);
  CHECK(g_rows == 1);                                      // the first bundle committed
  return 0;
}

// The F80 probe: one state-bearing emit every 121 s, just past the quiet gap,
// for a day. Every emit opens a bundle, so every one is a row and must spend
// the ceiling. Before F80 this committed 714 rows.
static int test_emits_just_past_the_gap_spend_the_ceiling() {
  power_on();
  const uint32_t step = CSI_BUNDLER_MAX_GAP_MS + kSecond;
  const uint32_t steps = (24 * kHour) / step;
  for (uint32_t i = 0; i < steps; ++i) {
    (void)emit("present");
    g_now_ms += step;
  }
  csi_event_flush_bundles();
  std::printf("  emit every %u s for a day: %u rows (ceiling %u/hour)\n",
              (unsigned)(step / kSecond), (unsigned)g_rows, (unsigned)kCeiling);
  CHECK(g_rows > 0);
  CHECK(g_rows <= 24u * kCeiling);
  CHECK(max_rows_in_an_hour() <= kCeiling + 1u);   // +1: the flush at the end
  return 0;
}

// What the refund is for, kept: sustained presence refreshing its open
// bundle every 30 s spends one slot per BUNDLE, not one per refresh, so a
// real state change after 45 minutes of it still gets through. (Each
// 10-minute window is a new bundle and a new row, so it does spend one slot
// per window: an hour of unbroken presence fills a 6/hour ceiling on its
// own. That cost is the ceiling's, older than F80, and is tracked in the
// backlog as F90.)
static int test_refreshes_of_an_open_bundle_stay_free() {
  power_on();
  uint32_t admitted = 0, emits = 0;
  for (uint32_t t = 0; t < 45 * kMinute; t += 30 * kSecond) {
    admitted += emit("present") != 0 ? 1 : 0;
    ++emits;
    g_now_ms += 30 * kSecond;
    csi_bundler_tick();   // as both trees run it, every loop
  }
  CHECK(admitted == emits);                    // none dropped at the ceiling
  CHECK(g_rows == 4);                          // windows 1-4 closed; the 5th is open
  CHECK(emit("absent") != 0);                  // the transition is not dropped
  csi_event_flush_bundles();
  CHECK(g_rows == 6);
  CHECK(max_rows_in_an_hour() <= kCeiling);
  return 0;
}

// A mix: refreshes inside the gap, then gaps past it, alternating. Rows in
// any hour stay at the ceiling.
static int test_mixed_refreshes_and_gaps_hold_the_ceiling() {
  power_on();
  for (int round = 0; round < 200; ++round) {
    for (int k = 0; k < 3; ++k) {
      (void)emit(round % 2 ? "present" : "moving");
      g_now_ms += 50 * kSecond;
    }
    g_now_ms += CSI_BUNDLER_MAX_GAP_MS;   // quiet: the bundle is overdue
    csi_bundler_tick();
  }
  csi_event_flush_bundles();
  CHECK(g_rows > 0);
  CHECK(max_rows_in_an_hour() <= kCeiling + 1u);
  return 0;
}

// #1762's gap probe, on an anchored day: one state-bearing emit every 121 s,
// nothing else ticking the bundler. Each emit finds its key's bundle 121 s
// old, so the admit closes it (gap >= 120 s) and opens a new one: every one
// is a row, and the ceiling allows 6 an hour. Exactly 144, so not starved
// either: every hour still gets its six.
static int test_a_gap_reopen_spends_exactly_the_ceiling() {
  power_on_counter_anchored();
  const uint32_t start = g_now_ms;
  const uint32_t step = CSI_BUNDLER_MAX_GAP_MS + kSecond;
  while (g_now_ms + step <= start + 24 * kHour) {
    (void)emit("a");
    g_now_ms += step;
  }
  csi_event_flush_bundles();   // the last bundle commits too
  std::printf("  gap reopen, anchored day: %u rows (ceiling allows %u)\n",
              (unsigned)g_rows, (unsigned)(24u * kCeiling));
  CHECK(g_rows <= 24u * kCeiling);
  CHECK(g_rows == 24u * kCeiling);
  return 0;
}

// #1762's window probe: state "a" refreshed every 60 s, so its bundle never
// reaches the quiet gap and closes on its 10-minute window, at a refresh,
// inside that refresh's admit (nothing else ticks). The reopened bundle is a
// row like any other. Beside it the module tries one new state every 10
// minutes. Six rows an hour in all: before F80 every window reopen of "a" was
// refunded and the new states had the ceiling to themselves, 286 rows a day
// (143 of them "a").
static int test_a_window_reopen_spends_the_ceiling() {
  power_on_counter_anchored();
  const uint32_t start = g_now_ms;
  int k = 0;
  for (uint32_t t = 0; t + kMinute <= 24 * kHour; t += kMinute) {
    g_now_ms = start + t;
    (void)emit("a");
    if (t % (10 * kMinute) == 0) {
      char s[16];
      std::snprintf(s, sizeof(s), "s%d", k++);
      (void)emit(s);
    }
  }
  g_now_ms = start + 24 * kHour - 1;
  csi_event_flush_bundles();
  std::printf("  window reopen: %u rows in a day (%u of state a; ceiling allows %u)\n",
              (unsigned)g_rows, (unsigned)g_rows_by_state_a, (unsigned)(24u * kCeiling));
  CHECK(g_rows <= 24u * kCeiling);
  return 0;
}

// What F80 must not break, counted in slots: thirty refreshes of "a" inside
// one bundle, then new states. The merges gave their slots back, so five
// new states open (six slots, one spent on "a"), the sixth is refused at the
// ceiling, and the lot commits as six rows, one of them "a".
static int test_a_merge_gives_its_slot_back() {
  power_on_counter_anchored();
  CHECK(emit("a") != 0);
  for (int i = 1; i <= 30; ++i) {
    g_now_ms += 15 * kSecond;   // 7.5 minutes, gaps of 15 s: one bundle
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
  if (test_admit_reports_merge_after_its_own_expiry()) return 1;
  if (test_emits_just_past_the_gap_spend_the_ceiling()) return 1;
  if (test_refreshes_of_an_open_bundle_stay_free()) return 1;
  if (test_mixed_refreshes_and_gaps_hold_the_ceiling()) return 1;
  if (test_a_gap_reopen_spends_exactly_the_ceiling()) return 1;
  if (test_a_window_reopen_spends_the_ceiling()) return 1;
  if (test_a_merge_gives_its_slot_back()) return 1;
  std::printf("test_csi_bundler_ceiling: %d checks passed\n", g_checks);
  return 0;
}
