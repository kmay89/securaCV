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
#include <string>

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

// The face's caption: the last label nightlight_ui_create() puts on the
// screen (glance, date, caption in portrait; the caption after the bird in
// landscape).
static std::string caption() {
  const lv_obj_t* last = nullptr;
  for (const lv_obj_t* c : g_scr->children)
    if (c->kind == fake_lvgl::kLabel) last = c;
  return last != nullptr ? last->text : std::string("<no caption>");
}

// The clock: the one container of four digits and two colon dots.
static int clock_opa() {
  for (const lv_obj_t* c : g_scr->children)
    if (c->kind == fake_lvgl::kObj && c->children.size() == 6 &&
        c->children[0]->children.size() == 7)
      return c->opa;
  return -1;
}

static NightlightState visiting(CanaryMood m, canary::care::VisitKind k) {
  NightlightState st = state(m);
  st.visit.kind = k;
  return st;
}

// The bird's translate_y: the visit's lift plus the tumble's fall.
static int bird_ty() {
  const lv_obj_t* b = canary_mark_obj();
  return b != nullptr ? b->ty : -9999;
}

int main() {
  // A turn in the first seconds of uptime wears its Startle too (F279): the
  // mark's Tilt/Startle ration starts as "never reacted". Its clock used to
  // start at 0, so a nightlight turned 7 s after power-on refused the
  // tumble's Startle as if one had just played (runtime_turn.sh measured it on
  // all 12 turns). The fake's tick is 0 when the process starts.
  std::printf("a turn 7 s after power-on wears its Startle (F279):\n");
  {
    CHECK(now() == 0, "this case runs at power-on (tick %u)", (unsigned)now());
    fake_lvgl::run(7000);
    const YSpan early = turn(320, 180, 1, CanaryMood::Idle);
    report("a quarter turn 7 s after power-on", early);
    CHECK(startled(early), "a turn 7 s after power-on hops with its Startle (y %d..%d, seat %d)",
          early.lo, early.hi, seat_y());
  }

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
    fake_lvgl::run(9000);  // past the 8 s ration since the last Startle
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
    CHECK(caption().empty(), "a face built at boot (%u ms of uptime) says nothing (\"%s\")",
          (unsigned)now(), caption().c_str());
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
    CHECK(caption().empty(), "a face rebuilt after a turn, before its update, says nothing "
          "(\"%s\")", caption().c_str());
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

  // F280: the face's moments read by elapsed ms, past 2^31 and across the
  // wrap. A face built without a turn at 2^31 + 60 s of uptime said "wheee!"
  // (its zeroed tumble deadline read as still to come) and stopped setting
  // the clock's opacity, so a visit no longer dimmed it.
  std::printf("the face's moments past 24.8 days of uptime (F280):\n");
  {
    using canary::care::VisitKind;
    fake_lvgl::tick() = 0x80000000u + 120000u;
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    std::printf("  a face built at %u ms: caption \"%s\", clock opa %d\n", (unsigned)now(),
                caption().c_str(), clock_opa());
    CHECK(caption().empty(), "a face built without a turn after 24.8 days says nothing "
          "(\"%s\")", caption().c_str());
    nightlight_ui_update(g_fleet, now(), visiting(CanaryMood::Idle, VisitKind::Stretch));
    CHECK(caption() == canary::care::visit_caption(VisitKind::Stretch) &&
              clock_opa() == LV_OPA_40,
          "a visit after 24.8 days dims the clock and names itself (caption \"%s\", clock "
          "opa %d)", caption().c_str(), clock_opa());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    CHECK(caption().empty() && clock_opa() == LV_OPA_COVER,
          "...and the visit's end brings the clock back (caption \"%s\", clock opa %d)",
          caption().c_str(), clock_opa());
    // A life glance brightens the caption for 6 s, and then it goes quiet.
    nightlight_ui_life_glance(now());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    CHECK(caption() == "here with you", "a life glance after 24.8 days shows its line (\"%s\")",
          caption().c_str());
    fake_lvgl::run(5900);
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    CHECK(caption() == "here with you", "...and still shows it 5.9 s on (\"%s\")",
          caption().c_str());
    fake_lvgl::run(200);
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    CHECK(caption().empty(), "...for 6 s only (\"%s\" 6.1 s on)", caption().c_str());
    // A turn there still says its word for the tumble's 1.4 s.
    fake_lvgl::run(9000);
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(1, now());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    CHECK(caption() == "wheee!", "a turn after 24.8 days says its word (\"%s\")",
          caption().c_str());
    fake_lvgl::run(1500);
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    CHECK(caption().empty(), "...for the tumble's 1.4 s only (\"%s\" 1.5 s on)",
          caption().c_str());
    // While the tumble plays, the clock's breath-in animation owns its
    // opacity: 300 ms into a turn the update leaves the animation's value
    // (it rises from 20% to full over 900 ms), not the full clock.
    fake_lvgl::run(9000);
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(1, now());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    fake_lvgl::run(300);
    const int breath_in = clock_opa();
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    std::printf("  300 ms into a turn: the breath-in has the clock at opa %d, the update "
                "leaves %d\n", breath_in, clock_opa());
    CHECK(breath_in > LV_OPA_20 && breath_in < LV_OPA_COVER && clock_opa() == breath_in,
          "300 ms into a turn the update leaves the clock to its breath-in (opa %d, the "
          "animation's %d)", clock_opa(), breath_in);
    // A face rebuilt 300 ms after a turn, with no tumble of its own (a
    // rebuild of the active face), plays none of the turn's moments: no
    // word, and a visit there dims the clock and names itself.
    fake_lvgl::run(9000);
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(1, now());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    fake_lvgl::run(300);
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_update(g_fleet, now(), visiting(CanaryMood::Idle, VisitKind::Stretch));
    CHECK(caption() == canary::care::visit_caption(VisitKind::Stretch) &&
              clock_opa() == LV_OPA_40,
          "a face rebuilt 300 ms after a turn plays none of its moments (caption \"%s\", "
          "clock opa %d)", caption().c_str(), clock_opa());
    // ...and across the counter's wrap at 49.7 days: a turn 0.5 s before it
    // keeps its word 1 s on (past the wrap) and drops it 1.5 s on.
    fake_lvgl::tick() = 0u - 500u;
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(1, now());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    fake_lvgl::run(1000);
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    CHECK(caption() == "wheee!", "a turn 0.5 s before the wrap still says its word 1 s on, at "
          "%u ms (\"%s\")", (unsigned)now(), caption().c_str());
    fake_lvgl::run(500);
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    CHECK(caption().empty(), "...and not 1.5 s on, at %u ms (\"%s\")", (unsigned)now(),
          caption().c_str());
  }

  // F284: a portrait half turn during a visit keeps the companion's lift.
  // The fall from above used to animate translate_y to 0 while stage_bird()
  // owned translate_y for the lift (-66 * h / 320); create() resets the
  // staging, the first update re-staged the running visit under the fall,
  // and the fall's last frame put the bird back on its perch.
  std::printf("a turn during a visit keeps the companion's lift (F284):\n");
  {
    using canary::care::VisitKind;
    const NightlightState visit = visiting(CanaryMood::Idle, VisitKind::Stretch);
    fake_lvgl::run(9000);
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_update(g_fleet, now(), visit);
    const int lift = -(66 * 320) / 320;
    CHECK(bird_ty() == lift, "the visit lifts the bird to translate_y %d (%d)", lift, bird_ty());
    // The half turn: portrait to portrait, the bird falling from above.
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(2, now());
    nightlight_ui_update(g_fleet, now(), visit);
    fake_lvgl::run(200);
    const int mid = bird_ty();
    nightlight_ui_update(g_fleet, now(), visit);
    fake_lvgl::run(800);  // the 700 ms fall is over
    nightlight_ui_update(g_fleet, now(), visit);
    std::printf("  a half turn mid-visit: translate_y %d at 200 ms, %d after the fall (lift %d)\n",
                mid, bird_ty(), lift);
    CHECK(mid < lift, "200 ms into the fall the bird is still above its lift (%d, lift %d)", mid,
          lift);
    CHECK(bird_ty() == lift, "after a half turn mid-visit the bird keeps its lift (translate_y "
          "%d, lift %d)", bird_ty(), lift);
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    CHECK(bird_ty() == 0, "the visit's end puts the bird back on its perch (%d)", bird_ty());
    // A visit that begins mid-fall: the fall lands on the lift.
    fake_lvgl::run(9000);
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(2, now());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    fake_lvgl::run(300);
    nightlight_ui_update(g_fleet, now(), visit);
    fake_lvgl::run(700);
    CHECK(bird_ty() == lift, "a visit begun mid-fall: the fall lands on the lift (%d, lift %d)",
          bird_ty(), lift);
    // A fall with no visit lands on the perch, as it did.
    fake_lvgl::run(9000);
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(2, now());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    fake_lvgl::run(1000);
    CHECK(bird_ty() == 0, "a fall with no visit lands on the perch (%d)", bird_ty());
    // On the landscape glass the perch is already center stage (no lift): a
    // half turn mid-visit lands at 0.
    fake_lvgl::run(9000);
    fresh_glass(320, 180);
    nightlight_ui_create();
    nightlight_ui_tumble(2, now());
    nightlight_ui_update(g_fleet, now(), visit);
    fake_lvgl::run(1000);
    CHECK(bird_ty() == 0, "landscape, a half turn mid-visit lands at 0 (%d)", bird_ty());
    // A face rebuilt mid-fall takes the fall's offset with the old bird: the
    // old bird's animation goes with the old screen and never writes the
    // fall's last 0, so the rebuild itself starts the new bird with no fall.
    // A rebuild 300 ms into a portrait half turn (no tumble of its own),
    // then a visit: the bird is at the lift, and back at 0 when it ends.
    fake_lvgl::run(9000);
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(2, now());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    fake_lvgl::run(300);
    const int fall_left = bird_ty();
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_update(g_fleet, now(), visit);
    std::printf("  rebuilt 300 ms into a fall (translate_y %d then): %d in a visit (lift %d)\n",
                fall_left, bird_ty(), lift);
    CHECK(fall_left < 0, "300 ms into the fall the bird is still above its perch (%d)",
          fall_left);
    CHECK(bird_ty() == lift, "a face rebuilt mid-fall, then a visit: the bird is at the lift "
          "(%d, lift %d)", bird_ty(), lift);
    fake_lvgl::run(1500);
    nightlight_ui_update(g_fleet, now(), visit);
    CHECK(bird_ty() == lift, "...and stays there (%d, lift %d)", bird_ty(), lift);
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    CHECK(bird_ty() == 0, "...and the visit's end puts it on its perch (%d)", bird_ty());
    // A quarter turn onto the landscape glass 300 ms into a portrait fall:
    // the new bird slides in from the side, and a visit there has no lift.
    fake_lvgl::run(9000);
    fresh_glass(180, 320);
    nightlight_ui_create();
    nightlight_ui_tumble(2, now());
    nightlight_ui_update(g_fleet, now(), state(CanaryMood::Idle));
    fake_lvgl::run(300);
    fresh_glass(320, 180);
    nightlight_ui_create();
    nightlight_ui_tumble(1, now());
    nightlight_ui_update(g_fleet, now(), visit);
    CHECK(bird_ty() == 0, "a quarter turn onto the landscape glass mid-fall, then a visit: "
          "translate_y 0 (%d)", bird_ty());
    fake_lvgl::run(1500);
    nightlight_ui_update(g_fleet, now(), visit);
    CHECK(bird_ty() == 0, "...and still 0 after the slide (%d)", bird_ty());
  }

  if (g_fail == 0) {
    std::printf("ALL NIGHTLIGHT TUMBLE TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
