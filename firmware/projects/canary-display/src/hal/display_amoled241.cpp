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
// ⚠️ THE PANEL RESET RIDES THE TCA9554 (V2: EXIO0, 10 kΩ pull-DOWN). The
//    expander boots with every line an input, so the pull-down holds the
//    RM690B0 in reset until display_init makes EXIO0 an output and drives
//    it high. Miss that and the glass is black forever while serial shows
//    every stage "up": QSPI writes carry no acknowledgement. The release is
//    read back from the expander's input register and logged.
// ⚠️ GPIO21 IS THE PANEL'S TE OUTPUT and GPIO3 the touch INT on V2 (they
//    were the resets on the earlier revision CircuitPython describes).
//    Nothing here drives either. touch_read polls the FT6336 over I2C.
// ⚠️ PANEL WINDOW: the 450-px axis starts 16 columns into controller RAM
//    (LCD_COL_OFFSET) — the offset is baked into the panel constructor.
// ⚠️ VENDOR INIT: the stock Arduino_RM690B0 table is the LilyGO T4-S3
//    personality: its 0x5A/0x5B writes ("SWIRE FOR BV6804") program the
//    T4-S3 panel module's AMOLED power chip, values Waveshare's init for
//    THIS board never sends. So begin() plays Waveshare's V2 sequence
//    INSTEAD of the stock table (tftInit override below), byte for byte as
//    the vendor's three V2 display examples send it. Bench-verify colors
//    and geometry on first boot — a wrong MADCTL shows mirrored geometry,
//    a wrong color order swaps red/blue.
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

// Waveshare's init for the V2 board, in the vendor's order: the same
// lcd_init_cmds[] in 02_Example/Arduino/09_LVGL_Test,
// ESP-IDF/09_LVGL_V9_Test (lcd_bsp/display_bsp.c) and
// ESP-IDF/10_FactoryProgram of github.com/waveshareteam/
// ESP32-S3-Touch-AMOLED-2.41-V2. Deltas from the stock Arduino_RM690B0
// table: no 0x5A/0x5B SWIRE writes (the T4-S3's power chip), and the
// window before display-on. The page-0x13 0xEB=0x0E write the
// CircuitPython (earlier-revision) init adds is not in any V2 example, so
// it is not here either.
// The address window (CASET 16..465 / PASET 0..599) is written BEFORE
// display-on, exactly where the vendor writes it. Arduino_TFT::begin()
// writes the same window again afterwards (from the constructor offsets),
// which is harmless. MADCTL stays the library's (portrait, rotation 0 =
// 0x00, as in the vendor's portrait build; its 0x30 is the landscape
// option), and brightness stays 0 until the splash lights the glass (the
// vendor ends on 0x51=0xFF; the splash's backlight_set is that write).
// The window bytes are DERIVED from pins.h, never typed: a wrong window is
// a panel that accepts every pixel and shows none of them.
constexpr uint16_t kColFirst = LCD_COL_OFFSET;                    // 16
constexpr uint16_t kColLast = LCD_COL_OFFSET + LCD_WIDTH - 1;     // 465
constexpr uint16_t kRowFirst = LCD_ROW_OFFSET;                    // 0
constexpr uint16_t kRowLast = LCD_ROW_OFFSET + LCD_HEIGHT - 1;    // 599
static_assert(kColLast < RM690B0_TFTWIDTH, "column window past controller RAM");
static_assert(kRowLast < RM690B0_TFTHEIGHT, "row window past controller RAM");

static const uint8_t waveshare241_init_operations[] = {
    BEGIN_WRITE,
    WRITE_C8_D8, 0xFE, 0x20,  // MCS page 0x20
    WRITE_C8_D8, 0x26, 0x0A,  // bias
    WRITE_C8_D8, 0x24, 0x80,  // source output control
    WRITE_C8_D8, 0xFE, 0x00,  // back to the user page
    WRITE_C8_D8, 0x3A, 0x55,  // COLMOD: 16-bit RGB565
    WRITE_C8_D8, 0xC2, 0x00,  // vendor register
    END_WRITE,
    DELAY, 10,
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x35,    // TE on (vendor sends no parameter; each QSPI
                              // command is its own framed transaction)
    WRITE_C8_D8, 0x51, 0x00,  // brightness 0 — dark until the first frame
    END_WRITE,
    DELAY, 10,
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,    // sleep out
    END_WRITE,
    DELAY, 80,
    BEGIN_WRITE,
    WRITE_C8_BYTES, 0x2A, 4,  // CASET 16..465 (vendor: before display-on)
        (uint8_t)(kColFirst >> 8), (uint8_t)(kColFirst & 0xFF),
        (uint8_t)(kColLast >> 8), (uint8_t)(kColLast & 0xFF),
    WRITE_C8_BYTES, 0x2B, 4,  // PASET 0..599
        (uint8_t)(kRowFirst >> 8), (uint8_t)(kRowFirst & 0xFF),
        (uint8_t)(kRowLast >> 8), (uint8_t)(kRowLast & 0xFF),
    END_WRITE,
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,    // display on
    END_WRITE,
    DELAY, 10,
};

// Same class, different personality: begin() runs tftInit(), so overriding
// it swaps the stock T4-S3 table for the Waveshare sequence above while
// keeping the library's rotation and window handling. On V2 there is no
// reset GPIO (LCD_PIN_RST is -1): the hardware reset is the expander pulse
// in display_init, and this sends the software reset after it, as the
// vendor's esp_lcd driver does when it has no reset pin.
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

// ── TCA9554 IO expander: the panel and touch resets (EXIO0 / EXIO1) ──
// Only the two reset lines become outputs; the RTC/IMU interrupt lines and
// the header's EXIO5..7 stay inputs (the chip's power-on state).
constexpr uint8_t TCA_REG_INPUT  = 0x00;
constexpr uint8_t TCA_REG_OUTPUT = 0x01;
constexpr uint8_t TCA_REG_CONFIG = 0x03;  // 1 = input, 0 = output
constexpr uint8_t kResetLines =
    (uint8_t)((1u << EXIO_LCD_RESET) | (1u << EXIO_TOUCH_RESET));

bool tca_write(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(EXIO_I2C_ADDR);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

bool tca_read(uint8_t reg, uint8_t* v) {
  Wire.beginTransmission(EXIO_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)EXIO_I2C_ADDR, 1) != 1) return false;
  *v = (uint8_t)Wire.read();
  return true;
}

// The vendor's reset (example_exio_reset): assert, hold, release, then the
// wake-up time before the first command. Both resets go together; the
// FT6336 wants the same low pulse and more than the same wake time.
// Returns what the input register says EXIO0 is afterwards, so the log can
// tell a released panel from one still held by the pull-down.
enum class PanelReset { Released, StillLow, NoExpander };

PanelReset panel_reset_via_expander() {
  // Output latches first (resets asserted LOW), then direction: the lines
  // leave high-Z straight into a known level, never a glitch HIGH.
  if (!tca_write(TCA_REG_OUTPUT, 0x00) ||
      !tca_write(TCA_REG_CONFIG, (uint8_t)~kResetLines)) {
    return PanelReset::NoExpander;
  }
  delay(20);
  if (!tca_write(TCA_REG_OUTPUT, kResetLines)) return PanelReset::NoExpander;
  delay(120);
  uint8_t in = 0;
  if (!tca_read(TCA_REG_INPUT, &in)) return PanelReset::NoExpander;
  return (in & (1u << EXIO_LCD_RESET)) ? PanelReset::Released
                                       : PanelReset::StillLow;
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
  // First-light trace: the panel is dark until the splash, and on an
  // emissive glass "dark" is also what a dead rail, a refused bus and a
  // panel that drops every pixel look like. One numbered line per stage
  // lets a serial log say WHICH of those it is.
  log_line("DISP", digitalRead(PWR_LATCH_PIN) == HIGH
                       ? "first light 1/5: panel rail latched (GPIO16 high)."
                       : "first light 1/5: GPIO16 reads LOW after driving it high - "
                         "the panel has no supply.");

  // Shared I2C bus up-front (FT6336 + QMI8658 + PCF85063 + TCA9554): both
  // reset pulses below ride the expander on it, and the RTC seed (main.cpp)
  // rides this same begin.
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, I2C_FREQ_FAST);

  // Panel + touch hardware reset through the expander. This is the line
  // whose absence kept the V2 glass black: without it the 10 kΩ pull-down
  // holds the RM690B0 in reset and every QSPI byte below goes nowhere.
  switch (panel_reset_via_expander()) {
    case PanelReset::Released:
      log_line("DISP", "first light 2/5: panel reset released (TCA9554 EXIO0 reads high).");
      break;
    case PanelReset::StillLow:
      log_line("DISP", "first light 2/5: TCA9554 EXIO0 still reads LOW after driving it "
                       "high - the panel is held in reset and will stay dark.");
      break;
    case PanelReset::NoExpander:
      log_line("DISP", "first light 2/5: the TCA9554 did not answer at 0x20 - the panel's "
                       "reset rides it, so the glass will stay dark.");
      break;
  }

  // 4-lane QSPI to the RM690B0. Lane order per pins.h (D0..D3 = SDIO0..3).
  s_bus = new Arduino_ESP32QSPI(LCD_PIN_CS, LCD_PIN_SCLK, LCD_PIN_SDIO0,
                                LCD_PIN_SDIO1, LCD_PIN_SDIO2, LCD_PIN_SDIO3);

  // Portrait at rotation 0 with the 16-px column window into the
  // controller's 482-wide RAM. The window is symmetric (482-450-16 = 16),
  // so col_offset1/col_offset2 both carry it and rotation 2 stays exact.
  // The subclass plays Waveshare's V2 init (see the table above) instead
  // of the stock T4-S3 table and its SWIRE power writes.
  s_panel = new Arduino_RM690B0_Waveshare241(
      s_bus, LCD_PIN_RST, 0 /* rotation */,
      LCD_WIDTH, LCD_HEIGHT,
      LCD_COL_OFFSET, LCD_ROW_OFFSET, LCD_COL_OFFSET, LCD_ROW_OFFSET);

  log_line("DISP", "first light 3/5: QSPI bus + Waveshare 2.41 V2 init sequence…");
  if (!s_panel->begin(LCD_QSPI_HZ)) {
    log_line("DISP", "RM690B0 init FAILED (QSPI bus refused) — running headless.");
    return false;
  }
  // QSPI writes carry no acknowledgement: this line means the bytes went
  // out, not that the panel took them.
  log_line("DISP", "first light 4/5: init sent (sleep-out, window, display-on).");

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

  log_line("DISP", "first light 5/5: RAM cleared to black, brightness 0 until "
                   "the splash. All five lines and a dark glass after the splash: "
                   "the rail, reset and bus came up, so suspect the panel init or pixel path.");
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
void power_off(bool pressed_high) {
  log_line("PWR", "PWR held — going dark (latch drop + deep sleep).");
  panel_brightness(0);
  if (s_panel) s_panel->displayOff();
  delay(20);
  digitalWrite(PWR_LATCH_PIN, LOW);
  delay(50);  // on battery we are gone before this returns
#if defined(PWR_KEY_PIN) && (PWR_KEY_PIN >= 0)
  // Still here = USB power (the latch drop couldn't cut the rail). The
  // PWR model only decides after a settled release, but a level wake armed
  // under a held key is already true — the "off" would bounce straight back
  // on — so wait for the key to read released (plus a debounce breath)
  // before arming, so waking takes a fresh press. The wait is BOUNDED: the old one
  // looped forever on a key whose polarity it had guessed wrong, a dark
  // glass that never slept and never woke. If the key is somehow still
  // "down" after 10 s, sleep anyway with the timer as the way back.
  const int pressed_level = pressed_high ? HIGH : LOW;
  pinMode(PWR_KEY_PIN, INPUT_PULLUP);
  const uint32_t t0 = millis();
  bool released = false;
  while ((uint32_t)(millis() - t0) < 10000) {
    if (digitalRead(PWR_KEY_PIN) != pressed_level) {
      released = true;
      break;
    }
    delay(10);
  }
  delay(50);
  if (released) {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PWR_KEY_PIN, pressed_level);
  } else {
    log_line("PWR", "PWR key never released - waking on a 60 s timer instead.");
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
  }
#endif
  esp_deep_sleep_start();
}

}  // namespace canary::hal

#endif  // CD_FLAVOR_NIGHTSTAND && CD_AMOLED_GLASS
