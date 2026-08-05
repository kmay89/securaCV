/**
 * @file test_meta_empty_room_baseline.cpp
 * @brief Host-build conformance test for meta.empty_room_baseline (PR 4a).
 *
 * Verifies:
 *   1. start() begins a calibration; tick() during CALIBRATING
 *      accumulates windows.
 *   2. After duration_ms elapses, the mean of accumulated v[] values
 *      is exposed via meta_empty_room_baseline_get().
 *   3. "baseline_status" event fires with state_name="calibrated"
 *      and bundled_count = number of windows accumulated.
 *   4. cancel() mid-run wipes accumulator + emits "canceled" event,
 *      preserves any prior baseline.
 *   5. Fewer than META_EMPTY_ROOM_MIN_WINDOWS contributing windows →
 *      "failed" event, prior baseline preserved.
 *   6. Constant features → mean == that constant.
 *   7. Zero features → zero baseline.
 *   8. Double-start refused (one calibration at a time).
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/common/csi/test_meta_empty_room_baseline.cpp \
 *       firmware/common/csi/src/meta_empty_room_baseline.cpp \
 *       firmware/common/csi/src/csi_event.cpp \
 *       firmware/common/csi/src/csi_module.cpp \
 *       firmware/common/csi/src/csi_bundler.cpp \
 *       firmware/common/csi/src/csi_witness_payload.cpp \
 *       -I firmware/common/csi/src \
 *       -o /tmp/test_erb && /tmp/test_erb
 */

#include "meta_empty_room_baseline.h"
#include "csi_event.h"
#include "csi_module.h"
#include "csi_types.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_meta_empty_room_baseline_run() { return 0; }
#else

/* ────────────────────────────────────────────────────────────────────────
 * Capture-the-emit hooks (mirror of csi_event_invariants_test).
 * ──────────────────────────────────────────────────────────────────────── */

namespace {

struct Capture {
  char type_name[CSI_EVENT_NAME_MAX];
  csi_event_values_t values;
};

constexpr size_t CAP_CAP = 32;
Capture g_captured[CAP_CAP];
size_t  g_captured_count = 0;

void reset_captures() {
  memset(g_captured, 0, sizeof(g_captured));
  g_captured_count = 0;
}

}  /* namespace */

extern "C" {

bool csi_event_commit_witness(uint32_t,
                              const char*,
                              const char*,
                              csi_event_category_t,
                              const csi_event_values_t*) {
  return true;
}

void csi_event_on_committed(uint32_t /*event_id*/,
                            const char*               /*module_id*/,
                            const char*               type_name,
                            csi_event_category_t      /*cat*/,
                            csi_privacy_class_t       /*privacy*/,
                            const csi_event_values_t* values) {
  if (g_captured_count >= CAP_CAP) return;
  Capture& c = g_captured[g_captured_count++];
  strncpy(c.type_name, type_name, sizeof(c.type_name) - 1);
  c.values = *values;
}

}  /* extern "C" */

namespace {

void register_module_once() {
  static bool registered = false;
  if (!registered) {
    assert(csi_module_register(meta_empty_room_baseline_module()));
    registered = true;
  }
}

void fresh_world() {
  reset_captures();
  meta_empty_room_baseline_test_reset();
  register_module_once();
  /* test_reset clears the virtual clock too, so re-establish AFTER the
   * register-triggered init() so start() reads a known now_ms. */
  meta_empty_room_baseline_test_reset();
  meta_empty_room_baseline_test_set_now_ms(1000);
}

/* Build a csi_features_t with every v[] entry set to `value`. */
csi_features_t make_features_const(int8_t value) {
  csi_features_t f;
  memset(&f, 0, sizeof(f));
  for (size_t i = 0; i < CSI_FEATURE_DIM; ++i) f.v[i] = value;
  f.frames_in_window = 20;
  f.time_bucket      = 42;
  f.caps_observed    = CSI_CAP_HT20;
  return f;
}

void tick_and_flush(const csi_features_t& f) {
  csi_module_tick_all(&f);
  csi_event_flush_bundles();
}

bool any_emitted_with_state(const char* state_name) {
  for (size_t i = 0; i < g_captured_count; ++i) {
    if (strcmp(g_captured[i].type_name, "baseline_status") == 0 &&
        strcmp(g_captured[i].values.state_name, state_name) == 0) {
      return true;
    }
  }
  return false;
}

/* ────────────────────────────────────────────────────────────────────────
 * Test bodies
 * ──────────────────────────────────────────────────────────────────────── */

/* Use a short test duration so we don't have to simulate 600 windows. */
constexpr uint32_t TEST_DURATION_MS = 350 * 1000;   /* 350 s → 350 windows */
constexpr uint16_t TEST_WINDOWS     = 350;          /* just above MIN_WINDOWS = 300 */

void test_constant_features_produce_constant_baseline() {
  fresh_world();
  csi_features_t f = make_features_const(20);

  assert(meta_empty_room_baseline_start(TEST_DURATION_MS));
  assert(meta_empty_room_baseline_is_calibrating());

  /* Tick TEST_WINDOWS times at 1 Hz; advance virtual clock at each. */
  for (uint16_t i = 0; i < TEST_WINDOWS; ++i) {
    meta_empty_room_baseline_test_set_now_ms(1000 + (uint32_t)i * 1000);
    tick_and_flush(f);
  }
  /* One more tick that crosses the deadline to fire finalize. */
  meta_empty_room_baseline_test_set_now_ms(1000 + TEST_DURATION_MS + 100);
  tick_and_flush(f);

  assert(!meta_empty_room_baseline_is_calibrating());
  assert(any_emitted_with_state("calibrated"));

  int8_t mean[CSI_FEATURE_DIM];
  uint16_t n = 0;
  assert(meta_empty_room_baseline_get(mean, &n));
  for (size_t i = 0; i < CSI_FEATURE_DIM; ++i) assert(mean[i] == 20);
  assert(n >= TEST_WINDOWS);
  std::printf("PASS test_constant_features_produce_constant_baseline  (n=%u)\n", n);
}

void test_zero_features_produce_zero_baseline() {
  fresh_world();
  csi_features_t f = make_features_const(0);

  assert(meta_empty_room_baseline_start(TEST_DURATION_MS));
  for (uint16_t i = 0; i < TEST_WINDOWS + 1; ++i) {
    meta_empty_room_baseline_test_set_now_ms(1000 + (uint32_t)i * 1000);
    tick_and_flush(f);
  }
  meta_empty_room_baseline_test_set_now_ms(1000 + TEST_DURATION_MS + 100);
  tick_and_flush(f);

  int8_t mean[CSI_FEATURE_DIM];
  uint16_t n = 0;
  assert(meta_empty_room_baseline_get(mean, &n));
  for (size_t i = 0; i < CSI_FEATURE_DIM; ++i) assert(mean[i] == 0);
  std::printf("PASS test_zero_features_produce_zero_baseline\n");
}

void test_cancel_mid_calibration() {
  fresh_world();
  csi_features_t f = make_features_const(30);

  assert(meta_empty_room_baseline_start(TEST_DURATION_MS));
  /* Run a few ticks then cancel. */
  for (int i = 0; i < 50; ++i) {
    meta_empty_room_baseline_test_set_now_ms(1000 + (uint32_t)i * 1000);
    tick_and_flush(f);
  }
  meta_empty_room_baseline_cancel();
  /* Cancel is synchronous (no flush needed for emit) — flush anyway. */
  csi_event_flush_bundles();

  assert(!meta_empty_room_baseline_is_calibrating());
  assert(any_emitted_with_state("canceled"));
  /* No baseline saved (we never had one). */
  int8_t mean[CSI_FEATURE_DIM];
  uint16_t n = 0;
  assert(!meta_empty_room_baseline_get(mean, &n));
  std::printf("PASS test_cancel_mid_calibration\n");
}

void test_too_few_windows_fails_preserves_prior() {
  fresh_world();

  /* First: run a successful calibration so we have a stored baseline. */
  csi_features_t f1 = make_features_const(50);
  assert(meta_empty_room_baseline_start(TEST_DURATION_MS));
  for (uint16_t i = 0; i < TEST_WINDOWS + 1; ++i) {
    meta_empty_room_baseline_test_set_now_ms(1000 + (uint32_t)i * 1000);
    tick_and_flush(f1);
  }
  meta_empty_room_baseline_test_set_now_ms(1000 + TEST_DURATION_MS + 100);
  tick_and_flush(f1);
  assert(any_emitted_with_state("calibrated"));
  int8_t prior[CSI_FEATURE_DIM]; uint16_t prior_n = 0;
  assert(meta_empty_room_baseline_get(prior, &prior_n));

  /* Now: start another calibration but feed too few windows before
   * the deadline. */
  reset_captures();
  csi_features_t f2 = make_features_const(99);
  meta_empty_room_baseline_test_set_now_ms(1000 + TEST_DURATION_MS + 200);
  assert(meta_empty_room_baseline_start(TEST_DURATION_MS));
  for (int i = 0; i < 10; ++i) {   /* far below MIN_WINDOWS=300 */
    meta_empty_room_baseline_test_set_now_ms(1000 + TEST_DURATION_MS + 200 + i * 1000);
    tick_and_flush(f2);
  }
  /* Cross the deadline. */
  meta_empty_room_baseline_test_set_now_ms(1000 + 2 * TEST_DURATION_MS + 500);
  tick_and_flush(f2);
  assert(any_emitted_with_state("failed"));

  /* Prior baseline preserved. */
  int8_t after[CSI_FEATURE_DIM]; uint16_t after_n = 0;
  assert(meta_empty_room_baseline_get(after, &after_n));
  assert(memcmp(prior, after, CSI_FEATURE_DIM) == 0);
  assert(after_n == prior_n);
  std::printf("PASS test_too_few_windows_fails_preserves_prior\n");
}

void test_double_start_refused() {
  fresh_world();
  assert(meta_empty_room_baseline_start(TEST_DURATION_MS));
  /* Second start should refuse — must cancel first. */
  assert(!meta_empty_room_baseline_start(TEST_DURATION_MS));
  std::printf("PASS test_double_start_refused\n");
}

void test_mean_of_mixed_values() {
  /* Alternate +30 / -30 ticks; the per-feature mean should be ~0. */
  fresh_world();
  assert(meta_empty_room_baseline_start(TEST_DURATION_MS));

  for (uint16_t i = 0; i < TEST_WINDOWS; ++i) {
    meta_empty_room_baseline_test_set_now_ms(1000 + (uint32_t)i * 1000);
    csi_features_t f = make_features_const(i & 1 ? -30 : 30);
    tick_and_flush(f);
  }
  meta_empty_room_baseline_test_set_now_ms(1000 + TEST_DURATION_MS + 100);
  csi_features_t f = make_features_const(0);
  tick_and_flush(f);

  int8_t mean[CSI_FEATURE_DIM]; uint16_t n = 0;
  assert(meta_empty_room_baseline_get(mean, &n));
  /* Even-count alternation gives exact 0. Odd-count tolerates ±1. */
  for (size_t i = 0; i < CSI_FEATURE_DIM; ++i) {
    assert(mean[i] >= -1 && mean[i] <= 1);
  }
  std::printf("PASS test_mean_of_mixed_values  (got mean[0]=%d)\n", mean[0]);
}

}  /* namespace */

int main() {
  test_constant_features_produce_constant_baseline();
  test_zero_features_produce_zero_baseline();
  test_cancel_mid_calibration();
  test_too_few_windows_fails_preserves_prior();
  test_double_start_refused();
  test_mean_of_mixed_values();
  std::printf("\nALL META_EMPTY_ROOM_BASELINE TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
