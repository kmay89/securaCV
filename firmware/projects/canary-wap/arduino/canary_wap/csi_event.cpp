/**
 * @file csi_event.cpp
 * @brief Implementation of the privacy chokepoint, in-memory ring, and the
 *        glue between modules, the bundler, and the host commit hooks.
 *
 * Allocation policy: near-zero. Buffers are static and compile-time sized,
 * with one exception — the ~60 KB event ring is lazily ps_malloc'd into PSRAM
 * (see g_ring / ring_ensure) so it is not charged against the internal-DRAM
 * .bss segment, which the full build was tipping into overflow.
 *
 * Concurrency: emits originate from the main loop (the CSI HAL features
 * callback drives module ticks which call emit()), and from the NimBLE host
 * task: ble.scout emits an arrival from its advert callback in both trees,
 * and that admit can close another bundle and commit it on that task. There
 * is no ISR path.
 *
 * Every committed row takes its event id when it commits, from the one
 * allocator below (backlog F46: one id space, see csi_event_id_floor.h).
 * commit_row() takes the id and runs the commit hooks under one recursive
 * COMMIT LOCK, so whichever task commits, ids reach the hooks (witness
 * chain, SD event log, MQTT) in the order they were taken. Without it, two
 * tasks committing at once could hand id N+1 to the hooks before N, and
 * Home Assistant's replay gate refuses N once it has verified N+1. The
 * lock order is commit lock, then ring lock (persist_to_ring runs inside a
 * commit); nothing takes the commit lock while holding the ring lock. A
 * commit hook must not emit: a nested commit on the same task would take
 * the next id and reach the hooks before the row it interrupted.
 * firmware/scripts/check_csi_commit_order.py (run by check_csi_sync.sh)
 * holds the source to that shape.
 *
 * The in-memory event ring (g_ring / g_ring_head), however, is also read by
 * the HTTP server task: csi_event_recent() and csi_event_find() back the
 * /api/events/today endpoint, and csi_event_dismiss() is invoked from the
 * /api/events/dismiss POST handler. Those four ring-touching entry points
 * — plus the producer-side persist_to_ring() called from emit() and the
 * test-only csi_event_test_reset() — take a single FreeRTOS mutex so a web
 * read can never tear against an emit write. The host-test build (no
 * FreeRTOS) compiles the lock out: invariants tests are single-threaded.
 */

#include "csi_event.h"
#include "csi_bundler.h"
#include "csi_event_id_floor.h"   /* kIdSpaceBase: where the one id space starts */

#include <string.h>
#include <stdint.h>
#include <stdlib.h>   /* malloc fallback when PSRAM is unavailable / host build */

#ifdef ARDUINO
  #include <Arduino.h>
  static inline uint32_t csi_event_now_ms() { return millis(); }
#else
  #include <time.h>
  static inline uint32_t csi_event_now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
  }
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * RING MUTEX
 *
 * Guards every read/write of g_ring and g_ring_head. The longest critical
 * section is csi_event_recent()'s walk over CSI_EVENT_RING_CAP rows, which
 * is fine for a FreeRTOS mutex (the producer task can yield while the web
 * task drains). A spinlock with interrupts disabled would be too coarse.
 *
 * On the host-test build (CSI_TEST_HOST_BUILD or non-Arduino, non-ESP_PLATFORM)
 * the test driver is single-threaded, so the lock collapses to a no-op.
 * ────────────────────────────────────────────────────────────────────────── */

#if (defined(ARDUINO) || defined(ESP_PLATFORM)) && !defined(CSI_TEST_HOST_BUILD)
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
  namespace {
    SemaphoreHandle_t g_ring_mutex = nullptr;
    inline void ring_mutex_ensure() {
      if (g_ring_mutex == nullptr) g_ring_mutex = xSemaphoreCreateMutex();
    }
    struct RingLock {
      RingLock()  { ring_mutex_ensure(); if (g_ring_mutex) xSemaphoreTake(g_ring_mutex, portMAX_DELAY); }
      ~RingLock() { if (g_ring_mutex) xSemaphoreGive(g_ring_mutex); }
    };
    /* The commit lock (see the file header). Recursive, so a task that
     * already holds it (set_event_id_floor's caller, a commit's own ring
     * write) never deadlocks on itself. Created on first use through a
     * function-local static: ESP-IDF makes that initialization thread-safe
     * (__cxa_guard_acquire), so two tasks committing first at once still
     * share one mutex. Both Arduino-ESP32 cores build on ESP-IDF with
     * configUSE_RECURSIVE_MUTEXES set. */
    SemaphoreHandle_t commit_mutex() {
      static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
      return m;
    }
    struct CommitLock {
      CommitLock()  { if (commit_mutex()) xSemaphoreTakeRecursive(commit_mutex(), portMAX_DELAY); }
      ~CommitLock() { if (commit_mutex()) xSemaphoreGiveRecursive(commit_mutex()); }
    };
  }  /* namespace */
#else
  namespace { struct RingLock { RingLock() {} }; }    /* host test build is single-threaded */
  namespace { struct CommitLock { CommitLock() {} }; }  /* user-provided: no unused warning */
#endif

/* Internal hook from csi_module.cpp so dismiss can route correctly. */
extern "C" void csi_module_record_emission_(uint32_t event_id, const char* module_id);

/* ──────────────────────────────────────────────────────────────────────────
 * INTERNAL STATE
 * ────────────────────────────────────────────────────────────────────────── */

#ifndef CSI_EVENT_RING_CAP
/* In-memory dashboard cache for the Today receipts sheet and the daily
 * summary's walk. The witness chain is the source of truth for forensic
 * recall; this ring is just what the live UI scrolls over.
 *
 * 512 rows × ~120 B = ~60 KB, lazily allocated in PSRAM (see ring_ensure).
 * The meta.daily_summary module walks at most 64 rows of this cache, so a
 * 512-row ring gives it ~85 % chance of seeing the entire day even when
 * multiple modules emit at their default 6/hour ceiling concurrently.
 * For exact daily counts beyond the cache, walk the witness chain. */
#define CSI_EVENT_RING_CAP 512
#endif

#ifndef CSI_EVENT_MODULE_CEILING_CAP
#define CSI_EVENT_MODULE_CEILING_CAP CSI_MODULE_MAX
#endif
#ifndef CSI_MODULE_MAX
#define CSI_MODULE_MAX 16  /* mirror csi_module.cpp default */
#endif

namespace {

/* The event ring is ~60 KB (CSI_EVENT_RING_CAP rows × sizeof(record)). As a
 * static array that lands in internal DRAM .bss and was tipping dram0_0_seg
 * into overflow. Park it in PSRAM (the XIAO ESP32-S3 ships 8 MB OPI PSRAM,
 * pinned in sketch.yaml) via ps_malloc, falling back to internal RAM on parts
 * without PSRAM. Allocated once, lazily, and retained for the process lifetime
 * — functionally identical to the old static buffer, just no longer charged
 * against dram0_0_seg. Mirrors handle_events_today() in csi_integration.cpp. */
csi_event_record_t* g_ring = nullptr;
size_t              g_ring_head = 0;   /* next write slot */
/* Set by the first live commit (persist_to_ring). From then on
 * csi_event_inject refuses: an earlier boot's rows injected after a live
 * row would sit ahead of it in the ring's recency order. */
bool                g_ring_has_live = false;

constexpr size_t kRingBytes =
    (size_t)CSI_EVENT_RING_CAP * sizeof(csi_event_record_t);

/* Allocate the ring on first use. Returns false only if both PSRAM and the
 * internal-RAM fallback are exhausted, in which case ring-backed introspection
 * degrades gracefully — the durable witness chain is unaffected. Callers hold
 * the RingLock, so this one-time allocation is race-free. */
bool ring_ensure() {
  if (g_ring) return true;
#ifdef ARDUINO
  g_ring = (csi_event_record_t*)ps_malloc(kRingBytes);
  if (!g_ring) g_ring = (csi_event_record_t*)malloc(kRingBytes);
#else
  g_ring = (csi_event_record_t*)malloc(kRingBytes);
#endif
  if (g_ring) memset(g_ring, 0, kRingBytes);
  return g_ring != nullptr;
}
/* The one allocator (backlog F46). It starts at kIdSpaceBase on every
 * device, above every id an earlier firmware handed out, and the host's
 * restored floor only ever moves it up. Written only under the commit lock
 * (allocate_event_id, set_event_id_floor); read under the ring lock. */
uint32_t            g_next_event_id = csi_event_id_floor::kIdSpaceBase;
csi_privacy_class_t g_privacy_ceiling = CSI_PRIVACY_P0;

/* Per-module hourly counters (sliding 60-minute window via 6 × 10-minute
 * sub-buckets). When the bucket the emit() falls into would exceed the
 * ceiling, the emit is suppressed (counted as dropped, not buffered). */
struct ModuleCounter {
  char     id[CSI_EVENT_NAME_MAX];
  uint8_t  ceiling_override;     /* 0 = use manifest default */
  uint16_t buckets[6];           /* counts per 10-minute slot */
  uint32_t bucket_anchor_ms;     /* start time of buckets[0] */
};
ModuleCounter g_counters[CSI_EVENT_MODULE_CEILING_CAP] = {};
size_t        g_counter_count = 0;

ModuleCounter* find_or_create_counter(const char* module_id) {
  for (size_t i = 0; i < g_counter_count; ++i) {
    if (strcmp(g_counters[i].id, module_id) == 0) return &g_counters[i];
  }
  if (g_counter_count >= CSI_EVENT_MODULE_CEILING_CAP) return nullptr;
  ModuleCounter* c = &g_counters[g_counter_count++];
  strncpy(c->id, module_id, CSI_EVENT_NAME_MAX - 1);
  c->id[CSI_EVENT_NAME_MAX - 1] = '\0';
  c->ceiling_override = 0;
  memset(c->buckets, 0, sizeof(c->buckets));
  c->bucket_anchor_ms = csi_event_now_ms();
  return c;
}

void rotate_counter(ModuleCounter* c, uint32_t now_ms) {
  /* Advance anchor by full 10-minute slots, shifting buckets left. */
  while ((now_ms - c->bucket_anchor_ms) >= 10u * 60u * 1000u) {
    for (int i = 0; i < 5; ++i) c->buckets[i] = c->buckets[i + 1];
    c->buckets[5] = 0;
    c->bucket_anchor_ms += 10u * 60u * 1000u;
  }
}

uint16_t counter_window_total(const ModuleCounter* c) {
  uint16_t total = 0;
  for (int i = 0; i < 6; ++i) total += c->buckets[i];
  return total;
}

const csi_module_t* lookup_module(const char* id) {
  return csi_module_find(id);
}

const csi_event_decl_t* lookup_decl(const csi_module_t* m, const char* type_name) {
  if (!m) return nullptr;
  for (size_t i = 0; i < m->event_count; ++i) {
    if (strcmp(m->events[i].type_name, type_name) == 0) return &m->events[i];
  }
  return nullptr;
}

/* Enforce the allow-list: zero any field whose bit is set in present_fields
 * but not in the manifest's allowed_fields. */
void apply_allow_list(csi_event_values_t* v, uint32_t allowed) {
  const uint32_t present = v->present_fields;
  const uint32_t blocked = present & ~allowed;
  if (blocked == 0) return;

  if (blocked & CSI_FIELD_STATE_NAME)        v->state_name[0]       = '\0';
  if (blocked & CSI_FIELD_CONFIDENCE)        v->confidence[0]       = '\0';
  if (blocked & CSI_FIELD_DOMINANT_SIGNAL)   v->dominant_signal[0]  = '\0';
  if (blocked & CSI_FIELD_NOTE)              v->note[0]             = '\0';
  if (blocked & CSI_FIELD_DURATION_SEC)      v->duration_sec        = 0;
  if (blocked & CSI_FIELD_TIME_BUCKET)       v->time_bucket         = 0;
  if (blocked & CSI_FIELD_MOTION_SCORE)      v->motion_score        = 0;
  if (blocked & CSI_FIELD_BREATHING_SCORE)   v->breathing_score     = 0;
  if (blocked & CSI_FIELD_BREATHING_RATE)    v->breathing_rate_bpm  = 0;
  if (blocked & CSI_FIELD_BUNDLED_COUNT)     v->bundled_count       = 0;
  if (blocked & CSI_FIELD_DISMISSED)         v->dismissed           = 0;

  v->present_fields &= allowed;
}

/* Coarsen any timestamp-bearing field. Currently only `time_bucket` is in
 * scope, and it's already a 10-minute bucket index. We re-derive it from
 * the current monotonic time so a module that forgot to set it can't leak
 * a finer-grained value (e.g. a millisecond counter cast into the slot).
 *
 * NOTE: monotonic millis() is NOT aligned with wall-clock day on its own —
 * a device booted mid-afternoon would roll its time_bucket back to 0 at
 * boot+0, not at midnight. Both hosts therefore call
 * csi_event_set_clock_offset_minutes() from their GPS clock sync (the one
 * wall-clock source either tree has; canary main.cpp's updateCsiClockOffset,
 * canary-wap's update_csi_clock_offset), re-deriving the offset on every
 * pass with a set clock so it stays drift-corrected and survives millis()
 * rollover. Until the first GPS fix the offset is 0 and time_bucket is
 * session-relative. The hosts derive the offset from LOCAL wall time
 * (tz_rule::local_minute_of_day, common/time/tz_rule.h): once the household
 * time zone is set (repo sweep F28) bucket 0 and the quiet-hours window are
 * the household's midnight; while no zone is set it is UTC midnight, as
 * before. The per-pass recompute is also what carries a DST change. */
static int32_t s_clock_offset_minutes = 0;

/* Quiet-hours gating state. Set by csi_event_set_quiet_window(); read at
 * the top of every emit() to decide whether the event should be held back
 * (suppressed in favor of an end-of-window summary) or pass through.
 *
 * Anomaly events bypass the gate entirely — that's the whole point of the
 * night-time category. The summary itself uses module_id "meta.quiet_hours"
 * which is special-cased to pass straight through; without that escape
 * hatch the recursive emit would be held against itself. */
static bool     s_qh_enabled        = false;
static uint16_t s_qh_start_min      = 0;
static uint16_t s_qh_end_min        = 0;
static bool     s_qh_was_in_window  = false;
static uint16_t s_qh_held_count     = 0;

static int compute_minute_of_day_() {
  const int64_t mono_minutes = (int64_t)(csi_event_now_ms() / 60000u);
  const int64_t wall_minutes = mono_minutes + s_clock_offset_minutes;
  int64_t mod = wall_minutes % 1440;
  if (mod < 0) mod += 1440;
  return (int)mod;
}

static bool quiet_window_active_(int cur_min) {
  if (!s_qh_enabled) return false;
  if (s_qh_start_min == s_qh_end_min) return false;
  if (s_qh_start_min < s_qh_end_min) {
    return cur_min >= (int)s_qh_start_min && cur_min < (int)s_qh_end_min;
  }
  /* Crosses midnight: e.g. 23:00..07:00 means 23:00..23:59 OR 00:00..06:59. */
  return cur_min >= (int)s_qh_start_min || cur_min < (int)s_qh_end_min;
}

void coarsen_time_fields(csi_event_values_t* v) {
  if (!(v->present_fields & CSI_FIELD_TIME_BUCKET)) return;
  const int64_t mono_minutes = (int64_t)(csi_event_now_ms() / 60000u);
  const int64_t wall_minutes = mono_minutes + s_clock_offset_minutes;
  /* mod-144 in 10-minute buckets, modulo-safe for negative wall_minutes. */
  int64_t bucket = (wall_minutes / 10) % 144;
  if (bucket < 0) bucket += 144;
  v->time_bucket = (uint8_t)bucket;
}

/* Strings entering the ring must be ASCII printable + nul. Anything else
 * gets replaced with '?' so a buggy module can't smuggle binary side-data
 * through. Length is bounded by the field type. */
void sanitize_string_field(char* s, size_t cap) {
  bool seen_nul = false;
  for (size_t i = 0; i < cap; ++i) {
    if (seen_nul) { s[i] = '\0'; continue; }
    char c = s[i];
    if (c == '\0') { seen_nul = true; continue; }
    if (c < 0x20 || c > 0x7e) s[i] = '?';
  }
  s[cap - 1] = '\0';
}

void sanitize_strings(csi_event_values_t* v) {
  sanitize_string_field(v->state_name,      sizeof(v->state_name));
  sanitize_string_field(v->confidence,      sizeof(v->confidence));
  sanitize_string_field(v->dominant_signal, sizeof(v->dominant_signal));
  sanitize_string_field(v->note,            sizeof(v->note));
}

/* Weakly-overridable hook fired whenever the allocator hands out a
 * fresh event_id. The standalone library default is a no-op so a
 * third-party Arduino sketch links cleanly without supplying an NVS
 * back-end. canary-wap's csi_integration.cpp provides a strong
 * override that throttle-persists the floor so allocations stay
 * globally unique across reboots — without that the watermark in
 * csi_mqtt's reconnect backfill would be ambiguous (PR #395). */
extern "C" __attribute__((weak))
void csi_event_on_id_advance(uint32_t /*new_id*/) {}

/* Called only by commit_row(), under the commit lock. */
uint32_t allocate_event_id() {
  uint32_t id = g_next_event_id++;
  if (id == 0) id = g_next_event_id++;   /* never return 0; that means rejected */
  csi_event_on_id_advance(id);
  return id;
}

/* Synthesise the end-of-window summary by emitting one held_summary event
 * through the meta.quiet_hours module. The summary uses the `note` field
 * (not state_name) because stateless events bypass the bundler — without
 * that bypass the bundler would stomp `bundled_count` to 1 on slot
 * creation. The chokepoint persists this row directly.
 *
 * The call recurses into emit(); the module_id check in emit() lets it
 * pass straight through (no gating). If meta.quiet_hours isn't registered
 * (host didn't wire it up), the emit silently returns 0 and the held
 * count is still cleared so the next window starts from a known state. */
void flush_quiet_summary_() {
  if (s_qh_held_count == 0) return;
  const uint16_t held = s_qh_held_count;
  s_qh_held_count = 0;

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_NOTE
                   | CSI_FIELD_BUNDLED_COUNT
                   | CSI_FIELD_TIME_BUCKET;
  v.bundled_count  = held;
  /* `note` carries a stable schema the dashboard's Today sheet matches
   * on to render the moon glyph + held-count copy. */
  static const char kNote[] = "quiet_hours";
  for (size_t i = 0; i < sizeof(kNote); ++i) v.note[i] = kNote[i];

  (void)csi_event_emit("meta.quiet_hours", "held_summary", &v);
}

void persist_to_ring(uint32_t                  event_id,
                     const char*               module_id,
                     const char*               type_name,
                     csi_event_category_t      category,
                     csi_privacy_class_t       privacy,
                     const csi_event_values_t* values) {
  RingLock _lock;
  if (!ring_ensure()) return;
  csi_event_record_t* rec = &g_ring[g_ring_head];
  memset(rec, 0, sizeof(*rec));
  rec->event_id      = event_id;
  rec->first_seen_ms = csi_event_now_ms();
  rec->last_seen_ms  = rec->first_seen_ms;
  rec->category      = category;
  rec->privacy       = privacy;
  rec->bundled_count = values->bundled_count;
  rec->values        = *values;
  strncpy(rec->module_id, module_id, CSI_EVENT_NAME_MAX - 1);
  strncpy(rec->type_name, type_name, CSI_EVENT_NAME_MAX - 1);
  rec->module_id[CSI_EVENT_NAME_MAX - 1] = '\0';
  rec->type_name[CSI_EVENT_NAME_MAX - 1] = '\0';
  g_ring_head = (g_ring_head + 1) % CSI_EVENT_RING_CAP;
  g_ring_has_live = true;
}

/* Commit one row (backlog F46): take its id from the one allocator and run
 * the commit hooks, under the commit lock, so ids reach the hooks in the
 * order they were taken whichever task commits. Both commit paths come
 * here: emit()'s direct commit (ambient and stateless rows, which also land
 * in the ring) and a bundle's close (csi_event_commit_bundle_, called by
 * csi_bundler.cpp from the copy it made under its slot lock). P0 and
 * qualifying P1 rows go to the witness chain; P2 stays local; ambient rows
 * never persist. Returns the id. */
uint32_t commit_row(const char*               module_id,
                    const char*               type_name,
                    csi_privacy_class_t       privacy,
                    const csi_event_values_t* v,
                    bool                      to_ring) {
  CommitLock _commit;
  const uint32_t event_id = allocate_event_id();
  if (privacy <= CSI_PRIVACY_P1 && v->category != CSI_CATEGORY_AMBIENT) {
    (void)csi_event_commit_witness(event_id, module_id, type_name,
                                   v->category, v);
  }
  if (to_ring) persist_to_ring(event_id, module_id, type_name, v->category, privacy, v);
  csi_module_record_emission_(event_id, module_id);
  /* Stream callback (host-provided). */
  csi_event_on_committed(event_id, module_id, type_name,
                         v->category, privacy, v);
  return event_id;
}

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * PUBLIC API
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" {

void csi_event_values_init(csi_event_values_t* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
}

void csi_event_set_privacy_ceiling(csi_privacy_class_t ceiling) {
  g_privacy_ceiling = ceiling;
}

csi_privacy_class_t csi_event_get_privacy_ceiling(void) {
  return g_privacy_ceiling;
}

void csi_event_set_clock_offset_minutes(int32_t offset_minutes) {
  s_clock_offset_minutes = offset_minutes;
}

uint8_t csi_event_current_bucket(void) {
  /* Same derivation as coarsen_time_fields() — callers stamping a coarse
   * bucket into their own artifacts (sealed-snapshot vault header) must
   * never drift from the chokepoint's coarsening. Loop task only. */
  const int64_t mono_minutes = (int64_t)(csi_event_now_ms() / 60000u);
  const int64_t wall_minutes = mono_minutes + s_clock_offset_minutes;
  int64_t bucket = (wall_minutes / 10) % 144;
  if (bucket < 0) bucket += 144;
  return (uint8_t)bucket;
}

void csi_event_set_quiet_window(uint16_t start_min,
                                uint16_t end_min,
                                bool     enabled) {
  /* Clamp to [0, 1440). A range outside that wouldn't crash anything but
   * would silently never trigger; reject obviously-broken values up front. */
  if (start_min >= 1440) start_min = (uint16_t)(start_min % 1440);
  if (end_min   >= 1440) end_min   = (uint16_t)(end_min   % 1440);
  /* The setter is called from the HTTP server task on /api/settings POST,
   * while emit() runs on the main loop task. Keeping this purely a
   * three-word state update — no recursive emit, no held-buffer flush —
   * preserves the chokepoint's single-task invariant. The next emit
   * (1 Hz from the features callback on the main loop) detects the
   * in→out transition and flushes the summary itself. Single-word
   * writes are atomic on ESP32, so a concurrent emit reading these
   * fields sees either the old or the new value, never a torn mix. */
  s_qh_start_min = start_min;
  s_qh_end_min   = end_min;
  s_qh_enabled   = enabled;
}

void csi_event_set_module_ceiling(const char* module_id, uint8_t override_per_hour) {
  if (!module_id) return;
  ModuleCounter* c = find_or_create_counter(module_id);
  if (c) c->ceiling_override = override_per_hour;
}

uint32_t csi_event_emit(const char*               module_id,
                        const char*               type_name,
                        const csi_event_values_t* in_values) {
  if (!module_id || !type_name || !in_values) return 0;

  /* Quiet-hours window-close detection. If we just transitioned from
   * in-window to out-of-window since the last emit, flush the held
   * summary BEFORE processing the current event. The meta.quiet_hours
   * module is special-cased: its summary emits MUST pass straight
   * through, otherwise the recursive flush would gate against itself. */
  const bool is_qh_summary_module = (strcmp(module_id, "meta.quiet_hours") == 0);
  bool       qh_in_window         = false;
  if (!is_qh_summary_module) {
    const int cur_min = compute_minute_of_day_();
    qh_in_window = quiet_window_active_(cur_min);
    if (s_qh_was_in_window && !qh_in_window) {
      s_qh_was_in_window = false;
      flush_quiet_summary_();
    } else {
      s_qh_was_in_window = qh_in_window;
    }
  }

  /* 1. Module + event-type lookup. Unknown ids and types are silent drops. */
  const csi_module_t* m = lookup_module(module_id);
  if (!m) return 0;
  const csi_event_decl_t* decl = lookup_decl(m, type_name);
  if (!decl) return 0;

  /* 2. Privacy ceiling enforcement. */
  if (decl->privacy > g_privacy_ceiling) return 0;

  /* 3. Per-module hourly ceiling. */
  ModuleCounter* counter = find_or_create_counter(module_id);
  if (!counter) return 0;
  const uint32_t now_ms = csi_event_now_ms();
  rotate_counter(counter, now_ms);
  const uint8_t ceiling = counter->ceiling_override
                          ? counter->ceiling_override
                          : decl->default_ceiling_per_hour;
  if (ceiling > 0 && counter_window_total(counter) >= ceiling) {
    /* Ceiling exceeded — caller's emit is dropped. The bundler is the
     * primary anti-noise mechanism; the ceiling is a secondary guard. */
    return 0;
  }

  /* 4. Copy and clean values. */
  csi_event_values_t v = *in_values;
  sanitize_strings(&v);
  apply_allow_list(&v, decl->allowed_fields);
  coarsen_time_fields(&v);

  /* 4b. Quiet-hours hold. While the configured window is active, every
   *     non-anomaly event from a non-meta module is suppressed and a
   *     single counter is bumped instead. Anomaly events bypass the
   *     gate (the night-time category is precisely when unusual
   *     activity matters). The window-close transition detected in
   *     step 0 above flushes the count as one summary row. */
  if (qh_in_window
      && !is_qh_summary_module
      && v.category != CSI_CATEGORY_ANOMALY) {
    if (s_qh_held_count < 0xffff) s_qh_held_count++;
    return 0;
  }

  /* 5. Pre-account the emit against the per-module hourly ceiling. We
   *    increment first and roll back on rejection so that buffered emits
   *    (which only commit later via the bundler) still count toward the
   *    same cap as direct commits. A same-key REFRESH of a bundle that is
   *    already open is the one exception (see the BUFFERED branch), so we
   *    ask the bundler up front whether this admit would merge. */
  const bool refreshes_open_bundle =
      (v.present_fields & CSI_FIELD_STATE_NAME) && v.state_name[0] != '\0'
      && csi_bundler_has_open(module_id, type_name, v.state_name);
  counter->buckets[5] += 1;

  /* 6. Bundling. The bundler buffers same-state events into a single open
   *    bundle, ambient events bypass it, and stateless events fall through. */
  uint32_t handle = 0;
  csi_bundler_outcome_t outcome = csi_bundler_admit(
      module_id, type_name, decl->privacy, &v, &handle);

  if (outcome == CSI_BUNDLER_BUFFERED) {
    /* Rolled into an open bundle, or just opened a new one. An OPENING is
     * a future committed row and keeps its ceiling slot. A REFRESH of a
     * bundle that was already open produces no new row and must not
     * consume one: core.presence re-emits its open state at 5 / 20 / every
     * 60 windows to refresh duration and confidence, and with a 6/hour
     * ceiling those refreshes exhausted the cap in ~3 minutes of sustained
     * presence, after which the REAL state transitions were silently
     * dropped.
     *
     * The bundle has no event id yet: it takes one when it commits
     * (commit_row), and its dismiss route is recorded then. What returns
     * is the open bundle's handle, in [kHandleBase, kIdSpaceBase). */
    if (refreshes_open_bundle) counter->buckets[5] -= 1;
    return handle;
  }

  if (outcome == CSI_BUNDLER_DROPPED) {
    counter->buckets[5] -= 1;   /* roll back the pre-account */
    return 0;
  }

  /* outcome == CSI_BUNDLER_COMMIT: this emit is a pass-through (ambient or
   * stateless) and is committed here, into the ring too. */
  return commit_row(module_id, type_name, decl->privacy, &v, /*to_ring=*/true);
}

/* A closed bundle, from the copy csi_bundler.cpp made under its slot lock.
 * Internal to the CSI library (declared in csi_bundler.cpp). Closed bundles
 * do not enter the ring (unchanged by F46). */
uint32_t csi_event_commit_bundle_(const char*               module_id,
                                  const char*               type_name,
                                  csi_privacy_class_t       privacy,
                                  const csi_event_values_t* values) {
  if (!module_id || !type_name || !values) return 0;
  return commit_row(module_id, type_name, privacy, values, /*to_ring=*/false);
}

void csi_event_flush_bundles(void) {
  csi_bundler_flush_all();
}

size_t csi_event_recent(csi_event_record_t* out, size_t max) {
  if (!out || max == 0) return 0;
  RingLock _lock;
  if (!g_ring) return 0;   /* never allocated ⇒ no events to report */
  size_t copied = 0;
  /* Walk backwards from the head, skipping empty slots. */
  for (size_t step = 0; step < CSI_EVENT_RING_CAP && copied < max; ++step) {
    size_t idx = (g_ring_head + CSI_EVENT_RING_CAP - 1 - step) % CSI_EVENT_RING_CAP;
    if (g_ring[idx].event_id == 0) continue;
    out[copied++] = g_ring[idx];
  }
  return copied;
}

bool csi_event_find(uint32_t event_id, csi_event_record_t* out) {
  if (event_id == 0 || !out) return false;
  RingLock _lock;
  if (!g_ring) return false;   /* never allocated ⇒ event cannot exist */
  for (size_t i = 0; i < CSI_EVENT_RING_CAP; ++i) {
    if (g_ring[i].event_id == event_id) {
      *out = g_ring[i];
      return true;
    }
  }
  return false;
}

bool csi_event_dismiss(uint32_t event_id) {
  if (event_id == 0) return false;
  bool found = false;
  {
    RingLock _lock;
    if (!g_ring) return false;   /* never allocated ⇒ nothing to dismiss */
    for (size_t i = 0; i < CSI_EVENT_RING_CAP; ++i) {
      if (g_ring[i].event_id == event_id) {
        g_ring[i].values.dismissed = 1;
        g_ring[i].values.present_fields |= CSI_FIELD_DISMISSED;
        found = true;
        break;
      }
    }
  }
  /* csi_module_dispatch_dismiss() is invoked outside the ring lock so the
   * module callback can't accidentally re-enter the ring (e.g. via emit()) and
   * deadlock against the same non-recursive mutex. */
  if (found) csi_module_dispatch_dismiss(event_id);
  return found;
}

bool csi_event_inject(const csi_event_record_t* rec) {
  if (!rec || rec->event_id == 0) return false;

  /* The manifest, not the card, says what this type is and what it may
   * carry. An unknown module or type (renamed, removed, or never ours) is
   * refused, as emit() refuses it. */
  const csi_module_t* m = lookup_module(rec->module_id);
  if (!m) return false;
  const csi_event_decl_t* decl = lookup_decl(m, rec->type_name);
  if (!decl) return false;

  /* Clean a copy the way emit() cleans values, before taking the lock. */
  csi_event_record_t clean;
  memset(&clean, 0, sizeof(clean));
  clean.event_id = rec->event_id;
  clean.category = (rec->category == CSI_CATEGORY_AMBIENT
                    || rec->category == CSI_CATEGORY_ANOMALY)
                   ? rec->category : CSI_CATEGORY_EVENT;
  clean.privacy  = decl->privacy;
  strncpy(clean.module_id, m->id, CSI_EVENT_NAME_MAX - 1);
  strncpy(clean.type_name, decl->type_name, CSI_EVENT_NAME_MAX - 1);
  clean.module_id[CSI_EVENT_NAME_MAX - 1] = '\0';
  clean.type_name[CSI_EVENT_NAME_MAX - 1] = '\0';
  /* first_seen_ms / last_seen_ms stay 0: the earlier boot's monotonic clock
   * means nothing on this one. */

  csi_event_values_t v = rec->values;
  v.category       = clean.category;
  v.bundled_count  = rec->bundled_count;
  /* Every field the record could carry is treated as present, so the
   * allow-list below zeroes each one the manifest does not grant. */
  const uint8_t dismissed = rec->values.dismissed ? 1 : 0;
  v.present_fields = 0xFFFFFFFFu;
  sanitize_strings(&v);
  apply_allow_list(&v, decl->allowed_fields);
  if (v.time_bucket >= 144) v.time_bucket = 0;
  v.dismissed = dismissed;
  if (dismissed) v.present_fields |= CSI_FIELD_DISMISSED;
  clean.values        = v;
  clean.bundled_count = v.bundled_count;

  RingLock _lock;
  /* Privacy ceiling, read under the same lock as the rest of the decision.
   * The manifest's class is the one checked. */
  if (decl->privacy > g_privacy_ceiling) return false;
  /* An id this boot can still allocate is not an earlier boot's. */
  if (rec->event_id >= g_next_event_id) return false;
  /* Recency order: nothing older may land ahead of a live row. */
  if (g_ring_has_live) return false;
  if (!ring_ensure()) return false;
  for (size_t i = 0; i < CSI_EVENT_RING_CAP; ++i) {
    if (g_ring[i].event_id == rec->event_id) return false;   /* duplicate */
  }
  g_ring[g_ring_head] = clean;
  g_ring_head = (g_ring_head + 1) % CSI_EVENT_RING_CAP;
  return true;
}

void csi_event_test_reset(void) {
  RingLock _lock;
  if (g_ring) memset(g_ring, 0, kRingBytes);   /* leave unallocated rings lazy */
  g_ring_head = 0;
  g_ring_has_live = false;
  g_next_event_id = csi_event_id_floor::kIdSpaceBase;
  g_privacy_ceiling = CSI_PRIVACY_P0;
  memset(g_counters, 0, sizeof(g_counters));
  g_counter_count = 0;
  s_qh_enabled       = false;
  s_qh_start_min     = 0;
  s_qh_end_min       = 0;
  s_qh_was_in_window = false;
  s_qh_held_count    = 0;
  csi_bundler_reset();
}

/* ──────────────────────────────────────────────────────────────────────────
 * EVENT-ID FLOOR  (host-side persistence hook)
 *
 * Lets canary-wap's csi_integration.cpp NVS-persist g_next_event_id so
 * IDs stay globally unique across reboots — without this, csi_mqtt's
 * reconnect-backfill watermark gets ambiguous (a fresh boot's id=1
 * collides with a previous-boot's id=1, and the watermark logic can't
 * tell them apart). PR #395 worked around it by truncating the SD log
 * on every cold boot; PR #397 lifts that workaround once IDs are
 * globally monotonic.
 *
 * `floor` is the minimum id the next allocation must be at-or-above.
 * Set it from NVS at boot to whatever value the previous run last
 * persisted, plus a safety margin (the host writes "floor + N" every N
 * allocations, so a hard reset between persists loses at most N ids
 * but never reuses one).
 *
 * The setter only ever moves g_next_event_id FORWARD — a stale or
 * lower floor is ignored so a malicious / corrupt NVS read can't
 * rewind ids back into the live range. ────────────────────────────── */

void csi_event_set_event_id_floor(uint32_t floor) {
  CommitLock _commit;   /* the allocator's writer lock; then the ring's */
  RingLock _lock;
  if (floor > g_next_event_id) g_next_event_id = floor;
}

uint32_t csi_event_get_next_event_id(void) {
  RingLock _lock;
  return g_next_event_id;
}

/* ──────────────────────────────────────────────────────────────────────────
 * WEAK DEFAULT COMMIT HOOKS
 *
 * These exist so the library compiles standalone (no SecuraCV witness chain,
 * no SSE stream). The canary firmware overrides them with strong definitions
 * in the host translation unit.
 * ────────────────────────────────────────────────────────────────────────── */

__attribute__((weak))
bool csi_event_commit_witness(uint32_t,
                              const char*,
                              const char*,
                              csi_event_category_t,
                              const csi_event_values_t*) {
  /* No host witness chain — silently no-op. */
  return false;
}

__attribute__((weak))
void csi_event_on_committed(uint32_t,
                            const char*,
                            const char*,
                            csi_event_category_t,
                            csi_privacy_class_t,
                            const csi_event_values_t*) {
  /* No host stream callback — silently no-op. */
}

}  /* extern "C" */
