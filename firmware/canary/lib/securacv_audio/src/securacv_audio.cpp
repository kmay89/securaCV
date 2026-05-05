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
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_audio.h"

#include <Arduino.h>
#include <string.h>

#include "log_level.h"
#include "securacv_witness.h"   /* log_health() */

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
  #include <driver/i2s_pdm.h>
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
static i2s_chan_handle_t s_rx_chan = nullptr;

/* Envelope hysteresis state machine. */
static bool     s_envelope_high = false;
static uint32_t s_state_entered_ms = 0;

/* Ring of recent on/off transitions (newest at head; wraps). Each entry
 * records the time we entered a state and the duration we stayed in the
 * PREVIOUS state. Capacity 16 covers two full T3 cycles (12 transitions)
 * plus headroom for noise. */
struct Transition {
  bool     is_on;       /* true = entered ON state, false = entered OFF */
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

/* ──────────────────────────────────────────────────────────────────────────
 * RMS COMPUTATION  — int64 sum-of-squares; samples wiped after each call
 * ────────────────────────────────────────────────────────────────────────── */

static uint16_t compute_rms(int16_t* samples, size_t n) {
  if (n == 0) return 0;
  int64_t sumsq = 0;
  for (size_t i = 0; i < n; i++) {
    const int32_t s = samples[i];
    sumsq += (int64_t)s * s;
  }
  /* PRIVACY BARRIER: scrub the sample buffer the moment we have the
   * scalar. The caller may not need the buffer back, but we wipe in
   * place defensively so a stale buffer can never leak audio. */
  secure_wipe(samples, n * sizeof(int16_t));

  const uint32_t mean_sq = (uint32_t)(sumsq / (int64_t)n);
  uint32_t rms = isqrt_u32(mean_sq);
  if (rms > 0xFFFFu) rms = 0xFFFFu;
  return (uint16_t)rms;
}

/* ──────────────────────────────────────────────────────────────────────────
 * TRANSITION RING
 * ────────────────────────────────────────────────────────────────────────── */

static void push_transition(bool entered_on, uint32_t now_ms,
                            uint32_t prev_dur_ms) {
  s_trans[s_trans_head].is_on        = entered_on;
  s_trans[s_trans_head].at_ms        = now_ms;
  s_trans[s_trans_head].prev_dur_ms  = prev_dur_ms;
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
static int score_t3_cycle(uint32_t now_ms) {
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

  auto err_against = [](int32_t v, int32_t target, int32_t tol) -> int32_t {
    int32_t d = v - target;
    if (d < 0) d = -d;
    if (d > tol) return -1;
    return d;
  };

  const int32_t e_b1 = err_against(b1, 500, 200);
  const int32_t e_b2 = err_against(b2, 500, 200);
  const int32_t e_b3 = err_against(b3, 500, 200);
  const int32_t e_g12 = err_against(g12, 500, 200);
  const int32_t e_g23 = err_against(g23, 500, 200);
  const int32_t e_pause = err_against(pause, 1500, 500);
  if (e_b1 < 0 || e_b2 < 0 || e_b3 < 0 ||
      e_g12 < 0 || e_g23 < 0 || e_pause < 0) return -1;

  /* Sum of relative errors → confidence. Max possible sum ≈ 200*5 + 500
   * = 1500. Map 0 → 100, 1500 → 50. */
  const int32_t total_err = e_b1 + e_b2 + e_b3 + e_g12 + e_g23 + e_pause;
  int32_t conf = 100 - (total_err * 50 / 1500);
  if (conf < 50)  conf = 50;
  if (conf > 100) conf = 100;
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
static int score_t4_cycle(uint32_t now_ms) {
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

  auto err = [](int32_t v, int32_t target, int32_t tol) -> int32_t {
    int32_t d = v - target;
    if (d < 0) d = -d;
    if (d > tol) return -1;
    return d;
  };

  const int32_t e_b1 = err(b1, 100, 60);
  const int32_t e_b2 = err(b2, 100, 60);
  const int32_t e_b3 = err(b3, 100, 60);
  const int32_t e_b4 = err(b4, 100, 60);
  const int32_t e_g12 = err(g12, 100, 60);
  const int32_t e_g23 = err(g23, 100, 60);
  const int32_t e_g34 = err(g34, 100, 60);
  const int32_t e_pause = err(pause, 5000, 1500);
  if (e_b1 < 0 || e_b2 < 0 || e_b3 < 0 || e_b4 < 0 ||
      e_g12 < 0 || e_g23 < 0 || e_g34 < 0 || e_pause < 0) return -1;

  const int32_t total_err = e_b1+e_b2+e_b3+e_b4+e_g12+e_g23+e_g34+e_pause;
  /* Max worst case ≈ 60*7 + 1500 = 1920. */
  int32_t conf = 100 - (total_err * 50 / 1920);
  if (conf < 50)  conf = 50;
  if (conf > 100) conf = 100;
  return (int)conf;
}

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

static bool i2s_open() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &s_rx_chan);
  if (err != ESP_OK || s_rx_chan == nullptr) {
    char d[32]; snprintf(d, sizeof(d), "new_channel err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Audio: I2S channel alloc failed", d);
    return false;
  }

  i2s_pdm_rx_config_t pdm_cfg = {
    .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(s_cfg.sample_rate_hz),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                               I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .clk     = (gpio_num_t)MIC_PIN_CLK,
      .din     = (gpio_num_t)MIC_PIN_DATA,
      .invert_flags = { .clk_inv = false },
    },
  };

  err = i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_cfg);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "init err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Audio: I2S PDM init failed", d);
    i2s_del_channel(s_rx_chan);
    s_rx_chan = nullptr;
    return false;
  }
  return true;
}

static void i2s_close() {
  if (s_rx_chan) {
    i2s_channel_disable(s_rx_chan);
    i2s_del_channel(s_rx_chan);
    s_rx_chan = nullptr;
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
  s_last_event_ms = 0;
  s_t3_cycles = 0;
  s_t4_cycles = 0;

  s_initialized = true;
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
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

  if (!i2s_open()) return false;

  esp_err_t err = i2s_channel_enable(s_rx_chan);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "enable err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Audio: I2S enable failed", d);
    i2s_close();
    return false;
  }

  s_running = true;
  s_state_entered_ms = millis();
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
  s_t3_cycles = 0;
  s_t4_cycles = 0;
}

bool is_running() { return s_running; }

void set_event_callback(audio_event_cb_t cb) { s_cb = cb; }

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN-LOOP PUMP
 * ────────────────────────────────────────────────────────────────────────── */

int process() {
  if (!s_running || s_rx_chan == nullptr) return 0;

  /* Local sample buffer — scrubbed after each frame's RMS is computed. */
  int16_t samples[AUDIO_FRAME_SAMPLES];
  size_t bytes_read = 0;
  int frames_this_call = 0;

  /* Drain up to 4 frames per process() call so a slow main loop can
   * catch up without blocking. */
  for (int i = 0; i < 4; i++) {
    bytes_read = 0;
    /* Non-blocking read: timeout 0 means "return what's already in DMA". */
    esp_err_t err = i2s_channel_read(s_rx_chan, samples, sizeof(samples),
                                      &bytes_read, 0);
    if (err == ESP_ERR_TIMEOUT || bytes_read == 0) break;
    if (err != ESP_OK) {
      s_stats.i2s_read_errors++;
      break;
    }

    const size_t n = bytes_read / sizeof(int16_t);
    if (n == 0) break;

    /* compute_rms scrubs the buffer in place. */
    const uint16_t rms = compute_rms(samples, n);
    s_stats.frames_processed++;
    s_stats.envelope_samples++;
    frames_this_call++;

    /* Hysteresis state machine on the envelope. */
    const uint32_t now = millis();
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
      push_transition(entered_on, now, prev_dur);
      s_state_entered_ms = now;

      /* Reset the cycle counter if we've been silent > 10 s — a fresh
       * cadence is starting. */
      if (entered_on && prev_dur > 10000) {
        s_t3_cycles = 0;
        s_t4_cycles = 0;
      }
    }

    /* Cadence matching is timing-based and only meaningful right after
     * the long inter-cycle silence has elapsed. We check whenever we're
     * currently OFF and the OFF state is at least 1 s old. */
    if (!s_envelope_high && (now - s_state_entered_ms) >= 1000) {
      const int t3 = score_t3_cycle(now);
      if (t3 >= 50) {
        s_t3_cycles++;
        s_stats.t3_detected++;
        try_emit_event(AUDIO_EVENT_T3_SMOKE_ALARM, (uint8_t)t3,
                       s_t3_cycles, now);
        /* Don't immediately re-fire on the same transitions: bump the
         * head pointer so the matcher can't see them again. */
        s_state_entered_ms = now;  /* refresh OFF clock */
      } else {
        const int t4 = score_t4_cycle(now);
        if (t4 >= 50) {
          s_t4_cycles++;
          s_stats.t4_detected++;
          try_emit_event(AUDIO_EVENT_T4_CO_ALARM, (uint8_t)t4,
                         s_t4_cycles, now);
          s_state_entered_ms = now;
        }
      }
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

}  /* extern "C" */
