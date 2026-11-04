/*
 * Host test — when the canary's sensing snapshot holds a measured RSSI
 * (sweep F411).
 *
 * firmware/canary/src/main.cpp's mqtt_publish_sensing_update() writes the
 * sensing body's rssi_dbm only when sensing_rssi_measured() says the last
 * CSI window's mean was taken over a frame; a body without the key leaves
 * Home Assistant's Sensing RSSI where it was (tests/ha/
 * test_canary_sensing_rssi.py drives that on a real core). main.cpp is not
 * host-compiled, so this suite runs the predicate it calls on snapshots the
 * real pipeline produced: the canary's csi:: adapter over the canonical
 * csi_hal.cpp + csi_features.cpp, feeding securacv_sensing.cpp's
 * aggregator, against the stubbed esp_wifi CSI driver test_csi_hal_adapter
 * uses. It holds:
 *
 *   - before the first window there is no reading;
 *   - a window that heard nothing still closes and is counted
 *     (windows_seen moves) with a zero mean, and is not a reading — the
 *     state the first F411 guard (windows_seen > 0) published as 0 dBm;
 *   - a window of frames at -61 dBm is a reading of -61;
 *   - a window of frames all below the -85 dBm noise floor is not a reading
 *     (the HAL drops them before the window, so its mean is zero too);
 *   - frames the HAL handed on but the feature layer refused leave
 *     frames_in_window above zero and the RSSI count at zero: not a
 *     reading (fed as the vector finalize() emits for that window, since a
 *     failed amplitude-history allocation cannot be forced on the host).
 *
 * Host-only. The device build is firmware.yml's PlatformIO leg; main.cpp's
 * call of the predicate is pinned by
 * scripts/tests/test_ha_discovery_canary_sensing_render.py.
 */

#include "securacv_csi.h"
#include "csi_hal.h"
#include "log_level.h"
#include "securacv_sensing.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_timer.h>

/* ── Fake clock + Arduino glue ─────────────────────────────────────────── */
uint32_t   g_host_millis = 0;
HostSerial Serial;
int64_t esp_timer_get_time(void) { return (int64_t)g_host_millis * 1000; }

/* ── Stubbed esp_wifi CSI driver (no STA association: every frame passes
 *    the transmitter filter, as in test_csi_hal_adapter) ───────────────── */
static wifi_csi_cb_t g_cb = nullptr;
esp_err_t esp_wifi_set_csi_config(const wifi_csi_config_t*) { return ESP_OK; }
esp_err_t esp_wifi_set_csi_rx_cb(wifi_csi_cb_t cb, void*) { g_cb = cb; return ESP_OK; }
esp_err_t esp_wifi_set_csi(bool) { return ESP_OK; }
esp_err_t esp_wifi_set_channel(uint8_t, wifi_second_chan_t) { return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t*) { return ESP_ERR_WIFI_NOT_CONNECT; }
void log_health(LogLevel, LogCategory, const char*, const char*) {}

/* A 64-tone non-HT frame, as test_csi_hal_adapter injects. */
static void inject_frame(int8_t rssi) {
  int8_t buf[128];
  for (int i = 0; i < 128; i++) buf[i] = (int8_t)((i * 7) % 23 - 11);
  wifi_csi_info_t info = {};
  info.rx_ctrl.rssi = rssi; info.rx_ctrl.channel = 6; info.rx_ctrl.cwb = 0;
  info.buf = buf; info.len = 128; info.first_word_invalid = false;
  assert(g_cb != nullptr);
  g_cb(nullptr, &info);
}

/* Ten frames at the 20 Hz spacing, then the window closes (t += 1100 ms). */
static void window_of_frames(int8_t rssi) {
  for (int i = 0; i < 10; i++) {
    g_host_millis += 50;
    inject_frame(rssi);
    assert(csi::process() == 0);
  }
  g_host_millis += 600;
  assert(csi::process() == 1);
}

/* A window in which the radio heard nothing: it still closes. */
static void empty_window() {
  g_host_millis += 1100;
  assert(csi::process() == 1);
}

static sensing_state_t snap() {
  sensing_state_t s;
  sensing_snapshot(&s);
  printf("  windows_seen=%u frames_in_window=%u rssi_frames=%u rssi_dbm=%d measured=%d\n",
         (unsigned)s.windows_seen, (unsigned)s.frames_in_window, (unsigned)s.rssi_frames,
         (int)s.rssi_dbm, (int)sensing_rssi_measured(&s));
  return s;
}

int main() {
  sensing_init();
  csi_config_t cfg = CSI_CONFIG_DEFAULT;
  cfg.channel = 6;
  assert(cfg.rssi_floor_dbm == -85);   /* the floor the -90 window sits under */
  assert(csi::init(cfg));
  csi::set_features_callback([](const csi_features_t* f) { sensing_feed_csi(f); });
  assert(csi::start());

  sensing_state_t s = snap();
  assert(s.windows_seen == 0 && !sensing_rssi_measured(&s));
  assert(!sensing_rssi_measured(nullptr));
  puts("PASS before the first window there is no reading");

  empty_window();
  s = snap();
  assert(s.windows_seen == 1);          /* the window was seen... */
  assert(s.rssi_dbm == 0);              /* ...with the zero no radio measured */
  assert(s.frames_in_window == 0 && s.rssi_frames == 0);
  assert(!sensing_rssi_measured(&s));
  puts("PASS a window that heard nothing is seen but is not a reading");

  window_of_frames(-61);
  s = snap();
  assert(s.windows_seen == 2 && s.frames_in_window == 10 && s.rssi_frames == 10);
  assert(sensing_rssi_measured(&s) && s.rssi_dbm == -61);
  puts("PASS a window heard at -61 dBm is a reading of -61");

  empty_window();
  s = snap();
  assert(s.windows_seen == 3 && s.rssi_dbm == 0 && !sensing_rssi_measured(&s));
  puts("PASS the next quiet window is not a reading (the body leaves the key out)");

  window_of_frames(-90);
  s = snap();
  assert(s.windows_seen == 4 && s.frames_in_window == 0 && s.rssi_dbm == 0);
  assert(!sensing_rssi_measured(&s));
  csi_stats_t st = {};
  assert(csi::get_stats(&st) && st.frames_dropped_rssi == 10);
  puts("PASS a window of frames below the noise floor is not a reading");

  window_of_frames(-48);
  s = snap();
  assert(sensing_rssi_measured(&s) && s.rssi_dbm == -48);
  puts("PASS a reading after the quiet windows reads again");

  /* The vector finalize() emits for a window whose frames the feature layer
   * refused (its amplitude history could not be allocated): the HAL counted
   * them, the RSSI mean and the feature layer's frame count are zero. */
  csi_features_t refused;
  memset(&refused, 0, sizeof(refused));
  refused.frames_in_window = 10;
  sensing_feed_csi(&refused);
  s = snap();
  assert(s.windows_seen == 6 && s.frames_in_window == 10 && s.rssi_dbm == 0);
  assert(!sensing_rssi_measured(&s));
  puts("PASS frames the feature layer refused are not a reading");

  csi::deinit();
  puts("test_canary_sensing_rssi: ALL PASSED");
  return 0;
}
