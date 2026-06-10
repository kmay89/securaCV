/*
 * SecuraCV Canary — Acoustic Event Detection (PDM mic + cadence FSM)
 *
 * Pipeline:
 *   ┌──────────┐   ┌─────────┐   ┌──────────┐   ┌──────────┐
 *   │ PDM I2S  │──▶│  RMS    │──▶│  on/off  │──▶│  cadence │──▶ event_t
 *   │ 16 kHz   │   │  20 ms  │   │ hyst.    │   │  matcher │
 *   └──────────┘   └─────────┘   └──────────┘   └──────────┘
 *      raw int16     int32 mag    edges + ms     T3/T4 FSM
 *      WIPED on      no spectral  no envelope    no audio
 *      consume       content      shape          fingerprint
 *
 * Privacy is enforced structurally at every stage — see header doc.
 *
 * VENDORED COPY — adapted from firmware/canary/lib/securacv_audio/.
 * This copy deliberately diverges from the canonical lib in two ways:
 *   1. log_health() enum vocabulary uses this sketch's SCV_LOG_* /
 *      SCV_CAT_* constants (log_level.h here) instead of the core's
 *      LOG_LEVEL_* / LOG_CAT_* names.
 *   2. The whole implementation is gated on FEATURE_ACOUSTIC_EVENTS
 *      from build_config.h so MINIMAL profiles compile it out.
 * Drift beyond these adaptations is caught by the normalizing CI guard
 * firmware/scripts/check_audio_sync.sh — fix BOTH copies when it fires.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "build_config.h"

#if FEATURE_ACOUSTIC_EVENTS

#include "securacv_audio.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "log_level.h"
#include "health_log.h"   /* log_health() */

/* NVS namespace + key for the user's persisted mute intent. Read at
 * boot in main.cpp's audio_init block; written here from every control
 * path so the namespace/key live in one place. */
static const char* NVS_NAMESPACE = "securacv";
static const char* NVS_KEY_MIC_MUTED = "mic_muted";

/* The active canary tree's canary_config.h pre-dates Phase 2; it doesn't
 * yet expose the PDM mic pins. Provide local fallbacks (matching the
 * Seeed XIAO ESP32-S3 Sense schematic and firmware/boards/.../pins.h)
 * so this lib compiles standalone, but allow a build flag to override. */
#ifndef MIC_PIN_CLK
  #define MIC_PIN_CLK   42   /* PDM clock — XIAO ESP32-S3 Sense built-in */
#endif
#ifndef MIC_PIN_DATA
  #define MIC_PIN_DATA  41   /* PDM data — XIAO ESP32-S3 Sense built-in */
#endif

extern "C" {
  #include <esp_err.h>
  #include <driver/i2s.h>   /* legacy I2S API — works on ESP-IDF 4.4 (arduino-esp32 2.x) and 5.x */
}

namespace audio {

/* ──────────────────────────────────────────────────────────────────────────
 * SECURITY PRIMITIVES
 * ────────────────────────────────────────────────────────────────────────── */

static void secure_wipe(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) { *p++ = 0; }
  asm volatile("" ::: "memory");
}

/* Bit-by-bit integer sqrt — same form as securacv_csi. */
static uint32_t isqrt_u32(uint32_t n) {
  uint32_t root = 0;
  uint32_t bit = (uint32_t)1 << 30;
  while (bit > n) bit >>= 2;
  while (bit) {
    const uint32_t trial = root + bit;
    if (n >= trial) { n -= trial; root = (root >> 1) + bit; }
    else            { root >>= 1; }
    bit >>= 2;
  }
  return root;
}

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool s_initialized = false;
static bool s_running = false;
static audio_config_t s_cfg = AUDIO_CONFIG_DEFAULT;
static audio_event_cb_t s_cb = nullptr;
static bool s_i2s_installed = false;

/* Envelope hysteresis state machine. */
static bool     s_envelope_high = false;
static uint32_t s_state_entered_ms = 0;
/* Set to true once a cadence cycle has been declared during the current OFF
 * period; cleared on every new on/off transition. Without this flag, the
 * matcher would re-fire continuously while the inter-cycle pause is held
 * (the matcher is purely temporal and the same 6 transitions remain in
 * the ring). Resetting s_state_entered_ms instead would corrupt the
 * NEXT transition's prev_dur_ms — see review thread #351. */
static bool     s_cycle_matched = false;

#if FEATURE_ACOUSTIC_TRANSIENTS
/* Per-state-window accumulators for the band-ratio summary that we
 * record into the next push_transition(). Reset on every transition.
 * Frame_count gates "if we transition before even one frame elapsed
 * we report 100 (broadband) rather than dividing by zero." */
static uint32_t s_state_rms_sum     = 0;
static uint32_t s_state_hpf_rms_sum = 0;
static uint32_t s_state_frames      = 0;
/* Latest knock / doorbell / glass-break match guard — same role as
 * s_cycle_matched, separate flags so a T3 match doesn't block a knock
 * match on the same set of transitions. */
static bool     s_knock_matched      = false;
static bool     s_doorbell_matched   = false;
static bool     s_glass_matched      = false;
/* Per-event-type counters surfaced through audio_stats_t. */
static uint32_t s_knock_count        = 0;
static uint32_t s_doorbell_count     = 0;
static uint32_t s_glass_count        = 0;
#endif

/* Ring of recent on/off transitions (newest at head; wraps). Each entry
 * records the time we entered a state and the duration we stayed in the
 * PREVIOUS state. Capacity 16 covers two full T3 cycles (12 transitions)
 * plus headroom for noise. */
struct Transition {
  bool     is_on;       /* true = entered ON state, false = entered OFF */
  uint8_t  band_ratio_x100; /* (hpf_rms/full_rms) * 100 of the PREV state,
                             * clamped 0..200. 0 means "low-band dominant"
                             * (knock), ~100 means "broadband" (voice/music),
                             * >130 means "high-band dominant" (glass shatter).
                             * Only meaningful for ON→OFF transitions; on
                             * OFF→ON transitions reflects the silent-room
                             * noise floor and is ignored by detectors. */
  uint32_t at_ms;       /* millis() at the transition */
  uint32_t prev_dur_ms; /* duration of the PREVIOUS state */
};
static constexpr size_t TRANS_CAP = 16;
static Transition s_trans[TRANS_CAP];
static size_t s_trans_head = 0;     /* index of next slot to write */
static size_t s_trans_count = 0;    /* how many slots have valid data */

/* Last event emit timestamp (for the 250 ms storm cap). */
static uint32_t s_last_event_ms = 0;

/* Cadence-cycle counter per type, reset by silence > 10 s. */
static uint16_t s_t3_cycles = 0;
static uint16_t s_t4_cycles = 0;

/* Stats — updated from main loop only. */
static audio_stats_t s_stats;

/* Most recent 20 ms RMS scalar (and when we computed it). Exposed for the
 * live-level meter; this is the same number the on/off hysteresis uses,
 * not a second audio path. Wiped on mute / stop. Accessed cross-task
 * (main loop writes, HTTP task reads) — use __atomic_* on accessors. */
static uint16_t s_last_rms = 0;
static uint32_t s_last_rms_ms = 0;

/* User-requested mute state. Independent of s_running: when muted, we
 * never call i2s_open() in start(); when unmuted at boot, we run normally.
 * Cross-task: HTTP writes via mute(), main loop reads. */
static bool s_muted = false;

/* Cross-task action plumbing. HTTP handlers must NOT touch the I2S
 * driver directly — that would race audio_process() in the main loop
 * and crash at i2s_driver_uninstall(). Instead the handler atomically
 * sets *_pending and the main loop applies it from audio_process(). */
static bool     s_mute_request_pending = false;
static bool     s_mute_request_value   = false;
static uint8_t  s_mute_request_source  = AUDIO_MUTE_SOURCE_BOOT;
static bool     s_selftest_start_pending = false;
static uint32_t s_selftest_start_duration = 0;
static bool     s_selftest_stop_pending = false;

/* Application callback fired when a deferred mute is applied. Set from
 * main.cpp; routes the event into the sensing aggregator + witness chain. */
static audio_mute_cb_t s_mute_cb = nullptr;

/* Source + timestamp of the most recently APPLIED mute toggle. Surfaced
 * through audio_get_mute_info() so the dashboard can show "Muted by
 * Home Assistant" or "Muted by you" alongside the live mic state. */
static uint8_t  s_last_applied_mute_source = AUDIO_MUTE_SOURCE_BOOT;
static uint32_t s_last_applied_mute_ms     = 0;
static bool     s_last_applied_set         = false;

/* Self-test mode (relaxed thresholds, normal event callback suppressed).
 * Cross-task: main loop writes, HTTP task reads via selftest_status(). */
static bool     s_selftest_active = false;
static uint32_t s_selftest_deadline_ms = 0;
static uint8_t  s_selftest_matched_type = AUDIO_EVENT_NONE;
static uint8_t  s_selftest_matched_conf = 0;
static uint32_t s_selftest_transitions_seen = 0;

/* ──────────────────────────────────────────────────────────────────────────
 * RMS COMPUTATION  — int64 sum-of-squares; samples wiped after each call
 * ────────────────────────────────────────────────────────────────────────── */

/* PRIVACY BARRIER: the caller wipes the FULL sample buffer (sizeof(samples),
 * not n * 2) right after this returns. We don't wipe inside compute_rms
 * because n may be smaller than the buffer when i2s_read returns short —
 * a partial wipe would leave residual audio in the tail. */
static uint16_t compute_rms(const int16_t* samples, size_t n) {
  if (n == 0) return 0;
  int64_t sumsq = 0;
  for (size_t i = 0; i < n; i++) {
    const int32_t s = samples[i];
    sumsq += (int64_t)s * s;
  }
  const uint32_t mean_sq = (uint32_t)(sumsq / (int64_t)n);
  uint32_t rms = isqrt_u32(mean_sq);
  if (rms > 0xFFFFu) rms = 0xFFFFu;
  return (uint16_t)rms;
}

#if FEATURE_ACOUSTIC_TRANSIENTS
/* Cross-frame state for the HPF: we need the LAST sample from the
 * previous frame so the (x[n] - x[n-1]) difference at the frame boundary
 * is correct. Wiped on stop()/mute(). Two bytes. */
static int16_t s_hpf_prev_last = 0;

/* HPF RMS — first-order |1 - z^-1| filter (corner ≈ fs/4 ≈ 4 kHz at the
 * 16 kHz PDM rate). Mathematically: compute RMS of d[n] = x[n] - x[n-1]
 * across the frame. Cheap (one subtraction + one multiply per sample)
 * and good enough to tell "concentrated above 4 kHz" (glass shatter,
 * hi-hat) from "concentrated below 4 kHz" (knock, voice, slam).
 *
 * Same wipe contract as compute_rms — caller scrubs the buffer. */
static uint16_t compute_hpf_rms(const int16_t* samples, size_t n) {
  if (n == 0) return 0;
  int64_t sumsq = 0;
  int32_t prev = s_hpf_prev_last;
  for (size_t i = 0; i < n; i++) {
    const int32_t s = samples[i];
    const int32_t d = s - prev;
    sumsq += (int64_t)d * d;
    prev = s;
  }
  s_hpf_prev_last = (int16_t)prev;
  /* Divide by n (not n-1) — every sample produced a difference because
   * we used the cross-frame s_hpf_prev_last for the first one. */
  const uint32_t mean_sq = (uint32_t)(sumsq / (int64_t)n);
  uint32_t hpf = isqrt_u32(mean_sq);
  if (hpf > 0xFFFFu) hpf = 0xFFFFu;
  return (uint16_t)hpf;
}
#endif  /* FEATURE_ACOUSTIC_TRANSIENTS */

/* ──────────────────────────────────────────────────────────────────────────
 * TRANSITION RING
 * ────────────────────────────────────────────────────────────────────────── */

static void push_transition(bool entered_on, uint32_t now_ms,
                            uint32_t prev_dur_ms,
                            uint8_t prev_band_ratio_x100) {
  s_trans[s_trans_head].is_on             = entered_on;
  s_trans[s_trans_head].at_ms             = now_ms;
  s_trans[s_trans_head].prev_dur_ms       = prev_dur_ms;
  s_trans[s_trans_head].band_ratio_x100   = prev_band_ratio_x100;
  s_trans_head = (s_trans_head + 1) % TRANS_CAP;
  if (s_trans_count < TRANS_CAP) s_trans_count++;
}

/* Fetch the i-th most recent transition (i=0 = newest). Returns nullptr
 * if not enough transitions are buffered yet. */
static const Transition* recent(size_t i_back) {
  if (i_back >= s_trans_count) return nullptr;
  /* head points to next-write slot; (head - 1) is newest. */
  size_t idx = (s_trans_head + TRANS_CAP - 1 - i_back) % TRANS_CAP;
  return &s_trans[idx];
}

/* ──────────────────────────────────────────────────────────────────────────
 * CADENCE MATCHERS
 *
 * We score the timing fit against the published spec, then convert the
 * sum of timing errors into a 0..100 confidence. A perfect match scores
 * 100; the worst tolerated match scores ~50.
 * ────────────────────────────────────────────────────────────────────────── */

/* T3 — NFPA 72 / ISO 8201 smoke-alarm cadence.
 * Spec (NFPA 72 Annex Q-1): 0.5 s tone, 0.5 s off, 0.5 s tone, 0.5 s off,
 * 0.5 s tone, 1.5 s off. 4.0 s cycle. We tolerate ±200 ms on each beep
 * and ±500 ms on the inter-cycle pause to handle echoey rooms.
 *
 * To match a complete T3 cycle we need the last 6 transitions, in order
 * (newest first):
 *   recent(0): OFF entered after beep 3 — its prev_dur = beep 3 ON time
 *   recent(1): ON  entered for beep 3 — its prev_dur = inter-beep OFF
 *   recent(2): OFF entered after beep 2 — prev_dur = beep 2 ON
 *   recent(3): ON  entered for beep 2 — prev_dur = inter-beep OFF
 *   recent(4): OFF entered after beep 1 — prev_dur = beep 1 ON
 *   recent(5): ON  entered for beep 1 — prev_dur = inter-cycle pause (or boot)
 *
 * The inter-cycle pause is read from `now - recent(0).at_ms` once we've
 * been in OFF for at least 1.0 s, NOT from a transition's prev_dur — the
 * cycle isn't "complete" until that long pause has actually elapsed. */
static int score_t3_cycle(uint32_t now_ms, bool relaxed) {
  const Transition* t0 = recent(0);
  const Transition* t1 = recent(1);
  const Transition* t2 = recent(2);
  const Transition* t3 = recent(3);
  const Transition* t4 = recent(4);
  const Transition* t5 = recent(5);
  if (!t0 || !t1 || !t2 || !t3 || !t4 || !t5) return -1;

  /* Pattern check: alternation ON/OFF/ON/OFF/ON/OFF (newest first). */
  if ( t0->is_on || !t1->is_on ||
        t2->is_on || !t3->is_on ||
        t4->is_on || !t5->is_on) return -1;

  /* Beep durations (the ON state durations). */
  const int32_t b3 = (int32_t)t0->prev_dur_ms;  /* beep 3 ON */
  const int32_t g23 = (int32_t)t1->prev_dur_ms; /* gap 2→3 OFF */
  const int32_t b2 = (int32_t)t2->prev_dur_ms;  /* beep 2 ON */
  const int32_t g12 = (int32_t)t3->prev_dur_ms; /* gap 1→2 OFF */
  const int32_t b1 = (int32_t)t4->prev_dur_ms;  /* beep 1 ON */
  /* The inter-cycle pause: how long we've been in OFF since the last
   * transition (t0). We require ≥ 1.0 s before declaring a cycle. */
  const int32_t pause = (int32_t)(now_ms - t0->at_ms);
  if (pause < 1000) return -1;

  /* In relaxed (self-test) mode we double the beep/gap tolerance and
   * roughly double the pause tolerance. The 0.5 s targets stay the same
   * — we only widen the acceptance window so a user pressing their
   * alarm's TEST button at ~3 m through soft furnishings has the best
   * chance of matching. */
  const int32_t beep_tol = relaxed ? 400 : 200;
  const int32_t pause_tol = relaxed ? 1000 : 500;

  auto err_against = [](int32_t v, int32_t target, int32_t tol) -> int32_t {
    int32_t d = v - target;
    if (d < 0) d = -d;
    if (d > tol) return -1;
    return d;
  };

  const int32_t e_b1 = err_against(b1, 500, beep_tol);
  const int32_t e_b2 = err_against(b2, 500, beep_tol);
  const int32_t e_b3 = err_against(b3, 500, beep_tol);
  const int32_t e_g12 = err_against(g12, 500, beep_tol);
  const int32_t e_g23 = err_against(g23, 500, beep_tol);
  const int32_t e_pause = err_against(pause, 1500, pause_tol);
  if (e_b1 < 0 || e_b2 < 0 || e_b3 < 0 ||
      e_g12 < 0 || e_g23 < 0 || e_pause < 0) return -1;

  /* Sum of relative errors → confidence. We map total error → confidence
   * so a perfect match still scores ~100 and the worst tolerated relaxed
   * match still scores ~30; the normal floor is 50. */
  const int32_t total_err = e_b1 + e_b2 + e_b3 + e_g12 + e_g23 + e_pause;
  const int32_t worst = beep_tol * 5 + pause_tol;     /* sum of tolerances */
  const int32_t floor_conf = relaxed ? 30 : 50;
  int32_t conf = 100 - (total_err * (100 - floor_conf) / worst);
  if (conf < floor_conf) conf = floor_conf;
  if (conf > 100)        conf = 100;
  return (int)conf;
}

/* T4 — UL 2034 CO cadence.
 * Spec: 100 ms tone, 100 ms off, ×4, then 5 s silence. We tolerate ±50 ms
 * on each short beep (50% relative — the short durations are noisy in
 * echoey rooms) and ±1 s on the 5 s pause.
 *
 * Need 8 transitions (4 ON, 4 OFF), pattern (newest first):
 *   recent(0): OFF after beep 4 — prev_dur = beep 4 ON
 *   recent(1): ON  for beep 4 — prev_dur = gap 3→4 OFF
 *   ... etc, 6 more
 * Inter-cycle pause is again `now - recent(0).at_ms`. */
static int score_t4_cycle(uint32_t now_ms, bool relaxed) {
  const Transition* t[8];
  for (int i = 0; i < 8; i++) t[i] = recent(i);
  for (int i = 0; i < 8; i++) if (!t[i]) return -1;

  /* Alternation check. */
  for (int i = 0; i < 8; i++) {
    const bool expect_on = (i & 1) == 1;  /* i=1,3,5,7 are ON */
    if (t[i]->is_on != expect_on) return -1;
  }

  const int32_t b4 = (int32_t)t[0]->prev_dur_ms;
  const int32_t g34 = (int32_t)t[1]->prev_dur_ms;
  const int32_t b3 = (int32_t)t[2]->prev_dur_ms;
  const int32_t g23 = (int32_t)t[3]->prev_dur_ms;
  const int32_t b2 = (int32_t)t[4]->prev_dur_ms;
  const int32_t g12 = (int32_t)t[5]->prev_dur_ms;
  const int32_t b1 = (int32_t)t[6]->prev_dur_ms;

  const int32_t pause = (int32_t)(now_ms - t[0]->at_ms);
  if (pause < 3500) return -1;  /* require most of the 5 s gap before declaring */

  const int32_t beep_tol = relaxed ? 100 : 60;
  const int32_t pause_tol = relaxed ? 2000 : 1500;

  auto err = [](int32_t v, int32_t target, int32_t tol) -> int32_t {
    int32_t d = v - target;
    if (d < 0) d = -d;
    if (d > tol) return -1;
    return d;
  };

  const int32_t e_b1 = err(b1, 100, beep_tol);
  const int32_t e_b2 = err(b2, 100, beep_tol);
  const int32_t e_b3 = err(b3, 100, beep_tol);
  const int32_t e_b4 = err(b4, 100, beep_tol);
  const int32_t e_g12 = err(g12, 100, beep_tol);
  const int32_t e_g23 = err(g23, 100, beep_tol);
  const int32_t e_g34 = err(g34, 100, beep_tol);
  const int32_t e_pause = err(pause, 5000, pause_tol);
  if (e_b1 < 0 || e_b2 < 0 || e_b3 < 0 || e_b4 < 0 ||
      e_g12 < 0 || e_g23 < 0 || e_g34 < 0 || e_pause < 0) return -1;

  const int32_t total_err = e_b1+e_b2+e_b3+e_b4+e_g12+e_g23+e_g34+e_pause;
  const int32_t worst = beep_tol * 7 + pause_tol;
  const int32_t floor_conf = relaxed ? 30 : 50;
  int32_t conf = 100 - (total_err * (100 - floor_conf) / worst);
  if (conf < floor_conf) conf = floor_conf;
  if (conf > 100)        conf = 100;
  return (int)conf;
}

#if FEATURE_ACOUSTIC_TRANSIENTS
/* ──────────────────────────────────────────────────────────────────────────
 * PHASE 2b — KNOCK / DOORBELL / GLASS-BREAK DETECTORS
 *
 * All three run on the same OFF-with-≥1s-silence gate as T3/T4. The
 * detectors are heuristic by design (envelope + per-state band-ratio
 * only); see audio.h's privacy / scope contract.
 *
 * Knock          — exactly 3 short ON impulses (30..180 ms) with
 *                  near-even OFF gaps (60..400 ms) and low band-ratio
 *                  (< 130, low-band dominant — typical of knuckle/fist).
 *                  3 ON + 2 OFF = 5 transitions back, plus the trailing
 *                  silence we're standing in.
 *
 * Doorbell       — exactly 2 mid-length ON tones (250..900 ms) with a
 *                  short OFF gap (50..400 ms), both with mid band-ratio
 *                  (60..130 — neither knock-low nor shatter-high).
 *                  2 ON + 1 OFF = 3 transitions back.
 *
 * Glass break    — exactly 1 sustained ON state (800..3000 ms) with
 *                  HIGH band-ratio (≥ 130 — the broadband shatter
 *                  signature shifts energy above ~4 kHz). 1 ON = 2
 *                  transitions back (the ON-arrive and the OFF-arrive
 *                  we're now standing past).
 * ────────────────────────────────────────────────────────────────────────── */

static int score_knock_cycle(uint32_t now_ms) {
  /* Pattern (newest first):
   *   t0: OFF  — prev_dur = beep3 ON duration; ratio = beep3 band-ratio
   *   t1: ON   — prev_dur = gap2→3 OFF duration
   *   t2: OFF  — prev_dur = beep2 ON duration; ratio = beep2 band-ratio
   *   t3: ON   — prev_dur = gap1→2 OFF duration
   *   t4: OFF  — prev_dur = beep1 ON duration; ratio = beep1 band-ratio
   *
   * After t0 we are in OFF; we require ≥ 500 ms of trailing silence
   * before declaring a knock (otherwise a 4th impulse might still be
   * coming). Knocks shorter than 500 ms of trailing silence are also
   * VERY easily confused with the first 3 beeps of a T3 alarm. */
  const Transition* t0 = recent(0);
  const Transition* t1 = recent(1);
  const Transition* t2 = recent(2);
  const Transition* t3 = recent(3);
  const Transition* t4 = recent(4);
  if (!t0 || !t1 || !t2 || !t3 || !t4) return -1;
  if (t0->is_on || !t1->is_on || t2->is_on || !t3->is_on || t4->is_on) return -1;

  const int32_t b1 = (int32_t)t4->prev_dur_ms;
  const int32_t g12 = (int32_t)t3->prev_dur_ms;
  const int32_t b2 = (int32_t)t2->prev_dur_ms;
  const int32_t g23 = (int32_t)t1->prev_dur_ms;
  const int32_t b3 = (int32_t)t0->prev_dur_ms;
  const int32_t trailing = (int32_t)(now_ms - t0->at_ms);
  if (trailing < 500) return -1;

  /* Beep duration window: 30..180 ms. Knuckle impulses are sharp; a
   * 200+ ms "knock" is a slam or a TV thud, not a knock. */
  if (b1 < 30 || b1 > 180) return -1;
  if (b2 < 30 || b2 > 180) return -1;
  if (b3 < 30 || b3 > 180) return -1;
  /* Gap window: 60..400 ms. A human knocks 3–5 times per second. */
  if (g12 < 60 || g12 > 400) return -1;
  if (g23 < 60 || g23 > 400) return -1;
  /* Don't fire on patterns that drag on too long — a slow drumbeat
   * shouldn't read as a knock. */
  const int32_t span = b1 + g12 + b2 + g23 + b3;
  if (span > 1800) return -1;

  /* Low-band dominant during the impulses (band_ratio < 130 means HPF
   * energy is less than 1.3× full-band; impacts have most energy <2 kHz). */
  if (t4->band_ratio_x100 >= 130) return -1;
  if (t2->band_ratio_x100 >= 130) return -1;
  if (t0->band_ratio_x100 >= 130) return -1;

  /* Confidence: 100 minus the relative variance of beep durations and
   * gap durations — even-tempo knocks score best. */
  const int32_t b_mean = (b1 + b2 + b3) / 3;
  const int32_t b_var  = (abs(b1 - b_mean) + abs(b2 - b_mean) + abs(b3 - b_mean)) / 3;
  const int32_t g_var  = abs(g12 - g23);
  /* Variance ≤ 30 ms ⇒ ~100, variance ≥ 100 ms ⇒ ~50. */
  int32_t conf = 110 - (b_var + g_var);
  if (conf < 50) conf = 50;
  if (conf > 100) conf = 100;
  return (int)conf;
}

static int score_doorbell_cycle(uint32_t now_ms) {
  /* Pattern (newest first):
   *   t0: OFF — prev_dur = tone2 ON; ratio = tone2 band-ratio
   *   t1: ON  — prev_dur = gap OFF duration
   *   t2: OFF — prev_dur = tone1 ON; ratio = tone1 band-ratio
   * Required: at least 600 ms of trailing silence (doorbells finish then quiet). */
  const Transition* t0 = recent(0);
  const Transition* t1 = recent(1);
  const Transition* t2 = recent(2);
  if (!t0 || !t1 || !t2) return -1;
  if (t0->is_on || !t1->is_on || t2->is_on) return -1;

  const int32_t tone1 = (int32_t)t2->prev_dur_ms;
  const int32_t gap   = (int32_t)t1->prev_dur_ms;
  const int32_t tone2 = (int32_t)t0->prev_dur_ms;
  const int32_t trailing = (int32_t)(now_ms - t0->at_ms);
  if (trailing < 600) return -1;

  /* Tones: 250..900 ms. Shorter is a knock; longer is voice/music. */
  if (tone1 < 250 || tone1 > 900) return -1;
  if (tone2 < 250 || tone2 > 900) return -1;
  /* Gap: 50..400 ms. Quicker is staccato; slower is two separate words. */
  if (gap < 50 || gap > 400) return -1;

  /* Mid band-ratio: 60..130 (neither knock-low nor shatter-high). */
  if (t2->band_ratio_x100 < 60 || t2->band_ratio_x100 > 130) return -1;
  if (t0->band_ratio_x100 < 60 || t0->band_ratio_x100 > 130) return -1;

  /* Anti-T3 guard: a stray pair of beeps from a smoke alarm 500 ms apart
   * would otherwise look very doorbell-like at relaxed thresholds. Bail
   * if both tones are near 500 ms AND the gap is near 500 ms. */
  if (tone1 > 400 && tone1 < 650 && tone2 > 400 && tone2 < 650 &&
      gap   > 350 && gap   < 650) return -1;

  const int32_t t_var = abs(tone1 - tone2);
  /* Two tones within 100 ms of each other ⇒ high confidence; 400 ms ⇒ low. */
  int32_t conf = 110 - (t_var / 3);
  if (conf < 50) conf = 50;
  if (conf > 100) conf = 100;
  return (int)conf;
}

static int score_glass_break_cycle(uint32_t now_ms) {
  /* Pattern (newest first):
   *   t0: OFF — prev_dur = single sustained ON; ratio = ON band-ratio
   *   t1: ON  — prev_dur = previous OFF duration (don't care, must be quiet)
   * Required: at least 500 ms of trailing silence so we don't fire mid-noise. */
  const Transition* t0 = recent(0);
  const Transition* t1 = recent(1);
  if (!t0 || !t1) return -1;
  if (t0->is_on || !t1->is_on) return -1;

  const int32_t on_dur   = (int32_t)t0->prev_dur_ms;
  const int32_t trailing = (int32_t)(now_ms - t0->at_ms);
  if (trailing < 500) return -1;

  /* Sustained 800..3000 ms — characteristic of a full shatter (initial
   * impact + 1–2 s of falling-glass decay). Below 800 ms is too short
   * for a real shatter; above 3 s is some other sustained noise. */
  if (on_dur < 800 || on_dur > 3000) return -1;

  /* High band-ratio: ≥ 130 means HPF energy is at least 1.3× full-band,
   * which happens when most of the energy sits above ~4 kHz. Glass
   * shatter's broadband tail centers around 1–5 kHz with significant
   * energy above 4 kHz; voice, slams, knocks all stay below. */
  if (t0->band_ratio_x100 < 130) return -1;

  /* Confidence: higher ratio + longer duration ⇒ higher confidence.
   * Map the ratio (130..200) and duration (800..3000) into 60..100. */
  const int32_t ratio_score = ((int32_t)t0->band_ratio_x100 - 130) * 100 / 70;
  const int32_t dur_score   = (on_dur > 2000) ? 100 : (on_dur - 800) * 100 / 1200;
  int32_t conf = 60 + ((ratio_score + dur_score) * 40) / 200;
  if (conf < 60)  conf = 60;
  if (conf > 100) conf = 100;
  return (int)conf;
}
#endif  /* FEATURE_ACOUSTIC_TRANSIENTS */

/* ──────────────────────────────────────────────────────────────────────────
 * EVENT EMITTER
 * ────────────────────────────────────────────────────────────────────────── */

static void try_emit_event(uint8_t type, uint8_t conf, uint16_t cycle_count,
                           uint32_t now_ms) {
  if ((now_ms - s_last_event_ms) < 250) return;  /* storm cap */
  s_last_event_ms = now_ms;

  audio_event_t evt;
  memset(&evt, 0, sizeof(evt));
  evt.event_type  = type;
  evt.confidence  = conf;
  evt.cycle_count = cycle_count;
  evt.time_bucket = (uint8_t)((now_ms / (10UL * 60UL * 1000UL)) % 144);

  if (s_cb) s_cb(&evt);

  /* Don't outlive the call. */
  secure_wipe(&evt, sizeof(evt));
}

/* ──────────────────────────────────────────────────────────────────────────
 * I2S BRING-UP / TEAR-DOWN
 * ────────────────────────────────────────────────────────────────────────── */

/* Bring up I2S0 in PDM RX mode using the legacy driver. The XIAO ESP32-S3
 * Sense's MSM261D mic uses two pins: WS=GPIO 42 (clock), DATA=GPIO 41.
 * BCK is unused in PDM mode. */
static bool i2s_open() {
  i2s_config_t cfg = {};
  cfg.mode               = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  cfg.sample_rate        = s_cfg.sample_rate_hz;
  cfg.bits_per_sample    = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format     = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags   = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count      = 4;
  cfg.dma_buf_len        = AUDIO_FRAME_SAMPLES;  /* one 20 ms frame per buffer */
  cfg.use_apll           = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk         = 0;

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "install err=0x%x", (unsigned)err);
    log_health(SCV_LOG_WARNING, SCV_CAT_SENSOR,
               "Audio: I2S driver install failed", d);
    return false;
  }
  s_i2s_installed = true;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_PIN_NO_CHANGE;
  pins.ws_io_num    = MIC_PIN_CLK;     /* PDM clock */
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = MIC_PIN_DATA;    /* PDM data */
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin(I2S_NUM_0, &pins);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "set_pin err=0x%x", (unsigned)err);
    log_health(SCV_LOG_WARNING, SCV_CAT_SENSOR,
               "Audio: I2S pin config failed", d);
    i2s_driver_uninstall(I2S_NUM_0);
    s_i2s_installed = false;
    return false;
  }

  /* Clear the DMA buffer so the first read after start doesn't observe
   * stale RAM. zero_dma_buffer is well-defined post install+set_pin. */
  i2s_zero_dma_buffer(I2S_NUM_0);
  return true;
}

static void i2s_close() {
  if (s_i2s_installed) {
    i2s_driver_uninstall(I2S_NUM_0);
    s_i2s_installed = false;
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const audio_config_t& cfg) {
  if (s_initialized) return true;

  s_cfg = cfg;
  if (s_cfg.sample_rate_hz == 0) s_cfg.sample_rate_hz = AUDIO_SAMPLE_RATE_HZ;
  if (s_cfg.frame_ms == 0)       s_cfg.frame_ms       = AUDIO_FRAME_MS;
  /* If thresholds left at zero (caller used calloc), restore defaults. */
  if (s_cfg.rms_on_threshold == 0)  s_cfg.rms_on_threshold  = 800;
  if (s_cfg.rms_off_threshold == 0) s_cfg.rms_off_threshold = 400;

  memset(&s_stats, 0, sizeof(s_stats));
  memset(s_trans, 0, sizeof(s_trans));
  s_trans_head = 0;
  s_trans_count = 0;
  s_envelope_high = false;
  s_state_entered_ms = 0;
  s_cycle_matched = false;
  s_last_event_ms = 0;
  s_t3_cycles = 0;
  s_t4_cycles = 0;
  #if FEATURE_ACOUSTIC_TRANSIENTS
  s_hpf_prev_last = 0;
  s_state_rms_sum = 0;
  s_state_hpf_rms_sum = 0;
  s_state_frames = 0;
  s_knock_matched = false;
  s_doorbell_matched = false;
  s_glass_matched = false;
  s_knock_count = 0;
  s_doorbell_count = 0;
  s_glass_count = 0;
  #endif

  s_last_rms = 0;
  s_last_rms_ms = 0;
  s_selftest_active = false;
  s_selftest_deadline_ms = 0;
  s_selftest_matched_type = AUDIO_EVENT_NONE;
  s_selftest_matched_conf = 0;
  s_selftest_transitions_seen = 0;

  s_initialized = true;
  log_health(SCV_LOG_INFO, SCV_CAT_SENSOR,
             "Audio HAL initialized",
             "PDM 16 kHz mono, T3/T4 cadence detector armed");
  return true;
}

void deinit() {
  if (!s_initialized) return;
  if (s_running) {
    i2s_close();
    s_running = false;
  }
  memset(s_trans, 0, sizeof(s_trans));
  memset(&s_stats, 0, sizeof(s_stats));
  s_cb = nullptr;
  s_initialized = false;
}

bool start() {
  if (!s_initialized) return false;
  if (s_running)      return true;

  /* Legacy I2S driver doesn't have a separate channel-enable step —
   * i2s_driver_install() in i2s_open() leaves the channel in RX mode
   * and DMA filling immediately. */
  if (!i2s_open()) return false;

  s_running = true;
  s_state_entered_ms = millis();
  s_cycle_matched = false;
  return true;
}

void stop() {
  if (!s_running) return;
  i2s_close();
  s_running = false;
  /* Scrub envelope state so a subsequent start begins clean. */
  memset(s_trans, 0, sizeof(s_trans));
  s_trans_head = 0;
  s_trans_count = 0;
  s_envelope_high = false;
  s_cycle_matched = false;
  s_t3_cycles = 0;
  s_t4_cycles = 0;
  #if FEATURE_ACOUSTIC_TRANSIENTS
  /* Wipe the HPF state so the next start() doesn't carry sub-sample
   * memory across a mute boundary — same privacy intent as the
   * sample-buffer wipe. */
  s_hpf_prev_last = 0;
  s_state_rms_sum = 0;
  s_state_hpf_rms_sum = 0;
  s_state_frames = 0;
  s_knock_matched = false;
  s_doorbell_matched = false;
  s_glass_matched = false;
  #endif
  /* Wipe the published level — when muted, /api/audio/level returns zero. */
  s_last_rms = 0;
  s_last_rms_ms = 0;
}

bool is_running() { return s_running; }

void set_event_callback(audio_event_cb_t cb) { s_cb = cb; }

/* ──────────────────────────────────────────────────────────────────────────
 * RUNTIME MUTE
 * ────────────────────────────────────────────────────────────────────────── */

/* mute() is safe to call from any task. It records the user's intent
 * atomically and defers the actual I2S start/stop to the next
 * audio_process() tick in the main loop — calling i2s_driver_uninstall
 * from an HTTP task while the main loop is mid-i2s_read would crash. */
bool mute(bool muted, uint8_t source) {
  if (!s_initialized) return false;
  __atomic_store_n(&s_muted, muted, __ATOMIC_RELEASE);
  __atomic_store_n(&s_mute_request_value, muted, __ATOMIC_RELAXED);
  __atomic_store_n(&s_mute_request_source, source, __ATOMIC_RELAXED);
  __atomic_store_n(&s_mute_request_pending, true, __ATOMIC_RELEASE);
  return true;
}

bool is_muted() { return __atomic_load_n(&s_muted, __ATOMIC_ACQUIRE); }

void set_mute_callback(audio_mute_cb_t cb) { s_mute_cb = cb; }

void get_mute_info(audio_mute_info_t* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  const bool ever = __atomic_load_n(&s_last_applied_set, __ATOMIC_ACQUIRE);
  if (!ever) { out->age_ms = UINT32_MAX; return; }
  out->source = __atomic_load_n(&s_last_applied_mute_source, __ATOMIC_RELAXED);
  const uint32_t ts = __atomic_load_n(&s_last_applied_mute_ms, __ATOMIC_ACQUIRE);
  out->age_ms = (ts == 0) ? UINT32_MAX : (millis() - ts);
}

/* Called at boot from the main task BEFORE the HTTP server starts, so
 * we can synchronously start/stop the I2S driver without racing anything.
 * Used by main.cpp's boot path to honor the persisted NVS mute state. */
bool mute_sync_at_boot(bool muted) {
  if (!s_initialized) return false;
  s_muted = muted;
  /* Clear any stale pending request so the first audio_process() tick
   * doesn't immediately toggle I2S back. */
  __atomic_store_n(&s_mute_request_pending, false, __ATOMIC_RELAXED);
  bool ok;
  if (muted) {
    if (s_running) {
      i2s_close();
      s_running = false;
      memset(s_trans, 0, sizeof(s_trans));
      s_trans_head = 0;
      s_trans_count = 0;
      s_envelope_high = false;
      s_cycle_matched = false;
      s_t3_cycles = 0;
      s_t4_cycles = 0;
      s_last_rms = 0;
      s_last_rms_ms = 0;
      #if FEATURE_ACOUSTIC_TRANSIENTS
      /* Wipe the HPF carryover so the next unmute's first frame isn't
       * compared against a stale pre-mute sample. */
      s_hpf_prev_last = 0;
      s_state_rms_sum = 0;
      s_state_hpf_rms_sum = 0;
      s_state_frames = 0;
      s_knock_matched = false;
      s_doorbell_matched = false;
      s_glass_matched = false;
      #endif
    }
    log_health(SCV_LOG_INFO, SCV_CAT_SENSOR,
               "Audio: mic muted at boot", "I2S not started");
    ok = true;
  } else {
    ok = s_running ? true : start();
  }
  /* The boot path runs BEFORE set_mute_callback() is wired by main.cpp,
   * so we won't normally fire the callback from here. main.cpp emits a
   * single "boot-state" witness record after wiring the callback. We
   * still record the boot-mute fact so the dashboard immediately shows
   * "Muted at boot" instead of "Muted by ???" until the first user
   * toggle. Only marks the state if the boot was actually muted —
   * otherwise the field stays "never set" and the UI shows nothing. */
  if (muted && ok) {
    s_last_applied_mute_source = AUDIO_MUTE_SOURCE_BOOT;
    s_last_applied_mute_ms     = millis();
    s_last_applied_set         = true;
  }
  return ok;
}

/* ──────────────────────────────────────────────────────────────────────────
 * SELF-TEST
 * ────────────────────────────────────────────────────────────────────────── */

/* Called from the HTTP task: record intent only. The deadline /
 * active flag flip happens in audio_process() in the main loop. */
bool selftest_start(uint32_t duration_ms) {
  if (__atomic_load_n(&s_muted, __ATOMIC_ACQUIRE)) return false;
  if (!s_initialized) return false;
  if (duration_ms == 0)     duration_ms = 30000;
  if (duration_ms > 60000)  duration_ms = 60000;
  if (duration_ms < 5000)   duration_ms = 5000;
  __atomic_store_n(&s_selftest_start_duration, duration_ms, __ATOMIC_RELAXED);
  __atomic_store_n(&s_selftest_start_pending, true, __ATOMIC_RELEASE);
  return true;
}

void selftest_stop() {
  __atomic_store_n(&s_selftest_stop_pending, true, __ATOMIC_RELEASE);
}

bool selftest_status(audio_selftest_status_t* out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  /* Snapshot reads from cross-task state. Auto-expire on read so the
   * UI sees truth even if process() is paused for some reason —
   * deadline_ms is only written from the main loop after the pending
   * start flag is consumed, so reading it without atomics is safe
   * (the read may see a slightly old deadline; that's harmless). */
  const bool active = __atomic_load_n(&s_selftest_active, __ATOMIC_ACQUIRE);
  const uint32_t now = millis();
  const uint32_t deadline = s_selftest_deadline_ms;
  const bool expired = active && (int32_t)(now - deadline) >= 0;
  if (expired) {
    __atomic_store_n(&s_selftest_active, false, __ATOMIC_RELEASE);
  }
  out->active        = (active && !expired) ? 1 : 0;
  out->matched_type  = __atomic_load_n(&s_selftest_matched_type, __ATOMIC_RELAXED);
  out->matched_conf  = __atomic_load_n(&s_selftest_matched_conf, __ATOMIC_RELAXED);
  out->remaining_ms  = (active && !expired) ? (uint32_t)(deadline - now) : 0;
  out->transitions_seen =
      __atomic_load_n(&s_selftest_transitions_seen, __ATOMIC_RELAXED);
  return true;
}

/* ──────────────────────────────────────────────────────────────────────────
 * LIVE LEVEL + TRANSITIONS (for the UI test panel)
 * ────────────────────────────────────────────────────────────────────────── */

/* Callable from any task. The level scalar is published with atomic
 * stores from the main loop; we read with __atomic_load_n. */
bool get_live_level(uint16_t* rms_out, uint32_t* age_ms_out) {
  const bool running = s_running;
  const uint16_t rms = __atomic_load_n(&s_last_rms, __ATOMIC_ACQUIRE);
  const uint32_t ts  = __atomic_load_n(&s_last_rms_ms, __ATOMIC_RELAXED);
  if (rms_out) *rms_out = running ? rms : 0;
  if (age_ms_out) {
    *age_ms_out = (ts == 0 || !running) ? UINT32_MAX : (millis() - ts);
  }
  return running;
}

/* Callable from any task. The transition ring has a single writer
 * (audio_process() in the main loop) and a single reader (HTTP). We
 * read the head/count atomically so we get a consistent view of which
 * slots are valid; the entries themselves can still be torn (a
 * not-yet-finished write may leave dur_ms briefly inconsistent with
 * at_ms). That's acceptable here — the transition trace is a
 * diagnostic UI element, not a safety signal. */
size_t get_recent_transitions(audio_transition_t* out, size_t max,
                              uint32_t now_ms_or_zero) {
  if (!out || max == 0) return 0;
  const uint32_t now = now_ms_or_zero ? now_ms_or_zero : millis();
  const size_t head  = __atomic_load_n(&s_trans_head, __ATOMIC_ACQUIRE);
  const size_t count = __atomic_load_n(&s_trans_count, __ATOMIC_ACQUIRE);
  const size_t n = (count < max) ? count : max;
  for (size_t i = 0; i < n; i++) {
    /* Inline recent() against the snapshot we just loaded, so a
     * concurrent push_transition() can't shift the indices under us. */
    if (i >= count) break;
    const size_t idx = (head + TRANS_CAP - 1 - i) % TRANS_CAP;
    const Transition t = s_trans[idx];  /* copy by value */
    out[i].is_on     = t.is_on ? 1 : 0;
    out[i].reserved[0] = out[i].reserved[1] = out[i].reserved[2] = 0;
    out[i].age_ms    = (now >= t.at_ms) ? (now - t.at_ms) : 0;
    out[i].dur_ms    = t.prev_dur_ms;
  }
  return n;
}

/* Snapshot of the active runtime config. s_cfg is written only by
 * init(); after that it's read-only, so a plain copy is safe across
 * tasks. */
bool get_config(audio_config_t* out) {
  if (!out) return false;
  *out = s_cfg;
  return true;
}

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN-LOOP PUMP
 * ────────────────────────────────────────────────────────────────────────── */

int process() {
  /* Apply any cross-task requests in single-task context first — the
   * HTTP server task must NEVER touch the I2S driver directly, because
   * i2s_driver_uninstall() will crash if a parallel i2s_read() is in
   * flight. The handler atomically sets *_pending; we consume them here. */
  if (__atomic_exchange_n(&s_mute_request_pending, false, __ATOMIC_ACQUIRE)) {
    const bool want_mute = __atomic_load_n(&s_mute_request_value, __ATOMIC_RELAXED);
    const uint8_t source = __atomic_load_n(&s_mute_request_source, __ATOMIC_RELAXED);
    bool applied = false;
    if (want_mute && s_running) {
      i2s_close();
      s_running = false;
      memset(s_trans, 0, sizeof(s_trans));
      s_trans_head = 0;
      s_trans_count = 0;
      s_envelope_high = false;
      s_cycle_matched = false;
      s_t3_cycles = 0;
      s_t4_cycles = 0;
      __atomic_store_n(&s_last_rms, 0, __ATOMIC_RELEASE);
      __atomic_store_n(&s_last_rms_ms, 0, __ATOMIC_RELEASE);
      #if FEATURE_ACOUSTIC_TRANSIENTS
      /* Same rationale as stop() / mute_sync_at_boot — wipe HPF carryover
       * and per-state accumulators so the next unmute starts clean. */
      s_hpf_prev_last = 0;
      s_state_rms_sum = 0;
      s_state_hpf_rms_sum = 0;
      s_state_frames = 0;
      s_knock_matched = false;
      s_doorbell_matched = false;
      s_glass_matched = false;
      #endif
      log_health(SCV_LOG_INFO, SCV_CAT_SENSOR,
                 "Audio: mic muted by user", "I2S released");
      applied = true;
    } else if (!want_mute && !s_running && s_initialized) {
      if (i2s_open()) {
        s_running = true;
        s_state_entered_ms = millis();
        s_cycle_matched = false;
        #if FEATURE_ACOUSTIC_TRANSIENTS
        /* Belt-and-suspenders — also zero on the unmute path so even if
         * a future change skips the mute-path wipe the unmute side is
         * guaranteed to start with no stale state. */
        s_hpf_prev_last = 0;
        s_state_rms_sum = 0;
        s_state_hpf_rms_sum = 0;
        s_state_frames = 0;
        #endif
        log_health(SCV_LOG_INFO, SCV_CAT_SENSOR,
                   "Audio: mic unmuted", "I2S re-armed");
        applied = true;
      }
    }
    /* Tell the application a real state change happened (so it can sign
     * an audit-trail event into the witness chain). We deliberately do
     * NOT fire on no-op transitions (mute when already muted, etc.). */
    if (applied) {
      __atomic_store_n(&s_last_applied_mute_source, source, __ATOMIC_RELAXED);
      __atomic_store_n(&s_last_applied_mute_ms,     millis(), __ATOMIC_RELEASE);
      __atomic_store_n(&s_last_applied_set,         true,    __ATOMIC_RELEASE);
      if (s_mute_cb) s_mute_cb(want_mute, source);
    }
  }

  if (__atomic_exchange_n(&s_selftest_start_pending, false, __ATOMIC_ACQUIRE)) {
    const uint32_t d = __atomic_load_n(&s_selftest_start_duration, __ATOMIC_RELAXED);
    if (s_running) {
      __atomic_store_n(&s_selftest_matched_type, AUDIO_EVENT_NONE, __ATOMIC_RELAXED);
      __atomic_store_n(&s_selftest_matched_conf, 0, __ATOMIC_RELAXED);
      __atomic_store_n(&s_selftest_transitions_seen, 0, __ATOMIC_RELAXED);
      s_selftest_deadline_ms = millis() + d;
      __atomic_store_n(&s_selftest_active, true, __ATOMIC_RELEASE);
    }
  }

  if (__atomic_exchange_n(&s_selftest_stop_pending, false, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&s_selftest_active, false, __ATOMIC_RELEASE);
  }

  if (!s_running || !s_i2s_installed) return 0;

  /* Local sample buffer; wiped (full size) after RMS is computed so no
   * residual audio can persist on the stack regardless of how short the
   * I2S read came back. */
  int16_t samples[AUDIO_FRAME_SAMPLES];
  size_t bytes_read = 0;
  int frames_this_call = 0;

  /* Drain up to 4 frames per process() call so a slow main loop can
   * catch up without blocking. */
  for (int i = 0; i < 4; i++) {
    bytes_read = 0;
    /* Non-blocking read: timeout 0 returns immediately if no DMA buffer
     * is ready. Legacy i2s_read returns ESP_OK with bytes_read==0 in
     * that case (rather than ESP_ERR_TIMEOUT). */
    esp_err_t err = i2s_read(I2S_NUM_0, samples, sizeof(samples),
                             &bytes_read, 0);
    if (err == ESP_ERR_TIMEOUT || bytes_read == 0) {
      /* Always wipe the stack buffer even if the read produced nothing —
       * a previous iteration may have left audio in it. */
      secure_wipe(samples, sizeof(samples));
      break;
    }
    if (err != ESP_OK) {
      s_stats.i2s_read_errors++;
      secure_wipe(samples, sizeof(samples));
      break;
    }

    const size_t n = bytes_read / sizeof(int16_t);
    if (n == 0) {
      secure_wipe(samples, sizeof(samples));
      break;
    }

    const uint16_t rms = compute_rms(samples, n);
    #if FEATURE_ACOUSTIC_TRANSIENTS
    /* Compute the high-pass-band RMS BEFORE the wipe, while samples are
     * still alive. The result is a single uint16 like full-RMS; we
     * accumulate per-state averages below and only the per-transition
     * AVERAGE ratio is retained — never the per-frame value. */
    const uint16_t hpf_rms = compute_hpf_rms(samples, n);
    #endif
    /* PRIVACY BARRIER: wipe the FULL buffer (not just n samples) so any
     * tail beyond the short read is also zeroed. compute_rms takes
     * `const int16_t*` precisely so it can't accidentally leave the wipe
     * to the callee. */
    secure_wipe(samples, sizeof(samples));

    s_stats.frames_processed++;
    s_stats.envelope_samples++;
    frames_this_call++;

    #if FEATURE_ACOUSTIC_TRANSIENTS
    /* Accumulate per-state band-ratio so we can summarize the just-
     * ended state into one byte at the next transition. Saturate the
     * sums with 2^31 headroom on the full-band gate — the HPF sum can
     * grow up to ~2x the full-band sum, so gating at 0x7FFFFFFF keeps
     * BOTH accumulators below uint32 range. */
    if (s_state_rms_sum < 0x7FFFFFFFUL) {
      s_state_rms_sum     += rms;
      s_state_hpf_rms_sum += hpf_rms;
      s_state_frames++;
    }
    #endif

    /* Hysteresis state machine on the envelope. */
    const uint32_t now = millis();
    /* Publish the RMS scalar for the UI level meter. This is the same
     * number the hysteresis uses — we're not adding a second audio path,
     * we're just publishing the existing scalar with a release store so
     * a concurrent HTTP read can pick up the value safely. */
    __atomic_store_n(&s_last_rms, rms, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_rms_ms, now, __ATOMIC_RELAXED);

    bool transition = false;
    bool entered_on = false;

    if (!s_envelope_high && rms >= s_cfg.rms_on_threshold) {
      transition = true;
      entered_on = true;
      s_envelope_high = true;
      s_stats.on_transitions++;
    } else if (s_envelope_high && rms <= s_cfg.rms_off_threshold) {
      transition = true;
      entered_on = false;
      s_envelope_high = false;
      s_stats.off_transitions++;
    }

    if (transition) {
      const uint32_t prev_dur = (s_state_entered_ms == 0)
                                ? 0 : (now - s_state_entered_ms);
      uint8_t prev_band_ratio = 100;  /* 100 = "broadband" default */
      #if FEATURE_ACOUSTIC_TRANSIENTS
      /* Summarize the just-ended state's band concentration. The ratio
       * is hpf_avg / full_avg * 100. Sums over the same frames so the
       * mean cancels out — just divide. Skip when frames==0 (transition
       * fired immediately at boot) or full_avg==0 (silent state — the
       * ratio is undefined; report 100 = "no opinion"). */
      if (s_state_frames > 0 && s_state_rms_sum > 0) {
        const uint32_t r = (uint32_t)(((uint64_t)s_state_hpf_rms_sum * 100u) / s_state_rms_sum);
        prev_band_ratio = (uint8_t)(r > 200u ? 200u : r);
      }
      s_state_rms_sum = 0;
      s_state_hpf_rms_sum = 0;
      s_state_frames = 0;
      #endif
      push_transition(entered_on, now, prev_dur, prev_band_ratio);
      s_state_entered_ms = now;
      /* Every new transition rearms the cadence matcher — the matcher
       * may now declare a fresh cycle. */
      s_cycle_matched = false;
      #if FEATURE_ACOUSTIC_TRANSIENTS
      s_knock_matched    = false;
      s_doorbell_matched = false;
      s_glass_matched    = false;
      #endif

      if (__atomic_load_n(&s_selftest_active, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&s_selftest_transitions_seen, 1, __ATOMIC_RELAXED);
      }

      /* Reset the cycle counter if we've been silent > 10 s — a fresh
       * cadence is starting. */
      if (entered_on && prev_dur > 10000) {
        s_t3_cycles = 0;
        s_t4_cycles = 0;
      }
    }

    /* Self-test auto-expiry. */
    const bool st_active_now =
        __atomic_load_n(&s_selftest_active, __ATOMIC_ACQUIRE);
    if (st_active_now && (int32_t)(now - s_selftest_deadline_ms) >= 0) {
      __atomic_store_n(&s_selftest_active, false, __ATOMIC_RELEASE);
    }
    const bool relaxed = st_active_now &&
        (int32_t)(now - s_selftest_deadline_ms) < 0;

    /* Cadence matching is timing-based and only meaningful right after
     * the long inter-cycle silence has elapsed. We check whenever we're
     * currently OFF, the OFF state is at least 1 s old, AND we haven't
     * already declared a cycle for this OFF period (otherwise the same
     * 6 transitions in the ring would re-trigger every iteration). */
    if (!s_envelope_high && !s_cycle_matched &&
        (now - s_state_entered_ms) >= 1000) {
      const int t3 = score_t3_cycle(now, relaxed);
      const int min_conf = relaxed ? 30 : 50;
      if (t3 >= min_conf) {
        s_t3_cycles++;
        s_stats.t3_detected++;
        if (relaxed) {
          /* Test-only: record match locally, DO NOT fire the event
           * callback. The user pressed their alarm's TEST button; we
           * must not flow that into Home Assistant smoke automations. */
          __atomic_store_n(&s_selftest_matched_type,
                           (uint8_t)AUDIO_EVENT_T3_SMOKE_ALARM,
                           __ATOMIC_RELAXED);
          __atomic_store_n(&s_selftest_matched_conf,
                           (uint8_t)t3, __ATOMIC_RELAXED);
        } else {
          try_emit_event(AUDIO_EVENT_T3_SMOKE_ALARM, (uint8_t)t3,
                         s_t3_cycles, now);
        }
        /* Block re-firing on the same set of transitions until a new
         * on/off transition arrives. We deliberately do NOT touch
         * s_state_entered_ms — that would corrupt the next transition's
         * prev_dur_ms and cause the next cycle's beep-1 timing to
         * misfit. */
        s_cycle_matched = true;
      } else {
        const int t4 = score_t4_cycle(now, relaxed);
        if (t4 >= min_conf) {
          s_t4_cycles++;
          s_stats.t4_detected++;
          if (relaxed) {
            __atomic_store_n(&s_selftest_matched_type,
                             (uint8_t)AUDIO_EVENT_T4_CO_ALARM,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&s_selftest_matched_conf,
                             (uint8_t)t4, __ATOMIC_RELAXED);
          } else {
            try_emit_event(AUDIO_EVENT_T4_CO_ALARM, (uint8_t)t4,
                           s_t4_cycles, now);
          }
          s_cycle_matched = true;
        }
      }

      #if FEATURE_ACOUSTIC_TRANSIENTS
      /* Phase 2b detectors. These never run under self-test mode — the
       * user pressing TEST on an alarm is what selftest_active gates,
       * and we don't want a test press to ALSO fire a stray glass-break
       * automation. They share the same OFF-with-1s-silence gate as
       * T3/T4 so they only run once per "thing-then-quiet" cycle. */
      if (!relaxed && !s_cycle_matched) {
        if (!s_knock_matched) {
          const int k = score_knock_cycle(now);
          if (k >= 50) {
            s_knock_count++;
            s_stats.knock_detected++;
            try_emit_event(AUDIO_EVENT_KNOCK, (uint8_t)k, s_knock_count, now);
            s_knock_matched = true;
          }
        }
        if (!s_doorbell_matched) {
          const int d = score_doorbell_cycle(now);
          if (d >= 60) {
            s_doorbell_count++;
            s_stats.doorbell_detected++;
            try_emit_event(AUDIO_EVENT_DOORBELL, (uint8_t)d, s_doorbell_count, now);
            s_doorbell_matched = true;
          }
        }
        if (!s_glass_matched) {
          const int g = score_glass_break_cycle(now);
          if (g >= 70) {
            s_glass_count++;
            s_stats.glass_break_detected++;
            try_emit_event(AUDIO_EVENT_GLASS_BREAK, (uint8_t)g, s_glass_count, now);
            s_glass_matched = true;
          }
        }
      }
      #endif  /* FEATURE_ACOUSTIC_TRANSIENTS */
    }
  }

  return frames_this_call;
}

/* ──────────────────────────────────────────────────────────────────────────
 * INTROSPECTION
 * ────────────────────────────────────────────────────────────────────────── */

bool get_stats(audio_stats_t* out) {
  if (!out) return false;
  *out = s_stats;
  return true;
}

const char* event_name(uint8_t type) {
  switch (type) {
    case AUDIO_EVENT_NONE:           return "none";
    case AUDIO_EVENT_T3_SMOKE_ALARM: return "smoke_alarm_t3";
    case AUDIO_EVENT_T4_CO_ALARM:    return "co_alarm_t4";
    case AUDIO_EVENT_KNOCK:          return "knock";
    case AUDIO_EVENT_DOORBELL:       return "doorbell";
    case AUDIO_EVENT_GLASS_BREAK:    return "glass_break";
    default:                         return "unknown";
  }
}

}  /* namespace audio */

/* ──────────────────────────────────────────────────────────────────────────
 * C API
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" {

bool audio_init(const audio_config_t* config) {
  audio_config_t cfg = AUDIO_CONFIG_DEFAULT;
  if (config) cfg = *config;
  return audio::init(cfg);
}
void audio_deinit(void)              { audio::deinit(); }
bool audio_start(void)               { return audio::start(); }
void audio_stop(void)                { audio::stop(); }
bool audio_is_running(void)          { return audio::is_running(); }
void audio_set_event_callback(audio_event_cb_t cb) { audio::set_event_callback(cb); }
int  audio_process(void)             { return audio::process(); }
bool audio_get_stats(audio_stats_t* out) { return audio::get_stats(out); }
const char* audio_event_name(uint8_t t) { return audio::event_name(t); }

bool audio_mute(bool muted, uint8_t source) { return audio::mute(muted, source); }
bool audio_is_muted(void)               { return audio::is_muted(); }
bool audio_mute_sync_at_boot(bool muted){ return audio::mute_sync_at_boot(muted); }
void audio_set_mute_callback(audio_mute_cb_t cb) { audio::set_mute_callback(cb); }

bool audio_save_mute_intent(bool muted) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;
  prefs.putBool(NVS_KEY_MIC_MUTED, muted);
  prefs.end();
  return true;
}

void audio_get_mute_info(audio_mute_info_t* out) { audio::get_mute_info(out); }

bool audio_selftest_start(uint32_t duration_ms) {
  return audio::selftest_start(duration_ms);
}
void audio_selftest_stop(void)          { audio::selftest_stop(); }
bool audio_selftest_status(audio_selftest_status_t* out) {
  return audio::selftest_status(out);
}

bool audio_get_config(audio_config_t* out) { return audio::get_config(out); }

bool audio_get_live_level(uint16_t* rms_out, uint32_t* age_ms_out) {
  return audio::get_live_level(rms_out, age_ms_out);
}

size_t audio_get_recent_transitions(audio_transition_t* out, size_t max,
                                    uint32_t now_ms_or_zero) {
  return audio::get_recent_transitions(out, max, now_ms_or_zero);
}

}  /* extern "C" */

#endif  /* FEATURE_ACOUSTIC_EVENTS */
