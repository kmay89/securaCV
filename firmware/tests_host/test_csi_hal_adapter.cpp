/*
 * Host link test — the canary product's csi:: adapter over the ONE CSI HAL.
 *
 * firmware/canary/lib/securacv_csi/src/securacv_csi.cpp used to be a second
 * copy of csi_hal.cpp + csi_features.cpp, kept equal by hand (roadmap 22).
 * It is now a thin csi:: spelling of csi_hal::, and the canary PlatformIO
 * envs compile the canonical two files beside it. This test links exactly
 * that trio on the host, against a stubbed esp_wifi CSI driver
 * (stubs/csi_hal/), and pushes synthetic frames through the callback the
 * HAL registers. It proves, with a real linker rather than a grep:
 *
 *   - the adapter resolves to the canonical symbols (a HAL body that grew
 *     back under the same external name would fail to link as a duplicate);
 *   - firmware/canary/include/health_log.h answers csi_hal.cpp's
 *     `__has_include("health_log.h")` probe, so HAL diagnostics reach the
 *     canary health log (log_health) instead of only Serial;
 *   - the driver callback is registered from ONE site, once per start();
 *   - the two behaviors ported from the former canary copy hold: v[25]
 *     (dropped-frame estimate) is filled from the configured rate, and
 *     the silence watchdog fires on the csi::process() path — the former
 *     csi_hal:: shim only checked it in a process() nobody called.
 *
 * Host-only. The device build is firmware.yml's PlatformIO leg;
 * firmware/scripts/check_csi_sync.sh is the shape gate on the adapter.
 */

#include "securacv_csi.h"
#include "csi_hal.h"
#include "csi_module.h"   /* one csi_features_t: this TU sees both headers */
#include "log_level.h"

#include <assert.h>
#include <stdio.h>
#include <type_traits>

#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_timer.h>

/* ── Fake clock + Arduino glue ─────────────────────────────────────────── */
uint32_t   g_host_millis = 0;
HostSerial Serial;
int64_t esp_timer_get_time(void) { return (int64_t)g_host_millis * 1000; }

/* ── Stubbed esp_wifi CSI driver ───────────────────────────────────────── */
static wifi_csi_cb_t g_cb = nullptr;
static int  g_registrations = 0;   /* non-null esp_wifi_set_csi_rx_cb calls */
static int  g_toggles = 0;         /* esp_wifi_set_csi transitions */
static bool g_csi_enabled = false;
esp_err_t esp_wifi_set_csi_config(const wifi_csi_config_t*) { return ESP_OK; }
esp_err_t esp_wifi_set_csi_rx_cb(wifi_csi_cb_t cb, void*) {
  g_cb = cb;
  if (cb) g_registrations++;
  return ESP_OK;
}
esp_err_t esp_wifi_set_csi(bool en) {
  if (en != g_csi_enabled) g_toggles++;
  g_csi_enabled = en;
  return ESP_OK;
}
esp_err_t esp_wifi_set_channel(uint8_t, wifi_second_chan_t) { return ESP_OK; }
/* No STA association in this harness: the transmitter filter stays disarmed
 * and every frame passes, which is what the counts below assume. The filter
 * itself is exercised by firmware/common/csi/csi_hal_transmitter_filter_test.cpp. */
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t*) { return ESP_ERR_WIFI_NOT_CONNECT; }

/* ── The canary health log the bridge routes into ──────────────────────── */
static int g_health_calls = 0;
static int g_notice_calls = 0;
void log_health(LogLevel level, LogCategory cat, const char* message, const char* detail) {
  g_health_calls++;
  if (level == LOG_LEVEL_NOTICE) g_notice_calls++;
  assert(cat == LOG_CAT_SENSOR);
  printf("  health[%d]: %s%s%s\n", (int)level, message, detail ? " | " : "", detail ? detail : "");
}

/* ── Feature sink + frame source ───────────────────────────────────────── */
static int g_windows = 0;
static csi_features_t g_last;
static void on_features(const csi_features_t* f) { g_windows++; g_last = *f; }

/* A 64-tone non-HT frame (128 bytes) with a deterministic non-zero pattern,
 * enough for csi_lltf_select() to canonicalize to the 52 L-LTF tones. */
static void inject_frame(int8_t rssi) {
  int8_t buf[128];
  for (int i = 0; i < 128; i++) buf[i] = (int8_t)((i * 7) % 23 - 11);
  wifi_csi_info_t info = {};
  info.rx_ctrl.rssi = rssi; info.rx_ctrl.channel = 6; info.rx_ctrl.cwb = 0;
  info.buf = buf; info.len = 128; info.first_word_invalid = false;
  assert(g_cb != nullptr);
  g_cb(nullptr, &info);
}

int main() {
  static_assert(sizeof(csi_features_t) == 36, "one csi_types.h struct on both sides");
  static_assert(std::is_same<csi::FeaturesCallback, csi_hal::FeaturesCallback>::value,
                "csi:: and csi_hal:: share the callback type");

  /* init: the HAL's own line arrives through the health_log.h bridge, and
   * the adapter adds the advisory-channel notice the former copy logged. */
  csi_config_t cfg = CSI_CONFIG_DEFAULT;
  cfg.channel = 6;
  assert(csi::init(cfg));
  assert(g_notice_calls == 1);
  assert(g_health_calls >= 2);
  assert(!csi::is_running() && !csi_hal::is_running() && !csi_is_running());
  puts("PASS init routes through the health_log.h bridge + advisory notice");

  /* start: exactly one driver registration, from csi_hal.cpp. */
  csi::set_features_callback(on_features);
  assert(csi::start());
  assert(g_registrations == 1);
  assert(csi::is_running() && csi_hal::is_running() && csi_is_running());
  puts("PASS start registers the driver callback once");

  /* ten frames at the 20 Hz rate-limit spacing, then the window closes. */
  for (int i = 0; i < 10; i++) {
    g_host_millis += 50;
    inject_frame(-50);
    assert(csi::process() == 0);
  }
  g_host_millis += 600;                     /* t = 1100 ms */
  assert(csi::process() == 1);
  assert(g_windows == 1);
  printf("  window: frames=%d dropped=%d ch=%d bw=%d caps=0x%02x\n",
         g_last.v[24], g_last.v[25], g_last.v[26], g_last.v[27], g_last.caps_observed);
  assert(g_last.v[24] == 10);               /* frames_received */
  assert(g_last.v[25] == 10);               /* dropped_estimate = 20 Hz x 1 s - 10 (ported) */
  assert(g_last.v[26] == 6);                /* observed channel */
  assert(g_last.caps_observed & CSI_CAP_HT20);
  assert(csi_hal::get_observed_channel() == 6 && csi_hal::is_channel_in_sync());
  puts("PASS one window through the adapter fills v[24..26] incl. the ported v[25]");

  csi_stats_t st = {};
  assert(csi::get_stats(&st));
  assert(st.frames_received == 10 && st.windows_emitted == 1 && st.frames_dropped_rate == 0);
  csi_stats_t st_c = {};
  assert(csi_get_stats(&st_c) && st_c.frames_received == st.frames_received);
  assert(csi::get_caps() == csi_hal::get_caps());
  assert(csi::get_caps() & CSI_CAP_HT40);   /* CONFIG_IDF_TARGET_ESP32S3 path (ported) */
  puts("PASS stats/caps agree across csi::, csi_hal:: and the C API");

  /* watchdog: live on this path. 6 s of silence -> one off/on toggle. */
  const int toggles_before = g_toggles;
  g_host_millis += 6000;
  csi::process();
  assert(g_toggles == toggles_before + 2);
  assert(csi_hal::get_watchdog_recovery_count() == 1);
  puts("PASS silence watchdog fires from csi::process()");

  /* stop unregisters (no new registration); restart registers again from
   * the same single site. */
  csi::stop();
  assert(!csi::is_running() && g_cb == nullptr && g_registrations == 1);
  assert(csi::start() && g_registrations == 2);
  assert(csi::conformance_check_no_mac_in_buffers());
  csi::deinit();
  assert(!csi::is_running());
  puts("PASS stop/start/deinit lifecycle");

  puts("test_csi_hal_adapter: ALL PASSED");
  return 0;
}
