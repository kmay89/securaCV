/**
 * @file csi_event.cpp
 * @brief Implementation of the privacy chokepoint, in-memory ring, and the
 *        glue between modules, the bundler, and the host commit hooks.
 *
 * Allocation policy: zero. All buffers are static, sized at compile time.
 *
 * Concurrency: emits originate from the main loop (the CSI HAL features
 * callback drives module ticks which call emit()); commits happen on the
 * same loop. There is no ISR path. The bundler and per-module ceiling
 * counters are therefore single-threaded.
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

#include <string.h>
#include <stdint.h>

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
  }  /* namespace */
#else
  namespace { struct RingLock {}; }  /* host test build is single-threaded */
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
 * 512 rows × ~120 B = ~60 KB. Comfortable on ESP32-S3 with PSRAM. The
 * meta.daily_summary module walks at most 64 rows of this cache, so a
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

csi_event_record_t  g_ring[CSI_EVENT_RING_CAP] = {};
size_t              g_ring_head = 0;   /* next write slot */
uint32_t            g_next_event_id = 1;
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
 * NOTE: monotonic millis() is NOT aligned with wall-clock day, so a
 * device booted mid-afternoon will roll its time_bucket back to 0 at
 * boot+0, not at midnight. The Phase 4 host integration calls
 * csi_event_set_clock_offset_minutes() at first sync to align the bucket
 * index with wall clock; until that lands, time_bucket is consistent
 * within a session and the meta.daily_summary module emits when its
 * own host-supplied clock indicates day-boundary, so the surface
 * inconsistency is contained. Tracked as a follow-up to Phase 4. */
static int32_t s_clock_offset_minutes = 0;

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

uint32_t allocate_event_id() {
  uint32_t id = g_next_event_id++;
  if (id == 0) id = g_next_event_id++;   /* never return 0; that means rejected */
  return id;
}

void persist_to_ring(uint32_t                  event_id,
                     const char*               module_id,
                     const char*               type_name,
                     csi_event_category_t      category,
                     csi_privacy_class_t       privacy,
                     const csi_event_values_t* values) {
  RingLock _lock;
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

void csi_event_set_module_ceiling(const char* module_id, uint8_t override_per_hour) {
  if (!module_id) return;
  ModuleCounter* c = find_or_create_counter(module_id);
  if (c) c->ceiling_override = override_per_hour;
}

uint32_t csi_event_emit(const char*               module_id,
                        const char*               type_name,
                        const csi_event_values_t* in_values) {
  if (!module_id || !type_name || !in_values) return 0;

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

  /* 5. Pre-account the emit against the per-module hourly ceiling. We
   *    increment first and roll back on rejection so that buffered emits
   *    (which only commit later via the bundler) still count toward the
   *    same cap as direct commits. */
  counter->buckets[5] += 1;

  /* 6. Bundling. The bundler buffers same-state events into a single open
   *    bundle, ambient events bypass it, and stateless events fall through. */
  uint32_t event_id = 0;
  csi_bundler_outcome_t outcome = csi_bundler_admit(
      module_id, type_name, decl->privacy, &v, &event_id);

  if (outcome == CSI_BUNDLER_BUFFERED) {
    /* Rolled into an open bundle (or just opened a new one). The bundle
     * will commit later via close_slot()'s commit hooks. */
    csi_module_record_emission_(event_id, module_id);
    return event_id;
  }

  if (outcome == CSI_BUNDLER_DROPPED) {
    counter->buckets[5] -= 1;   /* roll back the pre-account */
    return 0;
  }

  /* outcome == CSI_BUNDLER_COMMIT: this emit is a pass-through (ambient or
   * stateless) and must be persisted directly here. */
  if (event_id == 0) event_id = allocate_event_id();

  /* P0 and qualifying P1 events go to the witness chain; P2 stays local.
   * Ambient events never persist. */
  if (decl->privacy <= CSI_PRIVACY_P1 && v.category != CSI_CATEGORY_AMBIENT) {
    (void)csi_event_commit_witness(event_id, module_id, type_name,
                                   v.category, &v);
  }
  persist_to_ring(event_id, module_id, type_name, v.category, decl->privacy, &v);
  csi_module_record_emission_(event_id, module_id);

  /* Stream callback (host-provided). */
  csi_event_on_committed(event_id, module_id, type_name,
                         v.category, decl->privacy, &v);

  return event_id;
}

void csi_event_flush_bundles(void) {
  csi_bundler_flush_all();
}

size_t csi_event_recent(csi_event_record_t* out, size_t max) {
  if (!out || max == 0) return 0;
  RingLock _lock;
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

void csi_event_test_reset(void) {
  RingLock _lock;
  memset(g_ring, 0, sizeof(g_ring));
  g_ring_head = 0;
  g_next_event_id = 1;
  g_privacy_ceiling = CSI_PRIVACY_P0;
  memset(g_counters, 0, sizeof(g_counters));
  g_counter_count = 0;
  csi_bundler_reset();
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
