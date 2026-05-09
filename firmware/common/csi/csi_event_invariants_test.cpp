/**
 * @file csi_event_invariants_test.cpp
 * @brief Privacy-invariant fuzzer for the CSI event chokepoint.
 *
 * Asserts:
 *   1. A module's emit() that includes fields outside the manifest's
 *      allowed_fields gets those fields zeroed by the chokepoint.
 *   2. Privacy class P1 events are silently dropped while the host's
 *      privacy ceiling is P0.
 *   3. Privacy class P2 events never persist to the witness chain.
 *   4. Time fields are always coarsened to the 10-minute bucket; no
 *      finer-grained value survives the chokepoint.
 *   5. Strings entering the ring contain only printable ASCII.
 *   6. The bundler collapses 100 same-state emits within a 10-minute window
 *      into a single committed row.
 *   7. Per-module hourly ceiling caps emits past the limit.
 *   8. The witness-chain payload string built from a committed event
 *      includes the spec §2 metadata fields kv= / rs= / zn=.
 *
 * Build:
 *   - Standalone (host x86) for CI: g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/common/csi/csi_event_invariants_test.cpp \
 *       firmware/common/csi/src/csi_event.cpp \
 *       firmware/common/csi/src/csi_module.cpp \
 *       firmware/common/csi/src/csi_bundler.cpp \
 *       firmware/common/csi/src/csi_witness_payload.cpp \
 *       -I firmware/common/csi/src -o /tmp/csi_invariants && /tmp/csi_invariants
 *
 * Compiles cleanly inside an ESP32 firmware build too — guarded so it does
 * not run on-device unless CSI_TEST_RUN_ON_DEVICE is defined.
 */

#include "csi_event.h"
#include "csi_module.h"
#include "csi_bundler.h"
#include "csi_witness_payload.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef CSI_TEST_HOST_BUILD
#  if !defined(CSI_TEST_RUN_ON_DEVICE)
/* Building inside firmware but no opt-in to run; entry point becomes a stub. */
extern "C" int csi_event_invariants_run() { return 0; }
#    define CSI_INVARIANTS_NO_MAIN 1
#  endif
#endif

#ifndef CSI_INVARIANTS_NO_MAIN

namespace {

/* ── Track host commit hooks to verify behaviour ─────────────────────────── */

struct CapturedCommit {
  uint32_t              event_id;
  char                  module_id[CSI_EVENT_NAME_MAX];
  char                  type_name[CSI_EVENT_NAME_MAX];
  csi_event_category_t  category;
  csi_privacy_class_t   privacy;
  csi_event_values_t    values;
};

constexpr size_t CAPTURE_CAP = 256;
CapturedCommit g_captured[CAPTURE_CAP];
size_t         g_captured_count = 0;
size_t         g_witness_commit_count = 0;

void reset_captures() {
  memset(g_captured, 0, sizeof(g_captured));
  g_captured_count = 0;
  g_witness_commit_count = 0;
}

}  /* namespace */

extern "C" {

bool csi_event_commit_witness(uint32_t,
                              const char*,
                              const char*,
                              csi_event_category_t,
                              const csi_event_values_t*) {
  g_witness_commit_count++;
  return true;
}

void csi_event_on_committed(uint32_t                  event_id,
                            const char*               module_id,
                            const char*               type_name,
                            csi_event_category_t      category,
                            csi_privacy_class_t       privacy,
                            const csi_event_values_t* values) {
  if (g_captured_count >= CAPTURE_CAP) return;
  CapturedCommit* c = &g_captured[g_captured_count++];
  c->event_id = event_id;
  c->category = category;
  c->privacy  = privacy;
  c->values   = *values;
  strncpy(c->module_id, module_id, CSI_EVENT_NAME_MAX - 1);
  strncpy(c->type_name, type_name, CSI_EVENT_NAME_MAX - 1);
  c->module_id[CSI_EVENT_NAME_MAX - 1] = '\0';
  c->type_name[CSI_EVENT_NAME_MAX - 1] = '\0';
}

}  /* extern "C" */

namespace {

/* ── A test module with a tightly-scoped allow-list ──────────────────────── */

const csi_event_decl_t TEST_EVENTS[] = {
  {
    /* type_name */                "test_state",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_CONFIDENCE
                                  | CSI_FIELD_TIME_BUCKET
                                  | CSI_FIELD_MOTION_SCORE,
    /* privacy */                  CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  6,
  },
  {
    /* type_name */                "test_p1",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_BREATHING_RATE,
    /* privacy */                  CSI_PRIVACY_P1,
    /* default_ceiling_per_hour */  4,
  },
  {
    /* type_name */                "test_p2",
    /* allowed_fields */            CSI_FIELD_STATE_NAME,
    /* privacy */                  CSI_PRIVACY_P2,
    /* default_ceiling_per_hour */  0,
  },
};

void noop_init(const csi_module_settings_t*) {}
void noop_tick(const csi_features_t*) {}

const csi_module_t TEST_MODULE = {
  /* id */                "test.module",
  /* default_privacy */   CSI_PRIVACY_P0,
  /* events */            TEST_EVENTS,
  /* event_count */       sizeof(TEST_EVENTS) / sizeof(TEST_EVENTS[0]),
  /* init */              noop_init,
  /* tick */              noop_tick,
  /* on_event_dismissed */ nullptr,
  /* deinit */            nullptr,
};

/* ── Assertion helpers ───────────────────────────────────────────────────── */

int g_failures = 0;

#define EXPECT(cond, msg) do {                                        \
  if (!(cond)) {                                                      \
    fprintf(stderr, "[FAIL] %s:%d  %s  — %s\n",                        \
      __FILE__, __LINE__, #cond, msg);                                \
    g_failures++;                                                     \
  }                                                                   \
} while (0)

/* ── Tests ───────────────────────────────────────────────────────────────── */

void test_disallowed_fields_are_zeroed() {
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  /* state_name is allowed; breathing_rate is NOT allowed for "test_state". */
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_BREATHING_RATE;
  strncpy(v.state_name, "active", sizeof(v.state_name) - 1);
  v.breathing_rate_bpm = 14;

  uint32_t id = csi_event_emit("test.module", "test_state", &v);
  EXPECT(id != 0, "test_state emit should be accepted");
  csi_event_flush_bundles();

  bool found = false;
  for (size_t i = 0; i < g_captured_count; ++i) {
    if (strcmp(g_captured[i].values.state_name, "active") == 0) {
      found = true;
      EXPECT(g_captured[i].values.breathing_rate_bpm == 0,
             "disallowed breathing_rate field must be zeroed");
      EXPECT((g_captured[i].values.present_fields & CSI_FIELD_BREATHING_RATE) == 0,
             "disallowed bit must be cleared in present_fields");
    }
  }
  EXPECT(found, "captured commit must reflect the active state");
}

void test_privacy_p1_blocked_under_p0_ceiling() {
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);
  csi_event_set_privacy_ceiling(CSI_PRIVACY_P0);

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_BREATHING_RATE;
  strncpy(v.state_name, "p1_attempt", sizeof(v.state_name) - 1);
  v.breathing_rate_bpm = 14;

  uint32_t id = csi_event_emit("test.module", "test_p1", &v);
  EXPECT(id == 0, "P1 emit should be rejected when ceiling is P0");
  EXPECT(g_captured_count == 0, "no commit should have been delivered");
}

void test_privacy_p2_never_persists_to_witness() {
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);
  csi_event_set_privacy_ceiling(CSI_PRIVACY_P2);

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME;
  strncpy(v.state_name, "tune", sizeof(v.state_name) - 1);

  uint32_t id = csi_event_emit("test.module", "test_p2", &v);
  EXPECT(id != 0, "P2 emit should be accepted under P2 ceiling");
  csi_event_flush_bundles();

  EXPECT(g_witness_commit_count == 0,
         "P2 events must NOT call csi_event_commit_witness");
  EXPECT(g_captured_count > 0,
         "P2 events should still drive the in-memory ring (the stream hook fires)");
}

void test_time_fields_are_coarsened() {
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET;
  strncpy(v.state_name, "x", sizeof(v.state_name) - 1);
  v.time_bucket = 222;   /* out-of-range — chokepoint must rewrite */

  csi_event_emit("test.module", "test_state", &v);
  csi_event_flush_bundles();

  bool seen = false;
  for (size_t i = 0; i < g_captured_count; ++i) {
    if (strcmp(g_captured[i].values.state_name, "x") == 0) {
      EXPECT(g_captured[i].values.time_bucket < 144,
             "time_bucket must be coarsened to 0..143");
      seen = true;
    }
  }
  EXPECT(seen, "time-bucket capture must exist");
}

void test_strings_are_sanitized() {
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME;
  /* Stuff binary garbage in. */
  const char garbage[8] = { '\x01', 'e', 'v', '\x7f', 'b', 'a', 'd', '\0' };
  memcpy(v.state_name, garbage, sizeof(garbage));

  csi_event_emit("test.module", "test_state", &v);
  csi_event_flush_bundles();

  for (size_t i = 0; i < g_captured_count; ++i) {
    for (size_t k = 0; k < sizeof(g_captured[i].values.state_name); ++k) {
      char c = g_captured[i].values.state_name[k];
      if (c == '\0') break;
      EXPECT(c >= 0x20 && c <= 0x7e,
             "every byte must be printable ASCII after sanitize");
    }
  }
}

void test_bundler_collapses_burst() {
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_MOTION_SCORE;
  strncpy(v.state_name, "active", sizeof(v.state_name) - 1);
  v.motion_score = 50;

  for (int i = 0; i < 100; ++i) csi_event_emit("test.module", "test_state", &v);
  csi_event_flush_bundles();

  size_t active_commits = 0;
  for (size_t i = 0; i < g_captured_count; ++i) {
    if (strcmp(g_captured[i].values.state_name, "active") == 0) active_commits++;
  }
  EXPECT(active_commits <= 2,
         "100 same-state emits in a tight window must collapse to ≤ 2 commits");
}

void test_witness_payload_includes_metadata() {
  /* Spec/event_contract.md §2 mandates that every committed event carry
   * its kernel_version, ruleset_id, and zone_id alongside the existing
   * type / time-bucket / confidence fields. canary-wap's witness bridge
   * builds the signed payload via csi_witness_build_payload(); this test
   * exercises that builder directly with synthetic args and asserts the
   * three substrings appear in the output. */
  char buf[256];
  int len = csi_witness_build_payload(
    buf, sizeof(buf),
    "core.presence", "presence_changed",
    /*category=*/1,
    "active", "high",
    /*motion=*/40, /*breathing=*/0, /*bpm=*/0,
    /*duration_sec=*/0, /*time_bucket=*/72,
    "2.1.0-wap", "securacv:canary:v1.0", "home");
  EXPECT(len > 0, "payload build must succeed for typical inputs");
  EXPECT(strstr(buf, " kv=2.1.0-wap ") != nullptr,
         "payload must contain kv=<firmware_version>");
  EXPECT(strstr(buf, " rs=securacv:canary:v1.0 ") != nullptr,
         "payload must contain rs=<ruleset_id>");
  EXPECT(strstr(buf, " zn=home") != nullptr,
         "payload must contain zn=<zone_id>");

  /* Buffer-too-small must return -1 (defensive — small downstream
   * stack frames must not silently truncate the metadata fields). */
  char tiny[16];
  int tiny_len = csi_witness_build_payload(
    tiny, sizeof(tiny),
    "core.presence", "presence_changed", 1,
    "active", "high", 40, 0, 0, 0, 72,
    "2.1.0-wap", "securacv:canary:v1.0", "home");
  EXPECT(tiny_len == -1, "buffer-too-small must return -1, not a truncated payload");

  /* Null / empty inputs fall back to the dash sentinel rather than
   * skipping the field — keeps the wire format positionally stable. */
  char fb[256];
  int fb_len = csi_witness_build_payload(
    fb, sizeof(fb),
    nullptr, nullptr, 0, nullptr, nullptr,
    0, 0, 0, 0, 0,
    nullptr, nullptr, nullptr);
  EXPECT(fb_len > 0, "fallback build must succeed");
  EXPECT(strstr(fb, " kv=- ") != nullptr,
         "missing kernel_version must render as kv=-");
  EXPECT(strstr(fb, " rs=- ") != nullptr,
         "missing ruleset_id must render as rs=-");
  EXPECT(strstr(fb, " zn=-") != nullptr,
         "missing zone_id must render as zn=-");
}

void test_per_module_ceiling() {
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);

  /* test_state has default_ceiling_per_hour=6. Burst 30 distinct states so
   * the bundler doesn't collapse them, then verify the ceiling caps it. */
  for (int i = 0; i < 30; ++i) {
    csi_event_values_t v;
    csi_event_values_init(&v);
    v.category       = CSI_CATEGORY_EVENT;
    v.present_fields = CSI_FIELD_STATE_NAME;
    /* Distinct state names defeat the bundler. */
    snprintf(v.state_name, sizeof(v.state_name), "s%d", i);
    csi_event_emit("test.module", "test_state", &v);
  }
  csi_event_flush_bundles();

  EXPECT(g_captured_count <= 6,
         "per-module hourly ceiling (6) must cap commits");
}

}  /* namespace */

extern "C" int csi_event_invariants_run() {
  test_disallowed_fields_are_zeroed();
  test_privacy_p1_blocked_under_p0_ceiling();
  test_privacy_p2_never_persists_to_witness();
  test_time_fields_are_coarsened();
  test_strings_are_sanitized();
  test_bundler_collapses_burst();
  test_per_module_ceiling();
  test_witness_payload_includes_metadata();

  if (g_failures == 0) {
    fprintf(stderr, "[OK] csi_event invariants — all tests passed\n");
    return 0;
  }
  fprintf(stderr, "[FAIL] csi_event invariants — %d failures\n", g_failures);
  return 1;
}

#ifdef CSI_TEST_HOST_BUILD
int main() { return csi_event_invariants_run(); }
#endif

#endif  /* CSI_INVARIANTS_NO_MAIN */
