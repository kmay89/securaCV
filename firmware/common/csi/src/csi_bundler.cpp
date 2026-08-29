/**
 * @file csi_bundler.cpp
 * @brief Bounded, allocation-free implementation of the bundling rule.
 *
 * One open bundle per (module_id, type_name, state_name) tuple. Bundles
 * close (commit) when:
 *   - a new emit arrives with the same key but the gap > MAX_GAP_MS
 *   - the bundle has been open longer than WINDOW_MS
 *   - csi_bundler_flush_all() is called explicitly
 *   - a new emit arrives but no slot is free (oldest bundle is forced closed)
 *
 * Threading: emits run on the main loop only, but csi_bundler_snapshot_open()
 * is read from the esp_http_server worker task (the /api/events/today
 * handler serializes open bundles so a live alarm is visible before its
 * bundle closes). The slot table therefore takes a FreeRTOS mutex — the same
 * pattern as csi_event.cpp's ring — and, mirroring csi_event_dismiss's rule,
 * the commit hooks (witness signing, SD append, MQTT) NEVER run under the
 * lock: closes copy the slot out under the mutex and run the hooks from the
 * copy after release, still on the emitting (main-loop) task.
 */

#include "csi_bundler.h"

#include <string.h>

#ifdef ARDUINO
  #include <Arduino.h>
  static inline uint32_t bundler_now_ms() { return millis(); }
#else
  #include <time.h>
  static inline uint32_t bundler_now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
  }
#endif

/* Forward declaration of the chokepoint's commit hooks. The bundler calls
 * these when a bundle closes so the host application sees the merged row. */
extern "C" {
bool csi_event_commit_witness(uint32_t                  event_id,
                              const char*               module_id,
                              const char*               type_name,
                              csi_event_category_t      category,
                              const csi_event_values_t* values);
void csi_event_on_committed(uint32_t                  event_id,
                            const char*               module_id,
                            const char*               type_name,
                            csi_event_category_t      category,
                            csi_privacy_class_t       privacy,
                            const csi_event_values_t* values);
}

#ifndef CSI_BUNDLER_SLOTS
#define CSI_BUNDLER_SLOTS 8
#endif

/* Same lock shape as csi_event.cpp's RingLock: a real FreeRTOS mutex on
 * device, a no-op on the single-threaded host-test build. */
#if (defined(ARDUINO) || defined(ESP_PLATFORM)) && !defined(CSI_TEST_HOST_BUILD)
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
  namespace {
    SemaphoreHandle_t g_slot_mutex = nullptr;
    inline void slot_mutex_ensure() {
      if (g_slot_mutex == nullptr) g_slot_mutex = xSemaphoreCreateMutex();
    }
    struct SlotLock {
      SlotLock()  { slot_mutex_ensure(); if (g_slot_mutex) xSemaphoreTake(g_slot_mutex, portMAX_DELAY); }
      ~SlotLock() { if (g_slot_mutex) xSemaphoreGive(g_slot_mutex); }
    };
  }  /* namespace */
#else
  namespace { struct SlotLock {}; }  /* host test build is single-threaded */
#endif

namespace {

struct Slot {
  bool                used;
  char                module_id[CSI_EVENT_NAME_MAX];
  char                type_name[CSI_EVENT_NAME_MAX];
  char                state_name[CSI_EVENT_NAME_MAX];

  uint32_t            event_id;
  uint32_t            opened_ms;
  uint32_t            last_seen_ms;
  csi_event_values_t  values;
  csi_privacy_class_t privacy;
};

Slot     g_slots[CSI_BUNDLER_SLOTS] = {};
uint32_t g_next_event_id = 0x80000000u;  /* high bit set so bundler ids don't
                                            collide with chokepoint ids. */

bool same_key(const Slot* s,
              const char* module_id,
              const char* type_name,
              const char* state_name) {
  return strcmp(s->module_id, module_id) == 0
      && strcmp(s->type_name, type_name) == 0
      && strcmp(s->state_name, state_name) == 0;
}

uint32_t allocate_id() {
  uint32_t id = g_next_event_id++;
  if (id == 0) id = g_next_event_id++;
  return id;
}

/* Finalize a bundle's duration into the 16-bit field. uint32_t subtraction
 * wraps correctly across the ~49.7-day millis() rollover, so a naked
 * subtraction is the right computation as long as the actual interval is
 * < 2^32 ms. Bundle windows are bounded at 10 minutes by
 * CSI_BUNDLER_WINDOW_MS, so the bound holds. */
uint16_t span_seconds(uint32_t opened_ms, uint32_t last_seen_ms) {
  const uint32_t span_ms = last_seen_ms - opened_ms;
  uint32_t span_sec = span_ms / 1000u;
  if (span_sec > 65535u) span_sec = 65535u;
  return (uint16_t)span_sec;
}

/* Close a slot WITH THE LOCK HELD: finalize its fields, park a copy in
 * `pending`, and clear the slot. The commit hooks are deliberately NOT run
 * here — the caller runs run_commit_hooks() on each pending copy after
 * releasing the lock, so witness signing / SD / MQTT never execute under
 * the slot mutex (and never on the httpd task, since only main-loop
 * callers close slots). */
void close_slot_locked(Slot* s, Slot* pending, size_t* npending) {
  if (!s->used) return;
  s->values.duration_sec = span_seconds(s->opened_ms, s->last_seen_ms);
  s->values.present_fields |= CSI_FIELD_DURATION_SEC | CSI_FIELD_BUNDLED_COUNT;
  pending[(*npending)++] = *s;
  s->used = false;
  memset(s, 0, sizeof(*s));
}

/* Commit one closed bundle through the host hooks, from the copy.
 * Witness chain receives only P0 and P1 events (P2 is power-user/local). */
void run_commit_hooks(const Slot* c) {
  if (c->values.category != CSI_CATEGORY_AMBIENT
      && c->privacy <= CSI_PRIVACY_P1) {
    (void)csi_event_commit_witness(c->event_id, c->module_id, c->type_name,
                                   c->values.category, &c->values);
  }
  csi_event_on_committed(c->event_id, c->module_id, c->type_name,
                         c->values.category, c->privacy, &c->values);
}

Slot* find_open_slot(const char* module_id,
                     const char* type_name,
                     const char* state_name) {
  for (size_t i = 0; i < CSI_BUNDLER_SLOTS; ++i) {
    Slot* s = &g_slots[i];
    if (s->used && same_key(s, module_id, type_name, state_name)) return s;
  }
  return nullptr;
}

Slot* find_free_slot(Slot* pending, size_t* npending) {
  for (size_t i = 0; i < CSI_BUNDLER_SLOTS; ++i) {
    if (!g_slots[i].used) return &g_slots[i];
  }
  /* No free slot; force-close the oldest. Compare with signed subtraction so
   * the result is correct across the ~49.7-day millis() rollover — a slot
   * opened just before the wrap and one opened just after must still order
   * correctly. Open bundles are bounded by CSI_BUNDLER_WINDOW_MS (10 min),
   * so the spread between any two slots fits comfortably in int32_t. */
  Slot* oldest = &g_slots[0];
  for (size_t i = 1; i < CSI_BUNDLER_SLOTS; ++i) {
    if ((int32_t)(g_slots[i].opened_ms - oldest->opened_ms) < 0) {
      oldest = &g_slots[i];
    }
  }
  close_slot_locked(oldest, pending, npending);
  return oldest;
}

void expire_overdue(uint32_t now_ms, Slot* pending, size_t* npending) {
  for (size_t i = 0; i < CSI_BUNDLER_SLOTS; ++i) {
    Slot* s = &g_slots[i];
    if (!s->used) continue;
    const bool window_expired = (now_ms - s->opened_ms) >= CSI_BUNDLER_WINDOW_MS;
    const bool gap_too_large  = (now_ms - s->last_seen_ms) >= CSI_BUNDLER_MAX_GAP_MS;
    if (window_expired || gap_too_large) close_slot_locked(s, pending, npending);
  }
}

}  /* namespace */

extern "C" {

csi_bundler_outcome_t csi_bundler_admit(const char*         module_id,
                                        const char*         type_name,
                                        csi_privacy_class_t privacy,
                                        csi_event_values_t* values,
                                        uint32_t*           event_id_out) {
  if (!module_id || !type_name || !values) {
    if (event_id_out) *event_id_out = 0;
    return CSI_BUNDLER_DROPPED;
  }

  /* Ambient bypasses the bundler entirely. */
  if (values->category == CSI_CATEGORY_AMBIENT) {
    if (event_id_out) *event_id_out = 0;
    (void)privacy;
    return CSI_BUNDLER_COMMIT;
  }

  const uint32_t now_ms = bundler_now_ms();

  /* Closes decided under the lock, committed after it (see close_slot_locked).
   * At most every slot can close in one admit, so the bound is exact. */
  Slot pending[CSI_BUNDLER_SLOTS];
  size_t npending = 0;
  csi_bundler_outcome_t outcome;

  {
    SlotLock _lock;

    /* Garbage-collect overdue bundles before any matching attempt. This is
     * the only path that closes a slot due to time; all mutation happens on
     * the main loop, the lock exists for the httpd snapshot reader. */
    expire_overdue(now_ms, pending, &npending);

    /* The state_name acts as the bundling key. If the module didn't supply
     * one we can't bundle — return COMMIT and let the chokepoint persist. */
    if ((values->present_fields & CSI_FIELD_STATE_NAME) == 0
        || values->state_name[0] == '\0') {
      if (event_id_out) *event_id_out = 0;
      outcome = CSI_BUNDLER_COMMIT;
    } else if (Slot* open = find_open_slot(module_id, type_name, values->state_name)) {
      /* Roll into the existing bundle. */
      open->last_seen_ms = now_ms;
      open->values.bundled_count = (uint16_t)(open->values.bundled_count + 1);
      /* Score-style fields take the running max so the row reflects the peak
       * intensity within the bundle. */
      if (values->present_fields & CSI_FIELD_MOTION_SCORE
          && values->motion_score > open->values.motion_score) {
        open->values.motion_score = values->motion_score;
      }
      if (values->present_fields & CSI_FIELD_BREATHING_SCORE
          && values->breathing_score > open->values.breathing_score) {
        open->values.breathing_score = values->breathing_score;
      }
      /* BPM takes the most recent (P1 anyway, only emitted when stable). */
      if (values->present_fields & CSI_FIELD_BREATHING_RATE) {
        open->values.breathing_rate_bpm = values->breathing_rate_bpm;
      }
      /* Confidence promotes monotonically. */
      if (values->present_fields & CSI_FIELD_CONFIDENCE) {
        const char* a = open->values.confidence;
        const char* b = values->confidence;
        auto rank = [](const char* s) -> int {
          if (!s || !s[0]) return 0;
          if (strcmp(s, "tentative") == 0) return 1;
          if (strcmp(s, "observed")  == 0) return 2;
          if (strcmp(s, "confirmed") == 0) return 3;
          return 0;
        };
        if (rank(b) > rank(a)) {
          strncpy(open->values.confidence, b, sizeof(open->values.confidence) - 1);
          open->values.confidence[sizeof(open->values.confidence) - 1] = '\0';
        }
      }
      /* Mirror the live snapshot back to the caller so the dashboard sees the
       * up-to-date bundle row. */
      *values = open->values;
      if (event_id_out) *event_id_out = open->event_id;
      outcome = CSI_BUNDLER_BUFFERED;
    } else {
      /* New bundle. */
      Slot* fresh = find_free_slot(pending, &npending);
      fresh->used = true;
      strncpy(fresh->module_id,  module_id,           sizeof(fresh->module_id)  - 1);
      strncpy(fresh->type_name,  type_name,           sizeof(fresh->type_name)  - 1);
      strncpy(fresh->state_name, values->state_name,  sizeof(fresh->state_name) - 1);
      fresh->module_id[sizeof(fresh->module_id) - 1]   = '\0';
      fresh->type_name[sizeof(fresh->type_name) - 1]   = '\0';
      fresh->state_name[sizeof(fresh->state_name) - 1] = '\0';

      fresh->event_id     = allocate_id();
      fresh->opened_ms    = now_ms;
      fresh->last_seen_ms = now_ms;
      fresh->values       = *values;
      fresh->values.bundled_count = 1;
      fresh->values.present_fields |= CSI_FIELD_BUNDLED_COUNT;
      fresh->privacy      = privacy;

      if (event_id_out) *event_id_out = fresh->event_id;
      /* New bundle: caller should NOT persist directly. The bundle commits
       * later through the close path's commit hooks (witness + on_committed). */
      outcome = CSI_BUNDLER_BUFFERED;
    }
  }  /* lock released */

  for (size_t i = 0; i < npending; ++i) run_commit_hooks(&pending[i]);
  return outcome;
}

void csi_bundler_tick(void) {
  const uint32_t now_ms = bundler_now_ms();
  Slot pending[CSI_BUNDLER_SLOTS];
  size_t npending = 0;
  {
    SlotLock _lock;
    expire_overdue(now_ms, pending, &npending);
  }
  for (size_t i = 0; i < npending; ++i) run_commit_hooks(&pending[i]);
}

void csi_bundler_flush_all(void) {
  Slot pending[CSI_BUNDLER_SLOTS];
  size_t npending = 0;
  {
    SlotLock _lock;
    for (size_t i = 0; i < CSI_BUNDLER_SLOTS; ++i) {
      if (g_slots[i].used) close_slot_locked(&g_slots[i], pending, &npending);
    }
  }
  for (size_t i = 0; i < npending; ++i) run_commit_hooks(&pending[i]);
}

void csi_bundler_reset(void) {
  SlotLock _lock;
  memset(g_slots, 0, sizeof(g_slots));
  g_next_event_id = 0x80000000u;
}

size_t csi_bundler_open_count(void) {
  SlotLock _lock;
  size_t n = 0;
  for (size_t i = 0; i < CSI_BUNDLER_SLOTS; ++i) if (g_slots[i].used) ++n;
  return n;
}

size_t csi_bundler_snapshot_open(csi_event_record_t* out, size_t max) {
  if (!out || max == 0) return 0;
  size_t n = 0;
  {
    SlotLock _lock;
    for (size_t i = 0; i < CSI_BUNDLER_SLOTS && n < max; ++i) {
      const Slot* s = &g_slots[i];
      if (!s->used) continue;
      csi_event_record_t* r = &out[n++];
      memset(r, 0, sizeof(*r));
      r->event_id      = s->event_id;
      r->first_seen_ms = s->opened_ms;
      r->last_seen_ms  = s->last_seen_ms;
      r->category      = s->values.category;
      r->privacy       = s->privacy;
      r->bundled_count = s->values.bundled_count;
      r->values        = s->values;
      /* An open slot's duration_sec is only finalized at close — stamp the
       * LIVE span so the row never claims zero seconds for a bundle that
       * has been open for minutes. */
      r->values.duration_sec = span_seconds(s->opened_ms, s->last_seen_ms);
      strncpy(r->module_id, s->module_id, sizeof(r->module_id) - 1);
      strncpy(r->type_name, s->type_name, sizeof(r->type_name) - 1);
      r->module_id[sizeof(r->module_id) - 1] = '\0';
      r->type_name[sizeof(r->type_name) - 1] = '\0';
    }
  }
  /* Newest activity first, matching the ring's newest-first contract.
   * Signed subtraction keeps the order right across the millis() wrap. */
  for (size_t i = 1; i < n; ++i) {
    csi_event_record_t key = out[i];
    size_t j = i;
    while (j > 0 && (int32_t)(out[j - 1].last_seen_ms - key.last_seen_ms) < 0) {
      out[j] = out[j - 1];
      --j;
    }
    out[j] = key;
  }
  return n;
}

}  /* extern "C" */
