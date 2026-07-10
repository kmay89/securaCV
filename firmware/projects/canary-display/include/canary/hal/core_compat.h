// arduino-esp32 core 2.x ↔ 3.x compatibility shims.
//
// The display ships dual-core: the canonical PlatformIO build stays on the
// 2.0.17 line (espressif32@6.9.0 — the bench-validated release path), while
// the Arduino IDE path also supports core 3.x (the Boards Manager default,
// NimBLE 2.x, current GFX). Three core APIs changed between the majors; the
// shims below keep every caller single-spelling:
//
//   LEDC  — 2.x is channel-based (ledcSetup/ledcAttachPin/ledcWrite(ch,…)),
//           3.x is pin-based (ledcAttach/ledcWrite(pin,…)). Callers pass
//           BOTH pin and channel; each backend uses the one it needs.
//   TWDT  — 2.x: esp_task_wdt_init(seconds, panic). 3.x (IDF5): config
//           struct + reconfigure (the core arms the TWDT itself at boot).
//   NimBLE — handled locally in net/chirp_scan.cpp (scan-callback API), not
//           here: it is a library-version split, not a core-API one.
//
// ESP_ARDUINO_VERSION_MAJOR ships in both majors via Arduino.h.
#pragma once

#include <Arduino.h>
#include <esp_task_wdt.h>

namespace canary::hal {

#if ESP_ARDUINO_VERSION_MAJOR >= 3

inline void cc_ledc_setup(uint8_t pin, uint8_t /*ch*/, uint32_t freq_hz,
                          uint8_t res_bits) {
  ledcAttach(pin, freq_hz, res_bits);
}
inline void cc_ledc_write(uint8_t pin, uint8_t /*ch*/, uint32_t duty) {
  ledcWrite(pin, duty);
}
inline void cc_ledc_tone(uint8_t pin, uint8_t /*ch*/, uint32_t freq_hz) {
  ledcWriteTone(pin, freq_hz);
}

inline void cc_task_wdt_arm(uint32_t timeout_s) {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = timeout_s * 1000;
  cfg.idle_core_mask = 0;       // loop task only; idle tasks stay unwatched
  cfg.trigger_panic = true;
  // Core 3 usually arms the TWDT at boot — reconfigure it; fall back to a
  // fresh init when a sdkconfig variant shipped with it off.
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
  esp_task_wdt_add(NULL);
}

#else  // core 2.x

inline void cc_ledc_setup(uint8_t pin, uint8_t ch, uint32_t freq_hz,
                          uint8_t res_bits) {
  ledcSetup(ch, freq_hz, res_bits);
  ledcAttachPin(pin, ch);
}
inline void cc_ledc_write(uint8_t /*pin*/, uint8_t ch, uint32_t duty) {
  ledcWrite(ch, duty);
}
inline void cc_ledc_tone(uint8_t /*pin*/, uint8_t ch, uint32_t freq_hz) {
  ledcWriteTone(ch, freq_hz);
}

inline void cc_task_wdt_arm(uint32_t timeout_s) {
  esp_task_wdt_init(timeout_s, /*panic=*/true);
  esp_task_wdt_add(NULL);
}

#endif  // ESP_ARDUINO_VERSION_MAJOR

}  // namespace canary::hal
