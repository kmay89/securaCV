// include/canary/ui/motion.h — the motion engine's LVGL half.
//
// The pure math lives in motion_core.h (capability model, tier, curves,
// governor, weather-scene math — host-tested). This header is what the
// faces call: eased micro-transitions that degrade to an instant style
// write on the lean tier, the veil ground-swap, the frame-time sampling the
// governor feeds on, the backlight glide, and the weather-scene layer.
//
// Design rules the glue keeps (learned the hard way elsewhere in this tree):
//  - Every animation's var is the LVGL object it moves (or an engine-owned
//    static), so lv_obj_clean() reaps it — motion state never outlives the
//    widgets it points at (glance_ui's use-after-free lesson).
//  - Per-part opacity only, never group opacity or screen-load fades — the
//    LVGL v9 layer-compositing cliff on the 800x480 glass (onboard_ui's
//    documented halt). The veil is ONE solid object's bg_opa.
//  - Dual-major: only lv_anim APIs both 8.4 and 9.x serve, with the one
//    field-name split (time/duration) guarded in the implementation.
#pragma once
#include <stdint.h>
#include <lvgl.h>

#include "canary/ui/motion_core.h"

namespace canary::ui::motion {

// ── What this glass is (derived once, from the board facts) ──────────────
const Caps& caps();
Tier tier();

// ── The live gate ────────────────────────────────────────────────────────
// The render pass feeds the engine the three truths every class-gate needs;
// faces then just ask. Until the first set_context the gate is all-clear.
void set_context(bool night, bool alert_unacked, bool modal);
bool allowed(Fx f);            // fx_allowed() against the live gate + tier
uint32_t ms(Dur d);            // dur_ms() at this glass's tier

// ── The governor (loop integration) ──────────────────────────────────────
// The loop brackets lv_timer_handler() with one call: elapsed wall time in,
// decorative-quality level out (0 rich / 1 trimmed / 2 still). Decorative
// callers check quality(); semantic motion never consults it.
void frame_sample(uint32_t elapsed_ms);
uint8_t quality();
// The one-word ambient verdict: class gate AND governor both say yes.
bool ambient_ok();

// ── The engine's curves as lv_anim paths ─────────────────────────────────
int32_t path_out_cubic(const lv_anim_t* a);
int32_t path_in_out_cubic(const lv_anim_t* a);
int32_t path_out_back(const lv_anim_t* a);
int32_t path_out_quad(const lv_anim_t* a);

// ── Micro helpers ────────────────────────────────────────────────────────
// Ease one object's background opacity to `to` (Micro class): a short
// ease-out where the tier affords it, an instant style write where it does
// not (dur_ms(Micro) == 0 on Lean) or while an alarm owns the glass. Safe
// to call every render tick — a no-op when already there or in flight.
void seg_opa(lv_obj_t* obj, lv_opa_t to);

// Fade a label's ink (per-part text_opa) from -> to, with a start delay for
// staggered entrances. Snaps to `to` when Micro is refused.
void text_opa_fade(lv_obj_t* obj, lv_opa_t from, lv_opa_t to, Dur d,
                   uint32_t delay_ms);

// ── The veil ground-swap ─────────────────────────────────────────────────
// The transition the hard cut never gave us: one solid veil (top layer,
// colored to the TARGET ground) eases over the old face, `rebuild` runs
// under full cover, the veil lifts off the new one — a dip-to-ground that
// reads as the room changing. Lean tier (and a swap already in flight)
// calls `rebuild` directly: snap is that glass's honest answer.
void ground_swap(void (*rebuild)(), lv_color_t veil_color);

// ── Backlight glide ──────────────────────────────────────────────────────
// Ease the day backlight between ladder rungs on PWM glass; snap when
// urgent (an alarm's brightness is immediate), on binary backlights, and on
// the lean tier (no 60 Hz I2C chatter on the C3's expander). The very first
// call always snaps — boot must not fade in from an unknown level.
void backlight_glide(uint8_t level, bool urgent);
// Another path is taking the pin (a live editor, the night profile, the
// dawn ramp): stop any glide AND forget the engine's shadow of the level,
// so the next glide snaps and re-syncs rather than trusting a value the
// hardware no longer shows.
void backlight_glide_cancel();

// ── The weather-scene layer ──────────────────────────────────────────────
// A clipped field of soft shapes (motion_core's particle math) that lives
// behind a face's weather block. The engine owns its timer; the face owns
// its lifecycle: create once per build, feed the scene every update,
// tear down in the create-reset section (the statics-and-rebuild
// discipline). The layer hides itself whenever the Ambient gate or the
// governor says still — the face never has to remember the rules.
void wx_layer_create(lv_obj_t* parent, int16_t x, int16_t y, int16_t w,
                     int16_t h);
void wx_layer_set(WxScene s, lv_color_t ink);
void wx_layer_teardown();

}  // namespace canary::ui::motion
