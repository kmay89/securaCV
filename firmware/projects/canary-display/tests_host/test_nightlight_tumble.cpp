// Host test: the nightlight's tumble wears its Startle (F242).
//
// On an orientation commit main.cpp's nightlight_apply_orientation() cleans
// the screen, calls nightlight_ui_create() (a new bird, Hidden until it is
// given a mood) and nightlight_ui_tumble() (the companion slides in from
// the edge that was up), and the face's next nightlight_ui_update() gives
// the bird its mood. The tumble used to send canary_mark_react(Startle)
// itself, to a bird that was still Hidden, and canary_mark_react() refuses
// a hidden bird: no turn ever showed the Startle the face promises. The
// tumble now owes the Startle and the first update pays it, after the mood.
//
// This compiles the REAL src/ui/nightlight_ui.cpp and src/ui/canary_mark.cpp
// against fake_lvgl/lvgl.h, with the nightlight's config and pin map, and
// the real color engine and plumage (firmware/common/color) behind the lamp.
// What the face reads from the rest of the firmware (the port's canvas, the
// theme, the motion engine's segment fade, the lamp's persisted settings)
// is stood in for below; none of it moves the bird.
//
// The Startle is read off the bird the way canary_mark writes it: its hop
// lifts the bird's y offset off its seat (12 px at the default temperament),
// while the breath it replaces moves it 2 px either way (1 asleep). The
// tumble slides the bird's translate, not its y offset, so the two never mix.
//
// Prints "ALL NIGHTLIGHT TUMBLE TESTS PASSED" on success.

#include "canary/care/lantern.h"
#include "canary/care/nightlight_glue.h"
#include "canary/fleet/fleet_instance.h"
#include "canary/ui/canary_mark.h"
#include "canary/ui/look_state.h"
#include "canary/ui/lvgl_port.h"
#include "canary/ui/motion.h"
#include "canary/ui/nightlight_ui.h"
#include "canary/ui/theme.h"

#include <cstdio>

using namespace canary::ui;

// ── what the face reads from the rest of the firmware ─────────────────────
namespace {
int g_w = 320, g_h = 180;  // the port's logical canvas
int glyph8(const lv_font_t*, uint32_t, uint32_t) { return 8; }
lv_font_t g_font = {16, glyph8, nullptr};
}  // namespace

namespace canary::ui {
int16_t lvgl_port_width() { return (int16_t)g_w; }
int16_t lvgl_port_height() { return (int16_t)g_h; }
lv_color_t col_bg() { return lv_color_hex(0x000000); }
lv_color_t col_text() { return lv_color_hex(0xFFFFFF); }
lv_color_t col_muted() { return lv_color_hex(0x808080); }
lv_color_t sev_color(canary::fleet::Sev, bool) { return lv_color_hex(0xFF0000); }
const lv_font_t* font_caption() { return &g_font; }
const lv_font_t* font_body() { return &g_font; }
canary::color::LookParams& look_params() {
  static canary::color::LookParams p;
  return p;
}
canary::color::Plumage& song() {
  static canary::color::Plumage s;
  return s;
}
namespace motion {
void seg_opa(lv_obj_t*, lv_opa_t) {}
}  // namespace motion
}  // namespace canary::ui

namespace canary::care {
LanternModel& lantern() {
  static LanternModel m;
  return m;
}
uint8_t nightlight_lamp_bri() { return 255; }
bool nightlight_clock_12h() { return false; }
}  // namespace canary::care

// ── the test ──────────────────────────────────────────────────────────────
static int g_fail = 0;

#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("  FAIL (%s:%d): ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
      g_fail++;                                           \
    }                                                     \
  } while (0)

static canary::fleet::Fleet g_fleet;  // empty: nothing worse than all-quiet

static uint32_t now() { return fake_lvgl::tick(); }

static NightlightState state(CanaryMood m) {
  NightlightState st;
  st.bird = m;
  st.wifi_ok = true;
  st.mqtt_ok = true;
  st.time_valid = true;
  st.clock_hh = 12;
  st.clock_mm = 0;
  return st;
}

// The bird's y offset (what the breath and the hop write) over `ms`.
struct YSpan {
  int lo, hi;
};
static YSpan watch_y(uint32_t ms) {
  YSpan s = {1 << 30, -(1 << 30)};
  for (uint32_t t = 0; t < ms; t += 5) {
    fake_lvgl::run(5);
    const lv_obj_t* bird = canary_mark_obj();
    if (bird == nullptr) continue;
    const int y = lv_obj_get_style_y(bird, LV_PART_MAIN);
    if (y < s.lo) s.lo = y;
    if (y > s.hi) s.hi = y;
  }
  return s;
}

// A glass of the turned shape, the way the apply path leaves it: the old
// screen gone (the bird's timers with it), a fresh one active.
static lv_obj_t* g_scr = nullptr;
static void fresh_glass(int w, int h) {
  if (g_scr != nullptr) lv_obj_del(g_scr);
  g_w = w;
  g_h = h;
  fake_lvgl::disp_w() = w;
  fake_lvgl::disp_h() = h;
  g_scr = lv_obj_create(nullptr);
  fake_lvgl::active_screen() = g_scr;
}

// The seat the face aligns the bird to: the landscape perch's -(h / 12),
// the portrait one's V(-22).
static int seat_y() { return g_w > g_h ? -(g_h / 12) : -(22 * g_h) / 320; }

// One runtime turn: the face rebuilt, the tumble, and the first update
// `first_update_after` ms later wearing `mood`. Returns the y span over
// the 600 ms after that update (the hop is 240 ms up, 320 ms down).
static YSpan turn(int w, int h, uint8_t delta, CanaryMood mood,
                  uint32_t first_update_after = 0) {
  fresh_glass(w, h);
  nightlight_ui_create();
  nightlight_ui_tumble(delta, now());
  CHECK(canary_mark_obj() != nullptr &&
            lv_obj_has_flag(canary_mark_obj(), LV_OBJ_FLAG_HIDDEN),
        "the rebuilt face's bird is Hidden until its first mood");
  if (first_update_after) fake_lvgl::run(first_update_after);
  nightlight_ui_update(g_fleet, now(), state(mood));
  return watch_y(600);
}

static bool startled(const YSpan& s) { return s.lo <= seat_y() - 8; }

static void report(const char* what, const YSpan& s) {
  std::printf("  %-46s y %d..%d (seat %d)\n", what, s.lo, s.hi, seat_y());
}

int main() {
  std::printf("the tumble's Startle comes with the first mood (F242):\n");

  // Every turn the apply path makes, on both glass shapes, wears its Startle.
  struct Turn {
    const char* what;
    int w, h;
    uint8_t delta;
  };
  const Turn turns[] = {
      {"portrait -> landscape, a quarter clockwise", 320, 180, 1},
      {"landscape -> landscape, a half turn", 320, 180, 2},
      {"portrait -> landscape, three quarters", 320, 180, 3},
      {"landscape -> portrait, a quarter clockwise", 180, 320, 1},
      {"portrait -> portrait, a half turn", 180, 320, 2},
      {"landscape -> portrait, three quarters", 180, 320, 3},
  };
  for (const Turn& t : turns) {
    fake_lvgl::run(9000);  // past the mark's 8 s Tilt/Startle ration
    const YSpan s = turn(t.w, t.h, t.delta, CanaryMood::Idle);
    report(t.what, s);
    CHECK(startled(s), "%s: the bird should hop with its Startle (y %d..%d, seat %d)",
          t.what, s.lo, s.hi, seat_y());
    CHECK(s.hi <= seat_y() + 2, "%s: nothing below the breath (y %d..%d, seat %d)",
          t.what, s.lo, s.hi, seat_y());
    // ...and lands back on its breath, on the seat.
    const YSpan after = watch_y(3000);
    CHECK(after.lo == seat_y() - 2 && after.hi == seat_y() + 2,
          "%s: after the hop the bird breathes on its seat (y %d..%d, seat %d)", t.what,
          after.lo, after.hi, seat_y());
  }

  std::printf("...and is still refused asleep, rationed, and owed only once:\n");
  {
    // A night flip stays still: the Startle is refused asleep, and a wake a
    // moment later does not collect it.
    fake_lvgl::run(9000);
    const YSpan asleep = turn(320, 180, 1, CanaryMood::Asleep);
    report("asleep at the turn", asleep);
    CHECK(asleep.lo >= seat_y() - 1 && asleep.hi <= seat_y() + 1,
          "asleep: the bird only breathes (y %d..%d, seat %d)", asleep.lo, asleep.hi,
          seat_y());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    const YSpan woke = watch_y(600);
    report("woken 600 ms after a sleeping turn", woke);
    CHECK(!startled(woke), "a wake after a sleeping turn owes no Startle (y %d..%d)",
          woke.lo, woke.hi);
  }
  {
    // Rationed: a second turn 2 s after a Startle stays still; one 9 s after
    // it hops again.
    fake_lvgl::run(9000);
    const YSpan first = turn(320, 180, 1, CanaryMood::Idle);
    CHECK(startled(first), "the first turn hops (y %d..%d)", first.lo, first.hi);
    fake_lvgl::run(1400);
    const YSpan second = turn(180, 320, 3, CanaryMood::Idle);
    report("a second turn 2 s after a Startle", second);
    CHECK(!startled(second), "a turn 2 s after a Startle is rationed (y %d..%d)",
          second.lo, second.hi);
    fake_lvgl::run(9000);
    const YSpan third = turn(320, 180, 1, CanaryMood::Idle);
    report("a third turn 9 s after it", third);
    CHECK(startled(third), "a turn past the ration hops again (y %d..%d)", third.lo,
          third.hi);
  }
  {
    // Owed within the tumble's 1.4 s (while the caption shows its word): a
    // first update 1.2 s after the turn still pays it, one after the 1.4 s
    // pays nothing. (A busy loop can render the first update late; a window
    // cut shorter than the caption's would drop the Startle unnoticed.)
    fake_lvgl::run(9000);
    const YSpan slow = turn(320, 180, 1, CanaryMood::Idle, 1200);
    report("first update 1.2 s after the turn", slow);
    CHECK(startled(slow), "a first update inside the tumble's 1.4 s still hops (y %d..%d)",
          slow.lo, slow.hi);
    fake_lvgl::run(9000);
    const YSpan late = turn(320, 180, 1, CanaryMood::Idle, 1500);
    report("first update 1.5 s after the turn", late);
    CHECK(!startled(late), "a first update after the tumble owes no Startle (y %d..%d)",
          late.lo, late.hi);
  }
  {
    // A face built without a turn (a boot, a rebuild after a turn) owes none.
    fake_lvgl::run(9000);
    fresh_glass(320, 180);
    nightlight_ui_create();
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    const YSpan boot = watch_y(600);
    report("a face built at boot", boot);
    CHECK(!startled(boot), "a face built without a turn does not Startle (y %d..%d)",
          boot.lo, boot.hi);
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(1, now());
    fresh_glass(320, 180);
    nightlight_ui_create();  // rebuilt again before its first update
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    const YSpan rebuilt = watch_y(600);
    report("a face rebuilt after a turn, before its update", rebuilt);
    CHECK(!startled(rebuilt), "a rebuild drops the owed Startle (y %d..%d)", rebuilt.lo,
          rebuilt.hi);
    // The same past 2^31 ms of uptime (24.8 days), where the rebuilt face's
    // zeroed tumble clock reads as a tumble still playing: the rebuild must
    // drop what the turn owed, not lean on that clock.
    fake_lvgl::tick() = 0x80000000u + 60000u;
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(1, now());
    fresh_glass(320, 180);
    nightlight_ui_create();
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    const YSpan late_uptime = watch_y(600);
    report("...the same after 24.8 days of uptime", late_uptime);
    CHECK(!startled(late_uptime),
          "a rebuild after 24.8 days of uptime drops the owed Startle (y %d..%d)",
          late_uptime.lo, late_uptime.hi);
    // ...while a turn there still wears it.
    fake_lvgl::run(9000);
    const YSpan turned = turn(320, 180, 1, CanaryMood::Idle);
    report("a turn after 24.8 days of uptime", turned);
    CHECK(startled(turned), "a turn after 24.8 days of uptime hops (y %d..%d)",
          turned.lo, turned.hi);
  }

  if (g_fail == 0) {
    std::printf("ALL NIGHTLIGHT TUMBLE TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
