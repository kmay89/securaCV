/**
 * @file core_multilink_fusion.cpp
 * @brief core.multilink_fusion — implementation.
 *
 * Design (PR 3 v1, "motion_confirmed" only):
 *
 *   • Maintain a small per-peer table keyed on the 8-byte fingerprint.
 *     Each slot stores the last-received csi_features_t window plus the
 *     wall-clock time it arrived.
 *
 *   • In on_tick(local_features):
 *       1. Compute the local motion score the same way core.presence
 *          does (reduce magnitude of the phase-Doppler bands).
 *       2. Walk the peer table; expire entries older than
 *          CORE_MULTILINK_PEER_STALE_MS.
 *       3. Count how many fresh peers exceed the motion threshold.
 *       4. If local is also above threshold AND fresh_peers >= 1, that
 *          is two independent links agreeing — emit a confirmed event.
 *
 *   • Hysteresis: emit only on the rising edge (transition from
 *     "<2 links agree" to ">=2 links agree"). The bundler still
 *     deduplicates if the underlying signal flickers.
 *
 * Privacy: P0. The emitted event carries only state_name, confidence,
 * motion_score, and time_bucket — every field is bucketed/coarse and
 * already in the existing allow-list.
 *
 * What this v1 deliberately does NOT do (deferred):
 *   • Breathing fusion (require ≥2 links agreeing on BPM within
 *     tolerance) — needs the BPM extraction from breathing_score that
 *     the breathing module produces; defer to PR 3b.
 *   • Motion-direction inference from cross-link RSSI delta — PR 3c.
 *   • Per-event_id dismiss feedback — PR 3d.
 */

#include "core_multilink_fusion.h"
#include "csi_event.h"
#include "csi_module.h"

#include <stdlib.h>   /* abs() */
#include <string.h>

#ifndef CSI_TEST_HOST_BUILD
  #include <Arduino.h>   /* millis() — extern "C" linkage */
#endif

/* ────────────────────────────────────────────────────────────────────────
 * STATIC CONFIGURATION
 *
 * Mirrored from core_presence.cpp so a single-link "observed" event from
 * core.presence and a fusion "confirmed" event from here are computed
 * from the same primitive on the same window — otherwise the two could
 * disagree on whether motion is present at all.
 * ──────────────────────────────────────────────────────────────────────── */

namespace {

/* csi_features_t.v[] layout — mirrored from csi_features.h:11-18. */
constexpr int IDX_DOPPLER_BASE = 8;   /* phase-Doppler bands [8..11] */
constexpr int IDX_DOPPLER_END  = 12;

/* Motion threshold. Matches core_presence.cpp default. Tunable later. */
constexpr uint8_t MOTION_THRESHOLD = 35;

/* Per-peer link state. */
struct Link {
  uint8_t  fingerprint[CORE_MULTILINK_FINGERPRINT_LEN];
  uint32_t last_seen_ms;
  uint8_t  motion_score;          /* cached so on_tick doesn't recompute */
  bool     in_use;
};

static Link     s_links[CORE_MULTILINK_MAX_PEERS];

/* Whether we've already emitted the rising-edge "motion_confirmed"
 * event for the current contiguous stretch. Reset when fewer than 2
 * links agree. */
static bool     s_confirmed_emitted = false;

/* Virtual clock used by tests; on device we read millis() via the
 * mockable now_ms() helper below. */
#ifdef CSI_TEST_HOST_BUILD
static uint32_t s_test_now_ms = 0;
static bool     s_test_now_set = false;
#endif

inline uint32_t now_ms() {
#ifdef CSI_TEST_HOST_BUILD
  if (s_test_now_set) return s_test_now_ms;
  /* Process-static counter so the default test path is deterministic. */
  static uint32_t fake = 0;
  return fake;
#else
  /* On device, defer to Arduino's millis(). Arduino.h declares it
   * with C linkage; do NOT use a bare `extern uint32_t millis()`
   * here — that emits a mangled C++ symbol which fails to link
   * against the C-linkage runtime symbol. */
  return millis();
#endif
}

/* L1 magnitude over a band of the int8 feature vector. Byte-for-byte
 * identical to core_presence::reduce_magnitude (cpp lines 67-78): sum
 * of |v[lo..hi]| / n, capped at 127, returned as 0..127. The previous
 * revision of this helper used a sum-then-rescale-to-0..100 formula
 * which made the same MOTION_THRESHOLD (35) trigger at much weaker
 * signals than core_presence's — caught by the codex P1 / gemini HIGH
 * review on PR 456. The two modules MUST agree on "is there motion
 * at all?" for a given input, because the fusion-confirmed event is
 * meaningless if presence wouldn't even have flagged it observed. */
inline uint8_t reduce_magnitude(const int8_t* v, int lo, int hi) {
  int32_t sum = 0;
  for (int i = lo; i < hi; ++i) {
    sum += abs((int32_t)v[i]);
  }
  const int n = hi - lo;
  if (n <= 0) return 0;
  int32_t avg = sum / n;
  if (avg > 127) avg = 127;
  return (uint8_t)avg;
}

/* Find an existing link by fingerprint, or return nullptr. */
Link* find_link(const uint8_t fp[CORE_MULTILINK_FINGERPRINT_LEN]) {
  for (size_t i = 0; i < CORE_MULTILINK_MAX_PEERS; ++i) {
    if (s_links[i].in_use &&
        memcmp(s_links[i].fingerprint, fp, CORE_MULTILINK_FINGERPRINT_LEN) == 0) {
      return &s_links[i];
    }
  }
  return nullptr;
}

/* Pick a slot for a new link: prefer an empty slot, otherwise evict
 * the oldest. Returns a pointer into s_links. The "oldest" comparison
 * uses signed-delta arithmetic so it works correctly across the
 * ~49-day uint32_t millis() rollover — direct `<` would mis-order
 * timestamps that straddle the wrap. */
Link* allocate_link() {
  Link* victim = &s_links[0];
  for (size_t i = 0; i < CORE_MULTILINK_MAX_PEERS; ++i) {
    if (!s_links[i].in_use) return &s_links[i];
    if ((int32_t)(s_links[i].last_seen_ms - victim->last_seen_ms) < 0) {
      victim = &s_links[i];
    }
  }
  return victim;
}

/* Count how many in-use links are not stale (within
 * CORE_MULTILINK_PEER_STALE_MS) AND have motion >= threshold. Also
 * GCs stale links to keep the table tidy. */
size_t count_fresh_motion_links(uint32_t now) {
  size_t count = 0;
  for (size_t i = 0; i < CORE_MULTILINK_MAX_PEERS; ++i) {
    if (!s_links[i].in_use) continue;
    if ((now - s_links[i].last_seen_ms) >= CORE_MULTILINK_PEER_STALE_MS) {
      /* Don't fully evict — the slot may be reused by a peer whose
       * features show up later. Just don't count it. The slot is
       * eligible for eviction in allocate_link() under pressure. */
      continue;
    }
    if (s_links[i].motion_score >= MOTION_THRESHOLD) ++count;
  }
  return count;
}

/* Manifest. Single event type, P0. */
static const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */          "motion_confirmed",
    /* allowed_fields */     CSI_FIELD_STATE_NAME
                           | CSI_FIELD_CONFIDENCE
                           | CSI_FIELD_TIME_BUCKET
                           | CSI_FIELD_MOTION_SCORE
                           | CSI_FIELD_BUNDLED_COUNT
                           | CSI_FIELD_DISMISSED,
    /* privacy */            CSI_PRIVACY_P0,
    /* default_ceiling */    60,   /* at most 60 confirmed events per hour */
  },
};

/* Module lifecycle. init() may be NULL but we use it to reset state
 * so a re-register (e.g. during a Tuning Lab live reload) starts clean. */

void on_init(const csi_module_settings_t* /*settings*/) {
  memset(s_links, 0, sizeof(s_links));
  s_confirmed_emitted = false;
}

void on_tick(const csi_features_t* f) {
  if (f == nullptr) return;

  const uint32_t now = now_ms();
  const uint8_t local_motion =
      reduce_magnitude(f->v, IDX_DOPPLER_BASE, IDX_DOPPLER_END);

  const bool local_motion_present = local_motion >= MOTION_THRESHOLD;
  const size_t fresh_peer_motion = local_motion_present
      ? count_fresh_motion_links(now)
      : 0;

  /* Two-link confirmation: local + at least one fresh peer = 2 links. */
  const bool confirmed_now = local_motion_present && (fresh_peer_motion >= 1);

  if (confirmed_now && !s_confirmed_emitted) {
    csi_event_values_t v;
    csi_event_values_init(&v);
    v.category       = CSI_CATEGORY_EVENT;
    v.present_fields = CSI_FIELD_STATE_NAME
                     | CSI_FIELD_CONFIDENCE
                     | CSI_FIELD_TIME_BUCKET
                     | CSI_FIELD_MOTION_SCORE;
    strncpy(v.state_name, "motion",   sizeof(v.state_name)   - 1);
    strncpy(v.confidence, "confirmed", sizeof(v.confidence) - 1);
    v.motion_score = local_motion;
    v.time_bucket  = f->time_bucket;

    (void)csi_event_emit("core.multilink_fusion", "motion_confirmed", &v);
    s_confirmed_emitted = true;
  } else if (!confirmed_now && s_confirmed_emitted) {
    /* Fall edge: re-arm for the next rising edge. The bundler handles
     * "show the duration of the previous stretch" on the dashboard side. */
    s_confirmed_emitted = false;
  }
}

void on_dismissed(uint32_t /*event_id*/) {
  /* No per-event state to clear today. The bundler tracks dismissals
   * separately for ribbon-row rendering. */
}

void on_deinit() {
  memset(s_links, 0, sizeof(s_links));
  s_confirmed_emitted = false;
}

static const csi_module_t MODULE = {
  /* id */              "core.multilink_fusion",
  /* default_privacy */ CSI_PRIVACY_P0,
  /* events */          EVENTS,
  /* event_count */     sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */            on_init,
  /* tick */            on_tick,
  /* on_dismissed */    on_dismissed,
  /* deinit */          on_deinit,
};

}  /* namespace */

/* ────────────────────────────────────────────────────────────────────────
 * PUBLIC API
 * ──────────────────────────────────────────────────────────────────────── */

extern "C" {

const csi_module_t* core_multilink_fusion_module(void) {
  return &MODULE;
}

bool core_multilink_fusion_expire_link(
    const uint8_t fingerprint[CORE_MULTILINK_FINGERPRINT_LEN]) {
  if (fingerprint == nullptr) return false;
  Link* link = find_link(fingerprint);
  if (link == nullptr) return false;
  link->in_use = false;
  return true;
}

void core_multilink_fusion_ingest_peer_features(
    const uint8_t fingerprint[CORE_MULTILINK_FINGERPRINT_LEN],
    const csi_features_t* features) {
  if (fingerprint == nullptr || features == nullptr) return;

  Link* link = find_link(fingerprint);
  if (link == nullptr) {
    link = allocate_link();
    memcpy(link->fingerprint, fingerprint, CORE_MULTILINK_FINGERPRINT_LEN);
    link->in_use = true;
  }
  link->last_seen_ms = now_ms();
  link->motion_score = reduce_magnitude(features->v,
                                        IDX_DOPPLER_BASE, IDX_DOPPLER_END);
}

#ifdef CSI_TEST_HOST_BUILD
void core_multilink_fusion_test_reset(void) {
  memset(s_links, 0, sizeof(s_links));
  s_confirmed_emitted = false;
  s_test_now_set = false;
  s_test_now_ms = 0;
}

void core_multilink_fusion_test_set_now_ms(uint32_t now_ms_value) {
  s_test_now_ms  = now_ms_value;
  s_test_now_set = true;
}

size_t core_multilink_fusion_test_link_count(void) {
  size_t n = 0;
  for (size_t i = 0; i < CORE_MULTILINK_MAX_PEERS; ++i) {
    if (s_links[i].in_use) ++n;
  }
  return n;
}
#endif

}  /* extern "C" */
