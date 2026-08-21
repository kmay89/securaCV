// include/canary/ui/clock_face.h — the drawn Analog dial (ClockStyle::Analog).
//
// One dial, both 7" faces: the landscape bedside face and the portrait
// column build it in place of the segment digits when the saved clock style
// says so. Plain LVGL objects (a bordered circle, twelve marks, two lv_line
// hands) — no images, no canvas — so it recolors through the theme choke
// point and red-shifts at night exactly like the segments do.
//
// The caller owns the storage (the same statics-and-rebuild discipline the
// faces already keep): zero the struct on face create, build once, update
// per tick. lv_line keeps a POINTER to its points, which is why the point
// arrays live in this struct and not on a stack.
#pragma once
#include <lvgl.h>

namespace canary::ui {

#if LVGL_VERSION_MAJOR >= 9
using ClockLinePt = lv_point_precise_t;
#else
using ClockLinePt = lv_point_t;
#endif

struct AnalogClock {
  lv_obj_t* dial = nullptr;        // the bordered ring
  lv_obj_t* marks[12] = {nullptr}; // hour marks (dots — no rotation API needed)
  lv_obj_t* hand_h = nullptr;      // hour hand (lv_line)
  lv_obj_t* hand_m = nullptr;      // minute hand (lv_line)
  lv_obj_t* hub = nullptr;         // center cap
  ClockLinePt hpts[2] = {};        // lv_line points must outlive the object
  ClockLinePt mpts[2] = {};
  int cx = 0, cy = 0, r = 0;       // center + radius, parent coordinates
  // Minute-sweep state (the motion engine's eased tick): the angles the
  // hands currently SHOW, x10 degrees; -1 = never painted, so the first
  // valid time lands without a sweep from twelve.
  int32_t shown_h10 = -1;
  int32_t shown_m10 = -1;
};

// Build the dial centered on (cx, cy) with radius r. Colors land in update.
void analog_clock_build(AnalogClock* c, lv_obj_t* parent, int cx, int cy,
                        int r);

// Paint the time. `col` drives hands + the four cardinal marks, `muted` the
// ring and the rest — pass the face's day or red-shifted night colors and
// the dial follows the same law as every other element. With `valid` false
// the hands hide (the dial itself is the honest "waiting" shape).
void analog_clock_update(AnalogClock* c, int hh, int mm, lv_color_t col,
                         lv_color_t muted, bool valid);

}  // namespace canary::ui
