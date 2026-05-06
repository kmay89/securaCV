/*
 * csi_minimal — the smallest useful WiFi CSI sketch on ESP32-S3.
 *
 * What it does:
 *   1. Brings up Wi-Fi in STA mode and joins your home network.
 *   2. Initializes the SecuraCV CSI HAL (firmware/common/csi).
 *   3. Prints one feature window per second to Serial.
 *
 * What you need:
 *   - An ESP32-S3 / ESP32 / ESP32-C3 / ESP32-C6 board.
 *   - The SecuraCV CSI library installed:
 *       Arduino IDE: copy or symlink firmware/common/csi/ into
 *                    Documents/Arduino/libraries/csi/
 *       arduino-cli: pass --libraries firmware/common
 *       PlatformIO:  add lib_deps = SecuraCV CSI (or the local path)
 *
 * Output (one line per second):
 *
 *   t=4   motion=12   breathing= 3   rssi=-42dBm   frames=18/20
 *
 * That's it. No dashboard, no stream, no events — just the raw 32-dim
 * feature vector reduced to two numbers anyone can read. Hack from here.
 *
 * License: MIT (matches the library).
 */

#include <Arduino.h>
#include <WiFi.h>

#include <csi_hal.h>
#include <csi_features.h>
#include <csi_types.h>

/* Replace these with your home WiFi or use ESP_SmartConfig / WiFiManager. */
#ifndef MY_SSID
#define MY_SSID     "your-ssid"
#endif
#ifndef MY_PASSWORD
#define MY_PASSWORD "your-password"
#endif

/* The CSI features callback fires once per 1-second window. */
static uint32_t g_window_count = 0;

static uint8_t reduce_band(const int8_t* v, int from, int to) {
  int32_t s = 0;
  for (int i = from; i < to; ++i) {
    int8_t b = v[i];
    s += (b < 0) ? -(int32_t)b : (int32_t)b;
  }
  const int n = to - from;
  if (n <= 0) return 0;
  int32_t avg = s / n;
  if (avg > 127) avg = 127;
  return (uint8_t)avg;
}

static void on_csi_window(const csi_features_t* f, void* /*user*/) {
  g_window_count++;

  /* See firmware/common/csi/csi_features.h for the full 32-dim layout.
   *   v[0..7]   amplitude variance
   *   v[8..11]  phase-Doppler (motion)
   *   v[12..19] breathing FFT
   *   v[20..23] RSSI stats: mean, std, max, min  */
  const uint8_t motion    = reduce_band(f->v, 8, 12);
  const uint8_t breathing = reduce_band(f->v, 12, 20);
  const int8_t  rssi_mean = f->v[20];

  Serial.printf("t=%-4lu motion=%-3u breathing=%-3u rssi=%ddBm frames=%u\n",
                (unsigned long)g_window_count,
                (unsigned)motion,
                (unsigned)breathing,
                (int)rssi_mean,
                (unsigned)f->frames_in_window);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[csi_minimal] starting...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(MY_SSID, MY_PASSWORD);
  Serial.print("[csi_minimal] joining WiFi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[csi_minimal] WiFi did not connect; CSI will defer until it does.");
  } else {
    Serial.print("[csi_minimal] joined: ");
    Serial.println(WiFi.localIP());
  }

  csi_hal::Config cfg = csi_hal::Config::defaults();
  cfg.bandwidth_mhz     = 20;
  cfg.max_frame_rate_hz = 20;
  if (!csi_hal::init(cfg)) {
    Serial.println("[csi_minimal] csi_hal init failed; check ESP-IDF CSI build flags.");
    return;
  }
  csi_set_features_callback(on_csi_window, nullptr);
  if (!csi_hal::start()) {
    Serial.println("[csi_minimal] csi_hal start returned false (deferred — fine).");
  }

  Serial.println("[csi_minimal] running. Wave your hand near the device — the motion column should rise.");
}

void loop() {
  csi_hal::process();
  delay(10);
}
