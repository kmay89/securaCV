// src/hal/display_1in47.cpp — Nightstand flavor glass: a Waveshare ESP32
// 1.47" board (ST7789 172x320 portrait over 4-wire SPI). No touch panel;
// the WS2812 ambient LED (hal/ambient_led.cpp) and the BOOT button are the
// input surface.
//
// LVGL owns buffering/dirty-region rendering (ui/lvgl_port.cpp); this HAL
// exposes the bare panel and flushes arrive via draw16bitRGBBitmap.
// Backlight is real PWM (LEDC) — same two-profile (day/night) engine as the
// watch, which is what makes the bedside near-dark floor possible.
//
// ⚠️ ST7789 OFFSET: the 172-wide glass is a window into the controller's
//    240-wide RAM, so the panel is constructed with a 34-px column offset
//    (TFT_COL_OFFSET). Verify against your board revision.
// ⚠️ S3 board quirks: the -lcd147 S3 stick wants the HSPI port + BGR color
//    order + active-HIGH backlight (TFT_USE_HSPI / TFT_COLOR_ORDER_BGR in its
//    pins.h). These are community findings — bench-verify colors on first
//    boot; a swapped panel shows blue where it should show yellow.
#include <config.h>
#ifdef CD_FLAVOR_NIGHTSTAND

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "pins.h"
#include "canary/hal/display.h"
#include "canary/hal/core_compat.h"
#include "canary/log.h"

namespace canary::hal {

namespace {

Arduino_DataBus* s_bus = nullptr;
Arduino_GFX* s_panel = nullptr;

constexpr uint8_t LEDC_CHANNEL = 0;

// Two backlight profiles, identical to the watch: day 5 kHz / 8-bit (silent,
// coarse), night 1 kHz / 13-bit (the longer period keeps the shortest
// on-pulse above the backlight transistor's turn-on time, and 13 bits put
// ~30 distinguishable steps into the gap between "off" and duty 1/255 that
// the calibrated bedside floor needs).
constexpr uint32_t DAY_FREQ_HZ = 5000;
constexpr uint8_t  DAY_RES_BITS = 8;
constexpr uint32_t NIGHT_FREQ_HZ = 1000;
constexpr uint8_t  NIGHT_RES_BITS = 13;
constexpr uint16_t NIGHT_DUTY_MAX = 8191;

bool s_night_profile = false;

void ensure_profile(bool night) {
  if (night == s_night_profile) return;
  s_night_profile = night;
  if (night) {
    cc_ledc_reconfig(TFT_PIN_BL, LEDC_CHANNEL, NIGHT_FREQ_HZ, NIGHT_RES_BITS);
  } else {
    cc_ledc_reconfig(TFT_PIN_BL, LEDC_CHANNEL, DAY_FREQ_HZ, DAY_RES_BITS);
  }
}

}  // namespace

bool display_init() {
  // Backlight first, held dark until the first frame is flushed — no
  // white-flash at boot on a device that lives on a nightstand.
  cc_ledc_setup(TFT_PIN_BL, LEDC_CHANNEL, DAY_FREQ_HZ, DAY_RES_BITS);
  cc_ledc_write(TFT_PIN_BL, LEDC_CHANNEL, 0);

  // 4-wire SPI to the ST7789. MISO is shared with the TF slot on the C6 and
  // not broken out on the S3 (write-only panel either way); passing it is
  // harmless where -1.
  s_bus = new Arduino_ESP32SPI(TFT_PIN_DC, TFT_PIN_CS, TFT_PIN_SCK,
                               TFT_PIN_MOSI, TFT_PIN_MISO);

  // 172x320 IPS at rotation 0 with the 34-px column window offset into the
  // ST7789's 240-wide RAM. col_offset1/col_offset2 both carry the offset so
  // it lands correctly at rotation 0 and its mirror.
  s_panel = new Arduino_ST7789(
      s_bus, TFT_PIN_RST, 0 /* rotation */, true /* IPS */,
      TFT_WIDTH, TFT_HEIGHT,
      TFT_COL_OFFSET, TFT_ROW_OFFSET, TFT_COL_OFFSET, TFT_ROW_OFFSET);

  if (!s_panel->begin(TFT_SPI_HZ)) {
    log_line("DISP", "ST7789 init FAILED — running headless.");
    return false;
  }
  s_panel->fillScreen(0x0000);

  log_line("DISP", "ST7789 172x320 up (LVGL render, PWM backlight).");
  return true;
}

Arduino_GFX* gfx() { return s_panel; }

void display_flush() { /* LVGL flushes dirty regions itself */ }

void backlight_set(uint8_t level) {
  ensure_profile(false);
#if TFT_BL_ACTIVE_HIGH
  cc_ledc_write(TFT_PIN_BL, LEDC_CHANNEL, level);
#else
  cc_ledc_write(TFT_PIN_BL, LEDC_CHANNEL, 255 - level);
#endif
}

void backlight_night_set(uint16_t duty13) {
  if (duty13 > NIGHT_DUTY_MAX) duty13 = NIGHT_DUTY_MAX;
  ensure_profile(true);
#if TFT_BL_ACTIVE_HIGH
  cc_ledc_write(TFT_PIN_BL, LEDC_CHANNEL, duty13);
#else
  cc_ledc_write(TFT_PIN_BL, LEDC_CHANNEL, NIGHT_DUTY_MAX - duty13);
#endif
}

// No touch panel on the 1.47" boards. The signature stays (main.cpp polls it
// every loop pass); it simply never reports a touch. Attention is the BOOT
// button + the ambient LED beacon.
TouchSample touch_read() { return TouchSample{}; }

}  // namespace canary::hal

#endif  // CD_FLAVOR_NIGHTSTAND
