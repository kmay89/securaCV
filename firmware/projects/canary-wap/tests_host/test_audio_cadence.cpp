// Host-side tests for the PDM acoustic-event pipeline (securacv_audio.cpp).
//
// Compiles the REAL vendored module against stub Arduino/ESP-IDF headers
// (stubs/audio/) and feeds synthetic envelopes through a scripted i2s_read,
// so the full path — RMS, hysteresis, transition ring, cadence matchers,
// deferred mute — runs exactly as it does on-device, just with a fake clock
// and fake DMA.
//
// Covered:
//   1. NFPA 72 T3 smoke cadence (3×0.5 s beeps + 1.5 s pause) fires the
//      event callback with AUDIO_EVENT_T3_SMOKE_ALARM and conf ≥ 50.
//   2. A knock pattern (3 short low-band impulses) fires AUDIO_EVENT_KNOCK.
//   3. Hard mute is applied on the next process() tick, stops the stream,
//      fires the mute callback, and suppresses further events.
//   4. audio_set_thresholds() validates its arguments.
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
// The script is a list of (duration_ms, amplitude) segments. Each i2s_read
// call delivers one 20 ms frame of constant-amplitude samples from the
// script head and advances the fake clock by 20 ms; an exhausted script
// returns 0 bytes (the driver's "no DMA buffer ready" path).
struct Segment { uint32_t dur_ms; int16_t amplitude; };
static std::vector<Segment> g_script;
static size_t g_seg_idx = 0;
static uint32_t g_seg_consumed_ms = 0;
static bool g_driver_installed = false;

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
  const size_t n = size / sizeof(int16_t);  // one full frame (320 samples)
  int16_t* out = static_cast<int16_t*>(dest);
  for (size_t i = 0; i < n; i++) out[i] = amp;
  *bytes_read = n * sizeof(int16_t);
  g_seg_consumed_ms += 20;
  g_now_ms += 20;  // the frame "took" 20 ms of real time
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
  script_load({
      {2000, 0},      // settle: establish a long initial OFF state
      {500, LOUD}, {500, 0},   // beep 1 + gap
      {500, LOUD}, {500, 0},   // beep 2 + gap
      {500, LOUD},             // beep 3
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
  const int16_t LOUD = 2000;  // constant amplitude → near-zero HPF → low band
  script_load({
      {2000, 0},
      {100, LOUD}, {200, 0},   // impulse 1 + gap
      {100, LOUD}, {200, 0},   // impulse 2 + gap
      {100, LOUD},             // impulse 3
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

  // A loud cadence while muted must produce nothing.
  g_events.clear();
  script_load({
      {500, 2000}, {500, 0}, {500, 2000}, {500, 0}, {500, 2000}, {1600, 0},
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

int main() {
  test_t3_smoke_cadence();
  test_knock();
  test_mute_stops_events();
  test_threshold_validation();
  printf("test_audio_cadence: all tests passed\n");
  return 0;
}
