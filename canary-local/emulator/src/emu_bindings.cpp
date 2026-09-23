// canary-local/emulator/src/emu_bindings.cpp — the scenario console.
//
// Exported knobs the page uses to stage teaching moments: virtual-time
// scale (watch staleness deadlines and the bird's slow feelings without
// waiting an hour), NVS preseed/wipe (provisioned device vs true first
// boot), deterministic RNG for reproducible screenshots. Link-state and
// MQTT injection live next to their shims (emu_net.cpp, emu_mqtt.cpp).
#include <emscripten.h>
#include <lvgl.h>
#include <stdint.h>

#include <string>

#include "emu_bus.h"
#include "canary/ui/character.h"
#include "canary/glass_settings.h"

extern "C" {

// ── Character knobs (the lab's style rail) ──────────────────────────────
// Same three moves the on-glass picker makes (settings_ui.cpp): mutate the
// setting, mark it dirty (the debounced committer persists it, so an
// emulated reboot keeps the choice), apply the look. The render tick's
// ground-flip rebuild then repaints the live face — the page never touches
// pixels, it turns the same knob a finger would. Names/captions/ring order
// are read back from the firmware table so the page can never drift from
// the truth it demos.
// Bounds checks refuse garbage at the door. The firmware behind them
// already clamps (sanitize(), clamp_idx, ring modulo) — this is a bench
// API being polite, not the safety layer (review suggestion adopted).
static bool ch_valid(int n) {
  return n >= 0 && n < (int)canary::ui::character_count();
}
EMSCRIPTEN_KEEPALIVE void emu_apply_character(int n) {
  if (!ch_valid(n)) return;
  canary::glass::settings_mut().character = (uint8_t)n;
  canary::glass::settings_mark_dirty();
  canary::ui::character_apply((canary::ui::Character)n);
}
EMSCRIPTEN_KEEPALIVE int emu_character_count(void) {
  return (int)canary::ui::character_count();
}
EMSCRIPTEN_KEEPALIVE int emu_character_active(void) {
  return (int)canary::ui::active_character();
}
EMSCRIPTEN_KEEPALIVE int emu_character_at_ring(int pos) {
  if (!ch_valid(pos)) return -1;
  return (int)canary::ui::character_at_ring((uint8_t)pos);
}
EMSCRIPTEN_KEEPALIVE const char* emu_character_name(int n) {
  if (!ch_valid(n)) return "";
  return canary::ui::character_name((canary::ui::Character)n);
}
EMSCRIPTEN_KEEPALIVE const char* emu_character_caption(int n) {
  if (!ch_valid(n)) return "";
  return canary::ui::character_caption((canary::ui::Character)n);
}
// Swatch colors for the style rail, 0xRRGGBB — the page paints each chip
// in its Character's own ground/ink/accent, from the same table the glass
// wears. which: 0 = bg, 1 = text, 2 = accent.
EMSCRIPTEN_KEEPALIVE int emu_character_color(int n, int which) {
  if (!ch_valid(n)) return 0;
  const auto& pal =
      canary::ui::character_def((canary::ui::Character)n).pal;
  switch (which) {
    case 0: return (int)pal.bg;
    case 1: return (int)pal.text;
    case 2: return (int)pal.accent;
  }
  return 0;
}

EMSCRIPTEN_KEEPALIVE void emu_time_scale(double scale) {
  emu_clock_set_scale(scale);
}
EMSCRIPTEN_KEEPALIVE double emu_time_scale_get(void) {
  return emu_clock_get_scale();
}
EMSCRIPTEN_KEEPALIVE void emu_time_step_ms(double ms) { emu_clock_step(ms); }
EMSCRIPTEN_KEEPALIVE void emu_epoch_offset(double s) {
  emu_clock_set_epoch_offset(s);
}
EMSCRIPTEN_KEEPALIVE void emu_seed(unsigned int seed) { emu_rng_seed(seed); }

// Hex transport keeps arbitrary bytes intact across the JS string
// boundary (uint32 NVS values legitimately contain NULs).
EMSCRIPTEN_KEEPALIVE void emu_nvs_preseed_hex(const char* ns, const char* key,
                                              const char* hexstr) {
  uint8_t buf[256];
  int n = 0;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (int i = 0; hexstr[i] && hexstr[i + 1] && n < (int)sizeof(buf); i += 2) {
    const int hi = nib(hexstr[i]), lo = nib(hexstr[i + 1]);
    if (hi < 0 || lo < 0) return;
    buf[n++] = (uint8_t)((hi << 4) | lo);
  }
  emu_nvs_put(ns, key, buf, n);
}
EMSCRIPTEN_KEEPALIVE void emu_nvs_reset(void) { emu_nvs_wipe(); }

}  // extern "C"

// ── What the glass says (the probes' reader) ────────────────────────────
// A framebuffer read can tell that a line was cut to "..."; only the text
// can tell WHICH line is on the glass — e.g. that the setup key is still
// there after the stuck-phone hint (F45). Every label on the active screen,
// as JSON [{x,y,w,h,shown,opa,text_hex}]: its area on the panel, whether it
// and every parent are unhidden, its text opacity, and its text as LVGL
// holds it (in LVGL 8 LONG_DOT rewrites the label's own buffer, so a cut
// line reads here with its "..." and without its tail). Read-only.
namespace {

void hex_append(std::string& out, const char* s) {
  static const char H[] = "0123456789abcdef";
  for (const unsigned char* p = (const unsigned char*)s; p && *p; ++p) {
    out.push_back(H[*p >> 4]);
    out.push_back(H[*p & 0xF]);
  }
}

bool shown(lv_obj_t* obj) {
  for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  }
  return true;
}

void collect_labels(lv_obj_t* obj, std::string& out) {
  if (lv_obj_check_type(obj, &lv_label_class)) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    if (out.size() > 1) out += ",";
    out += "{\"x\":" + std::to_string(a.x1) + ",\"y\":" + std::to_string(a.y1) +
           ",\"w\":" + std::to_string(lv_area_get_width(&a)) +
           ",\"h\":" + std::to_string(lv_area_get_height(&a)) +
           ",\"shown\":" + (shown(obj) ? "1" : "0") + ",\"opa\":" +
           std::to_string((int)lv_obj_get_style_text_opa(obj, LV_PART_MAIN)) +
           ",\"text_hex\":\"";
    hex_append(out, lv_label_get_text(obj));
    out += "\"}";
  }
  const uint32_t n = lv_obj_get_child_cnt(obj);
  for (uint32_t i = 0; i < n; ++i) {
    collect_labels(lv_obj_get_child(obj, (int32_t)i), out);
  }
}

}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE const char* emu_screen_labels(void) {
  static std::string out;
  out = "[";
  lv_obj_t* scr = lv_scr_act();
  if (scr != nullptr) collect_labels(scr, out);
  out += "]";
  return out.c_str();
}
