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
// Three tests came with #1762, which fixed the same item in parallel (its
// own test_csi_bundle_ceiling.cpp, folded in here when #1763 merged). They
// start the day where the module's counter is created, so a day is exactly
// 24 of its hours, and pin the exact count as well as the bound: the gap
// probe commits exactly 144 rows (every hour still gets its six, not
// fewer); the WINDOW reopen (a key refreshed every 60 s, whose bundle
// closes on its 10-minute window inside a refresh's admit, beside one new
// state every 10 minutes) commits exactly 144 (286 rows a day before F80,
// 143 of them the refreshed key); and thirty merges into one open bundle
// leave five of the six slots for new states.
//
// What "an hour" means here (backlog F132): the counter keeps six 10-minute
// buckets, the current one and the five before it, and refuses an emit when
// they hold the ceiling. So the ceiling bounds OPENINGS in each of the
// counter's hours (any six consecutive buckets), not committed rows in every
// sliding 60 minutes: rows commit when their bundles close, two to ten
// minutes after they opened, and openings can bunch at a bucket edge.
// test_a_sliding_hour_can_hold_twice_the_ceiling pins how far apart the two
// can get: twice the ceiling.
//
// Backlog F90: a state held past its 10-minute window reopens a bundle every
// window, six an hour, which is core.presence's whole ceiling, and the
// ceiling was checked before the bundler: once it was full, every refresh of
// the held state's OPEN bundle was refused too, and the next change of state
// waited up to about ten minutes for a slot to age out (a probe of the old
// library saw 210 of 643 refreshes refused and a 301 s wait). Now an emit
// that would merge into its key's open bundle is never refused and spends
// no slot (csi_bundler_peek(), on the cleaned values, then
// csi_bundler_refresh()), and with a ceiling of two or more the last slot
// goes only to an opening whose (type, state) differs from the module's most
// recent opening. The F90 tests below hold one state for one, two and three
// hours at several clock phases (a refresh every 7 s, the bundler ticked
// every second, as both trees tick it once per loop), against a model of
// the bundler's rule kept in this file: no refresh of an open bundle is
// refused, the change of state after the hold is admitted at once, and the
// openings stay within the ceiling in every six buckets and 144 a day.
// Each fails on the library before F90. test_a_gap_reopen_spends_exactly_
// the_ceiling pinned the old rule's 144 (a held state took the last slot of
// every hour) and now pins 120, the last slot of each hour kept free.
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
  // +1 (7 measured, with or without the final flush's row): a row commits
  // when its bundle closes, after the bucket that counted its opening, and
  // the ceiling counts in its own 10-minute buckets, not per sliding 60
  // minutes (F132).
  CHECK(max_rows_in_an_hour() <= kCeiling + 1u);
  return 0;
}

// What the refund is for, kept: sustained presence refreshing its open
// bundle every 30 s spends one slot per BUNDLE, not one per refresh, so a
// real state change after 45 minutes of it still gets through. (Each
// 10-minute window is a new bundle and a new row, so it does spend one slot
// per window: an hour of unbroken presence would fill a 6/hour ceiling on
// its own. Since F90 the ceiling keeps its last slot for a change of state;
// the F90 tests below hold a state for hours.)
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

// A mix: refreshes inside the gap, then gaps past it, alternating. Openings
// stay at the ceiling in each of the counter's hours; committed rows in a
// sliding hour reach one more (7 measured: commit-at-close against the
// counter's 10-minute buckets, F132).
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
// is a row and spends a slot. Every one is the same state, so the last slot
// of each of the counter's hours stays free for a change of state (F90):
// five openings an hour, exactly 120 a day (144 before F90, which this test
// pinned: the held state's sixth reopening took the slot a transition
// needed). Not starved either: every hour still gets its five.
static int test_a_gap_reopen_spends_exactly_the_ceiling() {
  power_on_counter_anchored();
  const uint32_t start = g_now_ms;
  const uint32_t step = CSI_BUNDLER_MAX_GAP_MS + kSecond;
  while (g_now_ms + step <= start + 24 * kHour) {
    (void)emit("a");
    g_now_ms += step;
  }
  csi_event_flush_bundles();   // the last bundle commits too
  std::printf("  gap reopen, anchored day: %u rows (ceiling allows %u, its last slots kept)\n",
              (unsigned)g_rows, (unsigned)(24u * kCeiling));
  CHECK(g_rows <= 24u * kCeiling);
  CHECK(g_rows == 24u * (kCeiling - 1u));
  return 0;
}

// #1762's window probe: state "a" refreshed every 60 s, so its bundle never
// reaches the quiet gap and closes on its 10-minute window, at a refresh,
// inside that refresh's admit (nothing else ticks). The reopened bundle is a
// row like any other. Beside it the module tries one new state every 10
// minutes. Six openings in each of the counter's hours in all, exactly 144
// rows a day (a sliding 60 minutes of committed rows reaches 8 here, two of
// them the final flush's, which closes two bundles early: F132). Before F80
// every window reopen of "a" was refunded and the new states had the ceiling
// to themselves, 286 rows a day (143 of them "a").
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
  CHECK(g_rows == 24u * kCeiling);
  CHECK(max_rows_in_an_hour() <= 2u * kCeiling);
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

// The ceiling's hour is its own six 10-minute buckets (backlog F132). Six
// new states just before the end of the counter's first bucket fill it; the
// bucket rotates out of the six fifty minutes later, and six more open at
// once. Each six-bucket hour held six openings, yet one sliding 60 minutes
// holds twelve committed rows, about fifty minutes apart end to end. A fix
// that counts a true sliding hour changes this pin.
static int test_a_sliding_hour_can_hold_twice_the_ceiling() {
  power_on_counter_anchored();
  const uint32_t start = g_now_ms;   // the counter's first bucket starts here
  g_now_ms = start + 10 * kMinute - 6 * kSecond;
  for (int i = 0; i < kCeiling; ++i) {
    char s[16];
    std::snprintf(s, sizeof(s), "a%d", i);
    CHECK(emit(s) != 0);
    g_now_ms += kSecond;
  }
  CHECK(emit("over") == 0);                  // the six buckets are full
  while (g_now_ms < start + 60 * kMinute - kSecond) {
    g_now_ms += kSecond;
    csi_bundler_tick();
  }
  CHECK(g_rows == kCeiling);                 // the first six closed on their gap
  CHECK(emit("still") == 0);                 // the first bucket still counts
  g_now_ms = start + 60 * kMinute;           // ...until it rotates out
  for (int i = 0; i < kCeiling; ++i) {
    char s[16];
    std::snprintf(s, sizeof(s), "b%d", i);
    CHECK(emit(s) != 0);
    g_now_ms += kSecond;
  }
  CHECK(emit("over") == 0);
  for (int i = 0; i < 20 * 60; ++i) {
    g_now_ms += kSecond;
    csi_bundler_tick();
  }
  const uint32_t span_s = (g_row_ms[g_rows - 1] - g_row_ms[0]) / kSecond;
  std::printf("  bucket edge: %u rows, %u in one sliding hour, %u s from first to last\n",
              (unsigned)g_rows, (unsigned)max_rows_in_an_hour(), (unsigned)span_s);
  CHECK(g_rows == 2u * kCeiling);
  CHECK(span_s < 51u * 60u);
  CHECK(max_rows_in_an_hour() == 2u * kCeiling);
  return 0;
}

// ── F90: a refresh is never refused; the last slot waits for a change ───

static constexpr uint32_t kBucket = 10 * kMinute;   // the counter's bucket

// A model of one key's bundle, kept here from what this file emitted, by the
// bundler's rule (open from its opening until CSI_BUNDLER_WINDOW_MS after it
// or CSI_BUNDLER_MAX_GAP_MS after its last observation, whichever is first).
// It does not ask the library which emits merge: the model says.
struct KeyModel {
  bool open;
  uint32_t opened_ms;
  uint32_t last_ms;
  uint32_t handle;
  bool closed;          // a bundle of this key has closed
  uint32_t closed_ms;   // when it did (its window's end or its gap's)
};
static bool model_open(KeyModel* k) {
  if (k->open && (g_now_ms - k->opened_ms >= CSI_BUNDLER_WINDOW_MS ||
                  g_now_ms - k->last_ms >= CSI_BUNDLER_MAX_GAP_MS)) {
    k->open = false;
    k->closed = true;
    const uint32_t window_end = k->opened_ms + CSI_BUNDLER_WINDOW_MS;
    const uint32_t gap_end = k->last_ms + CSI_BUNDLER_MAX_GAP_MS;
    k->closed_ms = (int32_t)(window_end - gap_end) < 0 ? window_end : gap_end;
  }
  return k->open;
}

// What a run of emits did. A refresh is an emit the model says merges.
struct Tally {
  uint32_t refreshes;
  uint32_t refreshes_refused;   // must be 0 (F90 part a)
  uint32_t refreshes_moved;     // admitted, but not into the open bundle
  uint32_t attempts;            // openings tried
  uint32_t refused;             // openings refused (a full ceiling, or the last slot)
  uint32_t longest_closed_ms;   // the longest a key went from a close to its next opening
};

// Every opening this run admitted, with the counter's first bucket.
static uint32_t g_anchor_ms = 0;
static uint32_t g_open_at[4096];
static uint32_t g_open_n = 0;

static void start_run(uint8_t ceiling_override) {
  power_on_counter_anchored();
  if (ceiling_override) csi_event_set_module_ceiling("test.ceiling", ceiling_override);
  g_anchor_ms = g_now_ms;
  g_open_n = 0;
}

static void emit_modeled(const char* state, KeyModel* k, Tally* t) {
  const bool refresh = model_open(k);
  const uint32_t h = emit(state);
  if (refresh) {
    ++t->refreshes;
    if (h == 0) {
      ++t->refreshes_refused;
    } else if (h != k->handle) {
      ++t->refreshes_moved;
    } else {
      k->last_ms = g_now_ms;
    }
    return;
  }
  ++t->attempts;
  if (h == 0) {
    ++t->refused;
    return;
  }
  if (k->closed && g_now_ms - k->closed_ms > t->longest_closed_ms) {
    t->longest_closed_ms = g_now_ms - k->closed_ms;
  }
  *k = KeyModel{true, g_now_ms, g_now_ms, h, false, 0};
  if (g_open_n < sizeof(g_open_at) / sizeof(g_open_at[0])) g_open_at[g_open_n++] = g_now_ms;
}

// The most openings in any six consecutive buckets of the counter's.
static uint32_t max_openings_in_six_buckets() {
  static uint32_t per[2048];
  std::memset(per, 0, sizeof(per));
  uint32_t last = 0;
  for (uint32_t i = 0; i < g_open_n; ++i) {
    const uint32_t b = (g_open_at[i] - g_anchor_ms) / kBucket;
    if (b < 2048) ++per[b];
    if (b > last) last = b;
  }
  uint32_t best = 0;
  for (uint32_t b = 0; b <= last && b < 2048; ++b) {
    uint32_t n = 0;
    for (uint32_t j = b; j < b + 6 && j < 2048; ++j) n += per[j];
    if (n > best) best = n;
  }
  return best;
}

// Hold "present" for `hold_ms`, from `phase_ms` into the counter's first
// bucket: the bundler ticked every second (first in each second, as a loop
// pass ticks it), a refresh every 7 s. Then the change of state: "absent".
static void hold_then_change(uint32_t phase_ms, uint32_t hold_ms, Tally* t, KeyModel* change,
                             Tally* tc) {
  g_now_ms += phase_ms;
  const uint32_t start = g_now_ms;
  KeyModel held = {};
  for (uint32_t sec = 0; g_now_ms - start < hold_ms; ++sec) {
    csi_bundler_tick();
    if (sec % 7 == 0) emit_modeled("present", &held, t);
    g_now_ms += kSecond;
  }
  csi_bundler_tick();
  emit_modeled("absent", change, tc);
}

// One state held one, two and three hours, at eight clock phases. No
// refresh of the open bundle is refused, every one merges into it, the
// change of state is admitted at once, and the openings, the change's
// included, stay within the ceiling in every six buckets. The price of the
// kept slot: once an hour the held state has no open bundle until a slot
// ages out, never longer than a bucket (and the 7 s to the next refresh)
// after its bundle closed. Before F90 the
// ceiling filled with the held state's own reopenings within the first hour
// and then refused the open bundle's refreshes and the change alike.
static int test_a_held_state_never_refuses_a_refresh_or_the_change() {
  static const uint32_t kPhases[] = {0, kSecond, 6 * kSecond, 59 * kSecond,
                                     2 * kMinute + 13 * kSecond, 5 * kMinute,
                                     7 * kMinute + 41 * kSecond, kBucket - kSecond};
  for (uint32_t hours = 1; hours <= 3; ++hours) {
    for (uint32_t phase : kPhases) {
      start_run(0);
      Tally t = {}, tc = {};
      KeyModel change = {};
      hold_then_change(phase, hours * kHour, &t, &change, &tc);
      if (t.refreshes_refused != 0 || !change.open) {
        std::fprintf(stderr, "  hold %u h at phase %u s: %u of %u refreshes refused, change %s\n",
                     (unsigned)hours, (unsigned)(phase / kSecond), (unsigned)t.refreshes_refused,
                     (unsigned)t.refreshes, change.open ? "admitted" : "refused");
      }
      CHECK(t.refreshes > 0);
      CHECK(t.refreshes_refused == 0);
      CHECK(t.refreshes_moved == 0);
      CHECK(change.open);                          // admitted at once
      CHECK(tc.refused == 0);
      CHECK(max_openings_in_six_buckets() <= kCeiling);
      CHECK(t.longest_closed_ms <= kBucket + 7 * kSecond);
      if (hours == 3 && phase == 0) {
        std::printf("  hold 3 h: %u refreshes, none refused; %u of %u reopenings refused "
                    "(last slot or full), at most %u s without an open bundle; the change "
                    "admitted at once\n",
                    (unsigned)t.refreshes, (unsigned)t.refused, (unsigned)t.attempts,
                    (unsigned)(t.longest_closed_ms / kSecond));
      }
    }
  }
  return 0;
}

// A whole day of one held state: at most the ceiling's openings in every six
// buckets and at most 144 in the day, the transition at the end admitted.
static int test_a_day_held_stays_within_the_ceiling() {
  start_run(0);
  Tally t = {}, tc = {};
  KeyModel change = {};
  hold_then_change(0, 24 * kHour - kMinute, &t, &change, &tc);
  std::printf("  a day held: %u openings (%u allowed), at most %u in six buckets\n",
              (unsigned)g_open_n, (unsigned)(24u * kCeiling),
              (unsigned)max_openings_in_six_buckets());
  CHECK(t.refreshes_refused == 0);
  CHECK(change.open);
  CHECK(g_open_n <= 24u * kCeiling);
  CHECK(max_openings_in_six_buckets() <= kCeiling);
  return 0;
}

// States that keep changing get no more than the cap: the last-slot rule
// only ever refuses, it never adds a slot. Two states alternating every
// 121 s (every emit an opening), and three alternating every 7 s (each
// state's bundle stays open and reopens at its window), for a day each.
static int test_alternating_states_cannot_exceed_the_cap() {
  {
    start_run(0);
    Tally t = {};
    KeyModel a = {}, b = {};
    const uint32_t start = g_now_ms;
    for (uint32_t k = 0; g_now_ms - start < 24 * kHour; ++k) {
      csi_bundler_tick();
      emit_modeled(k % 2 ? "b" : "a", k % 2 ? &b : &a, &t);
      g_now_ms += CSI_BUNDLER_MAX_GAP_MS + kSecond;
    }
    std::printf("  a / b every 121 s for a day: %u openings, at most %u in six buckets\n",
                (unsigned)g_open_n, (unsigned)max_openings_in_six_buckets());
    CHECK(t.refreshes == 0);                       // every emit was an opening
    CHECK(g_open_n <= 24u * kCeiling);
    CHECK(max_openings_in_six_buckets() <= kCeiling);
  }
  {
    start_run(0);
    Tally t = {};
    KeyModel keys[3] = {};
    const char* const names[3] = {"a", "b", "c"};
    const uint32_t start = g_now_ms;
    for (uint32_t sec = 0; g_now_ms - start < 24 * kHour; ++sec) {
      csi_bundler_tick();
      if (sec % 7 == 0) emit_modeled(names[(sec / 7) % 3], &keys[(sec / 7) % 3], &t);
      g_now_ms += kSecond;
    }
    std::printf("  a / b / c every 7 s for a day: %u openings, at most %u in six buckets, "
                "%u refreshes, none refused\n",
                (unsigned)g_open_n, (unsigned)max_openings_in_six_buckets(), (unsigned)t.refreshes);
    CHECK(t.refreshes > 0);
    CHECK(t.refreshes_refused == 0);
    CHECK(t.refreshes_moved == 0);
    CHECK(g_open_n <= 24u * kCeiling);
    CHECK(max_openings_in_six_buckets() <= kCeiling);
  }
  return 0;
}

// The rule, emit by emit, at the default 6/hour. Five states open; the
// fifth's bundle is closed (as a tamper row closes its own); reopening it
// at the last slot is refused, a refresh of an open bundle still merges at
// the full ceiling, and a different state takes the last slot. A row that
// commits directly (no state name) takes any free slot and is no opening:
// it neither uses nor moves the rule.
static csi_event_values_t stateless() {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_MOTION_SCORE;
  v.motion_score = 40;
  return v;
}
static uint32_t emit_stateless() {
  csi_event_values_t v = stateless();
  return csi_event_emit("test.ceiling", "presence", &v);
}

static int test_the_last_slot_goes_to_a_change_of_state() {
  start_run(0);
  const uint32_t ha = emit("a");
  CHECK(ha != 0);
  static const char* const kMore[] = {"b", "c", "d", "e"};
  for (const char* st : kMore) CHECK(emit(st) != 0);
  csi_bundler_flush_key("test.ceiling", "presence", "e");
  CHECK(emit("e") == 0);                       // the last slot: no change of state
  CHECK(emit("a") == ha);                      // a refresh merges, slot or none
  CHECK(emit("f") != 0);                       // a change of state takes it
  CHECK(emit("g") == 0);                       // full
  CHECK(emit("a") == ha);                      // still merges at the full ceiling
  csi_event_flush_bundles();
  CHECK(g_rows == 6);

  // Rows with no state name are no openings.
  start_run(0);
  CHECK(emit("a") != 0);
  csi_bundler_flush_key("test.ceiling", "presence", "a");
  for (int i = 0; i < 4; ++i) CHECK(emit_stateless() != 0);
  CHECK(emit("a") == 0);                       // the last slot; "a" was the last opening
  CHECK(emit_stateless() != 0);                // a direct row takes it
  CHECK(emit("b") == 0);                       // full
  return 0;
}

// A ceiling of 1 behaves as before F90's last-slot rule: its one slot goes
// to whichever opening comes, the held state's included, and nothing is
// kept for a change. F90's first half still holds: the open bundle's
// refreshes merge (before F90 every one of them was refused, its own
// opening having filled the ceiling).
static int test_a_ceiling_of_one_has_no_slot_to_keep() {
  start_run(1);
  Tally t = {}, tc = {};
  KeyModel change = {};
  hold_then_change(0, 110 * kMinute, &t, &change, &tc);
  CHECK(t.refreshes > 0);
  CHECK(t.refreshes_refused == 0);
  CHECK(g_open_n == 2);                        // at the start, and once the first bucket aged out
  CHECK(g_open_at[1] - g_anchor_ms >= 6 * kBucket);
  CHECK(g_open_at[1] - g_anchor_ms < 6 * kBucket + 7 * kSecond);
  CHECK(!change.open);                         // the one slot is the held state's
  CHECK(max_openings_in_six_buckets() <= 1u);
  return 0;
}

// The override's top, 255. A held state is never refused at all; and the
// last-slot rule holds at 254 of 255 as it does at 5 of 6.
static int test_a_ceiling_of_255() {
  start_run(255);
  Tally t = {}, tc = {};
  KeyModel change = {};
  hold_then_change(0, 3 * kHour, &t, &change, &tc);
  CHECK(t.refreshes_refused == 0);
  CHECK(t.refused == 0);
  CHECK(change.open);

  start_run(255);
  char s[16];
  for (int i = 0; i < 254; ++i) {
    std::snprintf(s, sizeof(s), "s%d", i);
    CHECK(emit(s) != 0);
  }
  csi_bundler_flush_key("test.ceiling", "presence", "s253");
  CHECK(emit("s253") == 0);                    // slot 255, the same state
  CHECK(emit("other") != 0);                   // slot 255, a change
  CHECK(emit("more") == 0);                    // full
  return 0;
}

// The peek answers what admit would, and changes nothing: no close, no
// merge, no opening. csi_bundler_refresh() merges or does nothing.
static int test_the_peek_answers_what_admit_does_and_changes_nothing() {
  power_on();
  uint32_t h = 0, h2 = 0;
  csi_event_values_t v = presence("present");

  CHECK(csi_bundler_peek(nullptr, "presence", &v) == CSI_BUNDLER_DROPPED);
  csi_event_values_t amb = presence("present");
  amb.category = CSI_CATEGORY_AMBIENT;
  CHECK(csi_bundler_peek("test.ceiling", "presence", &amb) == CSI_BUNDLER_COMMIT);
  CHECK(csi_bundler_admit("test.ceiling", "presence", CSI_PRIVACY_P0, &amb, &h) ==
        CSI_BUNDLER_COMMIT);
  csi_event_values_t nostate = stateless();
  CHECK(csi_bundler_peek("test.ceiling", "presence", &nostate) == CSI_BUNDLER_COMMIT);
  csi_event_values_t empty = presence("");
  CHECK(csi_bundler_peek("test.ceiling", "presence", &empty) == CSI_BUNDLER_COMMIT);
  CHECK(!csi_bundler_refresh("test.ceiling", "presence", &nostate, &h2) && h2 == 0);

  CHECK(csi_bundler_peek("test.ceiling", "presence", &v) == CSI_BUNDLER_BUFFERED);
  CHECK(csi_bundler_open_count() == 0);
  CHECK(!csi_bundler_refresh("test.ceiling", "presence", &v, &h2) && h2 == 0);
  CHECK(csi_bundler_open_count() == 0);        // a refresh never opens
  CHECK(csi_bundler_admit("test.ceiling", "presence", CSI_PRIVACY_P0, &v, &h) ==
        CSI_BUNDLER_BUFFERED);

  g_now_ms += CSI_BUNDLER_MAX_GAP_MS - kSecond;
  v = presence("present");
  CHECK(csi_bundler_peek("test.ceiling", "presence", &v) == CSI_BUNDLER_MERGED);
  CHECK(csi_bundler_refresh("test.ceiling", "presence", &v, &h2) && h2 == h);
  CHECK(v.bundled_count == 2);                 // the live bundle, mirrored back

  // Quiet for the whole gap: admit would close it, so the peek says an
  // opening, and closes nothing.
  g_now_ms += CSI_BUNDLER_MAX_GAP_MS;
  v = presence("present");
  CHECK(csi_bundler_peek("test.ceiling", "presence", &v) == CSI_BUNDLER_BUFFERED);
  CHECK(csi_bundler_has_open("test.ceiling", "presence", "present"));
  CHECK(g_rows == 0);
  CHECK(csi_bundler_admit("test.ceiling", "presence", CSI_PRIVACY_P0, &v, &h2) ==
        CSI_BUNDLER_BUFFERED);
  CHECK(h2 != h);
  CHECK(g_rows == 1);

  // The window: refreshed every minute, so the gap never passes; at ten
  // minutes the peek says an opening, and refresh closes it without merging.
  h = h2;
  for (int i = 1; i < 10; ++i) {
    g_now_ms += kMinute;
    v = presence("present");
    CHECK(csi_bundler_peek("test.ceiling", "presence", &v) == CSI_BUNDLER_MERGED);
    CHECK(csi_bundler_admit("test.ceiling", "presence", CSI_PRIVACY_P0, &v, &h2) ==
          CSI_BUNDLER_MERGED);
  }
  g_now_ms += kMinute;
  v = presence("present");
  CHECK(csi_bundler_peek("test.ceiling", "presence", &v) == CSI_BUNDLER_BUFFERED);
  CHECK(g_rows == 1);
  CHECK(!csi_bundler_refresh("test.ceiling", "presence", &v, &h2) && h2 == 0);
  CHECK(g_rows == 2);                          // it closed, as admit's expiry does
  CHECK(csi_bundler_open_count() == 0);        // and nothing opened
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
  if (test_a_sliding_hour_can_hold_twice_the_ceiling()) return 1;
  if (test_the_peek_answers_what_admit_does_and_changes_nothing()) return 1;
  if (test_a_held_state_never_refuses_a_refresh_or_the_change()) return 1;
  if (test_a_day_held_stays_within_the_ceiling()) return 1;
  if (test_alternating_states_cannot_exceed_the_cap()) return 1;
  if (test_the_last_slot_goes_to_a_change_of_state()) return 1;
  if (test_a_ceiling_of_one_has_no_slot_to_keep()) return 1;
  if (test_a_ceiling_of_255()) return 1;
  std::printf("test_csi_bundler_ceiling: %d checks passed\n", g_checks);
  return 0;
}
