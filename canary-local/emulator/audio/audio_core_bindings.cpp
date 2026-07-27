// Browser ABI for the real Canary WAP acoustic detector.
//
// Emscripten links this boundary with the firmware's securacv_audio.cpp —
// the SAME cadence pipeline the WAP runs on-device: DC-removed RMS, the
// 3.4 kHz alarm-band tone gate, envelope hysteresis, and the NFPA-72 T3 /
// UL-2034 T4 templates. JavaScript may stage microphone PCM (the sensor
// boundary — exactly where the PDM mic sits on hardware), but it cannot
// reproduce or override any decision the firmware makes after those samples
// arrive: whether a beep is alarm-band-dominant, whether the cadence fits,
// whether an event fires. That verdict is the firmware's, in wasm.
//
// The platform edges securacv_audio.cpp expects — millis(), a no-op
// Preferences, and the legacy I2S driver — are stubbed here. i2s_read()
// drains a single frame the JS side wrote through audio_emu_frame_ptr(),
// so the real audio_process() loop runs unmodified: this is the same feed
// seam the host test (test_audio_cadence.cpp) drives with synthetic audio,
// pointed at a live microphone instead.

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "securacv_audio.h"
#include "log_level.h"
#include "driver/i2s.h"   // esp_err_t + the legacy I2S types/functions we stub

// ── platform edges the detector links against ──────────────────────────────

// millis() tracks the sample-stream clock: it advances one frame_ms per frame
// the bench feeds (see audio_emu_process_frame). The cadence math already runs
// off the internal stream clock; this only backs the event-throttle window.
static uint32_t g_now_ms = 0;
// C++ linkage to match the stub Arduino.h declaration the module compiles against.
unsigned long millis() { return g_now_ms; }

// The detector logs I2S bring-up problems; on the bench there is no I2S, so
// these resolve to nothing.
void health_log(LogLevel, LogCategory, const char*) {}
void log_health(LogLevel, LogCategory, const char*, const char*) {}

// One-frame "DMA ring": the JS side writes AUDIO_FRAME_SAMPLES int16 into
// g_frame, then calls process; i2s_read() hands that frame over exactly once,
// then reports the legacy "no data ready" signal so audio_process() drains a
// single 20 ms frame per call.
static int16_t g_frame[AUDIO_FRAME_SAMPLES];
static bool    g_frame_ready = false;

extern "C" {
esp_err_t i2s_driver_install(i2s_port_t, const i2s_config_t*, int, void*) { return ESP_OK; }
esp_err_t i2s_driver_uninstall(i2s_port_t) { return ESP_OK; }
esp_err_t i2s_set_pin(i2s_port_t, const i2s_pin_config_t*) { return ESP_OK; }
esp_err_t i2s_zero_dma_buffer(i2s_port_t) { return ESP_OK; }
esp_err_t i2s_read(i2s_port_t, void* dest, size_t size,
                   size_t* bytes_read, int /*timeout*/) {
  if (!g_frame_ready) { *bytes_read = 0; return ESP_ERR_TIMEOUT; }
  size_t want = size / sizeof(int16_t);
  if (want > AUDIO_FRAME_SAMPLES) want = AUDIO_FRAME_SAMPLES;
  memcpy(dest, g_frame, want * sizeof(int16_t));
  *bytes_read = want * sizeof(int16_t);
  g_frame_ready = false;   // consumed — next i2s_read this call sees the ring empty
  return ESP_OK;
}
}  // extern "C"

// ── event capture ──────────────────────────────────────────────────────────
namespace {
uint8_t  g_last_event = AUDIO_EVENT_NONE;   // fired since the last frame
uint8_t  g_last_conf  = 0;
uint16_t g_last_cycles = 0;
char     g_json[1200];

void on_event(const audio_event_t* e) {
  g_last_event  = e->event_type;
  g_last_conf   = e->confidence;
  g_last_cycles = e->cycle_count;
}
}  // namespace

extern "C" {

// The honest contract: the exact thresholds and cadence targets the firmware
// compiled, so the bench's meter ticks and cadence-trace tolerances are the
// device's, not the page's guesses.
EMSCRIPTEN_KEEPALIVE const char* audio_emu_contract_json() {
  audio_config_t cfg = AUDIO_CONFIG_DEFAULT;
  audio_get_config(&cfg);
  snprintf(g_json, sizeof(g_json),
    "{\"schema\":\"securacv.canary-wap.audio-core/v1\","
    "\"sample_rate_hz\":%u,\"frame_ms\":%u,\"frame_samples\":%d,"
    "\"tone_fc_hz\":%d,\"tone_min_x100\":%d,\"tone_min_relaxed\":%d,"
    "\"rms_on\":%u,\"rms_off\":%u,"
    "\"t3\":{\"beep_ms\":500,\"gap_ms\":500,\"pause_ms\":1500,\"beeps\":3,\"beep_tol\":200,\"pause_tol\":500},"
    "\"t4\":{\"beep_ms\":100,\"gap_ms\":100,\"pause_ms\":5000,\"beeps\":4},"
    "\"events\":{\"none\":0,\"t3_smoke\":1,\"t4_co\":2,\"knock\":3,\"doorbell\":4,\"glass\":5}}",
    (unsigned)cfg.sample_rate_hz, (unsigned)cfg.frame_ms, AUDIO_FRAME_SAMPLES,
    AUDIO_TONE_FC_HZ, AUDIO_TONE_MIN_X100, AUDIO_TONE_MIN_RELAXED,
    (unsigned)cfg.rms_on_threshold, (unsigned)cfg.rms_off_threshold);
  return g_json;
}

// (Re)start the detector from a clean slate. Idempotent.
EMSCRIPTEN_KEEPALIVE void audio_emu_reset() {
  audio_config_t cfg = AUDIO_CONFIG_DEFAULT;
  audio_deinit();
  g_now_ms = 0;
  g_frame_ready = false;
  g_last_event = AUDIO_EVENT_NONE;
  g_last_conf = 0;
  g_last_cycles = 0;
  audio_init(&cfg);
  audio_set_event_callback(on_event);
  audio_start();
}

EMSCRIPTEN_KEEPALIVE int   audio_emu_frame_samples() { return AUDIO_FRAME_SAMPLES; }
EMSCRIPTEN_KEEPALIVE int16_t* audio_emu_frame_ptr()  { return g_frame; }

// Envelope-sensitivity tuning (a bench "make it more sensitive" slider): the
// same runtime path the Device tab exposes. Validated inside the firmware.
EMSCRIPTEN_KEEPALIVE int audio_emu_set_thresholds(int on, int off) {
  if (on < 0) on = 0;
  if (on > 65535) on = 65535;
  if (off < 0) off = 0;
  if (off > 65535) off = 65535;
  return audio_set_thresholds((uint16_t)on, (uint16_t)off) ? 1 : 0;
}

// Feed the one frame JS just wrote into audio_emu_frame_ptr(), advance the
// stream clock one frame, run the real pipeline, and report what it saw.
// Returns a JSON snapshot: live level, whether we're currently in an ON
// (beep) state, any event that fired this frame, the running T3/T4 tallies,
// and the recent on/off transitions (the "show me the cadence" trace).
EMSCRIPTEN_KEEPALIVE const char* audio_emu_process_frame() {
  g_last_event = AUDIO_EVENT_NONE;
  g_frame_ready = true;
  g_now_ms += AUDIO_FRAME_MS;
  audio_process();

  uint16_t level = 0; uint32_t level_age = 0;
  audio_get_live_level(&level, &level_age);

  audio_stats_t st{};
  audio_get_stats(&st);

  audio_transition_t tr[8];
  size_t ntr = audio_get_recent_transitions(tr, 8, 0);

  int n = snprintf(g_json, sizeof(g_json),
    "{\"level\":%u,\"level_age_ms\":%lu,"
    "\"event\":%u,\"event_name\":\"%s\",\"confidence\":%u,\"cycle_count\":%u,"
    "\"t3_total\":%lu,\"t4_total\":%lu,\"zero_streak\":%lu,\"transitions\":[",
    (unsigned)level, (unsigned long)level_age,
    (unsigned)g_last_event, audio_event_name(g_last_event),
    (unsigned)g_last_conf, (unsigned)g_last_cycles,
    (unsigned long)st.t3_detected, (unsigned long)st.t4_detected,
    (unsigned long)st.zero_rms_streak);

  for (size_t i = 0; i < ntr && n > 0 && (size_t)n < sizeof(g_json) - 64; i++) {
    n += snprintf(g_json + n, sizeof(g_json) - n,
      "%s{\"is_on\":%d,\"tone_x100\":%u,\"dur_ms\":%lu,\"age_ms\":%lu}",
      i ? "," : "", tr[i].is_on ? 1 : 0, (unsigned)tr[i].tone_x100,
      (unsigned long)tr[i].dur_ms, (unsigned long)tr[i].age_ms);
  }
  snprintf(g_json + n, sizeof(g_json) - n, "]}");
  return g_json;
}

}  // extern "C"
