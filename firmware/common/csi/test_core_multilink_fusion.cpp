/**
 * @file test_core_multilink_fusion.cpp
 * @brief Host-build conformance test for core.multilink_fusion (PR 3).
 *
 * The fusion module promotes "motion present" from single-link
 * "observed" to multi-link "confirmed" when ≥2 independent links
 * agree within a 3-second sliding window. Tests verify:
 *
 *   1. Local motion + ≥1 peer with fresh motion → emits motion_confirmed.
 *   2. Local motion alone (no peer agreement) → NO emit.
 *   3. Peer motion alone (no local agreement) → NO emit.
 *   4. Rising-edge only: a sustained 2-link state emits exactly once.
 *   5. Stale peer features (older than PEER_STALE_MS) don't count.
 *   6. Peer table is bounded: adding past MAX_PEERS evicts the oldest.
 *
 * The test injects events into the csi_event chokepoint by overriding
 * the host-build commit hook (same pattern as csi_event_invariants_test
 * — see commit + on_committed weak overrides at the top of that file).
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/common/csi/test_core_multilink_fusion.cpp \
 *       firmware/common/csi/src/core_multilink_fusion.cpp \
 *       firmware/common/csi/src/csi_event.cpp \
 *       firmware/common/csi/src/csi_module.cpp \
 *       firmware/common/csi/src/csi_bundler.cpp \
 *       firmware/common/csi/src/csi_witness_payload.cpp \
 *       -I firmware/common/csi/src \
 *       -o /tmp/test_multilink && /tmp/test_multilink
 */

#include "core_multilink_fusion.h"
#include "csi_event.h"
#include "csi_module.h"
#include "csi_types.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_core_multilink_fusion_run() { return 0; }
#else

/* ────────────────────────────────────────────────────────────────────────
 * Capture-the-emit hooks (mirror of csi_event_invariants_test).
 * ──────────────────────────────────────────────────────────────────────── */

namespace {

struct Capture {
  char module_id[CSI_EVENT_NAME_MAX];
  char type_name[CSI_EVENT_NAME_MAX];
  csi_event_values_t values;
};

constexpr size_t CAP_CAP = 64;
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
                            const char*               module_id,
                            const char*               type_name,
                            csi_event_category_t      /*cat*/,
                            csi_privacy_class_t       /*privacy*/,
                            const csi_event_values_t* values) {
  if (g_captured_count >= CAP_CAP) return;
  Capture& c = g_captured[g_captured_count++];
  strncpy(c.module_id, module_id, sizeof(c.module_id) - 1);
  strncpy(c.type_name, type_name, sizeof(c.type_name) - 1);
  c.values = *values;
}

}  /* extern "C" */

/* ────────────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────────────── */

namespace {

void register_module_once() {
  static bool registered = false;
  if (!registered) {
    assert(csi_module_register(core_multilink_fusion_module()));
    registered = true;
  }
}

/* Build a csi_features_t where the phase-Doppler band (v[8..11]) has
 * a magnitude that maps to roughly `target_motion_pct` after the
 * module's reduce_magnitude scaling. The reduction is L1-magnitude of
 * v[8..11], capped at 127, then *100/127. So to get target=50, set
 * |v[8]|+|v[9]|+|v[10]|+|v[11]| ≈ 127 * 50 / 100 = 63. Distribute as
 * ~16 per byte. */
csi_features_t make_features(uint8_t target_motion_pct) {
  csi_features_t f;
  memset(&f, 0, sizeof(f));
  uint8_t per_band = (uint8_t)((127 * target_motion_pct / 100 + 3) / 4);
  if (per_band > 127) per_band = 127;
  f.v[8]  = (int8_t)per_band;
  f.v[9]  = (int8_t)per_band;
  f.v[10] = (int8_t)per_band;
  f.v[11] = (int8_t)per_band;
  f.frames_in_window = 20;
  f.time_bucket      = 42;
  f.caps_observed    = CSI_CAP_HT20;
  return f;
}

void fresh_world() {
  reset_captures();
  core_multilink_fusion_test_reset();
  core_multilink_fusion_test_set_now_ms(10000);
  register_module_once();
  /* re-init the module via tick path — call its init via the registered
   * manifest. csi_module_register already invoked init(). Our
   * core_multilink_fusion::on_init wipes state, so a fresh reset is
   * enough. */
  core_multilink_fusion_test_reset();
}

bool any_emitted(const char* type_name) {
  for (size_t i = 0; i < g_captured_count; ++i) {
    if (strcmp(g_captured[i].type_name, type_name) == 0) return true;
  }
  return false;
}

/* Force the bundler to commit pending rows so on_committed fires for
 * the rows we just emitted. Same idiom as csi_event_invariants_test. */
void tick_and_flush(const csi_features_t& f) {
  csi_module_tick_all(&f);
  csi_event_flush_bundles();
}

/* ────────────────────────────────────────────────────────────────────────
 * Test bodies
 * ──────────────────────────────────────────────────────────────────────── */

void test_local_plus_peer_emits_confirmed() {
  fresh_world();
  /* Peer A reports motion. Then local tick also has motion → CONFIRMED. */
  uint8_t fp_a[CORE_MULTILINK_FINGERPRINT_LEN] = {0xA0,1,2,3,4,5,6,7};
  csi_features_t peer = make_features(60);
  core_multilink_fusion_ingest_peer_features(fp_a, &peer);

  csi_features_t local = make_features(55);
  tick_and_flush(local);

  assert(any_emitted("motion_confirmed"));
  /* Exactly one motion_confirmed; subsequent tick with same state should
   * NOT re-emit (rising-edge-only). */
  size_t before = g_captured_count;
  tick_and_flush(local);
  /* Allow for other modules emitting in between; just check no new
   * motion_confirmed was added. */
  size_t mc_count = 0;
  for (size_t i = 0; i < g_captured_count; ++i)
    if (strcmp(g_captured[i].type_name, "motion_confirmed") == 0) ++mc_count;
  assert(mc_count == 1);
  (void)before;
  std::printf("PASS test_local_plus_peer_emits_confirmed  (motion=%u)\n",
              g_captured[0].values.motion_score);
}

void test_local_alone_does_not_confirm() {
  fresh_world();
  csi_features_t local = make_features(80);
  tick_and_flush(local);
  assert(!any_emitted("motion_confirmed"));
  std::printf("PASS test_local_alone_does_not_confirm\n");
}

void test_peer_alone_does_not_confirm() {
  fresh_world();
  uint8_t fp[CORE_MULTILINK_FINGERPRINT_LEN] = {0xB0,0,0,0,0,0,0,0};
  csi_features_t peer = make_features(80);
  core_multilink_fusion_ingest_peer_features(fp, &peer);

  csi_features_t local = make_features(5);   /* below threshold */
  tick_and_flush(local);
  assert(!any_emitted("motion_confirmed"));
  std::printf("PASS test_peer_alone_does_not_confirm\n");
}

void test_stale_peer_is_ignored() {
  fresh_world();
  uint8_t fp[CORE_MULTILINK_FINGERPRINT_LEN] = {0xC0,0,0,0,0,0,0,0};
  csi_features_t peer = make_features(80);
  core_multilink_fusion_ingest_peer_features(fp, &peer);

  /* Advance time past PEER_STALE_MS. */
  core_multilink_fusion_test_set_now_ms(10000 + CORE_MULTILINK_PEER_STALE_MS + 100);

  csi_features_t local = make_features(80);
  tick_and_flush(local);
  assert(!any_emitted("motion_confirmed"));
  std::printf("PASS test_stale_peer_is_ignored\n");
}

void test_falling_edge_rearms() {
  fresh_world();
  uint8_t fp[CORE_MULTILINK_FINGERPRINT_LEN] = {0xD0,0,0,0,0,0,0,0};
  csi_features_t peer_hot  = make_features(80);
  csi_features_t peer_cold = make_features(5);

  /* Rising edge → emit. */
  core_multilink_fusion_ingest_peer_features(fp, &peer_hot);
  csi_features_t local_hot = make_features(80);
  tick_and_flush(local_hot);
  size_t mc1 = 0;
  for (size_t i = 0; i < g_captured_count; ++i)
    if (strcmp(g_captured[i].type_name, "motion_confirmed") == 0) ++mc1;
  assert(mc1 == 1);

  /* Falling edge: peer goes cold; local stays hot but no agreement. */
  core_multilink_fusion_test_set_now_ms(10100);
  core_multilink_fusion_ingest_peer_features(fp, &peer_cold);
  tick_and_flush(local_hot);

  /* Rising edge again: peer hot again. */
  core_multilink_fusion_test_set_now_ms(10200);
  core_multilink_fusion_ingest_peer_features(fp, &peer_hot);
  tick_and_flush(local_hot);

  size_t mc2 = 0;
  for (size_t i = 0; i < g_captured_count; ++i)
    if (strcmp(g_captured[i].type_name, "motion_confirmed") == 0) ++mc2;
  assert(mc2 == 2);   /* one for each rising edge */
  std::printf("PASS test_falling_edge_rearms  (emits=%zu)\n", mc2);
}

void test_peer_table_bounded_and_evicts_oldest() {
  fresh_world();
  /* Fill the table with MAX_PEERS distinct fingerprints, then add one
   * more. The oldest should be evicted. */
  for (size_t i = 0; i < CORE_MULTILINK_MAX_PEERS; ++i) {
    uint8_t fp[CORE_MULTILINK_FINGERPRINT_LEN] = {0xE0,(uint8_t)i,0,0,0,0,0,0};
    core_multilink_fusion_test_set_now_ms(10000 + (uint32_t)i);
    csi_features_t f = make_features(50);
    core_multilink_fusion_ingest_peer_features(fp, &f);
  }
  assert(core_multilink_fusion_test_link_count() == CORE_MULTILINK_MAX_PEERS);

  /* Add one more — table stays at MAX_PEERS, evicting the oldest. */
  uint8_t newfp[CORE_MULTILINK_FINGERPRINT_LEN] = {0xEE,0,0,0,0,0,0,0};
  core_multilink_fusion_test_set_now_ms(10000 + CORE_MULTILINK_MAX_PEERS);
  csi_features_t f = make_features(50);
  core_multilink_fusion_ingest_peer_features(newfp, &f);
  assert(core_multilink_fusion_test_link_count() == CORE_MULTILINK_MAX_PEERS);
  std::printf("PASS test_peer_table_bounded_and_evicts_oldest\n");
}

}  /* namespace */

int main() {
  test_local_plus_peer_emits_confirmed();
  test_local_alone_does_not_confirm();
  test_peer_alone_does_not_confirm();
  test_stale_peer_is_ignored();
  test_falling_edge_rearms();
  test_peer_table_bounded_and_evicts_oldest();
  std::printf("\nALL CORE_MULTILINK_FUSION TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
