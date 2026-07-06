// Host-side tests for the PDM acoustic-event pipeline (securacv_audio.cpp).
//
// Compiles the REAL vendored module against stub Arduino/ESP-IDF headers
// (stubs/audio/) and feeds synthetic envelopes through a scripted i2s_read,
// so the full path — RMS, hysteresis, transition ring, cadence matchers,
// deferred mute — runs exactly as it does on-device, just with a fake clock
// and fake DMA.
//
// Covered:
//   1. NFPA 72 T3 smoke cadence (3×0.5 s alarm-band beeps + 1.5 s pause)
//      fires the event callback with AUDIO_EVENT_T3_SMOKE_ALARM, conf ≥ 50.
//   2. UL 2034 T4 CO cadence (4×0.1 s beeps + 5 s pause) fires
//      AUDIO_EVENT_T4_CO_ALARM.
//   3. A knock pattern (3 short low-band impulses) fires AUDIO_EVENT_KNOCK.
//   4. A two-tone mid-band chime fires AUDIO_EVENT_DOORBELL.
//   5. A sustained high-band burst fires AUDIO_EVENT_GLASS_BREAK.
//   6. Hard mute is applied on the next process() tick, stops the stream,
//      fires the mute callback, and suppresses further events.
//   7. audio_set_thresholds() validates its arguments.
//   8. Stage-1 tone gate: a T3-timed cadence whose "beeps" are OFF-BAND
//      (500 Hz) produces NO event — cadence alone must not read as smoke.
//   9. DC offset: a loud constant-offset segment produces NO envelope
//      transitions (the DC-removed RMS sees it as silence).
//  10. Sample-stream clock: T3 still matches when the wall clock is frozen
//      (frames drained in a burst after a stalled loop) — timing comes
//      from frames × frame_ms, not millis() at processing time.
//  11. audio_get_recent_transitions() reports the per-state tone ratio.
//
// Build/run: make (this dir). No Arduino runtime needed.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Arduino.h"
#include "driver/i2s.h"
#include "log_level.h"
#include "securacv_audio.h"

// ── Test-controlled clock ───────────────────────────────────────────────
static unsigned long g_now_ms = 0;
unsigned long millis() { return g_now_ms; }

// ── log_health stub (health_log.h declares it; firmware defines it in the
//    sketch — here we just swallow the calls) ────────────────────────────
void log_health(LogLevel, LogCategory, const char*, const char*) {}

// ── Scripted I2S timeline ───────────────────────────────────────────────
// The script is a list of (duration_ms, amplitude[, flip_every]) segments.
// Each i2s_read call delivers one 20 ms frame from the script head and
// advances the fake clock by 20 ms (unless frozen — test 10); an exhausted
// script returns 0 bytes (the driver's "no DMA buffer ready" path).
//
// flip_every shapes the spectrum. 0 = constant amplitude: pure DC, which
// the DC-removed RMS reads as SILENCE (test 9). k > 0 = square wave
// flipping sign every k samples (fundamental fs/2k = 8000/k Hz at 16 kHz):
//
//   • one-tap-difference HPF ratio (Phase 2b band_ratio): the difference
//     signal is ±2A at each flip, so hpf_rms ≈ 2A/√k, ratio ≈ 200/√k.
//     k=4 (2 kHz) ≈ 100 mid band (doorbell); k=1 (8 kHz) ≈ 200 high band
//     (glass); k=64 (125 Hz) ≈ 25 low band (knock).
//   • alarm-band tone ratio (stage-1 gate, biquad fc=3.4 kHz Q=1.8):
//     k=2 (4 kHz — pure tone at the passband edge) ≈ 86, in-band;
//     k=16 (500 Hz — only weak harmonics reach the band) ≈ 20, off-band.
struct Segment { uint32_t dur_ms; int16_t amplitude; uint16_t flip_every = 0; };
static std::vector<Segment> g_script;
static size_t g_seg_idx = 0;
static uint32_t g_seg_consumed_ms = 0;
static bool g_driver_installed = false;
static bool g_clock_frozen = false;  // test 10: stalled-main-loop simulation

static void script_load(const std::vector<Segment>& segs) {
  g_script = segs;
  g_seg_idx = 0;
  g_seg_consumed_ms = 0;
}

esp_err_t i2s_driver_install(i2s_port_t, const i2s_config_t*, int, void*) {
  g_driver_installed = true;
  return ESP_OK;
}
esp_err_t i2s_driver_uninstall(i2s_port_t) {
  g_driver_installed = false;
  return ESP_OK;
}
esp_err_t i2s_set_pin(i2s_port_t, const i2s_pin_config_t*) { return ESP_OK; }
esp_err_t i2s_zero_dma_buffer(i2s_port_t) { return ESP_OK; }

esp_err_t i2s_read(i2s_port_t, void* dest, size_t size, size_t* bytes_read,
                   int /*timeout_ticks*/) {
  *bytes_read = 0;
  if (!g_driver_installed) return ESP_OK;
  while (g_seg_idx < g_script.size() &&
         g_seg_consumed_ms >= g_script[g_seg_idx].dur_ms) {
    g_seg_idx++;
    g_seg_consumed_ms = 0;
  }
  if (g_seg_idx >= g_script.size()) return ESP_OK;  // script exhausted

  const int16_t amp = g_script[g_seg_idx].amplitude;
  const uint16_t flip = g_script[g_seg_idx].flip_every;
  const size_t n = size / sizeof(int16_t);  // one full frame (320 samples)
  int16_t* out = static_cast<int16_t*>(dest);
  for (size_t i = 0; i < n; i++) {
    out[i] = (flip && ((i / flip) & 1)) ? (int16_t)-amp : amp;
  }
  *bytes_read = n * sizeof(int16_t);
  g_seg_consumed_ms += 20;
  if (!g_clock_frozen) g_now_ms += 20;  // the frame "took" 20 ms of real time
  return ESP_OK;
}

// ── Event capture ───────────────────────────────────────────────────────
static std::vector<audio_event_t> g_events;
static void on_event(const audio_event_t* evt) { g_events.push_back(*evt); }

static int g_mute_cb_count = 0;
static bool g_mute_cb_last = false;
static void on_mute(bool muted, uint8_t /*source*/) {
  g_mute_cb_count++;
  g_mute_cb_last = muted;
}

// Pump the pipeline until the script is exhausted, plus `tail_ms` of
// scriptless time. NOTE: the cadence matchers only run while frames are
// arriving (on-device the mic never stops producing frames), so trailing
// pauses that a matcher must observe belong IN the script as silence
// segments; the scriptless tail only serves paths that act with no frames
// pending (e.g. the deferred mute request).
static void pump(uint32_t tail_ms) {
  for (int guard = 0; guard < 100000; guard++) {
    const int frames = audio_process();
    if (frames == 0) break;
  }
  const uint32_t deadline = g_now_ms + tail_ms;
  while (g_now_ms < deadline) {
    g_now_ms += 20;
    audio_process();
  }
}

static void reset_pipeline() {
  audio_deinit();
  g_events.clear();
  audio_config_t cfg = AUDIO_CONFIG_DEFAULT;
  assert(audio_init(&cfg));
  audio_set_event_callback(on_event);
  audio_set_mute_callback(on_mute);
  assert(audio_start());
}

// ── Tests ───────────────────────────────────────────────────────────────

static void test_t3_smoke_cadence() {
  reset_pipeline();
  const int16_t LOUD = 2000;  // RMS 2000 ≥ ON threshold 800
  // flip_every=2 → 4 kHz tone: inside the stage-1 alarm band, like a
  // real UL 217 sounder (3.0–4.0 kHz).
  script_load({
      {2000, 0},      // settle: establish a long initial OFF state
      {500, LOUD, 2}, {500, 0},   // beep 1 + gap
      {500, LOUD, 2}, {500, 0},   // beep 2 + gap
      {500, LOUD, 2},             // beep 3
      {1600, 0},               // inter-cycle pause; matcher needs ≥ 1000 ms
  });
  pump(/*tail_ms=*/0);

  bool saw_t3 = false;
  for (const auto& e : g_events) {
    if (e.event_type == AUDIO_EVENT_T3_SMOKE_ALARM) {
      saw_t3 = true;
      assert(e.confidence >= 50);
      assert(e.cycle_count >= 1);
    }
  }
  assert(saw_t3 && "T3 smoke cadence must fire the event callback");
  printf("ok  T3 smoke cadence detected (events=%zu)\n", g_events.size());
}

static void test_knock() {
  reset_pipeline();
  const int16_t LOUD = 2000;  // flip_every=64 → 125 Hz: low band (ratio ≈ 25)
  script_load({
      {2000, 0},
      {100, LOUD, 64}, {200, 0},   // impulse 1 + gap
      {100, LOUD, 64}, {200, 0},   // impulse 2 + gap
      {100, LOUD, 64},             // impulse 3
      {1200, 0},               // knock needs ≥ 500 ms trailing silence
  });
  pump(/*tail_ms=*/0);

  bool saw_knock = false;
  for (const auto& e : g_events) {
    if (e.event_type == AUDIO_EVENT_KNOCK) saw_knock = true;
  }
  assert(saw_knock && "3 short low-band impulses must read as a knock");
  printf("ok  knock pattern detected\n");
}


static void test_t4_co_cadence() {
  reset_pipeline();
  const int16_t LOUD = 2000;
  // UL 2034: 4 beeps of 100 ms with 100 ms gaps, then a 5 s pause. The
  // matcher declares once 3.5 s of the pause has elapsed. Beeps in-band
  // (flip_every=2 → 4 kHz) to clear the stage-1 tone gate.
  script_load({
      {2000, 0},
      {100, LOUD, 2}, {100, 0},
      {100, LOUD, 2}, {100, 0},
      {100, LOUD, 2}, {100, 0},
      {100, LOUD, 2},
      {4000, 0},
  });
  pump(/*tail_ms=*/0);

  bool saw_t4 = false;
  for (const auto& e : g_events) {
    if (e.event_type == AUDIO_EVENT_T4_CO_ALARM) {
      saw_t4 = true;
      assert(e.confidence >= 50);
    }
  }
  assert(saw_t4 && "T4 CO cadence must fire the event callback");
  printf("ok  T4 CO cadence detected\n");
}

static void test_doorbell() {
  reset_pipeline();
  // Two ~500 ms mid-band tones (flip_every=4 -> band ratio ~100, inside
  // the doorbell's 60..130 window) with a 200 ms gap, then quiet.
  const int16_t LOUD = 2000;
  script_load({
      {2000, 0},
      {500, LOUD, 4}, {200, 0},
      {500, LOUD, 4},
      // Trailing quiet: the global matcher gate needs >= 1 s of silence
      // before ANY matcher runs, then doorbell adds its own >= 600 ms.
      {1500, 0},
  });
  pump(/*tail_ms=*/0);

  bool saw_doorbell = false;
  for (const auto& e : g_events) {
    if (e.event_type == AUDIO_EVENT_DOORBELL) saw_doorbell = true;
  }
  assert(saw_doorbell && "two-tone mid-band chime must read as a doorbell");
  printf("ok  doorbell chime detected\n");
}

static void test_glass_break() {
  reset_pipeline();
  // One sustained ~1 s high-band burst (flip_every=1 -> ratio ~200, above
  // the >=130 high-band gate; duration inside the 800..3000 ms window).
  const int16_t LOUD = 2000;
  script_load({
      {2000, 0},
      {1000, LOUD, 1},
      {1500, 0},   // >= 1 s global matcher gate, then glass's own >= 500 ms
  });
  pump(/*tail_ms=*/0);

  bool saw_glass = false;
  for (const auto& e : g_events) {
    if (e.event_type == AUDIO_EVENT_GLASS_BREAK) saw_glass = true;
  }
  assert(saw_glass && "sustained high-band burst must read as glass break");
  printf("ok  glass-break burst detected\n");
}

static void test_mute_stops_events() {
  reset_pipeline();
  // Request mute from "another task": applied on the next process() tick.
  assert(audio_mute(true, AUDIO_MUTE_SOURCE_HTTP));
  g_mute_cb_count = 0;
  script_load({{200, 0}});
  pump(/*tail_ms=*/100);

  assert(audio_is_muted());
  assert(!audio_is_running());
  assert(!g_driver_installed && "hard mute must release the I2S driver");
  assert(g_mute_cb_count == 1 && g_mute_cb_last == true);

  // A loud in-band cadence while muted must produce nothing.
  g_events.clear();
  script_load({
      {500, 2000, 2}, {500, 0}, {500, 2000, 2}, {500, 0}, {500, 2000, 2},
      {1600, 0},
  });
  pump(/*tail_ms=*/0);
  assert(g_events.empty() && "no events may cross the boundary while muted");
  printf("ok  hard mute releases I2S and suppresses events\n");
}

static void test_threshold_validation() {
  reset_pipeline();
  assert(audio_set_thresholds(1200, 600));   // valid pair
  audio_config_t cfg;
  assert(audio_get_config(&cfg));
  assert(cfg.rms_on_threshold == 1200 && cfg.rms_off_threshold == 600);

  assert(!audio_set_thresholds(400, 400));   // on must exceed off
  assert(!audio_set_thresholds(400, 800));   // inverted
  assert(!audio_set_thresholds(100, 0));     // off must be nonzero
  assert(audio_get_config(&cfg));
  assert(cfg.rms_on_threshold == 1200 && "rejected pair must change nothing");
  printf("ok  threshold validation\n");
}

// Stage-1 tone gate: exact T3 TIMING, but the "beeps" are a 500 Hz tone
// (flip_every=16) whose harmonics barely reach the 2.6–4.4 kHz alarm
// band. Someone rhythmically slamming a door — or a bass line — can
// reproduce the cadence; only a real sounder reproduces the spectrum.
static void test_t3_rejects_offband_beeps() {
  reset_pipeline();
  const int16_t LOUD = 2000;
  script_load({
      {2000, 0},
      {500, LOUD, 16}, {500, 0},
      {500, LOUD, 16}, {500, 0},
      {500, LOUD, 16},
      {1600, 0},
  });
  pump(/*tail_ms=*/0);

  for (const auto& e : g_events) {
    assert(e.event_type != AUDIO_EVENT_T3_SMOKE_ALARM &&
           "off-band cadence must NOT read as a smoke alarm");
    assert(e.event_type != AUDIO_EVENT_T4_CO_ALARM);
  }
  // Sanity: the envelope DID see the cadence (this is a spectral
  // rejection, not a deaf pipeline).
  audio_stats_t st;
  assert(audio_get_stats(&st));
  assert(st.on_transitions >= 3);
  printf("ok  tone gate rejects off-band (500 Hz) T3 cadence\n");
}

// DC-removed RMS: a large constant offset (PDM mic DC bias) is not sound.
// Under the old sum-of-squares RMS this segment read as a permanent ON
// state that pinned the envelope and blinded the matcher forever.
static void test_dc_offset_reads_as_silence() {
  reset_pipeline();
  script_load({
      {1000, 2000, 0},   // flip_every=0: pure DC at 2.5× the ON threshold
      {500, 0},
  });
  pump(/*tail_ms=*/0);

  audio_stats_t st;
  assert(audio_get_stats(&st));
  assert(st.on_transitions == 0 && "DC offset must not trip the envelope");
  assert(g_events.empty());
  printf("ok  DC offset reads as silence\n");
}

// Sample-stream clock: freeze the wall clock while the whole T3 cycle is
// delivered — models a stalled main loop draining queued DMA frames in a
// burst. Under the old millis()-at-processing-time stamping, every frame
// collapsed onto one instant and no durations survived; with the stream
// clock (frames × 20 ms) the cadence must still match.
static void test_stream_clock_survives_frozen_wall_clock() {
  reset_pipeline();
  g_clock_frozen = true;
  const int16_t LOUD = 2000;
  script_load({
      {2000, 0},
      {500, LOUD, 2}, {500, 0},
      {500, LOUD, 2}, {500, 0},
      {500, LOUD, 2},
      {1600, 0},
  });
  pump(/*tail_ms=*/0);
  g_clock_frozen = false;

  bool saw_t3 = false;
  for (const auto& e : g_events) {
    if (e.event_type == AUDIO_EVENT_T3_SMOKE_ALARM) saw_t3 = true;
  }
  assert(saw_t3 && "cadence must match on the stream clock, not millis()");
  printf("ok  stream clock survives a frozen wall clock\n");
}

// Diagnostic surface: the transition ring must expose the per-state tone
// ratio so the dashboard can show WHY a beep did or didn't gate.
static void test_transitions_expose_tone_ratio() {
  reset_pipeline();
  const int16_t LOUD = 2000;
  script_load({
      {2000, 0},
      {500, LOUD, 2},    // one in-band beep
      {1200, 0},
  });
  pump(/*tail_ms=*/0);

  audio_transition_t trans[4];
  const size_t n = audio_get_recent_transitions(trans, 4, 0);
  assert(n >= 2);
  // Newest = the OFF entry that ended the beep; its tone ratio summarizes
  // the beep and must clear the stage-1 floor.
  assert(trans[0].is_on == 0);
  assert(trans[0].tone_x100 >= AUDIO_TONE_MIN_X100);
  assert(trans[0].dur_ms >= 400 && trans[0].dur_ms <= 600);

  // Same shape with an off-band beep: the ratio must sit under the floor.
  reset_pipeline();
  script_load({
      {2000, 0},
      {500, LOUD, 16},   // 500 Hz — off-band
      {1200, 0},
  });
  pump(/*tail_ms=*/0);
  const size_t m = audio_get_recent_transitions(trans, 4, 0);
  assert(m >= 2);
  assert(trans[0].is_on == 0);
  assert(trans[0].tone_x100 < AUDIO_TONE_MIN_X100);
  printf("ok  transitions expose the tone ratio (in-band=%u, off-band gated)\n",
         (unsigned)trans[0].tone_x100);
}

int main() {
  test_t3_smoke_cadence();
  test_t4_co_cadence();
  test_knock();
  test_doorbell();
  test_glass_break();
  test_mute_stops_events();
  test_threshold_validation();
  test_t3_rejects_offband_beeps();
  test_dc_offset_reads_as_silence();
  test_stream_clock_survives_frozen_wall_clock();
  test_transitions_expose_tone_ratio();
  printf("test_audio_cadence: all tests passed\n");
  return 0;
}
