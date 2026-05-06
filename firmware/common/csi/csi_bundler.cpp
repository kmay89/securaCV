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

void close_slot(Slot* s) {
  if (!s->used) return;
  /* Update final duration before commit. */
  const uint32_t span_ms = (s->last_seen_ms >= s->opened_ms)
                           ? (s->last_seen_ms - s->opened_ms) : 0;
  uint32_t span_sec = span_ms / 1000u;
  if (span_sec > 65535u) span_sec = 65535u;
  s->values.duration_sec = (uint16_t)span_sec;
  s->values.present_fields |= CSI_FIELD_DURATION_SEC | CSI_FIELD_BUNDLED_COUNT;

  /* Commit through the host hooks.
   * Witness chain receives only P0 and P1 events (P2 is power-user/local). */
  if (s->values.category != CSI_CATEGORY_AMBIENT
      && s->privacy <= CSI_PRIVACY_P1) {
    (void)csi_event_commit_witness(s->event_id, s->module_id, s->type_name,
                                   s->values.category, &s->values);
  }
  csi_event_on_committed(s->event_id, s->module_id, s->type_name,
                         s->values.category, s->privacy, &s->values);
  s->used = false;
  memset(s, 0, sizeof(*s));
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

Slot* find_free_slot() {
  for (size_t i = 0; i < CSI_BUNDLER_SLOTS; ++i) {
    if (!g_slots[i].used) return &g_slots[i];
  }
  /* No free slot; force-close the oldest. */
  Slot* oldest = &g_slots[0];
  for (size_t i = 1; i < CSI_BUNDLER_SLOTS; ++i) {
    if (g_slots[i].opened_ms < oldest->opened_ms) oldest = &g_slots[i];
  }
  close_slot(oldest);
  return oldest;
}

void expire_overdue(uint32_t now_ms) {
  for (size_t i = 0; i < CSI_BUNDLER_SLOTS; ++i) {
    Slot* s = &g_slots[i];
    if (!s->used) continue;
    const bool window_expired = (now_ms - s->opened_ms) >= CSI_BUNDLER_WINDOW_MS;
    const bool gap_too_large  = (now_ms - s->last_seen_ms) >= CSI_BUNDLER_MAX_GAP_MS;
    if (window_expired || gap_too_large) close_slot(s);
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

  /* Garbage-collect overdue bundles before any matching attempt. This is
   * the only path that closes a slot due to time; emit is single-threaded
   * so there's no race with the running module. */
  expire_overdue(now_ms);

  /* The state_name acts as the bundling key. If the module didn't supply
   * one we can't bundle — return COMMIT and let the chokepoint persist. */
  if ((values->present_fields & CSI_FIELD_STATE_NAME) == 0
      || values->state_name[0] == '\0') {
    if (event_id_out) *event_id_out = 0;
    return CSI_BUNDLER_COMMIT;
  }

  Slot* open = find_open_slot(module_id, type_name, values->state_name);
  if (open) {
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
    return CSI_BUNDLER_BUFFERED;
  }

  /* New bundle. */
  Slot* fresh = find_free_slot();
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
   * later through close_slot()'s commit hooks (witness + on_committed). */
  return CSI_BUNDLER_BUFFERED;
}

void csi_bundler_flush_all(void) {
  for (size_t i = 0; i < CSI_BUNDLER_SLOTS; ++i) {
    if (g_slots[i].used) close_slot(&g_slots[i]);
  }
}

void csi_bundler_reset(void) {
  memset(g_slots, 0, sizeof(g_slots));
  g_next_event_id = 0x80000000u;
}

size_t csi_bundler_open_count(void) {
  size_t n = 0;
  for (size_t i = 0; i < CSI_BUNDLER_SLOTS; ++i) if (g_slots[i].used) ++n;
  return n;
}

}  /* extern "C" */
