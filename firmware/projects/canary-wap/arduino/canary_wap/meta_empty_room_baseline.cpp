/**
 * @file meta_empty_room_baseline.cpp
 * @brief meta.empty_room_baseline — implementation.
 *
 * Accumulates int32 running sums of v[0..31] across the calibration
 * window so the final mean is integer division. No floating-point.
 * Allocation-free; one 32-entry int32 sum buffer + a window counter.
 *
 * Lifecycle:
 *   IDLE -> [start()] -> CALIBRATING -> [duration elapses] -> IDLE +
 *           baseline stored + "calibrated" event emitted.
 *
 *   CALIBRATING -> [cancel()] -> IDLE + "cancelled" event emitted.
 *
 *   CALIBRATING -> [duration elapses but fewer than MIN_WINDOWS
 *                   contributed] -> IDLE + "failed" event emitted +
 *                   prior baseline (if any) preserved.
 *
 * Threading contract: ALL entry points (start/cancel/get/is_calibrating
 * + the module's on_tick) must be invoked from the same task as the
 * csi_module dispatcher. This matches the existing csi_module spec
 * (csi_module.h: "tick() is called from the main loop (NOT an ISR)").
 * The integration layer must NOT call start()/cancel() from an ISR or
 * a separate FreeRTOS task; doing so races against the on_tick
 * accumulator. No atomics or mutex are added here because the
 * cross-task path doesn't exist in production today and adding the
 * locks would mask any future violation rather than surface it.
 */

#include "meta_empty_room_baseline.h"
#include "csi_event.h"
#include "csi_module.h"

#include <stdlib.h>   /* abs() */
#include <string.h>

#ifndef CSI_TEST_HOST_BUILD
  #include <Arduino.h>   /* millis() — extern "C" linkage */
#endif

namespace {

enum class State : uint8_t {
  IDLE = 0,
  CALIBRATING,
};

/* Accumulator state. Allocation-free; reset on every start().
 * s_in_progress_n is uint32_t — uint16_t would overflow ~18.2 h at 1 Hz
 * windows, and the API accepts arbitrary duration_ms (up to UINT32_MAX
 * ms ≈ 49 days). */
static State    s_state         = State::IDLE;
static int32_t  s_sum[CSI_FEATURE_DIM];   /* running sum of v[] */
static uint32_t s_in_progress_n = 0;       /* windows added so far */
static uint32_t s_started_ms    = 0;
static uint32_t s_duration_ms   = META_EMPTY_ROOM_DEFAULT_DURATION_MS;

/* Stored baseline (most recent successful calibration). Separate from
 * the accumulator so a cancelled / failed run doesn't clobber a good
 * baseline already in place. */
static int8_t   s_baseline_mean[CSI_FEATURE_DIM];
static uint32_t s_baseline_n   = 0;
static bool     s_baseline_ok  = false;

#ifdef CSI_TEST_HOST_BUILD
static uint32_t s_test_now_ms  = 0;
static bool     s_test_now_set = false;
#endif

inline uint32_t now_ms() {
#ifdef CSI_TEST_HOST_BUILD
  if (s_test_now_set) return s_test_now_ms;
  static uint32_t fake = 0;
  return fake;
#else
  /* Arduino's millis() has C linkage (extern "C" unsigned long); use
   * the runtime symbol via Arduino.h rather than a bare `extern uint32_t
   * millis()` forward-declare (which would emit a mangled C++ symbol
   * that fails to link). */
  return millis();
#endif
}

void emit_status(const char* status, uint32_t windows) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_BUNDLED_COUNT;
  strncpy(v.state_name, status, sizeof(v.state_name) - 1);
  /* csi_event_values_t.bundled_count is uint16_t; cap at its max so
   * a >65k-window calibration still produces a defined value. */
  v.bundled_count  = (windows > UINT16_MAX) ? UINT16_MAX : (uint16_t)windows;
  /* Event type_name is "baseline_status" rather than "baseline_calibrated"
   * because the same event carries state_name = "calibrated" | "cancelled"
   * | "failed". Consumers filter by state_name. */
  (void)csi_event_emit("meta.empty_room_baseline", "baseline_status", &v);
}

void finalize_calibration() {
  if (s_in_progress_n >= META_EMPTY_ROOM_MIN_WINDOWS) {
    for (size_t i = 0; i < CSI_FEATURE_DIM; ++i) {
      int32_t avg = s_sum[i] / (int32_t)s_in_progress_n;
      if (avg >  127) avg =  127;
      if (avg < -128) avg = -128;
      s_baseline_mean[i] = (int8_t)avg;
    }
    s_baseline_n  = s_in_progress_n;
    s_baseline_ok = true;
    emit_status("calibrated", s_in_progress_n);
  } else {
    /* Too few windows — keep the prior baseline (if any). */
    emit_status("failed", s_in_progress_n);
  }

  /* Reset accumulator either way. */
  memset(s_sum, 0, sizeof(s_sum));
  s_in_progress_n = 0;
  s_state         = State::IDLE;
}

void on_init(const csi_module_settings_t* /*settings*/) {
  s_state         = State::IDLE;
  memset(s_sum, 0, sizeof(s_sum));
  s_in_progress_n = 0;
  /* Do NOT clear s_baseline_* — a prior baseline survives an init()
   * call (e.g. on a Tuning Lab live reload). Use the test reset hook
   * to wipe everything. */
}

void on_tick(const csi_features_t* f) {
  if (s_state != State::CALIBRATING) return;

  /* Deadline check FIRST so the calibration finalizes (with whatever
   * windows have accumulated) even if the CSI stream stops mid-run.
   * Without this, on_tick would never run past the deadline because
   * the CSI HAL only ticks when a feature window is produced, and a
   * stalled HAL would leave the module stuck in CALIBRATING forever.
   *
   * Signed-delta against (started + duration) is wrap-safe across the
   * uint32_t millis() rollover. */
  if ((int32_t)(now_ms() - (s_started_ms + s_duration_ms)) >= 0) {
    finalize_calibration();
    return;
  }

  if (f == nullptr) return;

  /* Accumulate this window's v[] into the running int32 sums. */
  for (size_t i = 0; i < CSI_FEATURE_DIM; ++i) {
    s_sum[i] += (int32_t)f->v[i];
  }
  s_in_progress_n++;
}

void on_deinit() {
  s_state         = State::IDLE;
  memset(s_sum, 0, sizeof(s_sum));
  s_in_progress_n = 0;
}

static const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */          "baseline_status",
    /* allowed_fields */     CSI_FIELD_STATE_NAME
                           | CSI_FIELD_BUNDLED_COUNT
                           | CSI_FIELD_TIME_BUCKET,
    /* privacy */            CSI_PRIVACY_P0,
    /* default_ceiling */    24,   /* at most 24 events per hour */
  },
};

static const csi_module_t MODULE = {
  /* id */              "meta.empty_room_baseline",
  /* default_privacy */ CSI_PRIVACY_P0,
  /* events */          EVENTS,
  /* event_count */     sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */            on_init,
  /* tick */            on_tick,
  /* on_dismissed */    nullptr,
  /* deinit */          on_deinit,
};

}  /* namespace */

/* ────────────────────────────────────────────────────────────────────────
 * PUBLIC API
 * ──────────────────────────────────────────────────────────────────────── */

extern "C" {

const csi_module_t* meta_empty_room_baseline_module(void) {
  return &MODULE;
}

bool meta_empty_room_baseline_start(uint32_t duration_ms) {
  if (s_state == State::CALIBRATING) return false;
  memset(s_sum, 0, sizeof(s_sum));
  s_in_progress_n = 0;
  s_started_ms    = now_ms();
  s_duration_ms   = (duration_ms == 0)
                  ? META_EMPTY_ROOM_DEFAULT_DURATION_MS
                  : duration_ms;
  s_state         = State::CALIBRATING;
  return true;
}

void meta_empty_room_baseline_cancel(void) {
  if (s_state != State::CALIBRATING) return;
  const uint32_t partial = s_in_progress_n;
  memset(s_sum, 0, sizeof(s_sum));
  s_in_progress_n = 0;
  s_state         = State::IDLE;
  emit_status("cancelled", partial);
}

bool meta_empty_room_baseline_is_calibrating(void) {
  return s_state == State::CALIBRATING;
}

bool meta_empty_room_baseline_get(int8_t out_mean[CSI_FEATURE_DIM],
                                  uint16_t* out_window_count) {
  if (out_mean == nullptr) return false;
  if (!s_baseline_ok) {
    memset(out_mean, 0, CSI_FEATURE_DIM);
    if (out_window_count) *out_window_count = 0;
    return false;
  }
  memcpy(out_mean, s_baseline_mean, CSI_FEATURE_DIM);
  if (out_window_count) {
    /* s_baseline_n is uint32_t now; cap to the uint16_t public field. */
    *out_window_count = (s_baseline_n > UINT16_MAX) ? UINT16_MAX
                                                    : (uint16_t)s_baseline_n;
  }
  return true;
}

#ifdef CSI_TEST_HOST_BUILD
void meta_empty_room_baseline_test_reset(void) {
  s_state         = State::IDLE;
  memset(s_sum, 0, sizeof(s_sum));
  s_in_progress_n = 0;
  s_started_ms    = 0;
  s_duration_ms   = META_EMPTY_ROOM_DEFAULT_DURATION_MS;
  memset(s_baseline_mean, 0, sizeof(s_baseline_mean));
  s_baseline_n    = 0;
  s_baseline_ok   = false;
  s_test_now_set  = false;
  s_test_now_ms   = 0;
}

void meta_empty_room_baseline_test_set_now_ms(uint32_t v) {
  s_test_now_ms  = v;
  s_test_now_set = true;
}

uint32_t meta_empty_room_baseline_test_in_progress_count(void) {
  return s_in_progress_n;
}
#endif

}  /* extern "C" */
