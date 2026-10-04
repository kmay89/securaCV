// src/ui/motion.cpp — the motion engine's LVGL half. See the header for the
// contract and motion_core.h for the math; this file is deliberately thin
// glue: derive the capability profile from the board facts, keep the live
// gate and the governor, and wrap the pure curves in lv_anim clothing.
#include "flavor_config.h"
#include <Arduino.h>
#include <lvgl.h>

#include "motion.h"
#include "display.h"

#include "pins.h"

namespace canary::ui::motion {

namespace {

// ── The capability profile, from the same defines the HALs build on ──────
Caps derive_caps() {
  Caps c;
#ifdef CD_FLAVOR_DASH
  c.w = LCD_WIDTH;
  c.h = LCD_HEIGHT;
  c.bus = Bus::RgbPanel;
  c.bus_hz = LCD_PCLK_HZ;
#elif defined(LCD_QSPI_HZ)
  // GRAM-backed QSPI AMOLED (RM690B0 class): the panel holds its own
  // framebuffer and takes windowed command writes, so like an RGB panel it
  // is "not bus-bound" for the tier math — QspiCmd, not Spi.
  c.w = TFT_WIDTH;
  c.h = TFT_HEIGHT;
  c.bus = Bus::QspiCmd;
  c.bus_hz = LCD_QSPI_HZ;
#else
  c.w = TFT_WIDTH;
  c.h = TFT_HEIGHT;
  c.bus = Bus::Spi;
  c.bus_hz = TFT_SPI_HZ;
#endif
  // The Lab (EMU_BUILD_FLAVOR) deliberately keeps the flavor's PHYSICAL
  // bus here: the browser has no SPI wire, but substituting it away would
  // reclassify a bus-bound glass as Full and the preview would show motion
  // the silicon will never run (review catch on #1566). The emulator
  // previews the tier the hardware earns.
#if defined(HAS_PSRAM) && HAS_PSRAM
  c.psram = true;
#endif
#if defined(F_CPU)
  c.cpu_mhz = (uint16_t)(F_CPU / 1000000UL);
#else
  c.cpu_mhz = 240;   // the emulator shim carries no F_CPU
#endif
#ifdef CD_LEAN_BUILD
  c.lean = true;
#endif
#if defined(HAS_BACKLIGHT_PWM) && HAS_BACKLIGHT_PWM
  c.pwm_backlight = true;
#endif
  return c;
}

Caps s_caps;
Tier s_tier = Tier::Lean;
bool s_ready = false;

void ensure_ready() {
  if (s_ready) return;
  s_caps = derive_caps();
  s_tier = tier_for(s_caps);
  s_ready = true;
}

Gate s_gate;
Governor s_gov;

// One progress helper for every custom path: act_time/duration in Q10.
// The one lv_anim field whose NAME split across the majors.
int32_t progress1024(const lv_anim_t* a) {
#if LVGL_VERSION_MAJOR >= 9
  const uint32_t dur = (uint32_t)a->duration;
#else
  const uint32_t dur = (uint32_t)a->time;
#endif
  if (dur == 0) return kMotionOne;
  int32_t t = (int32_t)((uint64_t)(a->act_time < 0 ? 0 : a->act_time) *
                        (uint64_t)kMotionOne / dur);
  return t > kMotionOne ? kMotionOne : t;
}

int32_t path_map(const lv_anim_t* a, int32_t eased) {
  return lerp1024(a->start_value, a->end_value, eased);
}

// ── Anim exec callbacks (var is always the object — see header) ──────────
void bg_opa_exec(void* var, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t*)var, (lv_opa_t)v, LV_PART_MAIN);
}
void text_opa_exec(void* var, int32_t v) {
  lv_obj_set_style_text_opa((lv_obj_t*)var, (lv_opa_t)v, LV_PART_MAIN);
}

// ── The veil ─────────────────────────────────────────────────────────────
lv_obj_t* s_veil = nullptr;
void (*s_veil_rebuild)() = nullptr;
bool s_veil_busy = false;

void veil_lift_done(lv_anim_t*) {
  if (s_veil) lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
  s_veil_busy = false;
}

void veil_covered(lv_anim_t*) {
  // Full cover: swap the world under the veil, then lift it off the new
  // face. The rebuild runs inside this callback on purpose — nothing can
  // repaint between the clean and the build, so there is no naked frame.
  if (s_veil_rebuild) s_veil_rebuild();
  s_veil_rebuild = nullptr;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_veil);
  lv_anim_set_exec_cb(&a, bg_opa_exec);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_0);
  lv_anim_set_time(&a, dur_ms(Dur::Medium, s_tier));
  lv_anim_set_path_cb(&a, path_out_cubic);
  lv_anim_set_ready_cb(&a, veil_lift_done);
  lv_anim_start(&a);
}

// ── Backlight glide ──────────────────────────────────────────────────────
int32_t s_bl_shadow = -1;   // last level the engine commanded; -1 = never
int s_bl_holder = 0;        // anim var: engine-owned, never deleted

void bl_exec(void*, int32_t v) {
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  s_bl_shadow = v;
  canary::hal::backlight_set((uint8_t)v);
}

// ── The weather-scene layer ──────────────────────────────────────────────
constexpr int WX_MAX = 14;   // = wx_particle_count(Rain, Full)

lv_obj_t* s_wx_field = nullptr;
lv_obj_t* s_wx_p[WX_MAX] = {nullptr};
lv_timer_t* s_wx_timer = nullptr;
WxScene s_wx_scene = WxScene::Still;
int16_t s_wx_w = 0, s_wx_h = 0;
uint32_t s_wx_seed = 0x5eedcafe;

void wx_style_particle(lv_obj_t* o, WxScene s, const WxParticle& p,
                       int16_t fw, int16_t fh) {
  switch (s) {
    case WxScene::Rain:
      lv_obj_set_size(o, 2, p.px);
      lv_obj_set_style_radius(o, 1, 0);
      break;
    case WxScene::Snow:
      lv_obj_set_size(o, p.px, p.px);
      lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
      break;
    case WxScene::Clouds: {
      // p.px is the depth layer: nearer = larger.
      const int16_t cw = (int16_t)(fw * (11 - 3 * p.px) / 20);
      const int16_t ch = (int16_t)(fh / 6);
      lv_obj_set_size(o, cw, ch);
      lv_obj_set_style_radius(o, ch / 2, 0);
      break;
    }
    case WxScene::Fog: {
      const int16_t bw = (int16_t)(fw * 12 / 10);
      const int16_t bh = (int16_t)(fh / 6);
      lv_obj_set_size(o, bw, bh);
      lv_obj_set_style_radius(o, bh / 2, 0);
      break;
    }
    default:
      break;
  }
}

void wx_tick(lv_timer_t*) {
  if (!s_wx_field || s_wx_scene == WxScene::Still) return;
  // The layer keeps its own honesty: a modal, an alarm, night, a trimmed
  // governor, or a face that lost the glass all read as "hold still".
  if (lv_obj_get_screen(s_wx_field) != lv_scr_act() || !ambient_ok()) {
    lv_obj_add_flag(s_wx_field, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(s_wx_field, LV_OBJ_FLAG_HIDDEN);
  const uint32_t now = millis();
  const int n = wx_particle_count(s_wx_scene, s_tier);
  for (int i = 0; i < n && i < WX_MAX; i++) {
    if (!s_wx_p[i]) continue;
    const WxParticle p =
        wx_particle(s_wx_scene, s_wx_seed, i, now, s_wx_w, s_wx_h);
    lv_obj_set_pos(s_wx_p[i], p.x, p.y);
    lv_obj_set_style_bg_opa(s_wx_p[i], p.opa, 0);
  }
}

}  // namespace

// ── Capability + gate ────────────────────────────────────────────────────

const Caps& caps() {
  ensure_ready();
  return s_caps;
}

Tier tier() {
  ensure_ready();
  return s_tier;
}

void set_context(bool night, bool alert_unacked, bool modal) {
  s_gate.night = night;
  s_gate.alert_unacked = alert_unacked;
  s_gate.modal = modal;
}

bool allowed(Fx f) { return fx_allowed(f, s_gate, tier()); }

uint32_t ms(Dur d) { return dur_ms(d, tier()); }

// ── Governor ─────────────────────────────────────────────────────────────

void frame_sample(uint32_t elapsed_ms) {
  governor_step(s_gov, elapsed_ms * 1000u, frame_budget_us(tier()));
}

uint8_t quality() { return s_gov.level; }

bool ambient_ok() { return allowed(Fx::Ambient) && s_gov.level == 0; }

// ── Paths ────────────────────────────────────────────────────────────────

int32_t path_out_cubic(const lv_anim_t* a) {
  return path_map(a, ease_out_cubic(progress1024(a)));
}
int32_t path_in_out_cubic(const lv_anim_t* a) {
  return path_map(a, ease_in_out_cubic(progress1024(a)));
}
int32_t path_out_back(const lv_anim_t* a) {
  return path_map(a, ease_out_back(progress1024(a)));
}
int32_t path_out_quad(const lv_anim_t* a) {
  return path_map(a, ease_out_quad(progress1024(a)));
}

// ── Micro helpers ────────────────────────────────────────────────────────

void seg_opa(lv_obj_t* obj, lv_opa_t to) {
  if (!obj) return;
  lv_anim_t* run = lv_anim_get(obj, bg_opa_exec);
  if (run) {
    if ((lv_opa_t)run->end_value == to) return;   // already flying there
    lv_anim_del(obj, bg_opa_exec);
  }
  const lv_opa_t cur = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
  if (cur == to) return;
  const uint32_t d = ms(Dur::Micro);
  if (d == 0 || !allowed(Fx::Micro)) {
    lv_obj_set_style_bg_opa(obj, to, LV_PART_MAIN);
    return;
  }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, bg_opa_exec);
  lv_anim_set_values(&a, cur, to);
  lv_anim_set_time(&a, d);
  lv_anim_set_path_cb(&a, path_out_cubic);
  lv_anim_start(&a);
}

void text_opa_fade(lv_obj_t* obj, lv_opa_t from, lv_opa_t to, Dur d,
                   uint32_t delay_ms) {
  if (!obj) return;
  const uint32_t dur = ms(d);
  if (dur == 0 || !allowed(Fx::Micro)) {
    lv_obj_set_style_text_opa(obj, to, LV_PART_MAIN);
    return;
  }
  lv_anim_del(obj, text_opa_exec);
  lv_obj_set_style_text_opa(obj, from, LV_PART_MAIN);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, text_opa_exec);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_time(&a, dur);
  lv_anim_set_delay(&a, delay_ms);
  lv_anim_set_path_cb(&a, path_out_cubic);
  lv_anim_start(&a);
}

// ── The veil ─────────────────────────────────────────────────────────────

void ground_swap(void (*rebuild)(), lv_color_t veil_color) {
  if (!rebuild) return;
  ensure_ready();
  if (s_tier == Tier::Lean || s_veil_busy || !lv_scr_act()) {
    // Snap is the lean tier's honest transition; a swap mid-swap collapses
    // to the newest truth rather than queueing a stale one.
    rebuild();
    return;
  }
  if (!s_veil) {
    // Same construction as lvgl_port's scrim: top layer, oversized so any
    // orientation is covered, never a touch target.
    s_veil = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_veil);
    lv_obj_set_style_border_width(s_veil, 0, 0);
    lv_obj_set_style_radius(s_veil, 0, 0);
    lv_obj_clear_flag(s_veil, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_veil, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_veil, 0, 0);
    lv_obj_set_size(s_veil, 960, 960);
  }
  s_veil_busy = true;
  s_veil_rebuild = rebuild;
  lv_obj_set_style_bg_color(s_veil, veil_color, 0);
  lv_obj_set_style_bg_opa(s_veil, LV_OPA_0, 0);
  lv_obj_clear_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_veil);   // above the brightness scrim, always
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_veil);
  lv_anim_set_exec_cb(&a, bg_opa_exec);
  lv_anim_set_values(&a, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&a, dur_ms(Dur::Micro, s_tier));
  lv_anim_set_path_cb(&a, path_in_out_cubic);
  lv_anim_set_ready_cb(&a, veil_covered);
  lv_anim_start(&a);
}

// ── Backlight glide ──────────────────────────────────────────────────────

void backlight_glide(uint8_t level, bool urgent) {
  ensure_ready();
  const bool can_glide =
      s_caps.pwm_backlight && s_tier != Tier::Lean && s_bl_shadow >= 0;
  if (urgent || !can_glide) {
    backlight_glide_cancel();
    s_bl_shadow = level;
    canary::hal::backlight_set(level);
    return;
  }
  if (s_bl_shadow == (int32_t)level) {
    lv_anim_t* run = lv_anim_get(&s_bl_holder, bl_exec);
    if (!run) return;   // already there, nothing in flight
  }
  lv_anim_t* run = lv_anim_get(&s_bl_holder, bl_exec);
  if (run && run->end_value == (int32_t)level) return;   // already flying there
  lv_anim_del(&s_bl_holder, bl_exec);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &s_bl_holder);
  lv_anim_set_exec_cb(&a, bl_exec);
  lv_anim_set_values(&a, s_bl_shadow, level);
  lv_anim_set_time(&a, ms(Dur::Medium));
  lv_anim_set_path_cb(&a, path_out_quad);
  lv_anim_start(&a);
}

void backlight_glide_cancel() {
  lv_anim_del(&s_bl_holder, bl_exec);
  // A cancel means another path (the night profile, a live editor, the
  // dawn ramp) is about to own the pin, and the engine can no longer know
  // what the hardware shows. Forget the shadow, so the next glide SNAPS
  // and re-syncs instead of concluding "already there" against a panel
  // that is actually sitting on the night floor (review catch on #1566:
  // without this, a morning that returns to yesterday's ambient level
  // never switched the LEDC back off the night profile).
  s_bl_shadow = -1;
}

// ── The weather-scene layer ──────────────────────────────────────────────

void wx_layer_create(lv_obj_t* parent, int16_t x, int16_t y, int16_t w,
                     int16_t h) {
  if (!parent || w <= 0 || h <= 0) return;
  ensure_ready();
  if (s_tier == Tier::Lean) return;   // the lean glass never carries a field
  s_wx_field = lv_obj_create(parent);
  lv_obj_remove_style_all(s_wx_field);
  lv_obj_set_pos(s_wx_field, x, y);
  lv_obj_set_size(s_wx_field, w, h);
  lv_obj_set_style_bg_opa(s_wx_field, LV_OPA_0, 0);
  lv_obj_set_style_border_width(s_wx_field, 0, 0);
  lv_obj_clear_flag(s_wx_field, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_wx_field, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_wx_field, LV_OBJ_FLAG_HIDDEN);
  s_wx_w = w;
  s_wx_h = h;
  s_wx_scene = WxScene::Still;
}

void wx_layer_set(WxScene s, lv_color_t ink) {
  if (!s_wx_field) return;
  if (s == s_wx_scene) {
    // Same weather; keep the field's ink honest against a palette change.
    for (auto* o : s_wx_p)
      if (o) lv_obj_set_style_bg_color(o, ink, 0);
    return;
  }
  s_wx_scene = s;
  lv_obj_clean(s_wx_field);
  for (auto& o : s_wx_p) o = nullptr;
  if (s == WxScene::Still) {
    lv_obj_add_flag(s_wx_field, LV_OBJ_FLAG_HIDDEN);
    if (s_wx_timer) {
      lv_timer_del(s_wx_timer);
      s_wx_timer = nullptr;
    }
    return;
  }
  const uint32_t now = millis();
  const int n = wx_particle_count(s, s_tier);
  for (int i = 0; i < n && i < WX_MAX; i++) {
    lv_obj_t* o = lv_obj_create(s_wx_field);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, ink, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    const WxParticle p = wx_particle(s, s_wx_seed, i, now, s_wx_w, s_wx_h);
    wx_style_particle(o, s, p, s_wx_w, s_wx_h);
    lv_obj_set_pos(o, p.x, p.y);
    lv_obj_set_style_bg_opa(o, p.opa, 0);
    s_wx_p[i] = o;
  }
  const uint32_t period = wx_tick_ms(s, s_tier);
  if (s_wx_timer) {
    lv_timer_set_period(s_wx_timer, period);
  } else {
    s_wx_timer = lv_timer_create(wx_tick, period, nullptr);
  }
  // Visibility lands on the first tick, through the same gate as always.
}

void wx_layer_teardown() {
  // The face is rebuilding: the objects die with lv_obj_clean, so only the
  // engine's pointers and timer need to forget them.
  if (s_wx_timer) {
    lv_timer_del(s_wx_timer);
    s_wx_timer = nullptr;
  }
  s_wx_field = nullptr;
  for (auto& o : s_wx_p) o = nullptr;
  s_wx_scene = WxScene::Still;
}

}  // namespace canary::ui::motion
