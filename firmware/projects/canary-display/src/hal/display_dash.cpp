// src/hal/display_dash.cpp — Dash flavor glass: Waveshare
// ESP32-S3-Touch-LCD-4.3 (800x480 over the S3's parallel RGB565 LCD
// peripheral + GT911 touch + CH422G IO expander).
//
// The RGB peripheral scans a PSRAM framebuffer continuously, so the UI
// draws straight into it (no flush). The CH422G owns touch-reset and the
// backlight-enable line — and backlight through the expander is ON/OFF
// only, which is why FEATURE_BACKLIGHT_DIM is 0 on this flavor and night
// mode is dark-theme + scheduled backlight-off (see the UX doc).
#include <config.h>
#ifdef CD_FLAVOR_DASH

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

#include "pins.h"
#include "canary/hal/display.h"
#include "canary/log.h"

namespace canary::hal {

namespace {

Arduino_ESP32RGBPanel* s_rgbpanel = nullptr;
Arduino_RGB_Display* s_display = nullptr;
uint8_t s_exio_state = 0xFF;   // CH422G output latch cache (write-only chip)

// CH422G: write-only expander with fixed sub-addresses — 0x24 selects the
// mode register (0x01 = outputs enabled), 0x38 latches EXIO0..7.
bool ch422g_mode_output() {
  Wire.beginTransmission(CH422G_ADDR_SYS);
  Wire.write(0x01);
  return Wire.endTransmission() == 0;
}

bool ch422g_write(uint8_t value) {
  Wire.beginTransmission(CH422G_ADDR_OUT);
  Wire.write(value);
  if (Wire.endTransmission() != 0) return false;
  s_exio_state = value;
  return true;
}

void ch422g_set_bits(uint8_t mask, bool high) {
  ch422g_write(high ? (s_exio_state | mask) : (s_exio_state & ~mask));
}

// GT911: 16-bit register addressing.
uint8_t s_gt911_addr = TOUCH_I2C_ADDR;

bool gt911_read(uint16_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(s_gt911_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)s_gt911_addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

bool gt911_write_u8(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(s_gt911_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

constexpr uint16_t GT911_REG_STATUS = 0x814E;
constexpr uint16_t GT911_REG_POINT1 = 0x8150;
constexpr uint16_t GT911_REG_PRODUCT = 0x8140;

}  // namespace

bool display_init() {
  // Expander first: touch reset high, backlight OFF until the first frame
  // is painted (no white-flash at boot).
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, I2C_FREQ_FAST);
  ch422g_mode_output();
  ch422g_write(0xFF & ~(uint8_t)CH422G_BIT_BACKLIGHT);
  // GT911 address-select dance: INT low during reset selects 0x5D.
  pinMode(TOUCH_PIN_INT, OUTPUT);
  digitalWrite(TOUCH_PIN_INT, LOW);
  ch422g_set_bits(CH422G_BIT_TOUCH_RST, false);
  delay(10);
  ch422g_set_bits(CH422G_BIT_TOUCH_RST, true);
  delay(60);
  pinMode(TOUCH_PIN_INT, INPUT);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // Bounce buffers (GFX 1.6.x, the core-3 pairing): two small internal-SRAM
  // staging buffers the LCD DMA streams from instead of reading PSRAM
  // directly. Without them the panel visibly jitters/shifts sideways the
  // moment WiFi comes up — the radio and the renderer contend for PSRAM
  // bandwidth and the RGB peripheral underruns (seen on the 4.3B bench).
  // 2 x 800x10 px costs 32 KiB of internal RAM; 10 rows divides 480 evenly.
  s_rgbpanel = new Arduino_ESP32RGBPanel(
      LCD_PIN_DE, LCD_PIN_VSYNC, LCD_PIN_HSYNC, LCD_PIN_PCLK,
      LCD_PIN_R3, LCD_PIN_R4, LCD_PIN_R5, LCD_PIN_R6, LCD_PIN_R7,
      LCD_PIN_G2, LCD_PIN_G3, LCD_PIN_G4, LCD_PIN_G5, LCD_PIN_G6, LCD_PIN_G7,
      LCD_PIN_B3, LCD_PIN_B4, LCD_PIN_B5, LCD_PIN_B6, LCD_PIN_B7,
      0 /* hsync_polarity */, LCD_HSYNC_FRONT_PORCH, LCD_HSYNC_PULSE, LCD_HSYNC_BACK_PORCH,
      0 /* vsync_polarity */, LCD_VSYNC_FRONT_PORCH, LCD_VSYNC_PULSE, LCD_VSYNC_BACK_PORCH,
      1 /* pclk_active_neg */, LCD_PCLK_HZ,
      false /* useBigEndian */, 0 /* de_idle_high */, 0 /* pclk_idle_high */,
      (size_t)LCD_WIDTH * 10 /* bounce_buffer_size_px */);
#else
  // GFX 1.4.9 (the core-2 pairing) predates the bounce-buffer parameter —
  // this path can show WiFi-load jitter on the 4.3B; the shipping answer
  // for that pairing is under evaluation (lower PCLK trades refresh rate).
  s_rgbpanel = new Arduino_ESP32RGBPanel(
      LCD_PIN_DE, LCD_PIN_VSYNC, LCD_PIN_HSYNC, LCD_PIN_PCLK,
      LCD_PIN_R3, LCD_PIN_R4, LCD_PIN_R5, LCD_PIN_R6, LCD_PIN_R7,
      LCD_PIN_G2, LCD_PIN_G3, LCD_PIN_G4, LCD_PIN_G5, LCD_PIN_G6, LCD_PIN_G7,
      LCD_PIN_B3, LCD_PIN_B4, LCD_PIN_B5, LCD_PIN_B6, LCD_PIN_B7,
      0 /* hsync_polarity */, LCD_HSYNC_FRONT_PORCH, LCD_HSYNC_PULSE, LCD_HSYNC_BACK_PORCH,
      0 /* vsync_polarity */, LCD_VSYNC_FRONT_PORCH, LCD_VSYNC_PULSE, LCD_VSYNC_BACK_PORCH,
      1 /* pclk_active_neg */, LCD_PCLK_HZ);
#endif
  s_display = new Arduino_RGB_Display(LCD_WIDTH, LCD_HEIGHT, s_rgbpanel,
                                      0 /* rotation */, true /* auto_flush */);
  if (!s_display->begin()) {
    log_line("DISP", "RGB panel init FAILED — running headless.");
    return false;
  }
  s_display->fillScreen(0x0000);

  // Touch probe (product id should read "911").
  uint8_t pid[4] = {0};
  if (!gt911_read(GT911_REG_PRODUCT, pid, 4)) {
    s_gt911_addr = TOUCH_I2C_ADDR_ALT;
    if (!gt911_read(GT911_REG_PRODUCT, pid, 4)) {
      log_line("DISP", "GT911 not answering on 0x5D/0x14 — touch disabled.");
      s_gt911_addr = 0;
    }
  }
  if (s_gt911_addr) {
    log_header("DISP");
    canary::dbg_serial().printf("GT911 up at 0x%02X (product %c%c%c)\n",
                                s_gt911_addr, pid[0], pid[1], pid[2]);
  }

  log_line("DISP", "RGB 800x480 up (direct framebuffer, on/off backlight).");
  return true;
}

Arduino_GFX* gfx() { return s_display; }

void display_flush() { /* RGB peripheral scans continuously — nothing to do */ }

void backlight_set(uint8_t level) {
  // Hardware truth: the expander line is binary. Any nonzero intent = on.
  ch422g_set_bits(CH422G_BIT_BACKLIGHT, level > 0);
}

TouchSample touch_read() {
  TouchSample s;
  if (!s_gt911_addr) return s;
  uint8_t status = 0;
  if (!gt911_read(GT911_REG_STATUS, &status, 1)) return s;
  if (!(status & 0x80)) return s;          // no fresh coordinate frame
  const uint8_t n = status & 0x0F;
  if (n >= 1) {
    uint8_t p[4];
    if (gt911_read(GT911_REG_POINT1, p, 4)) {
      s.touched = true;
      s.x = (int16_t)(p[0] | (p[1] << 8));
      s.y = (int16_t)(p[2] | (p[3] << 8));
    }
  }
  gt911_write_u8(GT911_REG_STATUS, 0);     // ack the frame
  return s;
}

}  // namespace canary::hal

#endif  // CD_FLAVOR_DASH
