// src/hal/display_amoled241.cpp — the flagship glance glass: the Waveshare
// ESP32-S3-Touch-AMOLED-2.41's RM690B0 450x600 AMOLED on a 4-lane QSPI bus,
// with an FT6336 touch layer polled over the shared I2C bus. Rides the
// nightstand flavor (CD_FLAVOR_NIGHTSTAND) exactly like the Touch-1.69 —
// same portrait face, same tap/long-press ladder — with CD_AMOLED_GLASS
// swapping this HAL in for the ST7789 display_1in47.cpp.
//
// LVGL owns buffering/dirty-region rendering (ui/lvgl_port.cpp); this HAL
// exposes the bare panel and flushes arrive via draw16bitRGBBitmap.
//
// AMOLED changes the brightness story: there is no backlight pin at all —
// brightness is the RM690B0's 0x51 command (0..255), and a black pixel
// simply does not emit. The two-profile day/night contract the family
// expects (backlight_set / backlight_night_set) maps onto that one command;
// the 13-bit night scale compresses to 8 bits with a non-zero floor, the
// same arrangement the C3's EXIO register path uses.
//
// ⚠️ POWER LATCH FIRST: GPIO16 holds the board's power rail AND the panel
//    supply (see pins.h). display_init raises it and waits for the rail to
//    settle before touching the bus; main.cpp raises it even earlier so a
//    battery boot can't lose the rail when the PWR button is released.
// ⚠️ TOUCH INT RIDES THE TCA9554 (EXIO2) — there is no GPIO to attach, so
//    touch_read polls the FT6336, which answers over I2C regardless.
// ⚠️ PANEL WINDOW: the 450-px axis starts 16 columns into controller RAM
//    (LCD_COL_OFFSET) — the offset is baked into the panel constructor.
// ⚠️ VENDOR INIT: the stock Arduino_RM690B0 table is the LilyGO T4-S3
//    personality, and it is NOT safe on this glass: its 0x5A/0x5B writes
//    ("SWIRE FOR BV6804") program the T4-S3 panel module's AMOLED power
//    chip over SWIRE — values the bench-tested init for THIS panel (the
//    CircuitPython waveshare_esp32_s3_amoled_241 board.c) never touches.
//    A wrong SWIRE program is exactly the failure that looks fine on
//    serial: the controller ACKs every command and emits zero light.
//    So begin() plays the Waveshare sequence INSTEAD of the stock table
//    (tftInit override below), matching the bench order — including the
//    page-0x13 0xEB=0x0E register BEFORE sleep-out, where the bench sets
//    it, not patched in afterwards. Bench-verify colors and geometry on
//    first boot — a wrong MADCTL shows mirrored geometry, a wrong color
//    order swaps red/blue.
#include <config.h>
#if defined(CD_FLAVOR_NIGHTSTAND) && defined(CD_AMOLED_GLASS)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <stdio.h>

#include "pins.h"

#include "canary/hal/display.h"
#include "canary/log.h"

namespace canary::hal {

namespace {

// The bench-tested init for THIS panel, transcribed from the CircuitPython
// waveshare_esp32_s3_amoled_241 board.c (the pin map's provenance), in the
// bench order. Two deliberate deltas from the stock Arduino_RM690B0 table:
//   - NO 0x5A/0x5B ("SWIRE FOR BV6804") — that pair programs the LilyGO
//     T4-S3 module's AMOLED power chip; the bench init for this panel
//     leaves its power defaults alone, and a wrong SWIRE program is an
//     unlit panel that still ACKs every command.
//   - The page-0x13 0xEB=0x0E register lands BEFORE sleep-out, where the
//     bench sets it — not replayed after the panel is already on.
// The window (CASET/PASET), MADCTL and final brightness are omitted on
// purpose: Arduino_TFT::begin() writes the first two right after this
// table (same 16..465/0..599 values via the constructor offsets), and
// brightness stays 0 until the first frame is flushed (display_init).
static const uint8_t waveshare241_init_operations[] = {
    BEGIN_WRITE,
    WRITE_C8_D8, 0xFE, 0x20,  // MCS page 0x20
    WRITE_C8_D8, 0x26, 0x0A,  // bias
    WRITE_C8_D8, 0x24, 0x80,  // source output control
    WRITE_C8_D8, 0xFE, 0x13,  // page 0x13
    WRITE_C8_D8, 0xEB, 0x0E,  // vendor register (bench: before sleep-out)
    WRITE_C8_D8, 0xFE, 0x00,  // back to the user page
    WRITE_C8_D8, 0x3A, 0x55,  // COLMOD: 16-bit RGB565
    WRITE_C8_D8, 0xC2, 0x00,  // vendor register
    END_WRITE,
    DELAY, 10,
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x35,    // TE on (bench sends no parameter; each QSPI
                              // command is its own framed transaction)
    WRITE_C8_D8, 0x51, 0x00,  // brightness 0 — dark until the first frame
    END_WRITE,
    DELAY, 10,
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,    // sleep out
    END_WRITE,
    DELAY, 80,
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,    // display on
    END_WRITE,
    DELAY, 10,
};

// Same class, different personality: begin() runs tftInit(), so overriding
// it swaps the stock T4-S3 table for the Waveshare sequence above while
// keeping the library's reset pulse, rotation and window handling.
class Arduino_RM690B0_Waveshare241 : public Arduino_RM690B0 {
 public:
  using Arduino_RM690B0::Arduino_RM690B0;

 protected:
  void tftInit() override {
    if (_rst != GFX_NOT_DEFINED) {
      pinMode(_rst, OUTPUT);
      digitalWrite(_rst, HIGH);
      delay(100);
      digitalWrite(_rst, LOW);
      delay(RM690B0_RST_DELAY);
      digitalWrite(_rst, HIGH);
      delay(RM690B0_RST_DELAY);
    } else {
      _bus->sendCommand(RM690B0_SWRESET);
      delay(RM690B0_RST_DELAY);
    }
    _bus->batchOperation(waveshare241_init_operations,
                         sizeof(waveshare241_init_operations));
  }
};

Arduino_DataBus* s_bus = nullptr;
Arduino_RM690B0* s_panel = nullptr;

// ── FT6336 (FocalTech FT6x36 register protocol), polled ──
constexpr uint8_t FT_REG_TD_STATUS = 0x02;  // touch count in the low nibble;
                                            // P1 XH/XL/YH/YL follow at 0x03..
constexpr uint8_t FT_REG_CHIP_ID   = 0xA3;  // FT6336 answers 0x64

bool ft_read(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_I2C_ADDR, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

// ── TCA9554 IO expander (EXIO2 = the touch INT we deliberately poll) ──
// Probed for diagnosability only: every line stays an input (the power-on
// default; config register all-ones), so a future bench assignment of the
// unverified EXIO lines starts from a known-quiet state.
constexpr uint8_t TCA_REG_CONFIG = 0x03;

bool tca_probe() {
  Wire.beginTransmission(EXIO_I2C_ADDR);
  Wire.write(TCA_REG_CONFIG);
  Wire.write(0xFF);  // all inputs — the chip's own reset state, re-asserted
  return Wire.endTransmission() == 0;
}

// The one true brightness sink on this glass: the RM690B0's 0x51 command.
// Every settings/UI path lands here, so the panel and the stored preference
// can never disagree about what the eye sees.
void panel_brightness(uint8_t v) {
  if (!s_bus) return;
  s_bus->beginWrite();
  s_bus->writeC8D8(0x51, v);
  s_bus->endWrite();
}

}  // namespace

bool display_init() {
  // Power latch before anything: GPIO16 holds the rail AND feeds the panel.
  // main.cpp already raised it at the top of setup(); re-assert and give the
  // AMOLED supply the settle time the vendor boot code allows.
  pinMode(PWR_LATCH_PIN, OUTPUT);
  digitalWrite(PWR_LATCH_PIN, HIGH);
  delay(200);

  // Shared I2C bus up-front (FT6336 + QMI8658 + PCF85063 + TCA9554): the
  // touch reset pulse below needs the bus only for the controllers behind
  // it, but the RTC seed (main.cpp) rides this same begin.
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, I2C_FREQ_FAST);

  if (!tca_probe()) {
    // Not fatal: the expander only carries the touch INT we poll anyway.
    log_line("DISP", "TCA9554 quiet at 0x20 — continuing (touch is polled).");
  }

  // FT6336 hardware reset, then give it the wake-up time FocalTech asks for.
#if TOUCH_PIN_RST >= 0
  pinMode(TOUCH_PIN_RST, OUTPUT);
  digitalWrite(TOUCH_PIN_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_PIN_RST, HIGH);
  delay(120);
#endif

  // 4-lane QSPI to the RM690B0. Lane order per pins.h (D0..D3 = SDIO0..3).
  s_bus = new Arduino_ESP32QSPI(LCD_PIN_CS, LCD_PIN_SCLK, LCD_PIN_SDIO0,
                                LCD_PIN_SDIO1, LCD_PIN_SDIO2, LCD_PIN_SDIO3);

  // Portrait at rotation 0 with the 16-px column window into the
  // controller's 482-wide RAM. The window is symmetric (482-450-16 = 16),
  // so col_offset1/col_offset2 both carry it and rotation 2 stays exact.
  // The subclass plays the Waveshare init personality (see the table
  // above) — begin() with the stock class would program the T4-S3's SWIRE
  // power values into this panel's power chip and the glass stays dark.
  s_panel = new Arduino_RM690B0_Waveshare241(
      s_bus, LCD_PIN_RST, 0 /* rotation */,
      LCD_WIDTH, LCD_HEIGHT,
      LCD_COL_OFFSET, LCD_ROW_OFFSET, LCD_COL_OFFSET, LCD_ROW_OFFSET);

  if (!s_panel->begin(LCD_QSPI_HZ)) {
    log_line("DISP", "RM690B0 init FAILED — running headless.");
    return false;
  }

  // Dark until the first frame is flushed. The init table already parks
  // brightness at 0 (unlike the stock table's 0xD0 — on an emissive panel
  // a lit boot IS a white flash); re-assert for the re-init path.
  panel_brightness(0);
  s_panel->fillScreen(0x0000);

  // Touch probe — informational; the FT6336 answers its chip-id register
  // even before the first touch.
  uint8_t id = 0;
  if (ft_read(FT_REG_CHIP_ID, &id, 1)) {
    char msg[48];
    snprintf(msg, sizeof(msg), "FT6336 up (chip id 0x%02X) - touch armed.", id);
    log_line("DISP", msg);
  } else {
    log_line("DISP", "FT6336 quiet at boot — touch armed, will keep polling.");
  }

  log_line("DISP", "RM690B0 AMOLED portrait up (QSPI, LVGL render).");
  return true;
}

Arduino_GFX* gfx() { return s_panel; }

void display_flush() { /* LVGL flushes dirty regions itself */ }

// ── Brightness: intent in, panel command out ─────────────────────────────
// Day path: 0..255 maps straight onto the 0x51 command.
void backlight_set(uint8_t level) {
  panel_brightness(level);
}

// Night path: the family's 13-bit night scale compresses onto the panel's
// 256 steps (>>5), but never rounds to zero while a glow was asked for —
// a calibrated floor must still emit light, not silently go dark. AMOLED
// emission at level 1 is genuinely dim, which is the whole point of this
// glass at 3 a.m.
void backlight_night_set(uint16_t duty13) {
  constexpr uint16_t NIGHT_DUTY_MAX = 8191;
  if (duty13 > NIGHT_DUTY_MAX) duty13 = NIGHT_DUTY_MAX;
  uint8_t d8 = (uint8_t)(duty13 >> 5);
  if (d8 == 0 && duty13 > 0) d8 = 1;
  panel_brightness(d8);
}

// ── SD slot: dedicated SDMMC 1-bit, DAT3 on a native GPIO ────────────────
// Unlike the dash family there is no expander latch to release: a gentle
// pull-up on DAT3 keeps an inserted card negotiating SD mode, and the SDMMC
// host takes over from there (fleet/sd_archive.cpp).
bool sd_dat3_release() {
  pinMode(SD_PIN_D3, INPUT_PULLUP);
  return true;
}

// ── Touch: FT6336, one 5-byte burst per poll ─────────────────────────────
// TD_STATUS + P1 XH/XL/YH/YL. Coordinates arrive in the panel's native
// portrait frame (450x600). Bench-verify on first boot: a mirrored axis
// here is a settings sheet whose taps land on the wrong row.
TouchSample touch_read() {
  TouchSample s;
  uint8_t buf[5];
  if (!ft_read(FT_REG_TD_STATUS, buf, sizeof(buf))) return s;
  const uint8_t n = buf[0] & 0x0F;
  if (n == 0 || n > 5) return s;   // 0 fingers, or a glitched read
  s.touched = true;
  s.x = (int16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
  s.y = (int16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
  return s;
}

// ── Honest power-off (the case PWR button's hold gesture) ────────────────
// Panel to sleep, emission zero, latch dropped. On battery the latch drop
// IS power-off; on a USB-powered desk the rail survives, so deep sleep with
// wake on the same button gives the identical user story: hold to darken,
// press to come back.
void power_off() {
  log_line("PWR", "PWR held — going dark (latch drop + deep sleep).");
  panel_brightness(0);
  if (s_panel) s_panel->displayOff();
  delay(20);
  digitalWrite(PWR_LATCH_PIN, LOW);
  delay(50);  // on battery we are gone before this returns
#if defined(PWR_KEY_PIN) && (PWR_KEY_PIN >= 0)
  // Still here = USB power (the latch drop couldn't cut the rail). This
  // fires on the HOLD DEADLINE, so the active-low key is still down — and a
  // level-low wake armed under a held key is already true: the "off" would
  // bounce straight back on. Wait out the release (plus a debounce breath)
  // before arming, so waking takes a fresh press.
  pinMode(PWR_KEY_PIN, INPUT_PULLUP);
  while (digitalRead(PWR_KEY_PIN) == LOW) delay(10);
  delay(50);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PWR_KEY_PIN, 0 /* wake on press */);
#endif
  esp_deep_sleep_start();
}

}  // namespace canary::hal

#endif  // CD_FLAVOR_NIGHTSTAND && CD_AMOLED_GLASS
