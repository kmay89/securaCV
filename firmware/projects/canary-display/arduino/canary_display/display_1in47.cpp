// src/hal/display_1in47.cpp — Nightstand flavor glass: a Waveshare ESP32
// portrait ST7789 board over 4-wire SPI (the 1.47" 172x320 boards, and the
// Touch-1.69's 240x280 — geometry and offsets come from pins.h). On the
// 1.47" boards there is no touch panel; the WS2812 ambient LED
// (hal/ambient_led.cpp) and the BOOT button are the input surface. On a
// board that carries a CST816-family touch layer (FEATURE_TOUCH +
// HAS_TOUCH, e.g. the Touch-1.69's CST816T), touch_read() is real and
// main.cpp's standard tap/long-press ladder works by finger — the
// LED-less glass is both the ambient surface and the input.
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
#include "flavor_config.h"
#ifdef CD_FLAVOR_NIGHTSTAND

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "pins.h"  // MUST precede the touch gate: HAS_TOUCH lives here, not
                   // in config.h (same contract settings_ui.cpp documents
                   // for HAS_ISOLATED_IO — gate after the board header).

// The CST816 path costs nothing on the touchless 1.47" boards: it is
// double-gated (flavor turns it on AND the board carries the panel).
#if defined(FEATURE_TOUCH) && FEATURE_TOUCH && \
    defined(HAS_TOUCH) && HAS_TOUCH
#define CD_1IN47_TOUCH 1
#include <Wire.h>
#endif

#include "display.h"
#include "core_compat.h"
#include "log.h"

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

#ifdef CD_1IN47_TOUCH
// CST816-family registers (same map the watch's CST816S uses; the
// Touch-1.69's CST816T answers identically for gesture/fingers/coords).
constexpr uint8_t CST_REG_GESTURE = 0x01;  // gesture id
constexpr uint8_t CST_REG_FINGERS = 0x02;  // finger count
constexpr uint8_t CST_REG_XPOS_H  = 0x03;  // [3:0] = x[11:8]

bool cst816_read(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_I2C_ADDR, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}
#endif  // CD_1IN47_TOUCH

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

#ifdef CD_1IN47_TOUCH
  // Touch: shared I2C bus (CST816T + QMI8658 + PCF85063 live on it). The
  // CST816 wants its reset released before it will ACK; some revisions
  // then nap until touched — a quiet probe is normal, not fatal.
#if TOUCH_PIN_RST >= 0
  pinMode(TOUCH_PIN_RST, OUTPUT);
  digitalWrite(TOUCH_PIN_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_PIN_RST, HIGH);
  delay(50);
#endif
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, I2C_FREQ_FAST);
#if TOUCH_PIN_INT >= 0
  pinMode(TOUCH_PIN_INT, INPUT);
#endif
  uint8_t probe = 0;
  if (!cst816_read(CST_REG_FINGERS, &probe, 1)) {
    log_line("DISP", "CST816 quiet at boot (naps until touched) — touch armed.");
  }
#endif  // CD_1IN47_TOUCH

  log_line("DISP", "ST7789 portrait up (LVGL render, PWM backlight).");
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

#ifdef CD_1IN47_TOUCH
TouchSample touch_read() {
  TouchSample s;
  uint8_t buf[6];
  // gesture, fingers, xH, xL, yH, yL — one burst from 0x01.
  if (!cst816_read(CST_REG_GESTURE, buf, sizeof(buf))) return s;
  if (buf[1] == 0) return s;
  s.touched = true;
  s.x = (int16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
  s.y = (int16_t)(((buf[4] & 0x0F) << 8) | buf[5]);
  return s;
}
#else
// No touch panel on the 1.47" boards. The signature stays (main.cpp polls it
// every loop pass); it simply never reports a touch. Attention is the BOOT
// button + the ambient LED beacon.
TouchSample touch_read() { return TouchSample{}; }
#endif  // CD_1IN47_TOUCH

}  // namespace canary::hal

#endif  // CD_FLAVOR_NIGHTSTAND
