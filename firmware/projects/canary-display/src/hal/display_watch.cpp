// src/hal/display_watch.cpp — Watch flavor glass: Seeed Round Display for
// XIAO (GC9A01 240x240 over SPI + CST816S touch over I2C).
//
// Rendering goes through an offscreen Arduino_Canvas (240*240*2 = 112.5 KB,
// PSRAM-backed on the XIAO ESP32-S3) flushed once per UI frame — a round
// glance face redrawn in place would shimmer otherwise. Backlight is real
// PWM (LEDC), which is what makes the bedside near-dark floor possible.
#include <config.h>
#ifdef CD_FLAVOR_WATCH

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

#include "pins.h"
#include "canary/hal/display.h"
#include "canary/log.h"

namespace canary::hal {

namespace {

Arduino_DataBus* s_bus = nullptr;
Arduino_GFX* s_panel = nullptr;
Arduino_Canvas* s_canvas = nullptr;

constexpr uint8_t LEDC_CHANNEL = 0;
constexpr uint32_t LEDC_FREQ_HZ = 5000;
constexpr uint8_t LEDC_RES_BITS = 8;

// CST816S registers (vendor datasheet + the usual community drivers).
constexpr uint8_t CST_REG_GESTURE = 0x01;  // gesture id
constexpr uint8_t CST_REG_FINGERS = 0x02;  // finger count
constexpr uint8_t CST_REG_XPOS_H  = 0x03;  // [3:0] = x[11:8]

bool cst816s_read(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_I2C_ADDR, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

}  // namespace

bool display_init() {
  // Backlight first, held dark until the first frame is flushed — no
  // white-flash at boot on a device that may live in a bedroom.
  ledcSetup(LEDC_CHANNEL, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttachPin(TFT_PIN_BL, LEDC_CHANNEL);
  ledcWrite(LEDC_CHANNEL, 0);

  s_bus = new Arduino_ESP32SPI(TFT_PIN_DC, TFT_PIN_CS, TFT_PIN_SCK,
                               TFT_PIN_MOSI, TFT_PIN_MISO);
  s_panel = new Arduino_GC9A01(s_bus, GFX_NOT_DEFINED /* no reset pin */,
                               0 /* rotation */, true /* IPS */);
  s_canvas = new Arduino_Canvas(TFT_WIDTH, TFT_HEIGHT, s_panel);
  if (!s_canvas->begin(TFT_SPI_HZ)) {
    log_line("DISP", "GC9A01/canvas init FAILED — running headless.");
    return false;
  }
  s_canvas->fillScreen(0x0000);
  s_canvas->flush();

  // Touch: shared I2C bus (CST816S + PCF8563 RTC live on it).
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, I2C_FREQ_FAST);
  pinMode(TOUCH_PIN_INT, INPUT);
  uint8_t probe = 0;
  if (!cst816s_read(CST_REG_FINGERS, &probe, 1)) {
    // Some CST816S revisions nap until touched — a failed probe here is
    // normal, not fatal. Report and move on.
    log_line("DISP", "CST816S quiet at boot (naps until touched) — touch armed.");
  }

  log_line("DISP", "GC9A01 240x240 up (canvas render, PWM backlight).");
  return true;
}

Arduino_GFX* gfx() { return s_canvas; }

void display_flush() {
  if (s_canvas) s_canvas->flush();
}

void backlight_set(uint8_t level) {
#if TFT_BL_ACTIVE_HIGH
  ledcWrite(LEDC_CHANNEL, level);
#else
  ledcWrite(LEDC_CHANNEL, 255 - level);
#endif
}

TouchSample touch_read() {
  TouchSample s;
  uint8_t buf[6];
  // gesture, fingers, xH, xL, yH, yL — one burst from 0x01.
  if (!cst816s_read(CST_REG_GESTURE, buf, sizeof(buf))) return s;
  if (buf[1] == 0) return s;
  s.touched = true;
  s.x = (int16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
  s.y = (int16_t)(((buf[4] & 0x0F) << 8) | buf[5]);
  return s;
}

}  // namespace canary::hal

#endif  // CD_FLAVOR_WATCH
