/*
 * Host privacy + behavior test for the wifi.channel_activity CSI module.
 *
 * Compiles the REAL module logic (staged copy) against stub implementations of
 * the chokepoint symbols (csi_event_emit / csi_event_values_init) and the
 * settings reader, capturing what the module tries to emit. No Arduino, no
 * ESP-IDF, no witness chain — just the pure detection logic.
 *
 * What it pins (spec/canary_free_signals_v0.md Invariants A/E/F + the ambient,
 * never-persisted contract):
 *   - The module only ever emits CSI_CATEGORY_AMBIENT (which csi_event.cpp
 *     never commits to the witness chain / SD / NVS).
 *   - Its manifest allow-list carries ONLY coarse P0 fields
 *     (state_name / time_bucket / motion_score) — no identity field exists.
 *   - The emitted event is a fixed literal ("channel_active") with a bounded
 *     0..100 intensity, regardless of the (arbitrary) input feature bytes —
 *     so no raw aggregate byte can leak through as an identifier.
 *   - Behavior: warmup suppresses emits; a quiet channel stays silent; a burst
 *     above the room's own baseline fires exactly one glow (then cools down).
 */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "csi_types.h"
#include "csi_module.h"
#include "csi_event.h"
#include "wifi_channel_activity.h"

static int g_checks = 0;
#define CHECK(cond) do { assert(cond); ++g_checks; } while (0)

/* ── Captured emits from the stubbed chokepoint ─────────────────────────── */
struct Captured {
  int                  count;
  char                 module_id[32];
  char                 type_name[32];
  csi_event_values_t   values;
};
static Captured g_last;

extern "C" {

void csi_event_values_init(csi_event_values_t* out) {
  memset(out, 0, sizeof(*out));
}

uint32_t csi_event_emit(const char* module_id, const char* type_name,
                        const csi_event_values_t* values) {
  g_last.count++;
  strncpy(g_last.module_id, module_id ? module_id : "", sizeof(g_last.module_id) - 1);
  strncpy(g_last.type_name, type_name ? type_name : "", sizeof(g_last.type_name) - 1);
  g_last.values = *values;
  return (uint32_t)g_last.count;   /* non-zero = "accepted" */
}

int32_t csi_module_settings_int(const csi_module_settings_t* s, const char* key,
                                int32_t default_value) {
  (void)s; (void)key;
  return default_value;            /* exercise the shipped defaults */
}

} /* extern "C" */

/* ── Helpers ────────────────────────────────────────────────────────────── */
static csi_features_t make_window(int8_t rssi_std, int8_t frames_dropped) {
  csi_features_t f;
  memset(&f, 0, sizeof(f));
  f.v[21] = rssi_std;        /* [20..23] RSSI mean/std/max/min */
  f.v[25] = frames_dropped;  /* [24..27] frames/dropped/chan/bw */
  return f;
}

static void reset_capture() { memset(&g_last, 0, sizeof(g_last)); }

int main() {
  const csi_module_t* m = wifi_channel_activity_module();
  CHECK(m != nullptr);
  CHECK(strcmp(m->id, "wifi.channel_activity") == 0);
  CHECK(m->default_privacy == CSI_PRIVACY_P0);
  CHECK(m->tick != nullptr);

  /* ── Manifest: only coarse P0 fields, no identity ─────────────────────── */
  const uint32_t ALLOWED =
      CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET | CSI_FIELD_MOTION_SCORE;
  CHECK(m->event_count == 1);
  for (size_t i = 0; i < m->event_count; ++i) {
    const csi_event_decl_t& e = m->events[i];
    CHECK((e.allowed_fields & ~ALLOWED) == 0);   /* nothing beyond the coarse set */
    CHECK((e.allowed_fields & CSI_FIELD_NOTE) == 0);
    CHECK((e.allowed_fields & CSI_FIELD_DOMINANT_SIGNAL) == 0);
    CHECK((e.allowed_fields & CSI_FIELD_BREATHING_RATE) == 0);  /* the only P1 field */
    CHECK(e.privacy == CSI_PRIVACY_P0);
  }

  m->init(nullptr);

  /* ── Warmup: no emit during the priming window even if it's loud ──────── */
  reset_capture();
  for (int i = 0; i < 60; ++i) { csi_features_t f = make_window(80, 90); m->tick(&f); }
  CHECK(g_last.count == 0);   /* warmup fills the baseline, never emits */

  /* Re-init to a clean baseline, then prime with a QUIET room. */
  m->init(nullptr);
  reset_capture();
  for (int i = 0; i < 60; ++i) { csi_features_t f = make_window(2, 1); m->tick(&f); }
  CHECK(g_last.count == 0);

  /* A quiet window after warmup stays silent (no burst vs. its own baseline). */
  { csi_features_t f = make_window(2, 1); m->tick(&f); }
  CHECK(g_last.count == 0);

  /* ── A burst above the room's baseline fires exactly one ambient glow ─── */
  { csi_features_t f = make_window(60, 90); m->tick(&f); }
  CHECK(g_last.count == 1);
  CHECK(strcmp(g_last.module_id, "wifi.channel_activity") == 0);
  CHECK(strcmp(g_last.type_name, "channel_active") == 0);

  /* AMBIENT is the whole "not persistent" guarantee — csi_event.cpp gates the
   * witness/SD/NVS commit on category != AMBIENT. */
  CHECK(g_last.values.category == CSI_CATEGORY_AMBIENT);

  /* Output is a fixed literal + a bounded 0..100 intensity; no raw feature
   * byte leaks through as an identifier, whatever the input looked like. */
  CHECK(strcmp(g_last.values.state_name, "channel_active") == 0);
  CHECK(g_last.values.motion_score > 0 && g_last.values.motion_score <= 100);
  CHECK((g_last.values.present_fields & ~ALLOWED) == 0);

  /* ── Cooldown: an immediately-following burst does NOT double-fire ────── */
  { csi_features_t f = make_window(60, 90); m->tick(&f); }
  CHECK(g_last.count == 1);   /* still 1 — cooled down */

  printf("test_wifi_channel_activity_privacy: %d checks passed\n", g_checks);
  return 0;
}
