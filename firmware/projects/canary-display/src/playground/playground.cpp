// src/playground/playground.cpp — Dev Playground driver (FEATURE_PLAYGROUND).
//
// Owns the bench-mode peripherals on the Waveshare 4.3B terminal block:
// isolated DI/DO through the CH422G (via the dash HAL's expander calls),
// and the I2C sensor header's known guests (VEML7700/BH1750 light,
// VL53L0X time-of-flight, MPR121 capacitive touch) with minimal in-repo
// drivers — no new library dependencies, so the Arduino/PlatformIO
// library matrices stay untouched. Sensors are hot-pluggable: a periodic
// bus census probes for newcomers and re-inits a sensor that reappears.
//
// Safety posture (the reason this mode exists — see the playground doc):
//   - Nothing here touches a raw ESP32 GPIO; every external wire lands on
//     an isolated or bused interface.
//   - DO channels only ever drive on explicit on-glass action, and every
//     drive is bounded: pulses are 1.5 s, latches auto-release after 30 s.
//   - No network stack runs in this mode (main.cpp never initializes it).
#include <config.h>
#if defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND && defined(CD_FLAVOR_DASH)

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <stdarg.h>

#include "pins.h"
#include "canary/playground/playground.h"
#include "canary/playground/playground_ui.h"
#include "canary/hal/display.h"
#include "canary/ui/lvgl_port.h"
#include "canary/version.h"
#include "boot/boot_banner.h"

namespace canary::playground {

namespace {

// ── Tunables ────────────────────────────────────────────────────────────

// Interpreted polarity of an energized isolated input as seen on the
// expander bit. The optocoupler pulls the EXIO line LOW when the field
// side is energized per Waveshare's demo sources — VERIFY at bench and
// flip here if your revision reads inverted (the UI shows the raw bit
// alongside the interpretation exactly so this is easy to catch).
constexpr int  DI_ACTIVE_LEVEL   = 0;
constexpr uint32_t DI_POLL_MS    = 20;    // 50 Hz, 2-sample debounce
constexpr uint32_t DO_PULSE_MS   = 1500;  // bounded pulse
constexpr uint32_t DO_LATCH_MS   = 30000; // latch safety auto-off
constexpr uint32_t LIGHT_POLL_MS = 500;
constexpr uint32_t TOF_POLL_MS   = 100;
constexpr uint32_t PAD_POLL_MS   = 50;
constexpr uint32_t SCAN_EVERY_MS = 3000;  // hot-plug census
constexpr uint32_t SNAP_EVERY_MS = 1000;  // serial snapshot
constexpr uint32_t UI_EVERY_MS   = 150;

// Sensor addresses (see the board README's reserved-address table).
constexpr uint8_t ADDR_VEML7700 = 0x10;
constexpr uint8_t ADDR_BH1750   = 0x5C;  // ADDR pin HIGH; 0x23 is CH422G's!
constexpr uint8_t ADDR_VL53L0X  = 0x29;
constexpr uint8_t ADDR_MPR121   = 0x5A;

PgState g;

// MPR121 sensitivity presets: touch/release deltas. Lower = more
// sensitive = triggers through thicker plastic. The shell-thickness test
// piece procedure lives in the playground doc §Cap touch.
struct PadPreset { const char* name; uint8_t touch; uint8_t release; };
constexpr PadPreset PAD_PRESETS[] = {
    {"contact", 12, 6},
    {"2mm shell", 8, 4},
    {"4mm shell", 4, 2},
    {"max gain", 2, 1},
};
constexpr uint8_t PAD_PRESET_COUNT = sizeof(PAD_PRESETS) / sizeof(PAD_PRESETS[0]);

constexpr uint16_t TOF_TRIPS[] = {50, 100, 200, 400};

// ── Small I2C helpers (shared Wire bus with GT911 + CH422G) ─────────────

bool i2c_ack(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool wr8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool rd(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

bool rd8(uint8_t addr, uint8_t reg, uint8_t* v) { return rd(addr, reg, v, 1); }

// ── Serial protocol (PG1 — playground doc §Comms) ───────────────────────

void say_evt(const char* station, const char* fmt, ...) {
  char body[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  Serial.printf("PG1 %lu EVT %s %s\r\n", (unsigned long)millis(), station, body);
}

// ── Light: VEML7700 (preferred) or BH1750 strapped to 0x5C ──────────────

bool veml_init() {
  // ALS_CONF (0x00): gain x1, 100 ms integration, no interrupt, enabled.
  Wire.beginTransmission(ADDR_VEML7700);
  Wire.write(0x00);
  Wire.write(0x00);  // LSB
  Wire.write(0x00);  // MSB
  return Wire.endTransmission() == 0;
}

bool veml_read(float* lux) {
  Wire.beginTransmission(ADDR_VEML7700);
  Wire.write(0x04);  // ALS output
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)ADDR_VEML7700, 2) != 2) return false;
  const uint16_t raw = (uint16_t)Wire.read() | ((uint16_t)Wire.read() << 8);
  *lux = raw * 0.0576f;  // gain x1 @ 100 ms
  return true;
}

bool bh1750_init() {
  Wire.beginTransmission(ADDR_BH1750);
  Wire.write(0x01);  // power on
  if (Wire.endTransmission() != 0) return false;
  Wire.beginTransmission(ADDR_BH1750);
  Wire.write(0x10);  // continuous H-resolution
  return Wire.endTransmission() == 0;
}

bool bh1750_read(float* lux) {
  if (Wire.requestFrom((int)ADDR_BH1750, 2) != 2) return false;
  const uint16_t raw = ((uint16_t)Wire.read() << 8) | (uint16_t)Wire.read();
  *lux = raw / 1.2f;
  return true;
}

// ── ToF: VL53L0X minimal continuous driver ──────────────────────────────
//
// Deliberately small: power-on defaults + the reference stop-variable
// dance + back-to-back continuous mode (the essence of Pololu's
// startContinuous()). Good to a few percent on the bench — enough to
// exercise wiring, mounting, and the trip logic. If a production feature
// ever needs calibrated ranging, take the full Pololu driver instead.

uint8_t s_tof_stop_var = 0;

bool tof_init() {
  uint8_t model = 0;
  if (!rd8(ADDR_VL53L0X, 0xC0, &model) || model != 0xEE) return false;
  // Standard unlock/read of the stop variable (vendor init sequence).
  wr8(ADDR_VL53L0X, 0x88, 0x00);
  wr8(ADDR_VL53L0X, 0x80, 0x01);
  wr8(ADDR_VL53L0X, 0xFF, 0x01);
  wr8(ADDR_VL53L0X, 0x00, 0x00);
  rd8(ADDR_VL53L0X, 0x91, &s_tof_stop_var);
  wr8(ADDR_VL53L0X, 0x00, 0x01);
  wr8(ADDR_VL53L0X, 0xFF, 0x00);
  wr8(ADDR_VL53L0X, 0x80, 0x00);
  // Back-to-back continuous ranging.
  wr8(ADDR_VL53L0X, 0x80, 0x01);
  wr8(ADDR_VL53L0X, 0xFF, 0x01);
  wr8(ADDR_VL53L0X, 0x00, 0x00);
  wr8(ADDR_VL53L0X, 0x91, s_tof_stop_var);
  wr8(ADDR_VL53L0X, 0x00, 0x01);
  wr8(ADDR_VL53L0X, 0xFF, 0x00);
  wr8(ADDR_VL53L0X, 0x80, 0x00);
  return wr8(ADDR_VL53L0X, 0x00, 0x02);  // SYSRANGE_START: continuous
}

bool tof_read(uint16_t* mm) {
  uint8_t status = 0;
  if (!rd8(ADDR_VL53L0X, 0x13, &status)) return false;   // RESULT_INT_STATUS
  if (!(status & 0x07)) return false;                    // no new sample
  uint8_t r[2];
  if (!rd(ADDR_VL53L0X, 0x14 + 10, r, 2)) return false;  // range in mm
  *mm = ((uint16_t)r[0] << 8) | r[1];
  wr8(ADDR_VL53L0X, 0x0B, 0x01);                         // clear interrupt
  return true;
}

// ── Cap touch: MPR121 12-electrode controller ───────────────────────────

void pad_apply_thresholds(uint8_t touch, uint8_t release) {
  for (uint8_t i = 0; i < 12; i++) {
    wr8(ADDR_MPR121, 0x41 + 2 * i, touch);
    wr8(ADDR_MPR121, 0x42 + 2 * i, release);
  }
}

bool pad_init(uint8_t preset) {
  if (!wr8(ADDR_MPR121, 0x80, 0x63)) return false;  // soft reset
  delay(1);
  wr8(ADDR_MPR121, 0x5E, 0x00);  // stop mode for config
  // Baseline filter defaults (MPR121 quick-start values).
  wr8(ADDR_MPR121, 0x2B, 0x01); wr8(ADDR_MPR121, 0x2C, 0x01);
  wr8(ADDR_MPR121, 0x2D, 0x0E); wr8(ADDR_MPR121, 0x2E, 0x00);
  wr8(ADDR_MPR121, 0x2F, 0x01); wr8(ADDR_MPR121, 0x30, 0x05);
  wr8(ADDR_MPR121, 0x31, 0x01); wr8(ADDR_MPR121, 0x32, 0x00);
  wr8(ADDR_MPR121, 0x33, 0x00); wr8(ADDR_MPR121, 0x34, 0x00);
  wr8(ADDR_MPR121, 0x35, 0x00);
  pad_apply_thresholds(PAD_PRESETS[preset].touch, PAD_PRESETS[preset].release);
  wr8(ADDR_MPR121, 0x5C, 0x10);  // AFE: 16 uA charge current
  wr8(ADDR_MPR121, 0x5D, 0x24);  // filter: 0.5 us encoding, 4 samples
  return wr8(ADDR_MPR121, 0x5E, 0x8F);  // run: 12 electrodes, baseline on
}

bool pad_read(uint16_t* bits) {
  uint8_t b[2];
  if (!rd(ADDR_MPR121, 0x00, b, 2)) return false;
  *bits = (uint16_t)(b[0] | (b[1] << 8)) & 0x0FFF;
  return true;
}

// ── Isolated DI/DO ──────────────────────────────────────────────────────

void di_update(DiChannel* ch, bool raw, uint32_t now, const char* station) {
  const bool active = ((raw ? 1 : 0) == DI_ACTIVE_LEVEL);
  ch->raw = raw;
  if (active == ch->active) return;
  ch->active = active;
  if (active) {
    ch->events++;
    ch->last_edge_ms = now;
    ch->active_ms = 0;
    say_evt(station, "state=active count=%lu", (unsigned long)ch->events);
    // Doorbell "ding" link: a DI0 press rings whatever DO0 drives.
    if (ch == &g.di0) action_do_pulse(0);
  } else {
    ch->active_ms = now - ch->last_edge_ms;
    say_evt(station, "state=clear held_ms=%lu", (unsigned long)ch->active_ms);
  }
}

DoChannel* do_ch(int ch) { return ch == 0 ? &g.do0 : &g.do1; }
uint8_t    do_bit(int ch) { return ch == 0 ? ISO_OUT_BIT_DO0 : ISO_OUT_BIT_DO1; }

void do_drive(int ch, bool sink) {
  if (canary::hal::expander_od_set(do_bit(ch), sink)) {
    do_ch(ch)->sinking = sink;
    say_evt(ch == 0 ? "chime" : "strobe", "out=%s", sink ? "on" : "off");
  }
}

// ── Timers ──────────────────────────────────────────────────────────────

uint32_t t_di = 0, t_light = 0, t_tof = 0, t_pad = 0, t_scan = 0, t_snap = 0,
         t_ui = 0;

// Touch tap detection (the playground owns the glass; main.cpp's gesture
// policy is bypassed in this mode).
bool     s_touch_down = false;
int16_t  s_tx = 0, s_ty = 0;
uint32_t s_touch_down_ms = 0;

}  // namespace

// ── Public model / actions ──────────────────────────────────────────────

const PgState& state() { return g; }

const char* pad_preset_name(uint8_t idx) {
  return PAD_PRESETS[idx % PAD_PRESET_COUNT].name;
}

const char* i2c_device_name(uint8_t addr) {
  switch (addr) {
    case ADDR_VEML7700: return "VEML7700 light";
    case ADDR_BH1750:   return "BH1750 light";
    case 0x23:          return "CH422G (WR_OC)";
    case 0x24:          return "CH422G (WR_SET)";
    case 0x26:          return "CH422G (RD_IO)";
    case 0x38:          return "CH422G (WR_IO)";
    case ADDR_VL53L0X:  return "VL53L0X ToF";
    case 0x28: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
                        return "CAP1188 touch";
    case 0x39:          return "APDS-9960";
    case 0x44: case 0x45: return "SHT4x temp/RH";
    case ADDR_MPR121: case 0x5B: return "MPR121 touch";
    case 0x5D: case 0x14: return "GT911 panel touch";
    case 0x70:          return "TCA9548A mux";
    case 0x76: case 0x77: return "BME280/BMP280";
    default:            return "";
  }
}

bool i2c_addr_reserved(uint8_t addr) {
  return addr == 0x23 || addr == 0x24 || addr == 0x26 || addr == 0x38 ||
         addr == 0x5D || addr == 0x14;
}

void action_do_pulse(int ch) {
  DoChannel* c = do_ch(ch);
  c->latched = false;
  c->pulses++;
  c->auto_off_at_ms = millis() + DO_PULSE_MS;
  do_drive(ch, true);
}

void action_do_latch_toggle(int ch) {
  DoChannel* c = do_ch(ch);
  if (c->sinking) {
    c->latched = false;
    c->auto_off_at_ms = 0;
    do_drive(ch, false);
  } else {
    c->latched = true;
    c->auto_off_at_ms = millis() + DO_LATCH_MS;  // safety: never latched forever
    do_drive(ch, true);
  }
}

void action_pad_cycle_preset() {
  g.pad.preset = (uint8_t)((g.pad.preset + 1) % PAD_PRESET_COUNT);
  if (g.pad.found) {
    pad_apply_thresholds(PAD_PRESETS[g.pad.preset].touch,
                         PAD_PRESETS[g.pad.preset].release);
  }
  say_evt("captouch", "preset=%s", PAD_PRESETS[g.pad.preset].name);
}

void action_tof_cycle_trip() {
  constexpr uint8_t n = sizeof(TOF_TRIPS) / sizeof(TOF_TRIPS[0]);
  uint8_t i = 0;
  while (i < n && TOF_TRIPS[i] != g.tof.trip_mm) i++;
  g.tof.trip_mm = TOF_TRIPS[(i + 1) % n];
  say_evt("tof", "trip_mm=%u", (unsigned)g.tof.trip_mm);
}

// ── Census / hot-plug ───────────────────────────────────────────────────

namespace {

void census(uint32_t now) {
  (void)now;
  I2cCensus c;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    // The CH422G command addresses are write-decoded; probing them is
    // harmless but listing them as "devices" would be noise — the census
    // reports the bus as a wiring aid, reserved slots are annotated by
    // the UI via i2c_addr_reserved().
    if (!i2c_ack(a)) continue;
    if (c.count < sizeof(c.addrs)) c.addrs[c.count++] = a;
  }
  g.bus = c;

  // Hot-plug: (re)init any known sensor that answers but isn't bound yet.
  const bool veml = i2c_ack(ADDR_VEML7700);
  const bool bh   = !veml && i2c_ack(ADDR_BH1750);
  if ((veml || bh) && !g.light.found) {
    if (veml && veml_init()) { g.light.found = true; g.light.addr = ADDR_VEML7700; }
    else if (bh && bh1750_init()) { g.light.found = true; g.light.addr = ADDR_BH1750; }
    if (g.light.found) say_evt("light", "attached=0x%02X", g.light.addr);
  } else if (!veml && !bh && g.light.found) {
    g.light.found = false;
    say_evt("light", "detached=1");
  }

  const bool tof = i2c_ack(ADDR_VL53L0X);
  if (tof && !g.tof.found) {
    if (tof_init()) { g.tof.found = true; say_evt("tof", "attached=0x%02X", ADDR_VL53L0X); }
  } else if (!tof && g.tof.found) {
    g.tof.found = false;
    g.tof.valid = false;
    say_evt("tof", "detached=1");
  }

  const bool pad = i2c_ack(ADDR_MPR121);
  if (pad && !g.pad.found) {
    if (pad_init(g.pad.preset)) { g.pad.found = true; say_evt("captouch", "attached=0x%02X", ADDR_MPR121); }
  } else if (!pad && g.pad.found) {
    g.pad.found = false;
    g.pad.touched_bits = 0;
    say_evt("captouch", "detached=1");
  }
}

void snapshot() {
  Serial.printf(
      "PG1 %lu SNAP di0=%d di1=%d do0=%s do1=%s lux=%.1f tof_mm=%u "
      "tof_ok=%d pads=0x%03X preset=%s i2c=%u\r\n",
      (unsigned long)millis(), g.di0.active ? 1 : 0, g.di1.active ? 1 : 0,
      g.do0.sinking ? "on" : "off", g.do1.sinking ? "on" : "off",
      (double)(g.light.found ? g.light.lux : -1.0f),
      (unsigned)(g.tof.valid ? g.tof.mm : 0), g.tof.found ? 1 : 0,
      (unsigned)g.pad.touched_bits, PAD_PRESETS[g.pad.preset].name,
      (unsigned)g.bus.count);
}

}  // namespace

// ── Mode entry points ───────────────────────────────────────────────────

void playground_setup() {
  boot_line("              .--------.");
  boot_line("              | [] []  |        DEV PLAYGROUND (bench mode)");
  boot_line("              '--------'        no WiFi - no MQTT - no OTA");
  boot_separator();
  boot_kv("Board", BOARD_NAME);
  boot_kv("Doc",   "docs/hardware/dev_playground_43b.md");
  boot_kvf("DI",   "DI0/DI1 via CH422G (5-36V isolated, poll %lu ms)",
           (unsigned long)DI_POLL_MS);
  boot_kvf("DO",   "DO0/DO1 open-drain (pulse %lu ms, latch auto-off %lu s)",
           (unsigned long)DO_PULSE_MS, (unsigned long)(DO_LATCH_MS / 1000));
  boot_kvf("I2C",  "sensor header on GPIO%d/%d (shared with GT911+CH422G)",
           I2C_PIN_SDA, I2C_PIN_SCL);
  boot_blank();

  g.display_ok = canary::hal::display_init();
  if (g.display_ok) g.display_ok = canary::ui::lvgl_port_init();

  // Outputs released before anything else can happen — the safe state is
  // the boot state.
  canary::hal::expander_od_set(ISO_OUT_BIT_DO0 | ISO_OUT_BIT_DO1, false);

  if (g.display_ok) {
    playground_ui_create();
    playground_ui_update(g);
    lv_timer_handler();
    canary::hal::backlight_set(255);  // bench mode: glass always on
  }

  Serial.printf("PG1 HELLO board=%s fw=%s\r\n", BOARD_ID, CANARY_FW_VERSION);
  boot_scene_ready(
      "Playground up. Wire a peripheral to the terminal block,",
      "watch the glass and this console. Nothing leaves the bench.",
      NULL);
}

void playground_loop() {
  const uint32_t now = millis();

  // ── Touch: simple tap routing (down -> release inside 600 ms) ──
  const auto ts = canary::hal::touch_read();
  if (ts.touched && !s_touch_down) {
    s_touch_down = true;
    s_touch_down_ms = now;
    s_tx = ts.x;
    s_ty = ts.y;
  } else if (!ts.touched && s_touch_down) {
    s_touch_down = false;
    if (now - s_touch_down_ms < 600 && g.display_ok) {
      playground_ui_handle_tap(s_tx, s_ty);
    }
  }

  // ── Isolated inputs (debounced 2-sample agreement at 50 Hz) ──
  if (now - t_di >= DI_POLL_MS) {
    t_di = now;
    static uint8_t last_raw = 0xFF;
    uint8_t raw = 0;
    // A failed/short expander read skips the sample entirely — with
    // active-low inputs, a defaulted 0 would debounce two bus glitches
    // into phantom doorbell/intrusion events and a DO0 ding (review
    // catch). last_raw is left alone so the glitch also can't reset a
    // real edge's debounce.
    if (canary::hal::expander_read_inputs(&raw)) {
      if (raw == last_raw) {  // two consecutive identical reads = stable
        di_update(&g.di0, (raw & ISO_IN_BIT_DI0) != 0, now, "doorbell");
        di_update(&g.di1, (raw & ISO_IN_BIT_DI1) != 0, now, "intrusion");
      }
      last_raw = raw;
    }
    // Live hold timer while a channel is active (gap timing on the glass).
    if (g.di0.active) g.di0.active_ms = now - g.di0.last_edge_ms;
    if (g.di1.active) g.di1.active_ms = now - g.di1.last_edge_ms;
  }

  // ── Output safety timers ──
  for (int ch = 0; ch < 2; ch++) {
    DoChannel* c = do_ch(ch);
    if (c->sinking && c->auto_off_at_ms &&
        (int32_t)(now - c->auto_off_at_ms) >= 0) {
      c->latched = false;
      c->auto_off_at_ms = 0;
      do_drive(ch, false);
    }
  }

  // ── Sensors ──
  if (g.light.found && now - t_light >= LIGHT_POLL_MS) {
    t_light = now;
    float lux = 0;
    const bool ok = (g.light.addr == ADDR_VEML7700) ? veml_read(&lux)
                                                    : bh1750_read(&lux);
    if (ok) g.light.lux = lux;
  }

  if (g.tof.found && now - t_tof >= TOF_POLL_MS) {
    t_tof = now;
    uint16_t mm = 0;
    if (tof_read(&mm)) {
      g.tof.valid = (mm > 0 && mm < 2000);
      g.tof.mm = mm;
      const bool tripped = g.tof.valid && mm < g.tof.trip_mm;
      if (tripped && !g.tof.tripped) {
        g.tof.trips++;
        say_evt("tof", "trip=1 mm=%u count=%lu", (unsigned)mm,
                (unsigned long)g.tof.trips);
      }
      g.tof.tripped = tripped;
    }
  }

  if (g.pad.found && now - t_pad >= PAD_POLL_MS) {
    t_pad = now;
    uint16_t bits = 0;
    if (pad_read(&bits)) {
      if (bits & ~g.pad.touched_bits) {  // any new electrode down
        g.pad.events++;
        say_evt("captouch", "pads=0x%03X count=%lu", (unsigned)bits,
                (unsigned long)g.pad.events);
      }
      g.pad.touched_bits = bits;
    }
  }

  if (now - t_scan >= SCAN_EVERY_MS) {
    t_scan = now;
    census(now);
  }

  if (now - t_snap >= SNAP_EVERY_MS) {
    t_snap = now;
    snapshot();
  }

  if (g.display_ok && now - t_ui >= UI_EVERY_MS) {
    t_ui = now;
    playground_ui_update(g);
  }

  if (g.display_ok) lv_timer_handler();
  delay(5);
}

}  // namespace canary::playground

#endif  // FEATURE_PLAYGROUND && CD_FLAVOR_DASH
