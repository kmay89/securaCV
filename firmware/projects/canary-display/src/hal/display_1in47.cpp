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
// EXCEPTION: the C3-LCD-1.47 (TFT_USE_EXIO) has no LEDC backlight pin — its
// CS/RST/backlight ride Waveshare's I2C expander, brightness is an 8-bit
// register write, and the nightlight's CD_BL_MAX_PCT heat ceiling is
// enforced right at that register so nothing above it can ever be written.
//
// ⚠️ ST7789 OFFSET: the 172-wide glass is a window into the controller's
//    240-wide RAM, so the panel is constructed with a 34-px column offset
//    (TFT_COL_OFFSET). Verify against your board revision.
// ⚠️ S3 board quirks: the -lcd147 S3 stick wants the HSPI port + BGR color
//    order + active-HIGH backlight (TFT_USE_HSPI / TFT_COLOR_ORDER_BGR in its
//    pins.h). These are community findings — bench-verify colors on first
//    boot; a swapped panel shows blue where it should show yellow.
#include <config.h>
// The AMOLED 2.41 rides the same nightstand flavor but swaps this ST7789
// HAL out for display_amoled241.cpp (RM690B0 QSPI) — CD_AMOLED_GLASS is
// that board's config selector, not a feature flag.
#if defined(CD_FLAVOR_NIGHTSTAND) && !defined(CD_AMOLED_GLASS)

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

// The C3-LCD-1.47's EXIO path: LCD CS, LCD RST and the BACKLIGHT PWM all
// live behind Waveshare's I2C IO expander (pins.h defines TFT_USE_EXIO) —
// there is no LEDC backlight pin on that board at all. Brightness becomes a
// one-byte register write; the register protocol below mirrors the vendor
// demo (github.com/waveshareteam/ESP32-C3-LCD-1.47) exactly.
#if defined(TFT_USE_EXIO) && TFT_USE_EXIO
#define CD_1IN47_EXIO 1
#include <Wire.h>
#endif
#ifdef CD_NIGHTLIGHT
#include <Wire.h>   // the QMI8658 rides the same shared bus
#endif

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

#ifdef CD_1IN47_EXIO
// ── Waveshare EXIO expander (I2C 0x24) ──
// Register protocol per the vendor demo's io_extension component: one mode
// register arms the IO byte, the output register carries all eight lines at
// once (so we cache the byte), and the PWM register takes a plain 0..255
// duty for the backlight.
constexpr uint8_t EXIO_REG_MODE   = 0x02;
constexpr uint8_t EXIO_REG_OUTPUT = 0x03;
constexpr uint8_t EXIO_REG_PWM    = 0x05;
// The vendor's power-on output byte: everything high except IO3 (matches
// IO_OUTPUT_INIT 0xF7 in the demo — CS lines idle high, RST released).
uint8_t s_exio_out = 0xF7;

bool exio_write_reg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(EXIO_I2C_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool exio_output(uint8_t io, bool high) {
  if (high) s_exio_out |= (uint8_t)(1u << io);
  else      s_exio_out &= (uint8_t)~(1u << io);
  return exio_write_reg(EXIO_REG_OUTPUT, s_exio_out);
}

// The one true brightness sink on this board. `duty8` is 0..255; the
// compile-time CD_BL_MAX_PCT cap (the nightlight's 50% heat budget —
// enclosed PETG case) is applied HERE, below every settings/UI path, so no
// stored preference and no API call can exceed it: a can't, not a won't.
#ifndef CD_BL_MAX_PCT
#define CD_BL_MAX_PCT 100
#endif
void exio_backlight(uint8_t duty8) {
  // A ceiling, not a scale: dim values pass through exactly (the calibrated
  // night floor must not halve to zero); only the top clips.
  constexpr uint8_t kCap = (uint8_t)((255u * CD_BL_MAX_PCT) / 100u);
  exio_write_reg(EXIO_REG_PWM, duty8 > kCap ? kCap : duty8);
}

// The vendor ST7789T init table (Vernon_ST7789T), replayed verbatim after
// the stock begin(): voltage/gamma set, INVON, and the 0x48 MADCTL
// (MX | BGR) the stock rotation-0 init does not write. Kept as data so a
// bench correction is a table edit, not a code rewrite.
struct PanelCmd { uint8_t cmd; const uint8_t* data; uint8_t len; uint16_t delay_ms; };
constexpr uint8_t P_MADCTL0[] = {0x00};
constexpr uint8_t P_COLMOD[]  = {0x55};
constexpr uint8_t P_B0[] = {0x00, 0xE8};
constexpr uint8_t P_B2[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
constexpr uint8_t P_B7[] = {0x75};
constexpr uint8_t P_BB[] = {0x1A};
constexpr uint8_t P_C0[] = {0x80};
constexpr uint8_t P_C2[] = {0x01, 0xFF};
constexpr uint8_t P_C3[] = {0x13};
constexpr uint8_t P_C4[] = {0x20};
constexpr uint8_t P_C6[] = {0x0F};
constexpr uint8_t P_D0[] = {0xA4, 0xA1};
constexpr uint8_t P_E0[] = {0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38,
                            0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30};
constexpr uint8_t P_E1[] = {0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37,
                            0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E};
#ifndef TFT_MADCTL_ROT0
#define TFT_MADCTL_ROT0 0x48
#endif
constexpr uint8_t P_MADCTL1[] = {TFT_MADCTL_ROT0};
constexpr PanelCmd P_INIT[] = {
    {0x11, nullptr, 0, 100},                       // SLPOUT
    {0x36, P_MADCTL0, sizeof(P_MADCTL0), 0},
    {0x3A, P_COLMOD, sizeof(P_COLMOD), 0},         // RGB565
    {0xB0, P_B0, sizeof(P_B0), 0},
    {0xB2, P_B2, sizeof(P_B2), 0},                 // porch
    {0xB7, P_B7, sizeof(P_B7), 0},
    {0xBB, P_BB, sizeof(P_BB), 0},                 // VCOM
    {0xC0, P_C0, sizeof(P_C0), 0},
    {0xC2, P_C2, sizeof(P_C2), 0},
    {0xC3, P_C3, sizeof(P_C3), 0},
    {0xC4, P_C4, sizeof(P_C4), 0},
    {0xC6, P_C6, sizeof(P_C6), 0},
    {0xD0, P_D0, sizeof(P_D0), 0},
    {0xE0, P_E0, sizeof(P_E0), 0},                 // gamma +
    {0xE1, P_E1, sizeof(P_E1), 0},                 // gamma -
    {0x21, nullptr, 0, 0},                         // INVON (IPS)
    {0x29, nullptr, 0, 0},                         // DISPON
    {0x2C, nullptr, 0, 20},                        // RAMWR settle
    {0x36, P_MADCTL1, sizeof(P_MADCTL1), 0},       // MX | BGR
};

void panel_vendor_init() {
  if (!s_bus) return;
  for (const auto& c : P_INIT) {
    s_bus->beginWrite();
    s_bus->writeCommand(c.cmd);
    for (uint8_t i = 0; i < c.len; i++) s_bus->write(c.data[i]);
    s_bus->endWrite();
    if (c.delay_ms) delay(c.delay_ms);
  }
}
#endif  // CD_1IN47_EXIO

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
#ifndef CD_1IN47_EXIO
  if (night) {
    cc_ledc_reconfig(TFT_PIN_BL, LEDC_CHANNEL, NIGHT_FREQ_HZ, NIGHT_RES_BITS);
  } else {
    cc_ledc_reconfig(TFT_PIN_BL, LEDC_CHANNEL, DAY_FREQ_HZ, DAY_RES_BITS);
  }
#endif
  // EXIO: one fixed-frequency 8-bit PWM inside the expander — there is no
  // profile to switch; night resolution is handled by mapping in the setters.
}

}  // namespace

bool display_init() {
  // Backlight first, held dark until the first frame is flushed — no
  // white-flash at boot on a device that lives on a nightstand.
#ifdef CD_1IN47_EXIO
  // Bring up the shared I2C bus (EXIO expander + QMI8658) and take the
  // expander through the vendor's init dance: all-output mode, CS lines
  // idle high, backlight held dark until the face is drawn.
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, I2C_FREQ_FAST);
  if (!exio_write_reg(EXIO_REG_MODE, 0xFF)) {
    log_line("DISP", "EXIO expander quiet at 0x24 — running headless.");
    return false;
  }
  exio_write_reg(EXIO_REG_OUTPUT, s_exio_out);
  exio_backlight(0);
  // Hardware reset through the expander (the RST line is not a GPIO), then
  // select the panel: the LCD is the only device we drive on this SPI bus
  // (the TF slot keeps its own CS high), so CS can simply stay low.
  exio_output(EXIO_IO_LCD_RST, false);
  delay(10);
  exio_output(EXIO_IO_LCD_RST, true);
  delay(120);
  exio_output(EXIO_IO_SD_CS, true);
  exio_output(EXIO_IO_LCD_CS, false);
#else
  cc_ledc_setup(TFT_PIN_BL, LEDC_CHANNEL, DAY_FREQ_HZ, DAY_RES_BITS);
  cc_ledc_write(TFT_PIN_BL, LEDC_CHANNEL, 0);
#endif

  // 4-wire SPI to the ST7789. MISO is shared with the TF slot on the C6 and
  // not broken out on the S3 (write-only panel either way); passing it is
  // harmless where -1. On the EXIO board CS/RST are expander lines (handled
  // above), so the bus and panel both get "not defined" for them — the
  // TFT_PIN_* values there are -1, which IS Arduino_GFX's GFX_NOT_DEFINED.
  s_bus = new Arduino_ESP32SPI(TFT_PIN_DC, TFT_PIN_CS, TFT_PIN_SCK,
                               TFT_PIN_MOSI, TFT_PIN_MISO);

  // Portrait IPS at rotation 0 with the column window offset into the
  // ST7789's 240-wide RAM (34 px on the 172-wide glass, 30 px on the C3's
  // 180-wide one — pins.h owns the numbers). col_offset1/col_offset2 both
  // carry the offset so it lands correctly at rotation 0 and its mirror.
  s_panel = new Arduino_ST7789(
      s_bus, TFT_PIN_RST, 0 /* rotation */, true /* IPS */,
      TFT_WIDTH, TFT_HEIGHT,
      TFT_COL_OFFSET, TFT_ROW_OFFSET, TFT_COL_OFFSET, TFT_ROW_OFFSET);

  if (!s_panel->begin(TFT_SPI_HZ)) {
    log_line("DISP", "ST7789 init FAILED — running headless.");
    return false;
  }
#if defined(CD_1IN47_EXIO) && defined(TFT_PANEL_VENDOR_INIT) && TFT_PANEL_VENDOR_INIT
  // Replay the vendor's ST7789T table on top of the stock init: gamma and
  // voltage set, INVON, and the MX|BGR MADCTL. Without this the panel shows
  // mirrored geometry and swapped red/blue.
  panel_vendor_init();
#endif
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
#ifdef CD_1IN47_EXIO
  // Expander PWM: plain 0..255 duty; the CD_BL_MAX_PCT heat cap is applied
  // inside exio_backlight, under every caller.
  exio_backlight(level);
#elif TFT_BL_ACTIVE_HIGH
  cc_ledc_write(TFT_PIN_BL, LEDC_CHANNEL, level);
#else
  cc_ledc_write(TFT_PIN_BL, LEDC_CHANNEL, 255 - level);
#endif
}

void backlight_night_set(uint16_t duty13) {
  if (duty13 > NIGHT_DUTY_MAX) duty13 = NIGHT_DUTY_MAX;
  ensure_profile(true);
#ifdef CD_1IN47_EXIO
  // The 13-bit night scale compresses onto the expander's 256 steps: >>5,
  // but never rounded to zero while a glow was asked for — a calibrated
  // floor of duty13=64 must still emit light, not silently go dark.
  uint8_t d8 = (uint8_t)(duty13 >> 5);
  if (d8 == 0 && duty13 > 0) d8 = 1;
  exio_backlight(d8);
#elif TFT_BL_ACTIVE_HIGH
  cc_ledc_write(TFT_PIN_BL, LEDC_CHANNEL, duty13);
#else
  cc_ledc_write(TFT_PIN_BL, LEDC_CHANNEL, NIGHT_DUTY_MAX - duty13);
#endif
}

#ifdef CD_NIGHTLIGHT
// ── Hardware rotation ─────────────────────────────────────────────────────
// The stock Arduino_ST7789::setRotation gives us everything geometric for
// free — logical width/height swap, the 30-px window offset migrating to
// the row axis in landscape (our offsets are symmetric, so its COL1/COL2
// ROW1/ROW2 shuffle is exact), and the address-window cache reset. What it
// gets wrong for THIS glass is only the MADCTL byte: its table assumes a
// stock panel (RGB order, unmirrored at rotation 0), while the vendor
// personality is MX|BGR at rotation 0.
//
// Composing "stock rotation matrix, then this panel's extra X mirror" in
// MADCTL terms toggles the MX bit (MX always names the controller's column
// axis, with or without MV), plus BGR always:
//
//   stock: r0=0x00        r1=MX|MV=0x60   r2=MX|MY=0xC0   r3=MY|MV=0xA0
//   ours:  r0=MX|BGR=0x48 r1=MV|BGR=0x28  r2=MY|BGR=0x88  r3=MX|MY|MV|BGR=0xE8
//
// Consistency proof: the r0 entry reproduces the vendor init's own MADCTL
// byte (TFT_MADCTL_ROT0) exactly. Bench-verify the two landscapes on first
// rotation — if they come up mirrored, the fix is swapping the r1/r3 bytes
// here, nothing structural.
void display_set_rotation(uint8_t rot) {
  if (!s_panel || !s_bus) return;
  rot &= 3;
  s_panel->setRotation(rot);
  static const uint8_t kMadctl[4] = {0x48, 0x28, 0x88, 0xE8};
  s_bus->beginWrite();
  s_bus->writeCommand(0x36);
  s_bus->write(kMadctl[rot]);
  s_bus->endWrite();
  // Clear the frame memory, because re-addressing it is not the same as
  // repainting it. The ST7789 still holds the PREVIOUS orientation's image,
  // now read out along transposed axes, and LVGL only ever flushes regions
  // it believes are dirty — so every pixel the rebuilt face doesn't happen
  // to cover survives as a leftover streak. The bird's yellow and the
  // clock's white are the brightest things on the old frame, so they are
  // what a user actually sees. A rotation invalidates the whole frame by
  // definition; the face is rebuilt immediately after, so this costs one
  // black frame and removes the entire class of artifact.
  s_panel->fillScreen(0x0000);
}

// ── QMI8658 IMU (shared I2C bus with the EXIO expander) ─────────────────
// Accel-only, low-power 21 Hz, +-4 g — a lamp asking "which way is down".
// Register protocol per the vendor demo's QMI8658 component; the address
// strap differs between board revisions, so init probes both.
namespace {
constexpr uint8_t QMI_WHO_AM_I  = 0x00;   // answers 0x05
constexpr uint8_t QMI_CTRL1     = 0x02;   // 0x40 = auto-increment addresses
constexpr uint8_t QMI_CTRL2     = 0x03;   // accel: aFS[6:4] | aODR[3:0]
constexpr uint8_t QMI_CTRL7     = 0x08;   // sensor enables: bit0 = accel
constexpr uint8_t QMI_AX_L      = 0x35;   // 6-byte XYZ burst starts here
uint8_t s_imu_addr = 0;                   // 0 = no IMU answered

bool imu_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(s_imu_addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool imu_read(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(s_imu_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)s_imu_addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}
}  // namespace

bool imu_init() {
  // Wire is already up (display_init's EXIO path begins the shared bus).
  const uint8_t addrs[2] = {IMU_I2C_ADDR, (uint8_t)(IMU_I2C_ADDR ^ 0x01)};
  for (uint8_t a : addrs) {
    s_imu_addr = a;
    uint8_t who = 0;
    if (imu_read(QMI_WHO_AM_I, &who, 1) && who == 0x05) {
      // ±4 g (aFS=001) + low-power 21 Hz (aODR=1101); accel only. If the
      // bench finds LP mode shy on a board revision, 0x14 (normal 128 Hz)
      // is the one-byte fallback.
      const bool ok = imu_write(QMI_CTRL1, 0x40) &&
                      imu_write(QMI_CTRL2, 0x1D) &&
                      imu_write(QMI_CTRL7, 0x01);
      if (ok) {
        log_line("IMU", "QMI8658 up — auto-orient armed.");
        return true;
      }
    }
  }
  s_imu_addr = 0;
  log_line("IMU", "QMI8658 quiet — auto-orient off, everything else fine.");
  return false;
}

bool imu_read_accel(int32_t* ax, int32_t* ay, int32_t* az) {
  if (!s_imu_addr) return false;
  uint8_t b[6];
  if (!imu_read(QMI_AX_L, b, sizeof(b))) return false;
  const int16_t x = (int16_t)((b[1] << 8) | b[0]);
  const int16_t y = (int16_t)((b[3] << 8) | b[2]);
  const int16_t z = (int16_t)((b[5] << 8) | b[4]);
  if (ax) *ax = x;
  if (ay) *ay = y;
  if (az) *az = z;
  return true;
}
#endif  // CD_NIGHTLIGHT

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

#endif  // CD_FLAVOR_NIGHTSTAND && !CD_AMOLED_GLASS
