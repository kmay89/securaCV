// src/playground/playground_ui.cpp — Dev Playground glass (FEATURE_PLAYGROUND).
//
// A bench instrument face for the 4.3B: the left third is the always-on
// PIN TRACKER (every terminal line + internal pin group, used/open icon +
// live state — the playground doc's budget table rendered live); the rest
// is one card per station, tap-through to wiring instructions + actions.
// Self-contained Quiet-Glass-flavored styling (local palette, stock
// Montserrat) so bench mode never depends on the Character/settings
// engines it bypasses. Dual-major LVGL: sticks to the v8/v9-stable API
// subset (objects, labels, styles) — no widgets, no indev.
#include "flavor_config.h"
#if (((defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND) || (defined(FEATURE_DEVMODE) && FEATURE_DEVMODE))) && defined(CD_FLAVOR_DASH)

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

#include "pins.h"
#include "playground.h"
#include "playground_ui.h"

namespace canary::playground {

namespace {

// ── Quiet Glass tokens, local copy (bench mode bypasses the theme engine) ──
lv_color_t c_bg()      { return lv_color_hex(0x000000); }
lv_color_t c_surface() { return lv_color_hex(0x141414); }
lv_color_t c_raised()  { return lv_color_hex(0x1C1C1C); }
lv_color_t c_edge()    { return lv_color_hex(0x262626); }
lv_color_t c_text()    { return lv_color_hex(0xEDEDED); }
lv_color_t c_muted()   { return lv_color_hex(0x9A9A9A); }
lv_color_t c_faint()   { return lv_color_hex(0x5C5C5C); }
lv_color_t c_ok()      { return lv_color_hex(0x4CAF50); }
lv_color_t c_warn()    { return lv_color_hex(0xFFB300); }
lv_color_t c_alert()   { return lv_color_hex(0xE53935); }
lv_color_t c_accent()  { return lv_color_hex(0x3AA3FF); }

// ── Geometry ────────────────────────────────────────────────────────────
constexpr int16_t W = 800, H = 480;
constexpr int16_t HDR_H = 42;
constexpr int16_t TRACK_W = 292;               // pin tracker column
constexpr int16_t GRID_X = TRACK_W + 8;        // stations area
constexpr int16_t CARD_W = 244, CARD_H = 80, CARD_GAP = 5;  // 2 x 5 grid

struct Rect {
  int16_t x, y, w, h;
  bool hit(int16_t px, int16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

// ── Station metadata (titles, wiring instructions, actions) ─────────────

enum Station : uint8_t {
  ST_DOORBELL = 0, ST_INTRUSION, ST_CHIME, ST_STROBE,
  ST_LIGHT, ST_TOF, ST_CAPTOUCH, ST_RS485, ST_CAN, ST_I2C, ST_COUNT
};

struct StationMeta {
  const char* title;
  const char* where;   // terminal it lives on
  const char* instructions;
  const char* act1;    // nullptr = no button
  const char* act2;
};

const StationMeta META[ST_COUNT] = {
    {"Doorbell", "DI0",
     "Wire (supply OFF first):\n"
     "1. External 5-24 V DC supply \"+\" -> DI COM.\n"
     "2. Doorbell button between supply \"-\" and DI0\n"
     "   (dry contact; wet/NPN/PNP also OK, 5-36 V).\n"
     "3. Power the supply, press the button.\n"
     "Presses count here and pulse DO0 (the \"ding\" link).\n"
     "Safe: DI is optocoupled - it never touches the S3.",
     nullptr, nullptr},
    {"Intrusion", "DI1",
     "PIR / reed contact / laser break-beam receiver:\n"
     "1. Supply \"+\" -> DI COM (shared with DI0).\n"
     "2. Sensor output (relay or open-collector) between\n"
     "   supply \"-\" and DI1.\n"
     "3. For a beam-gap sensor: aim emitter at receiver;\n"
     "   breaking the beam trips DI1 - the held-ms readout\n"
     "   is your gap timing.",
     nullptr, nullptr},
    {"Chime out", "DO0",
     "Isolated open-drain output (max 450 mA sink):\n"
     "1. External supply \"+\" -> load \"+\" (chime, LED,\n"
     "   relay coil - add a flyback diode across coils).\n"
     "2. Load \"-\" -> DO0. Supply \"-\" -> the isolated GND.\n"
     "3. PULSE drives 1.5 s; LATCH holds 30 s max.\n"
     "Every drive is bounded - outputs release themselves.",
     "PULSE 1.5s", "LATCH 30s"},
    {"Strobe out", "DO1",
     "Second isolated output - same wiring as DO0.\n"
     "Use it for a strobe/siren candidate while DO0\n"
     "holds the chime, to test both alert voices at\n"
     "once. PULSE = 1.5 s, LATCH auto-releases at 30 s.",
     "PULSE 1.5s", "LATCH 30s"},
    {"Light", "I2C",
     "Ambient light sensor on the I2C terminal:\n"
     "1. VEML7700 (addr 0x10, preferred) or BH1750 with\n"
     "   ADDR strapped HIGH (0x5C - its default 0x23 is\n"
     "   the CH422G's, do not use it!).\n"
     "2. VOUT->VCC GND->GND SDA->SDA SCL->SCL.\n"
     "   Check VOUT is 5 V or 3.3 V per your sensor.\n"
     "Hot-plug OK - the census attaches it in ~3 s.",
     nullptr, nullptr},
    {"ToF range", "I2C",
     "VL53L0X time-of-flight on the I2C terminal\n"
     "(addr 0x29). Wire like the light sensor.\n"
     "Live mm readout; dropping under the TRIP\n"
     "threshold counts one trip - point it across a\n"
     "doorway and it is your laser-gap prototype.\n"
     "Bench driver: uncalibrated, +/- a few percent.",
     "TRIP: %u mm", nullptr},
    {"Cap touch", "I2C",
     "MPR121 12-pad controller (addr 0x5A).\n"
     "Shell-thickness test (printed plastic coupons):\n"
     "1. Tape a coupon over an electrode.\n"
     "2. Cycle SENSITIVITY until a finger through the\n"
     "   coupon registers reliably, no ghost touches.\n"
     "3. Note preset vs thickness in the bench log -\n"
     "   that pair is the enclosure design input.",
     "SENS: %s", nullptr},
    {"RS485", "A/B",
     "RS485 - the industrial serial bus. One A/B pair\n"
     "daisy-chains up to 32 devices on two wires: energy\n"
     "meters, PLCs, VFDs, HVAC controllers, alarm panels.\n"
     "1. A -> A, B -> B (share a ground reference).\n"
     "2. Match the line: 9600 8N1 here (Modbus RTU).\n"
     "3. 120 ohm terminators on long runs, both ends.\n"
     "PROBE reads holding register 0 of slave 1 and shows\n"
     "the value. Shares GPIO44/43 with the USB console.",
     "PROBE", nullptr},
    {"CAN bus", "H/L",
     "CAN 2.0 / TWAI - the vehicle & automation bus. Two\n"
     "wires (H/L), multi-master, every node hears every\n"
     "frame: cars (OBD-II / J1939), CANopen building gear,\n"
     "gate / barrier controllers, fleet telematics.\n"
     "1. H -> H, L -> L. 120 ohm at BOTH bus ends.\n"
     "2. Match the bit rate: 500 kbit/s here.\n"
     "SEND FRAME transmits one test frame; received frames\n"
     "are counted and logged. Dedicated transceiver.",
     "SEND FRAME", nullptr},
    {"I2C census", "bus",
     "Live scan of the shared GPIO8/9 bus (every 3 s).\n"
     "Reserved here: 0x23/0x24/0x26/0x38 = CH422G\n"
     "commands, 0x5D/0x14 = GT911. A sensor set to a\n"
     "reserved address will misbehave AND can glitch\n"
     "backlight/touch - re-strap it before wiring.",
     nullptr, nullptr},
};

// ── Pin tracker rows ────────────────────────────────────────────────────

enum RowKind : uint8_t {
  ROW_DI0, ROW_DI1, ROW_DICOM, ROW_DO0, ROW_DO1,
  ROW_I2C, ROW_VOUT, ROW_RS485, ROW_CAN, ROW_VIN,
  ROW_LCD, ROW_TOUCH, ROW_USB, ROW_SD, ROW_FREE, ROW_COUNT
};

struct RowDef { const char* name; RowKind kind; };

const RowDef ROWS[ROW_COUNT] = {
    {"DI0 doorbell", ROW_DI0},   {"DI1 intrusion", ROW_DI1},
    {"DI COM", ROW_DICOM},       {"DO0 chime", ROW_DO0},
    {"DO1 strobe", ROW_DO1},     {"I2C 8/9", ROW_I2C},
    {"VOUT", ROW_VOUT},          {"RS485 44/43", ROW_RS485},
    {"CAN 15/16", ROW_CAN},      {"VIN 6-36V", ROW_VIN},
    {"LCD x21", ROW_LCD},        {"Touch INT 4", ROW_TOUCH},
    {"USB 19/20", ROW_USB},      {"SD 11-13", ROW_SD},
    {"Free GPIO", ROW_FREE},
};

// ── Widget handles ──────────────────────────────────────────────────────

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_hdr_status = nullptr;
lv_obj_t* s_row_icon[ROW_COUNT];
lv_obj_t* s_row_val[ROW_COUNT];
lv_obj_t* s_card[ST_COUNT];
lv_obj_t* s_card_val[ST_COUNT];
lv_obj_t* s_card_sub[ST_COUNT];
Rect      s_card_rect[ST_COUNT];

// Detail panel (one station expanded).
lv_obj_t* s_detail = nullptr;   // nullptr = grid showing
int       s_detail_st = -1;
lv_obj_t* s_detail_val = nullptr;
lv_obj_t* s_btn1 = nullptr, * s_btn2 = nullptr;
lv_obj_t* s_btn1_lbl = nullptr, * s_btn2_lbl = nullptr;
Rect      s_btn1_rect, s_btn2_rect, s_back_rect;

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t col,
                   const char* txt) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, col, 0);
  lv_label_set_text(l, txt);
  return l;
}

lv_obj_t* mk_panel(lv_obj_t* parent, lv_color_t bg) {
  lv_obj_t* p = lv_obj_create(parent);
  lv_obj_set_style_bg_color(p, bg, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(p, c_edge(), 0);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_radius(p, 6, 0);
  lv_obj_set_style_pad_all(p, 6, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  return p;
}

void detail_close() {
  if (s_detail) {
    lv_obj_del(s_detail);
    s_detail = nullptr;
    s_detail_st = -1;
    s_detail_val = nullptr;
    s_btn1 = s_btn2 = s_btn1_lbl = s_btn2_lbl = nullptr;
  }
  for (int i = 0; i < ST_COUNT; i++)
    lv_obj_clear_flag(s_card[i], LV_OBJ_FLAG_HIDDEN);
}

void detail_open(int st);

// Refresh the action-button labels that embed live values.
void detail_refresh_buttons(const PgState& g) {
  if (s_detail_st < 0) return;
  char buf[40];
  if (s_detail_st == ST_TOF && s_btn1_lbl) {
    snprintf(buf, sizeof(buf), "TRIP: %u mm", (unsigned)g.tof.trip_mm);
    lv_label_set_text(s_btn1_lbl, buf);
  }
  if (s_detail_st == ST_CAPTOUCH && s_btn1_lbl) {
    snprintf(buf, sizeof(buf), "SENS: %s", pad_preset_name(g.pad.preset));
    lv_label_set_text(s_btn1_lbl, buf);
  }
}

void detail_open(int st) {
  detail_close();
  for (int i = 0; i < ST_COUNT; i++)
    lv_obj_add_flag(s_card[i], LV_OBJ_FLAG_HIDDEN);

  s_detail_st = st;
  s_detail = mk_panel(s_scr, c_surface());
  lv_obj_set_pos(s_detail, GRID_X, HDR_H + 4);
  lv_obj_set_size(s_detail, W - GRID_X - 6, H - HDR_H - 10);
  lv_obj_set_style_pad_all(s_detail, 12, 0);

  const StationMeta& m = META[st];

  char title[48];
  snprintf(title, sizeof(title), "%s  •  %s", m.title, m.where);
  lv_obj_t* t = mk_label(s_detail, &lv_font_montserrat_24, c_text(), title);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* back = mk_label(s_detail, &lv_font_montserrat_16, c_accent(),
                            LV_SYMBOL_LEFT "  back");
  lv_obj_align(back, LV_ALIGN_TOP_RIGHT, 0, 4);
  s_back_rect = {GRID_X + 300, HDR_H, W - GRID_X - 300, 60};

  s_detail_val = mk_label(s_detail, &lv_font_montserrat_36, c_accent(), "-");
  lv_obj_align(s_detail_val, LV_ALIGN_TOP_LEFT, 0, 40);

  lv_obj_t* body = mk_label(s_detail, &lv_font_montserrat_14, c_muted(),
                            m.instructions);
  lv_obj_set_width(body, W - GRID_X - 40);
  lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(body, 4, 0);
  lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 96);

  const int16_t by = H - HDR_H - 10 - 62;   // buttons at panel bottom
  if (m.act1) {
    s_btn1 = mk_panel(s_detail, c_raised());
    lv_obj_set_style_border_color(s_btn1, c_accent(), 0);
    lv_obj_set_pos(s_btn1, 0, by - 24);
    lv_obj_set_size(s_btn1, 220, 48);
    s_btn1_lbl = mk_label(s_btn1, &lv_font_montserrat_16, c_text(), m.act1);
    lv_obj_center(s_btn1_lbl);
    s_btn1_rect = {(int16_t)(GRID_X + 12), (int16_t)(HDR_H + 4 + by - 24 + 12),
                   220, 48};
  }
  if (m.act2) {
    s_btn2 = mk_panel(s_detail, c_raised());
    lv_obj_set_style_border_color(s_btn2, c_warn(), 0);
    lv_obj_set_pos(s_btn2, 236, by - 24);
    lv_obj_set_size(s_btn2, 220, 48);
    s_btn2_lbl = mk_label(s_btn2, &lv_font_montserrat_16, c_text(), m.act2);
    lv_obj_center(s_btn2_lbl);
    s_btn2_rect = {(int16_t)(GRID_X + 12 + 236),
                   (int16_t)(HDR_H + 4 + by - 24 + 12), 220, 48};
  }
  detail_refresh_buttons(state());
}

// ── Row + card value rendering ──────────────────────────────────────────

void set_row(int r, const char* icon, lv_color_t icol, const char* val,
             lv_color_t vcol) {
  lv_label_set_text(s_row_icon[r], icon);
  lv_obj_set_style_text_color(s_row_icon[r], icol, 0);
  lv_label_set_text(s_row_val[r], val);
  lv_obj_set_style_text_color(s_row_val[r], vcol, 0);
}

void update_tracker(const PgState& g) {
  char b[48];
  for (int r = 0; r < ROW_COUNT; r++) {
    switch (ROWS[r].kind) {
      case ROW_DI0:
        snprintf(b, sizeof(b), g.di0.active ? "ACTIVE  n=%lu" : "idle  n=%lu",
                 (unsigned long)g.di0.events);
        set_row(r, LV_SYMBOL_OK, g.di0.active ? c_ok() : c_muted(), b,
                g.di0.active ? c_ok() : c_muted());
        break;
      case ROW_DI1:
        snprintf(b, sizeof(b), g.di1.active ? "ACTIVE  n=%lu" : "idle  n=%lu",
                 (unsigned long)g.di1.events);
        set_row(r, LV_SYMBOL_OK, g.di1.active ? c_ok() : c_muted(), b,
                g.di1.active ? c_ok() : c_muted());
        break;
      case ROW_DICOM:
        set_row(r, LV_SYMBOL_OK, c_muted(), "input common", c_faint());
        break;
      case ROW_DO0:
        snprintf(b, sizeof(b), g.do0.sinking ? "ON  p=%lu" : "off  p=%lu",
                 (unsigned long)g.do0.pulses);
        set_row(r, LV_SYMBOL_OK, g.do0.sinking ? c_warn() : c_muted(), b,
                g.do0.sinking ? c_warn() : c_muted());
        break;
      case ROW_DO1:
        snprintf(b, sizeof(b), g.do1.sinking ? "ON  p=%lu" : "off  p=%lu",
                 (unsigned long)g.do1.pulses);
        set_row(r, LV_SYMBOL_OK, g.do1.sinking ? c_warn() : c_muted(), b,
                g.do1.sinking ? c_warn() : c_muted());
        break;
      case ROW_I2C: {
        // Count external guests (not the burned-in GT911/CH422G slots).
        int ext = 0;
        for (int i = 0; i < g.bus.count; i++)
          if (!i2c_addr_reserved(g.bus.addrs[i])) ext++;
        snprintf(b, sizeof(b), "shared bus • %d guest%s", ext,
                 ext == 1 ? "" : "s");
        set_row(r, LV_SYMBOL_OK, ext ? c_ok() : c_muted(), b,
                ext ? c_ok() : c_muted());
        break;
      }
      case ROW_VOUT:
        set_row(r, LV_SYMBOL_WARNING, c_warn(), "5V dflt • check!", c_warn());
        break;
      case ROW_RS485:
        if (g.rs485.replies) {
          snprintf(b, sizeof(b), "RTU • reg0=%u", (unsigned)g.rs485.last_val);
          set_row(r, LV_SYMBOL_OK, c_ok(), b, c_ok());
        } else {
          set_row(r, LV_SYMBOL_MINUS, g.rs485.polls ? c_warn() : c_faint(),
                  g.rs485.polls ? "no reply • check A/B" : "Modbus • tap to probe",
                  g.rs485.polls ? c_warn() : c_faint());
        }
        break;
      case ROW_CAN:
        if (g.can.rx) {
          snprintf(b, sizeof(b), "CAN • rx=%lu", (unsigned long)g.can.rx);
          set_row(r, LV_SYMBOL_OK, c_ok(), b, c_ok());
        } else {
          set_row(r, LV_SYMBOL_MINUS, g.can.tx ? c_warn() : c_faint(),
                  g.can.tx ? "sent • no rx yet" : "TWAI • tap to send",
                  g.can.tx ? c_warn() : c_faint());
        }
        break;
      case ROW_VIN:
        set_row(r, LV_SYMBOL_OK, c_muted(), "power in", c_faint());
        break;
      case ROW_LCD:
        set_row(r, LV_SYMBOL_OK, c_muted(), "panel (internal)", c_faint());
        break;
      case ROW_TOUCH:
        set_row(r, LV_SYMBOL_OK, c_muted(), "GT911 (internal)", c_faint());
        break;
      case ROW_USB:
        set_row(r, LV_SYMBOL_OK, c_muted(), "console/flash", c_faint());
        break;
      case ROW_SD:
        set_row(r, LV_SYMBOL_MINUS, c_faint(), "reserved • unused", c_faint());
        break;
      case ROW_FREE:
        set_row(r, LV_SYMBOL_CLOSE, c_alert(), "none - fully loaded",
                c_alert());
        break;
      default:
        break;
    }
  }
}

void card_value(int st, const PgState& g, char* out, size_t cap,
                const char** sub, lv_color_t* col) {
  *sub = "";
  *col = c_muted();
  switch (st) {
    case ST_DOORBELL:
      snprintf(out, cap, "%lu", (unsigned long)g.di0.events);
      *sub = g.di0.active ? "PRESSED" : "presses";
      *col = g.di0.active ? c_ok() : c_text();
      break;
    case ST_INTRUSION:
      if (g.di1.active) snprintf(out, cap, "%lus", (unsigned long)(g.di1.active_ms / 1000));
      else snprintf(out, cap, "%lu", (unsigned long)g.di1.events);
      *sub = g.di1.active ? "TRIPPED" : "events";
      *col = g.di1.active ? c_alert() : c_text();
      break;
    case ST_CHIME:
      snprintf(out, cap, "%s", g.do0.sinking ? "ON" : "off");
      *sub = g.do0.latched ? "latched • auto-off" : "tap for controls";
      *col = g.do0.sinking ? c_warn() : c_text();
      break;
    case ST_STROBE:
      snprintf(out, cap, "%s", g.do1.sinking ? "ON" : "off");
      *sub = g.do1.latched ? "latched • auto-off" : "tap for controls";
      *col = g.do1.sinking ? c_warn() : c_text();
      break;
    case ST_LIGHT:
      if (g.light.found) {
        snprintf(out, cap, "%.0f lx", (double)g.light.lux);
        *sub = g.light.addr == 0x10 ? "VEML7700" : "BH1750@0x5C";
        *col = c_text();
      } else {
        snprintf(out, cap, "-");
        *sub = "not attached";
      }
      break;
    case ST_TOF:
      if (g.tof.found && g.tof.valid) {
        snprintf(out, cap, "%u mm", (unsigned)g.tof.mm);
        *sub = g.tof.tripped ? "TRIPPED" : "ranging";
        *col = g.tof.tripped ? c_alert() : c_text();
      } else if (g.tof.found) {
        snprintf(out, cap, "> range");
        *sub = "ranging";
      } else {
        snprintf(out, cap, "-");
        *sub = "not attached";
      }
      break;
    case ST_CAPTOUCH:
      if (g.pad.found) {
        int n = 0;
        for (int i = 0; i < 12; i++) if (g.pad.touched_bits & (1 << i)) n++;
        snprintf(out, cap, "%d/12", n);
        *sub = pad_preset_name(g.pad.preset);
        *col = n ? c_ok() : c_text();
      } else {
        snprintf(out, cap, "-");
        *sub = "not attached";
      }
      break;
    case ST_RS485:
      if (g.rs485.last_ok) {
        snprintf(out, cap, "%u", (unsigned)g.rs485.last_val);
        *sub = "reg0 • slave 1";
        *col = c_ok();
      } else {
        snprintf(out, cap, "%s", g.rs485.polls ? "?" : "-");
        *sub = g.rs485.polls ? "no reply" : "tap to probe";
        *col = g.rs485.polls ? c_warn() : c_text();
      }
      break;
    case ST_CAN:
      if (g.can.rx) {
        snprintf(out, cap, "%lu", (unsigned long)g.can.rx);
        *sub = "frames in";
        *col = c_ok();
      } else {
        snprintf(out, cap, "%s", g.can.tx ? "tx" : "-");
        *sub = g.can.tx ? "sent • listening" : "tap to send";
        *col = g.can.tx ? c_warn() : c_text();
      }
      break;
    case ST_I2C: {
      int ext = 0;
      for (int i = 0; i < g.bus.count; i++)
        if (!i2c_addr_reserved(g.bus.addrs[i])) ext++;
      snprintf(out, cap, "%d", ext);
      *sub = "external devices";
      *col = c_text();
      break;
    }
    default:
      out[0] = 0;
      break;
  }
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────

void playground_ui_create() {
  s_scr = lv_scr_act();
  lv_obj_set_style_bg_color(s_scr, c_bg(), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_clean(s_scr);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

  // Header.
  lv_obj_t* title = mk_label(s_scr, &lv_font_montserrat_20, c_text(),
#if defined(FEATURE_DEVMODE) && FEATURE_DEVMODE && !(defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND)
                             "DEV MODE  •  hold 3s to exit");
#else
                             "DEV PLAYGROUND");
#endif
  lv_obj_set_pos(title, 10, 8);
  s_hdr_status = mk_label(s_scr, &lv_font_montserrat_14, c_muted(),
                          "bench mode • no network • " BOARD_NAME);
  lv_obj_align(s_hdr_status, LV_ALIGN_TOP_RIGHT, -10, 12);

  // Pin tracker panel.
  lv_obj_t* track = mk_panel(s_scr, c_surface());
  lv_obj_set_pos(track, 4, HDR_H + 4);
  lv_obj_set_size(track, TRACK_W, H - HDR_H - 10);
  lv_obj_t* th = mk_label(track, &lv_font_montserrat_14, c_faint(),
                          "PIN TRACKER - used " LV_SYMBOL_OK "  open "
                          LV_SYMBOL_MINUS "  check " LV_SYMBOL_WARNING);
  lv_obj_set_pos(th, 2, 0);
  for (int r = 0; r < ROW_COUNT; r++) {
    const int16_t y = (int16_t)(24 + r * 27);
    s_row_icon[r] = mk_label(track, &lv_font_montserrat_14, c_muted(),
                             LV_SYMBOL_MINUS);
    lv_obj_set_pos(s_row_icon[r], 2, y);
    lv_obj_t* name = mk_label(track, &lv_font_montserrat_14, c_text(),
                              ROWS[r].name);
    lv_obj_set_pos(name, 26, y);
    s_row_val[r] = mk_label(track, &lv_font_montserrat_12, c_muted(), "");
    lv_obj_set_pos(s_row_val[r], 140, (int16_t)(y + 2));
  }

  // Station cards, 2 x 5.
  for (int i = 0; i < ST_COUNT; i++) {
    const int col = i % 2, row = i / 2;
    const int16_t x = (int16_t)(GRID_X + col * (CARD_W + CARD_GAP));
    const int16_t y = (int16_t)(HDR_H + 4 + row * (CARD_H + CARD_GAP));
    s_card[i] = mk_panel(s_scr, c_surface());
    lv_obj_set_pos(s_card[i], x, y);
    lv_obj_set_size(s_card[i], CARD_W, CARD_H);
    s_card_rect[i] = {x, y, CARD_W, CARD_H};

    char t[40];
    snprintf(t, sizeof(t), "%s • %s", META[i].title, META[i].where);
    lv_obj_t* tl = mk_label(s_card[i], &lv_font_montserrat_14, c_muted(), t);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, 0);
    s_card_val[i] = mk_label(s_card[i], &lv_font_montserrat_28, c_text(), "-");
    lv_obj_align(s_card_val[i], LV_ALIGN_LEFT_MID, 0, 8);
    s_card_sub[i] = mk_label(s_card[i], &lv_font_montserrat_12, c_faint(), "");
    lv_obj_align(s_card_sub[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }
}

void playground_ui_update(const PgState& g) {
  if (!s_scr) return;
  update_tracker(g);

  char v[32];
  const char* sub;
  lv_color_t col;
  for (int i = 0; i < ST_COUNT; i++) {
    card_value(i, g, v, sizeof(v), &sub, &col);
    lv_label_set_text(s_card_val[i], v);
    lv_obj_set_style_text_color(s_card_val[i], col, 0);
    lv_label_set_text(s_card_sub[i], sub);
  }

  if (s_detail_st >= 0 && s_detail_val) {
    card_value(s_detail_st, g, v, sizeof(v), &sub, &col);
    char big[48];
    snprintf(big, sizeof(big), "%s  %s", v, sub);
    lv_label_set_text(s_detail_val, big);
    lv_obj_set_style_text_color(s_detail_val, col, 0);
    detail_refresh_buttons(g);
  }
}

void playground_ui_handle_tap(int16_t x, int16_t y) {
  if (s_detail) {
    if (s_btn1 && s_btn1_rect.hit(x, y)) {
      switch (s_detail_st) {
        case ST_CHIME:    action_do_pulse(0); break;
        case ST_STROBE:   action_do_pulse(1); break;
        case ST_TOF:      action_tof_cycle_trip(); break;
        case ST_CAPTOUCH: action_pad_cycle_preset(); break;
        case ST_RS485:    action_rs485_probe(); break;
        case ST_CAN:      action_can_send(); break;
        default: break;
      }
      playground_ui_update(state());
      return;
    }
    if (s_btn2 && s_btn2_rect.hit(x, y)) {
      if (s_detail_st == ST_CHIME) action_do_latch_toggle(0);
      if (s_detail_st == ST_STROBE) action_do_latch_toggle(1);
      playground_ui_update(state());
      return;
    }
    detail_close();   // any other tap = back
    playground_ui_update(state());
    return;
  }
  for (int i = 0; i < ST_COUNT; i++) {
    if (s_card_rect[i].hit(x, y)) {
      detail_open(i);
      playground_ui_update(state());
      return;
    }
  }
}

}  // namespace canary::playground

#endif  // (FEATURE_PLAYGROUND || FEATURE_DEVMODE) && CD_FLAVOR_DASH
