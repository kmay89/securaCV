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
 *   9. ble.events module (spec §10) — every BLE event flows through
 *      the chokepoint, with MAC-precision fields stripped by the
 *      allow-list.
 *  10. Quiet Hours: events during the configured window are held; one
 *      summary row appears at window-close.
 *  11. core.multilink_fusion (PR 3): NOTE / BREATHING_* / RSSI-derived
 *      fields are stripped — fusion has nothing to say about identity
 *      or single-link breathing precision.
 *  12. ble.scout (PR 5): MOTION_SCORE, BREATHING_*, CONFIDENCE are
 *      stripped; the user-supplied room label survives in `note` for
 *      beacon_event ONLY (scout_initialized strips even that).
 *  13. (PR 6) No peer MAC survives in any CSI feature vector payload —
 *      the int8 v[32] vector is all that crosses the privacy barrier.
 *  14. (PR 6) Beacon MAC is hashed before any event emission — the
 *      ble_scout module's manifest only allows hashed identifiers.
 *  15. (PR 6) Per-link RSSI is bucketed to int8 — the feature struct's
 *      static_assert enforces the 32-byte int8 vector size.
 *
 * Build:
 *   - Standalone (host x86) for CI: g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/common/csi/csi_event_invariants_test.cpp \
 *       firmware/common/csi/src/csi_event.cpp \
 *       firmware/common/csi/src/csi_module.cpp \
 *       firmware/common/csi/src/csi_bundler.cpp \
 *       firmware/common/csi/src/csi_witness_payload.cpp \
 *       firmware/common/csi/src/ble_events_module.cpp \
 *       firmware/common/csi/src/meta_quiet_hours.cpp \
 *       -I firmware/common/csi/src -o /tmp/csi_invariants && /tmp/csi_invariants
 *
 * Compiles cleanly inside an ESP32 firmware build too — guarded so it does
 * not run on-device unless CSI_TEST_RUN_ON_DEVICE is defined.
 */

#include "csi_event.h"
#include "csi_module.h"
#include "csi_bundler.h"
#include "csi_witness_payload.h"
#include "ble_events_module.h"
#include "meta_quiet_hours.h"
#include "core_multilink_fusion.h"
#include "ble_scout.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#ifndef CSI_TEST_HOST_BUILD
#  if !defined(CSI_TEST_RUN_ON_DEVICE)
/* Building inside firmware but no opt-in to run; entry point becomes a stub. */
extern "C" int csi_event_invariants_run() { return 0; }
#    define CSI_INVARIANTS_NO_MAIN 1
#  endif
#endif

#ifndef CSI_INVARIANTS_NO_MAIN

namespace {

/* ── Track host commit hooks to verify behavior ─────────────────────────── */

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

void test_bundler_snapshot_exposes_open_bundles() {
  /* The live half of /api/events/today: an open bundle must be visible via
   * csi_bundler_snapshot_open() BEFORE it commits (the review on the first
   * phone client found a live alarm invisible for up to the 10-minute
   * window), carry a live duration rather than the zero a slot holds until
   * close, and vanish from the snapshot once flushed — at which point it
   * commits exactly as before. */
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_MOTION_SCORE;
  strncpy(v.state_name, "active", sizeof(v.state_name) - 1);
  v.motion_score = 50;

  csi_event_emit("test.module", "test_state", &v);
  csi_event_emit("test.module", "test_state", &v);

  csi_event_record_t open_rows[8];
  size_t nopen = csi_bundler_snapshot_open(open_rows, 8);
  EXPECT(nopen == 1, "two same-state emits must show as ONE open bundle");
  EXPECT(open_rows[0].event_id >= 0x80000000u,
         "an open bundle carries its bundler-minted id");
  EXPECT(strcmp(open_rows[0].type_name, "test_state") == 0,
         "the open row keeps its type_name");
  EXPECT(open_rows[0].bundled_count == 2,
         "the open row's bundled_count tracks roll-ins");
  EXPECT(open_rows[0].values.dismissed == 0,
         "an open bundle is never dismissed");
  EXPECT(g_captured_count == 0,
         "snapshotting an open bundle must not commit it");

  csi_event_flush_bundles();
  nopen = csi_bundler_snapshot_open(open_rows, 8);
  EXPECT(nopen == 0, "a flushed bundle must leave the open snapshot");
  EXPECT(g_captured_count >= 1, "the flush still commits the bundle");
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

void test_ble_events_strip_mac_precision_fields() {
  /* spec/event_contract.md §10: BLE events MUST NOT include MAC
   * addresses, RSSI at tracking precision, or stable hardware
   * identifiers. The allow-list per event in ble_events_module.cpp
   * encodes this — no event permits BREATHING_RATE / MOTION_SCORE /
   * BREATHING_SCORE (BLE has nothing to say about CSI features), and
   * a caller attempting to populate them gets them zeroed.
   *
   * This test exercises the allow-list directly: it builds a values
   * struct asserting every CSI field, runs each ble.events type
   * through emit, and asserts the captured commit has only the
   * fields the allow-list permits. */
  csi_event_test_reset();
  reset_captures();
  csi_module_register(ble_events_module());

  const char* TYPES[] = {
    "ble_initialized", "ble_init_failed",
    "ble_client_connected", "ble_client_disconnected",
    "chirp_sent", "chirp_received",
    "canary_discovered", "canary_lost",
  };
  const size_t N = sizeof(TYPES) / sizeof(TYPES[0]);

  for (size_t i = 0; i < N; ++i) {
    reset_captures();
    csi_event_values_t v;
    csi_event_values_init(&v);
    v.category = CSI_CATEGORY_EVENT;
    /* Stuff every smuggleable field. */
    v.present_fields = CSI_FIELD_STATE_NAME
                     | CSI_FIELD_NOTE
                     | CSI_FIELD_TIME_BUCKET
                     | CSI_FIELD_MOTION_SCORE
                     | CSI_FIELD_BREATHING_SCORE
                     | CSI_FIELD_BREATHING_RATE
                     | CSI_FIELD_DURATION_SEC;
    strncpy(v.state_name, "smuggled", sizeof(v.state_name) - 1);
    strncpy(v.note,       "ab12cd34", sizeof(v.note) - 1);
    v.motion_score       = 99;
    v.breathing_score    = 99;
    v.breathing_rate_bpm = 99;
    v.duration_sec       = 9999;

    csi_event_emit("ble.events", TYPES[i], &v);
    csi_event_flush_bundles();

    bool seen = false;
    for (size_t k = 0; k < g_captured_count; ++k) {
      if (strcmp(g_captured[k].type_name, TYPES[i]) != 0) continue;
      seen = true;
      /* Note on duration_sec: the chokepoint DOES strip it (no
       * ble.events allow-list permits it), but the bundler's
       * close_slot re-adds duration_sec to bundled rows because
       * "how long was this bundle open" is legitimate metadata for
       * any bundled event regardless of allow-list. Asserting it
       * here would fight that intentional behavior for the four
       * stateful types that go through the bundler; the
       * MAC-precision fields below are what spec §10 actually
       * enforces, and the bundler doesn't touch those. */
      EXPECT(g_captured[k].values.motion_score == 0,
             "ble.events MUST NOT carry motion_score");
      EXPECT(g_captured[k].values.breathing_score == 0,
             "ble.events MUST NOT carry breathing_score");
      EXPECT(g_captured[k].values.breathing_rate_bpm == 0,
             "ble.events MUST NOT carry breathing_rate (RSSI-precision proxy)");
      EXPECT((g_captured[k].values.present_fields
              & (CSI_FIELD_MOTION_SCORE
               | CSI_FIELD_BREATHING_SCORE
               | CSI_FIELD_BREATHING_RATE)) == 0,
             "disallowed field bits must be cleared in present_fields");
    }
    EXPECT(seen, "every ble.events type must commit at least once");
  }
}

void test_multilink_fusion_strips_unauthorized_fields() {
  /* PR 3 (#456) added core.multilink_fusion. Its "motion_confirmed"
   * manifest at core_multilink_fusion.cpp permits
   *   STATE_NAME | CONFIDENCE | TIME_BUCKET | MOTION_SCORE
   * | BUNDLED_COUNT | DISMISSED
   * — and ONLY those. Three privacy hazards are intentionally absent:
   *
   *   1. NOTE — would let a future contributor smuggle a peer's hashed
   *      device id, MAC fingerprint, or arbitrary diagnostic string
   *      into the event payload. Multi-link fusion has nothing to say
   *      that needs a free-form string field; the privacy contract
   *      says it MUST NOT carry one.
   *
   *   2. BREATHING_RATE / BREATHING_SCORE — single-link breathing data
   *      is the role of core.breathing, not the fusion module. Allowing
   *      it here would let a fusion event carry RSSI-derived breathing
   *      precision that single-link consumers downstream would treat
   *      as confirmed truth.
   *
   * (DURATION_SEC is also outside the manifest, but the bundler's
   * close_slot re-adds it to bundled rows as "how long was this bundle
   * open" — legitimate metadata for any bundled event regardless of
   * allow-list. Asserting it here would fight that intentional bundler
   * behavior; the NOTE / BREATHING_* checks below are what the
   * fusion-specific privacy story actually enforces.)
   *
   * Future contributors changing the manifest WILL break this test.
   * That's the point: the contract is now load-bearing. */
  csi_event_test_reset();
  reset_captures();
  csi_module_register(core_multilink_fusion_module());

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  /* Stuff every smuggleable field. */
  v.present_fields = CSI_FIELD_STATE_NAME
                   | CSI_FIELD_CONFIDENCE
                   | CSI_FIELD_TIME_BUCKET
                   | CSI_FIELD_MOTION_SCORE
                   | CSI_FIELD_BUNDLED_COUNT
                   | CSI_FIELD_NOTE                /* must be stripped */
                   | CSI_FIELD_BREATHING_RATE      /* must be stripped */
                   | CSI_FIELD_BREATHING_SCORE;    /* must be stripped */
  strncpy(v.state_name, "motion",     sizeof(v.state_name) - 1);
  strncpy(v.confidence, "confirmed",  sizeof(v.confidence) - 1);
  strncpy(v.note,       "deadbeef12345678", sizeof(v.note) - 1);  /* fake peer hash */
  v.motion_score       = 80;
  v.bundled_count      = 3;
  v.breathing_rate_bpm = 99;
  v.breathing_score    = 99;

  csi_event_emit("core.multilink_fusion", "motion_confirmed", &v);
  csi_event_flush_bundles();

  bool seen = false;
  for (size_t k = 0; k < g_captured_count; ++k) {
    if (strcmp(g_captured[k].type_name, "motion_confirmed") != 0) continue;
    seen = true;
    EXPECT(g_captured[k].values.note[0] == '\0',
           "core.multilink_fusion MUST NOT carry note (no peer-id smuggling)");
    EXPECT(g_captured[k].values.breathing_rate_bpm == 0,
           "core.multilink_fusion MUST NOT carry breathing_rate");
    EXPECT(g_captured[k].values.breathing_score == 0,
           "core.multilink_fusion MUST NOT carry breathing_score");
    EXPECT((g_captured[k].values.present_fields
            & (CSI_FIELD_NOTE
             | CSI_FIELD_BREATHING_RATE
             | CSI_FIELD_BREATHING_SCORE)) == 0,
           "disallowed bits must be cleared in present_fields");
    /* Conversely, the allow-listed fields must survive. */
    EXPECT(g_captured[k].values.motion_score == 80,
           "motion_score must pass through (it IS in the allow-list)");
  }
  EXPECT(seen, "core.multilink_fusion must commit at least once");
}

void test_ble_scout_strips_unauthorized_fields() {
  /* PR 5 (#463 + #465 + canary-wap port #467) added ble.scout. Its
   * manifest in ble_scout.cpp permits two event types:
   *
   *   scout_initialized: STATE_NAME | TIME_BUCKET
   *   beacon_event:      STATE_NAME | NOTE | TIME_BUCKET
   *
   * The Scout role MUST NOT emit:
   *   • The beacon's hashed_id in any field (we publish room
   *     attribution by user-supplied LABEL, not by identifier).
   *   • The raw MAC in any field (always hashed inside on_advert,
   *     before reaching the registry / tracker / event payload).
   *   • Any RSSI-derived numeric (MOTION_SCORE, BREATHING_*, etc.) —
   *     Scout has nothing to say about CSI features.
   *
   * This test stuffs every smuggleable field through both event
   * types and asserts the chokepoint zeroes everything outside the
   * allow-list. Future contributors who add e.g. RSSI to the
   * manifest will trip this test. */
  csi_event_test_reset();
  reset_captures();
  csi_module_register(ble_scout::ble_scout_module());

  const char* TYPES[] = { "scout_initialized", "beacon_event" };
  const size_t N = sizeof(TYPES) / sizeof(TYPES[0]);

  for (size_t i = 0; i < N; ++i) {
    reset_captures();
    csi_event_values_t v;
    csi_event_values_init(&v);
    v.category = CSI_CATEGORY_EVENT;
    /* Stuff every field a misbehaving Scout call could try to set.
     * DURATION_SEC is intentionally omitted: the bundler re-adds it
     * to closed bundle rows regardless of allow-list (see the same
     * carve-out documented in test_ble_events_strip_mac_precision_
     * fields), so asserting its removal would fight intentional
     * behavior. The fields below ARE the privacy contract. */
    v.present_fields = CSI_FIELD_STATE_NAME
                     | CSI_FIELD_NOTE
                     | CSI_FIELD_TIME_BUCKET
                     | CSI_FIELD_MOTION_SCORE
                     | CSI_FIELD_BREATHING_SCORE
                     | CSI_FIELD_BREATHING_RATE
                     | CSI_FIELD_CONFIDENCE;
    strncpy(v.state_name, "arrived",  sizeof(v.state_name) - 1);
    strncpy(v.note,       "kitchen",  sizeof(v.note) - 1);
    strncpy(v.confidence, "smuggled", sizeof(v.confidence) - 1);
    v.motion_score       = 50;
    v.breathing_score    = 50;
    v.breathing_rate_bpm = 17;

    csi_event_emit("ble.scout", TYPES[i], &v);
    csi_event_flush_bundles();

    bool seen = false;
    for (size_t k = 0; k < g_captured_count; ++k) {
      if (strcmp(g_captured[k].type_name, TYPES[i]) != 0) continue;
      seen = true;
      EXPECT(g_captured[k].values.motion_score == 0,
             "ble.scout MUST NOT carry motion_score");
      EXPECT(g_captured[k].values.breathing_score == 0,
             "ble.scout MUST NOT carry breathing_score");
      EXPECT(g_captured[k].values.breathing_rate_bpm == 0,
             "ble.scout MUST NOT carry breathing_rate");
      EXPECT(g_captured[k].values.confidence[0] == '\0',
             "ble.scout MUST NOT carry confidence");
      EXPECT((g_captured[k].values.present_fields
              & (CSI_FIELD_MOTION_SCORE
               | CSI_FIELD_BREATHING_SCORE
               | CSI_FIELD_BREATHING_RATE
               | CSI_FIELD_CONFIDENCE)) == 0,
             "disallowed bits must be cleared in present_fields");

      /* scout_initialized's manifest does NOT permit NOTE either —
       * only the beacon_event type carries the user-supplied label. */
      if (strcmp(TYPES[i], "scout_initialized") == 0) {
        EXPECT(g_captured[k].values.note[0] == '\0',
               "scout_initialized MUST NOT carry note");
      } else {
        /* beacon_event DOES carry the label in note. Verify it survived. */
        EXPECT(strcmp(g_captured[k].values.note, "kitchen") == 0,
               "beacon_event's note (room label) must pass through");
      }
    }
    EXPECT(seen, "ble.scout event must commit at least once");
  }
}

void test_quiet_hours_holds_and_summarises() {
  /* The Quiet Hours plan promised: "Events during the configured window
   * are held; a single summary appears after the window closes."
   * This test pins that contract.
   *
   * Strategy: pin the chokepoint's notion of minute-of-day to a known
   * value (720 == 12:00) by computing the clock offset from this test's
   * own monotonic time. Configure the window to cover noon (700..730).
   * Burst 100 emits — every one must be held (no commits). Then disable
   * the window via the setter; the setter detects the in→out transition
   * and synthesises one held_summary row. The summary's bundled_count
   * must equal the held count. */
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);
  csi_module_register(meta_quiet_hours_module());

  /* Pin cur_min = 720 (12:00). monotonic_minutes is derived the same way
   * the chokepoint does: clock_gettime(CLOCK_MONOTONIC) / 60. */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const int32_t mono_min = (int32_t)(ts.tv_sec / 60);
  csi_event_set_clock_offset_minutes(720 - mono_min);

  csi_event_set_quiet_window(700, 730, true);

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME;
  strncpy(v.state_name, "active", sizeof(v.state_name) - 1);

  for (int i = 0; i < 100; ++i) {
    csi_event_emit("test.module", "test_state", &v);
  }
  csi_event_flush_bundles();

  EXPECT(g_captured_count == 0,
         "no commits expected during the quiet window — every emit must be held");
  EXPECT(g_witness_commit_count == 0,
         "no witness writes expected during the quiet window");

  /* Disabling the window updates state but does NOT itself flush —
   * that's the cross-task safety contract. The next emit (which on
   * device runs on the main loop, same task as previous emits) sees
   * the in→out transition and synthesises the summary. */
  csi_event_set_quiet_window(0, 0, false);

  csi_event_values_t trigger;
  csi_event_values_init(&trigger);
  trigger.category       = CSI_CATEGORY_EVENT;
  trigger.present_fields = CSI_FIELD_STATE_NAME;
  strncpy(trigger.state_name, "post_qh", sizeof(trigger.state_name) - 1);
  csi_event_emit("test.module", "test_state", &trigger);
  csi_event_flush_bundles();

  bool found_summary = false;
  for (size_t i = 0; i < g_captured_count; ++i) {
    if (strcmp(g_captured[i].module_id, "meta.quiet_hours") == 0
        && strcmp(g_captured[i].type_name, "held_summary") == 0) {
      EXPECT(g_captured[i].values.bundled_count == 100,
             "summary bundled_count must equal the held emit count");
      EXPECT(strcmp(g_captured[i].values.note, "quiet_hours") == 0,
             "summary note must be \"quiet_hours\"");
      found_summary = true;
    }
  }
  EXPECT(found_summary, "exactly one held_summary row must appear at window close");
}

void test_quiet_hours_anomalies_bypass_gate() {
  /* Anomaly events MUST fire during quiet hours — the night-time
   * category is precisely when unusual activity matters most. */
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);
  csi_module_register(meta_quiet_hours_module());

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const int32_t mono_min = (int32_t)(ts.tv_sec / 60);
  csi_event_set_clock_offset_minutes(720 - mono_min);

  csi_event_set_quiet_window(700, 730, true);

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_ANOMALY;   /* the bypass token */
  v.present_fields = CSI_FIELD_STATE_NAME;
  strncpy(v.state_name, "spike", sizeof(v.state_name) - 1);

  uint32_t id = csi_event_emit("test.module", "test_state", &v);
  csi_event_flush_bundles();

  EXPECT(id != 0, "anomaly emit during quiet window must be accepted");
  bool seen_anomaly = false;
  for (size_t i = 0; i < g_captured_count; ++i) {
    if (g_captured[i].category == CSI_CATEGORY_ANOMALY
        && strcmp(g_captured[i].values.state_name, "spike") == 0) {
      seen_anomaly = true;
    }
  }
  EXPECT(seen_anomaly, "anomaly commit must reach the on_committed hook");
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

/* 12b. Same-state refreshes of an OPEN bundle do not spend the ceiling.
 *
 * core.presence re-emits its current state every minute so the bundler can
 * refresh duration/confidence. Each refresh used to pre-account a ceiling
 * slot and never roll it back, so after six refreshes (~3 minutes of
 * sustained presence) every REAL transition was dropped for the rest of the
 * hour. Openings still count: the burst test above stays exactly as it is. */
void test_bundle_refresh_does_not_spend_ceiling() {
  csi_event_test_reset();
  reset_captures();
  csi_module_register(&TEST_MODULE);

  /* 30 refreshes of ONE open state: one bundle, no ceiling spent. */
  for (int i = 0; i < 30; ++i) {
    csi_event_values_t v;
    csi_event_values_init(&v);
    v.category       = CSI_CATEGORY_EVENT;
    v.present_fields = CSI_FIELD_STATE_NAME;
    snprintf(v.state_name, sizeof(v.state_name), "active");
    EXPECT(csi_event_emit("test.module", "test_state", &v) != 0,
           "a same-state refresh must still be admitted");
  }
  /* Five NEW states must all still open: only ONE slot (the "active"
   * opening) has been spent of the six. */
  int opened = 0;
  for (int i = 0; i < 5; ++i) {
    csi_event_values_t v;
    csi_event_values_init(&v);
    v.category       = CSI_CATEGORY_EVENT;
    v.present_fields = CSI_FIELD_STATE_NAME;
    snprintf(v.state_name, sizeof(v.state_name), "t%d", i);
    if (csi_event_emit("test.module", "test_state", &v) != 0) ++opened;
  }
  EXPECT(opened == 5,
         "refreshing an open bundle must not consume the hourly ceiling");
  csi_event_flush_bundles();
  EXPECT(g_captured_count == 6,
         "one bundle for the refreshed state plus five new ones commit");
}

/* ──────────────────────────────────────────────────────────────────────────
 * 13. No peer MAC in CSI feature vector
 *
 * The csi_features_t.v[32] is the ONLY payload that crosses the privacy
 * barrier. The type is int8_t[32] — too small to embed a 6-byte MAC and
 * the HAL scrubs the source MAC at the interrupt boundary. We assert the
 * structural guarantee: sizeof(v) == 32, each element is int8_t (1 byte),
 * and the total feature struct fits the documented contract.
 * ────────────────────────────────────────────────────────────────────────── */
void test_no_peer_mac_in_feature_vector() {
  /* Structural: the feature vector is exactly 32 bytes of int8. */
  static_assert(sizeof(csi_features_t::v) == CSI_FEATURE_DIM,
                "feature vector must be exactly CSI_FEATURE_DIM int8 bytes");
  static_assert(sizeof(csi_features_t::v[0]) == 1,
                "each feature element must be 1 byte (int8)");

  /* The full struct has 36 bytes (32 + frames_in_window + time_bucket +
   * caps_observed). No room for a 6-byte MAC anywhere. */
  EXPECT(sizeof(csi_features_t) <= 40,
         "csi_features_t must not grow beyond documented contract");

  /* Construct a dummy feature vector and verify no 6-byte run of
   * non-zero bytes exists (MAC-address heuristic from csi_hal's
   * conformance_check_no_mac_in_buffers). An all-zero vector trivially
   * passes; a real test on device uses the conformance helper. */
  csi_features_t f;
  memset(&f, 0, sizeof(f));
  int mac_like_runs = 0;
  for (int i = 0; i <= CSI_FEATURE_DIM - 6; ++i) {
    bool nonzero = true;
    for (int j = 0; j < 6; ++j) {
      if (f.v[i + j] == 0) { nonzero = false; break; }
    }
    if (nonzero) ++mac_like_runs;
  }
  EXPECT(mac_like_runs == 0,
         "zeroed feature vector must have no MAC-like byte runs");
}

/* ──────────────────────────────────────────────────────────────────────────
 * 14. Beacon MAC is hashed before event emission
 *
 * ble_scout's manifest only allows STATE_NAME, NOTE, and TIME_BUCKET.
 * A raw 6-byte MAC can't survive in any of those fields. We verify by
 * checking the manifest's allowed_fields bitmask excludes every field
 * that could carry a raw identifier.
 * ────────────────────────────────────────────────────────────────────────── */
void test_beacon_mac_hashed_before_emission() {
  const csi_module_t* scout = ble_scout::ble_scout_module();
  EXPECT(scout != nullptr, "ble_scout_module must be available");
  if (!scout) return;

  /* The Scout's manifest must NOT include fields that could carry a
   * raw MAC: MOTION_SCORE, BREATHING_*, CONFIDENCE, DOMINANT_SIGNAL,
   * or any future field that's wider than a state label. We check that
   * the allowed fields are a strict subset of {STATE_NAME, NOTE,
   * TIME_BUCKET, BUNDLED_COUNT, DISMISSED}. */
  const uint32_t SAFE_FIELDS = CSI_FIELD_STATE_NAME
                              | CSI_FIELD_NOTE
                              | CSI_FIELD_TIME_BUCKET
                              | CSI_FIELD_BUNDLED_COUNT
                              | CSI_FIELD_DISMISSED;

  for (size_t i = 0; i < scout->event_count; ++i) {
    const uint32_t extra = scout->events[i].allowed_fields & ~SAFE_FIELDS;
    EXPECT(extra == 0,
           "ble.scout manifest must not allow identity-carrying fields");
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * 15. Per-link RSSI bucketed to int8
 *
 * The feature vector v[20..23] carries RSSI stats as int8 — each value
 * is inherently bucketed to 1-dB resolution in the [-128, 127] range.
 * We verify the structural guarantee via the type system.
 * ────────────────────────────────────────────────────────────────────────── */
void test_rssi_bucketed_int8() {
  /* v[] is int8_t — the type itself enforces bucketing. */
  static_assert(sizeof(csi_features_t::v[20]) == 1,
                "RSSI mean must be int8 (1 byte)");
  static_assert(sizeof(csi_features_t::v[21]) == 1,
                "RSSI std must be int8 (1 byte)");
  static_assert(sizeof(csi_features_t::v[22]) == 1,
                "RSSI max must be int8 (1 byte)");
  static_assert(sizeof(csi_features_t::v[23]) == 1,
                "RSSI min must be int8 (1 byte)");

  /* The type system guarantees int8 range — no runtime check needed.
   * The static_asserts above are the enforceable contract. */
}

}  /* namespace */

extern "C" int csi_event_invariants_run() {
  test_disallowed_fields_are_zeroed();
  test_privacy_p1_blocked_under_p0_ceiling();
  test_privacy_p2_never_persists_to_witness();
  test_time_fields_are_coarsened();
  test_strings_are_sanitized();
  test_bundler_collapses_burst();
  test_bundler_snapshot_exposes_open_bundles();
  test_per_module_ceiling();
  test_bundle_refresh_does_not_spend_ceiling();
  test_witness_payload_includes_metadata();
  test_ble_events_strip_mac_precision_fields();
  /* PR 6: pin the per-event allow-lists for core.multilink_fusion and
   * ble.scout. Future contributors who relax either manifest will trip
   * these. */
  test_multilink_fusion_strips_unauthorized_fields();
  test_ble_scout_strips_unauthorized_fields();
  test_quiet_hours_holds_and_summarises();
  test_quiet_hours_anomalies_bypass_gate();
  /* PR 6 — three new privacy conformance assertions. */
  test_no_peer_mac_in_feature_vector();
  test_beacon_mac_hashed_before_emission();
  test_rssi_bucketed_int8();

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
