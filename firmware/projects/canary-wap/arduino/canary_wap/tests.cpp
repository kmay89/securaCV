/*
 * SecuraCV Canary — Test harness + red-team — Implementation
 *
 * Each red-team scenario carries its own local save/restore so running
 * the suite at boot time doesn't pollute production state.
 */

#include "tests.h"

#include "rf_presence.h"
#include "household.h"
#include "familiar.h"
#include "baseline.h"
#include "notify.h"
#include "federated.h"
#include "wizard.h"
#include "dp.h"
#include <csi_hal.h>  // Moved to firmware/common/csi/ — see csi_types.h header for build wiring.
#include "health_log.h"

#include <string.h>
#include <esp_system.h>      // esp_fill_random
#include <mbedtls/aes.h>

namespace tests {

// ────────────────────────────────────────────────────────────────────────────
// HELPERS
// ────────────────────────────────────────────────────────────────────────────

static inline void push_result(Report* r, bool conformance,
                               const char* name, bool passed) {
  if (conformance) {
    if (r->conformance_count >= MAX_MODULE_RESULTS) return;
    r->conformance[r->conformance_count++] = NamedResult{ name, passed };
    if (passed) r->conformance_passed++;
  } else {
    if (r->red_team_count >= MAX_REDTEAM_RESULTS) return;
    r->red_team[r->red_team_count++] = NamedResult{ name, passed };
    if (passed) r->red_team_passed++;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// CONFORMANCE BATTERY
// ────────────────────────────────────────────────────────────────────────────

bool run_all_conformance(Report* out) {
  if (!out) return false;
  // Zero only the conformance fields. If the caller invoked
  // run_all_red_team() first (or is reusing a Report struct), an
  // unconditional memset of the whole struct would clobber those
  // results — codex P2 on #320.
  out->conformance_count  = 0;
  out->conformance_passed = 0;
  for (size_t i = 0; i < MAX_MODULE_RESULTS; i++) {
    out->conformance[i] = NamedResult{ nullptr, false };
  }

  // Modules in include-order so failures point at the lowest-level
  // module that broke (saves debugging time).
  push_result(out, true, "csi_hal::no_mac_in_buffers",
              csi_hal::conformance_check_no_mac_in_buffers());
  push_result(out, true, "household::self_test",
              household::conformance_self_test());
  push_result(out, true, "household::no_mac_in_slots",
              household::conformance_no_mac_in_slots());
  push_result(out, true, "familiar::self_test",
              familiar::conformance_self_test());
  push_result(out, true, "baseline::self_test",
              baseline::conformance_self_test());
  push_result(out, true, "notify::self_test",
              notify::conformance_self_test());
  push_result(out, true, "federated::self_test",
              federated::conformance_self_test());
  push_result(out, true, "wizard::self_test",
              wizard::conformance_self_test());

  return out->conformance_passed == out->conformance_count;
}

// ────────────────────────────────────────────────────────────────────────────
// RED-TEAM 1 — MAC randomization replay
//
// Threat: an attacker who knows we hash MACs into a token map could
// flood us with unique random MACs to overflow the map and force
// eviction of legitimate tokens, or to embed a recognizable
// identifier in the side channel of memory layout.
//
// Defense: bounded token map (SESSION_TOKEN_MAP_SIZE = 32) with TTL-
// based eviction (OBSERVATION_TTL_MS = 60s); rotation wipes
// everything; observations carry only aggregate fields (no MAC).
// ────────────────────────────────────────────────────────────────────────────

bool red_team_mac_randomization_replay() {
  // Snapshot rotation count so we can verify rotation wiped state.
  const uint32_t epoch_before = rf_presence::get_session_epoch();

  // Stream 1024 distinct random MACs.
  for (uint32_t i = 0; i < 1024; i++) {
    uint8_t mac[6];
    esp_fill_random(mac, sizeof(mac));
    // Ensure the LSB pattern marks it NOT as an RPA (top 2 bits != 01),
    // so household::resolve_rpa returns false and the path runs through
    // the token derivation.
    mac[5] = (mac[5] & 0x3F) | 0xC0;   // static random (top bits = 11)
    rf_presence::feed_ble_scan(mac, /*rssi*/ -50, /*connectable*/ true);
  }

  // The token map is private to rf_presence; we use the conformance
  // check it already exposes.
  if (!rf_presence::conformance_check_no_mac_storage()) return false;
  if (!rf_presence::conformance_check_aggregate_only()) return false;

  // Force a session rotation; rf_presence guarantees this wipes the
  // token map. Verify the rotation happened (epoch incremented).
  rf_presence::rotate_session();
  const uint32_t epoch_after = rf_presence::get_session_epoch();
  if (epoch_after != epoch_before + 1) return false;

  // Post-rotation conformance must still hold.
  if (!rf_presence::conformance_check_no_mac_storage()) return false;
  if (!rf_presence::conformance_check_aggregate_only()) return false;

  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// RED-TEAM 2 — Cloned RPA
//
// Threat: an attacker observes our paired phone's RPA broadcasting,
// records it, and re-emits it from their own device hoping we treat
// the "cloned" address as household.
//
// Defense: RPA resolution is keyed on the IRK (a 16-byte secret
// known only to the bonded device + canary). A clone of the visible
// MAC bytes does NOT carry the IRK, so the AES-128 hash check in
// household::resolve_rpa fails. This test enrolls a TEST IRK,
// synthesizes a matching RPA (control), and a non-matching RPA
// (attack), and verifies the resolver gets both right.
// ────────────────────────────────────────────────────────────────────────────

// Re-implementation of the BT Core Spec ah() helper for synthesis.
// Same as the one inside household.cpp; duplicated here so we can
// generate the test vector without touching that module's internals.
static bool synth_ah(const uint8_t irk[16], const uint8_t r[3], uint8_t out[3]) {
  uint8_t padded[16] = {0};
  uint8_t cipher[16];
  padded[13] = r[2];
  padded[14] = r[1];
  padded[15] = r[0];

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_enc(&ctx, irk, 128) != 0 ||
      mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, padded, cipher) != 0) {
    mbedtls_aes_free(&ctx);
    return false;
  }
  mbedtls_aes_free(&ctx);
  out[0] = cipher[15]; out[1] = cipher[14]; out[2] = cipher[13];
  return true;
}

bool red_team_cloned_rpa() {
  // 1. Snapshot how many slots are occupied so we can leave the
  //    enrolled set untouched.
  const uint8_t baseline_count = household::count();

  // 2. Generate a fresh test IRK + a matching valid RPA.
  uint8_t test_irk[16];
  esp_fill_random(test_irk, sizeof(test_irk));
  const uint8_t prand[3] = { 0x12, 0x34, 0x40 };  // top 2 bits = 01 (RPA type)
  uint8_t hash[3];
  if (!synth_ah(test_irk, prand, hash)) return false;

  uint8_t valid_rpa[6];
  valid_rpa[0] = hash[0];
  valid_rpa[1] = hash[1];
  valid_rpa[2] = hash[2];
  valid_rpa[3] = prand[0];
  valid_rpa[4] = prand[1];
  valid_rpa[5] = prand[2];   // already has top 2 bits = 01

  // 3. Before enrollment, neither RPA should resolve.
  if (household::resolve_rpa(valid_rpa)) return false;

  // 4. Enroll the test IRK. Open enrollment first.
  household::begin_enrollment();
  const int slot = household::add_irk(test_irk, "_redteam_");
  household::end_enrollment();
  if (slot < 0) return false;

  // 5. valid_rpa now resolves; a CLONE with one byte flipped does NOT.
  if (!household::resolve_rpa(valid_rpa)) {
    household::remove_by_slot((uint8_t)slot);
    return false;
  }

  uint8_t cloned_rpa[6];
  memcpy(cloned_rpa, valid_rpa, 6);
  cloned_rpa[0] ^= 0x80;   // flip a bit in the hash → no longer matches AES output
  // Keep the RPA-type bits in mac[5] intact so it still LOOKS like an RPA.
  if (household::resolve_rpa(cloned_rpa)) {
    household::remove_by_slot((uint8_t)slot);
    return false;  // attacker would have walked in
  }

  // 6. A static random address (not an RPA at all) must not resolve
  //    even when bytes happen to match the hash bytes.
  uint8_t static_rand[6];
  memcpy(static_rand, valid_rpa, 6);
  static_rand[5] = (static_rand[5] & 0x3F) | 0xC0;   // top 2 bits = 11 (static)
  if (household::resolve_rpa(static_rand)) {
    household::remove_by_slot((uint8_t)slot);
    return false;
  }

  // 7. Cleanup: remove the test slot, verify count returned to baseline.
  household::remove_by_slot((uint8_t)slot);
  if (household::count() != baseline_count) return false;

  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// RED-TEAM 3 — Federated baseline poisoning
//
// Threat: a malicious peer on the Opera mesh submits a baseline share
// with absurdly large counts and sums, hoping to either overflow our
// int64 accumulator or take over the bucket distribution so anomaly
// detection collapses (everything looks normal).
//
// Defense: baseline::merge_remote_bucket caps add_count at
// REMOTE_MERGE_MAX_COUNT (=64) regardless of what the peer claims,
// and pre-clamps share.sum / share.sum_sq magnitudes before the
// multiply so the int64 arithmetic stays in range.
// ────────────────────────────────────────────────────────────────────────────

bool red_team_federated_poisoning() {
  const uint8_t test_bucket = 7;

  // Snapshot the bucket so we can restore it.
  baseline::RemoteBucketShare snap;
  if (!baseline::snapshot_bucket(test_bucket, &snap)) return false;

  // Build a poisoned share: max count, INT32_MAX sums, INT64_MAX sum_sq.
  baseline::RemoteBucketShare poison;
  poison.count = 65535;
  for (uint8_t i = 0; i < baseline::FEATURE_COUNT; i++) {
    poison.sum[i]    = INT32_MAX;
    poison.sum_sq[i] = INT64_MAX;
  }

  // Merge should succeed (it returns false only on invalid bucket /
  // saturated local), but the contribution must be clamped.
  baseline::merge_remote_bucket(test_bucket, poison);

  // Check the post-merge bucket: count must NOT exceed BUCKET_MAX_COUNT,
  // and the increase relative to the snapshot must NOT exceed
  // REMOTE_MERGE_MAX_COUNT.
  baseline::RemoteBucketShare after;
  if (!baseline::snapshot_bucket(test_bucket, &after)) return false;

  const uint16_t headroom_before =
      (snap.count >= baseline::BUCKET_MAX_COUNT)
      ? 0
      : (uint16_t)(baseline::BUCKET_MAX_COUNT - snap.count);
  const uint16_t expected_max_add =
      headroom_before > baseline::REMOTE_MERGE_MAX_COUNT
      ? baseline::REMOTE_MERGE_MAX_COUNT
      : headroom_before;
  const uint16_t actual_added = (uint16_t)(after.count - snap.count);

  if (actual_added > expected_max_add) {
    return false;  // peer's count was not properly clamped
  }
  if (after.count > baseline::BUCKET_MAX_COUNT) {
    return false;  // local cap not enforced
  }

  // sum / sum_sq must be finite (post-merge file has int32/int64 saturate
  // guards, so they should saturate at INT32_MAX / INT64_MAX rather than
  // wrap to negative).
  bool numeric_ok = true;
  for (uint8_t i = 0; i < baseline::FEATURE_COUNT; i++) {
    if (after.sum[i] < 0)    numeric_ok = false;  // overflow → wrapped negative
    if (after.sum_sq[i] < 0) numeric_ok = false;
  }

  // Restore the bucket from our pre-test snapshot. This uses the test-
  // only setter we added specifically so this scenario can run on
  // production devices without polluting the baseline.
  if (!baseline::overwrite_bucket_for_tests(test_bucket, snap)) return false;

  // Sanity-check that the restore actually round-tripped.
  baseline::RemoteBucketShare verify;
  if (!baseline::snapshot_bucket(test_bucket, &verify)) return false;
  if (verify.count != snap.count) return false;
  for (uint8_t i = 0; i < baseline::FEATURE_COUNT; i++) {
    if (verify.sum[i]    != snap.sum[i])    return false;
    if (verify.sum_sq[i] != snap.sum_sq[i]) return false;
  }

  return numeric_ok;
}

bool run_all_red_team(Report* out) {
  if (!out) return false;
  // Zero only the red-team fields. Symmetric with run_all_conformance
  // (codex P2 on #320). Without this, calling run_all_red_team() with a
  // stale Report whose red_team_count is already ≥ MAX_REDTEAM_RESULTS
  // would silently drop every push_result() entry.
  out->red_team_count  = 0;
  out->red_team_passed = 0;
  for (size_t i = 0; i < MAX_REDTEAM_RESULTS; i++) {
    out->red_team[i] = NamedResult{ nullptr, false };
  }

  push_result(out, false, "mac_randomization_replay",
              red_team_mac_randomization_replay());
  push_result(out, false, "cloned_rpa",
              red_team_cloned_rpa());
  push_result(out, false, "federated_poisoning",
              red_team_federated_poisoning());

  return out->red_team_passed == out->red_team_count;
}

// ────────────────────────────────────────────────────────────────────────────
// COMBINED RUN
// ────────────────────────────────────────────────────────────────────────────

bool run_all(Report* out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));

  const uint32_t t0 = millis();
  const bool conf_ok = run_all_conformance(out);
  // run_all_red_team pushes onto the same report; don't memset between calls.
  const bool rt_ok   = run_all_red_team(out);
  out->total_ms = millis() - t0;
  out->all_passed = conf_ok && rt_ok;

  if (out->all_passed) {
    health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "Tests: ALL PASS (conf %u/%u, rt %u/%u, %ums)",
      (unsigned)out->conformance_passed, (unsigned)out->conformance_count,
      (unsigned)out->red_team_passed,    (unsigned)out->red_team_count,
      (unsigned)out->total_ms);
  } else {
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Tests: FAIL (conf %u/%u, rt %u/%u, %ums)",
      (unsigned)out->conformance_passed, (unsigned)out->conformance_count,
      (unsigned)out->red_team_passed,    (unsigned)out->red_team_count,
      (unsigned)out->total_ms);
    // Log each failure individually so the operator can see what broke.
    for (uint8_t i = 0; i < out->conformance_count; i++) {
      if (!out->conformance[i].passed) {
        health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
          "Tests: conformance FAIL: %s", out->conformance[i].name);
      }
    }
    for (uint8_t i = 0; i < out->red_team_count; i++) {
      if (!out->red_team[i].passed) {
        health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
          "Tests: red_team FAIL: %s", out->red_team[i].name);
      }
    }
  }
  return out->all_passed;
}

}  // namespace tests
