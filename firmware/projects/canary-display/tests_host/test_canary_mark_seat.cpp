// Host test: the brand canary sits where its host placed it (F64).
//
// canary_mark records the bird's base once, at its first on-stage mood, and
// every pose, breath and hop writes the base back as the bird's style offset
// (lv_obj_set_x/y). The base used to be read with lv_obj_get_x/y, which is
// the LAID-OUT box: (0, 0) before LVGL's first layout pass, and afterwards
// the anchor's position plus the offset. So a bird whose first mood came
// before a layout pass lost its host's offset and rode its anchor (the round
// watch's onboarding bird at the panel's center, y 98..137, instead of
// CENTER -64), and one whose first mood came after a layout pass was moved
// by its anchor's own distance from the parent's corner (the glance bird at
// x 200 on a 240 px disc). canary_mark_rebase() re-read the base the same
// way right after the host's new align, so each onboarding scene walked the
// bird another anchor-distance off the glass.
//
// This test compiles the REAL src/ui/canary_mark.cpp against fake_lvgl/
// lvgl.h — LVGL 8's position rules (lv_obj_align stores anchor and offset,
// a layout pass places the box, lv_obj_get_x/y read the box) and nothing
// else — and holds the drawn box to the host's placement, before and after
// a layout pass, under every anchor, and across the onboarding's re-seats.
//
// The pinned boxes are what the real LVGL 8.4.0 drew, with the display's
// lv_conf, in a native harness that linked the display's own faces (the
// splash, glance_ui, dash_ui, portrait_ui, portrait7_ui, nightstand7_ui,
// nightlight_ui and onboard_ui) and wrapped canary_mark_create to read the
// bird's coords after every refresh. The old base reads drew, on the round
// watch: onboarding Hello y 98..102 (here: 34..38), PhoneJoined x 300 after
// the Join scene's re-seat (here: 100), the glance bird x 200 when it comes
// on stage after a layout pass (here: 100). The fake must reproduce those
// numbers, so it is held to LVGL, not to itself.
//
// It also holds which animations a mood change stops (F222): the mark's own
// (the breath, the hop, a wing flourish) and never its host's. The
// nightlight's tumble animates the bird object itself (its translate slides
// in from the edge that was up, nightlight_ui_tumble()) right after the face
// is built, and the first mood comes on the next update; a mood change that
// deleted every animation on the bird ended the slide at its first frame,
// and the companion stayed a whole fling off its perch.
//
// Prints "ALL CANARY MARK SEAT TESTS PASSED" on success.

#include "canary/ui/canary_mark.h"

#include <cstdio>

using namespace canary::ui;

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

// The drawn box over `ms` of animation, a layout pass after every 5 ms step
// (the refresh): its x1 and y1 ranges.
struct Span {
  int x_lo, x_hi, y_lo, y_hi;
};

static Span watch(lv_obj_t* scr, lv_obj_t* bird, uint32_t ms) {
  Span s = {1 << 30, -(1 << 30), 1 << 30, -(1 << 30)};
  for (uint32_t t = 0; t < ms; t += 5) {
    fake_lvgl::run(5);
    lv_obj_update_layout(scr);
    if (bird->x1 < s.x_lo) s.x_lo = bird->x1;
    if (bird->x1 > s.x_hi) s.x_hi = bird->x1;
    if (bird->y1 < s.y_lo) s.y_lo = bird->y1;
    if (bird->y1 > s.y_hi) s.y_hi = bird->y1;
  }
  return s;
}

// A screen of the glass's size and a full-size content layer on it (the
// onboarding's s_content, the glance's halo page).
static lv_obj_t* glass(int w, int h, lv_obj_t** content) {
  fake_lvgl::disp_w() = w;
  fake_lvgl::disp_h() = h;
  lv_obj_t* scr = lv_obj_create(nullptr);
  *content = lv_obj_create(scr);
  lv_obj_set_size(*content, (lv_coord_t)w, (lv_coord_t)h);
  return scr;
}

// The breath is ±2 px around the seat: the drawn y1 spans exactly
// [seat - 2, seat + 2] and x1 holds the seat.
static void check_seat(const char* what, const Span& s, int x, int y) {
  CHECK(s.x_lo == x && s.x_hi == x && s.y_lo == y - 2 && s.y_hi == y + 2,
        "%s: drawn x %d..%d y %d..%d, the host placed it at x %d y %d (the "
        "breath is y %d..%d)",
        what, s.x_lo, s.x_hi, s.y_lo, s.y_hi, x, y, y - 2, y + 2);
}

// 1. The onboarding's first scene: the bird is aligned and comes on stage in
//    onboard_ui_create(), before LVGL has laid the screen out.
static void test_first_mood_before_layout() {
  std::printf("first mood before a layout pass (onboarding Hello):\n");
  lv_obj_t* content = nullptr;
  lv_obj_t* scr = glass(240, 240, &content);
  lv_obj_t* bird = canary_mark_create(content, 40);
  lv_obj_align(bird, LV_ALIGN_CENTER, 0, -64);
  canary_mark_rebase();
  canary_mark_mood(CanaryMood::Idle);
  const Span s = watch(scr, bird, 3000);
  std::printf("  drawn x %d y %d..%d\n", s.x_lo, s.y_lo, s.y_hi);
  // CENTER -64 on the 240 px disc: (240 - 40) / 2 - 64 = 36. LVGL 8.4 drew
  // the old base read at y 98..102: the panel's center.
  check_seat("round watch, Hello", s, 100, 36);
}

// 2. A face whose bird comes on stage only after a layout pass (the glance's
//    perch while the mood engine had it hidden).
static void test_first_mood_after_layout() {
  std::printf("first mood after a layout pass (glance perch):\n");
  lv_obj_t* page = nullptr;
  lv_obj_t* scr = glass(240, 240, &page);
  lv_obj_t* bird = canary_mark_create(page, 40);
  lv_obj_align(bird, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_update_layout(scr);
  canary_mark_mood(CanaryMood::Idle);
  const Span s = watch(scr, bird, 3000);
  std::printf("  drawn x %d y %d..%d\n", s.x_lo, s.y_lo, s.y_hi);
  // LVGL 8.4 drew the old base read at x 200: the TOP_MID anchor's 100 px,
  // read back as the offset and laid out from the anchor again.
  check_seat("round watch, glance", s, 100, 26);
}

// 3. The onboarding's re-seats: each scene aligns the bird, calls
//    canary_mark_rebase(), then sets its mood, and LVGL runs between scenes.
static void test_rebase_walk() {
  std::printf("the onboarding's re-seats (round watch, no QR):\n");
  lv_obj_t* content = nullptr;
  lv_obj_t* scr = glass(240, 240, &content);
  lv_obj_t* bird = canary_mark_create(content, 40);
  lv_obj_align(bird, LV_ALIGN_CENTER, 0, -64);
  struct Scene {
    const char* name;
    lv_align_t align;
    int y_ofs;
    CanaryMood mood;
    int seat_y;  // the drawn y1 the placement names
  };
  // join_bird_top on the round watch's default stack is 94 (the card's
  // seat); every other scene keeps CENTER -64 (y 36).
  const Scene scenes[] = {
      {"Hello", LV_ALIGN_CENTER, -64, CanaryMood::Idle, 36},
      {"Join (no QR)", LV_ALIGN_TOP_MID, 94, CanaryMood::Idle, 94},
      {"PhoneJoined", LV_ALIGN_CENTER, -64, CanaryMood::Idle, 36},
      {"Connecting", LV_ALIGN_CENTER, -64, CanaryMood::Idle, 36},
      {"Fail", LV_ALIGN_CENTER, -64, CanaryMood::Hidden, -1},
      {"Connecting", LV_ALIGN_CENTER, -64, CanaryMood::Idle, 36},
      {"Join (no QR)", LV_ALIGN_TOP_MID, 94, CanaryMood::Idle, 94},
      {"Success", LV_ALIGN_CENTER, -64, CanaryMood::Happy, 36},
  };
  for (size_t i = 0; i < sizeof(scenes) / sizeof(scenes[0]); ++i) {
    const Scene& sc = scenes[i];
    lv_obj_align(bird, sc.align, 0, (lv_coord_t)sc.y_ofs);
    canary_mark_rebase();
    canary_mark_mood(sc.mood);
    const Span s = watch(scr, bird, 3000);
    if (sc.seat_y < 0) {
      CHECK(lv_obj_has_flag(bird, LV_OBJ_FLAG_HIDDEN), "%s: bird on stage",
            sc.name);
      continue;
    }
    std::printf("  %-13s drawn x %d y %d..%d\n", sc.name, s.x_lo, s.y_lo,
                s.y_hi);
    if (sc.mood == CanaryMood::Happy) {
      // The hop rises from the seat (up to the 18 px apex ceiling), then
      // the breath settles back around it; nothing below seat + 2.
      CHECK(s.x_lo == 100 && s.x_hi == 100 && s.y_hi == sc.seat_y + 2 &&
                s.y_lo < sc.seat_y - 2 && s.y_lo >= sc.seat_y - 18,
            "%s: drawn x %d..%d y %d..%d, seat y %d", sc.name, s.x_lo, s.x_hi,
            s.y_lo, s.y_hi, sc.seat_y);
    } else {
      // LVGL 8.4 drew the old reads walking right and down by the anchor's
      // distance per scene: PhoneJoined at x 300 y 194..198.
      check_seat(sc.name, s, 100, sc.seat_y);
    }
  }
}

// 4. Every anchor, with an offset on both axes, its first mood before and
//    after a layout pass: the drawn box is the anchor plus the offset.
static void test_every_anchor() {
  std::printf("every anchor (172x320 parent, a 40 px bird at +7, -11):\n");
  const int W = 172, H = 320, B = 40, DX = 7, DY = -11;
  struct Anchor {
    const char* name;
    lv_align_t align;
    int x, y;  // the anchor's top-left for a B px box in a W x H parent
  };
  const Anchor anchors[] = {
      {"DEFAULT", LV_ALIGN_DEFAULT, 0, 0},
      {"TOP_LEFT", LV_ALIGN_TOP_LEFT, 0, 0},
      {"TOP_MID", LV_ALIGN_TOP_MID, (W - B) / 2, 0},
      {"TOP_RIGHT", LV_ALIGN_TOP_RIGHT, W - B, 0},
      {"BOTTOM_LEFT", LV_ALIGN_BOTTOM_LEFT, 0, H - B},
      {"BOTTOM_MID", LV_ALIGN_BOTTOM_MID, (W - B) / 2, H - B},
      {"BOTTOM_RIGHT", LV_ALIGN_BOTTOM_RIGHT, W - B, H - B},
      {"LEFT_MID", LV_ALIGN_LEFT_MID, 0, (H - B) / 2},
      {"RIGHT_MID", LV_ALIGN_RIGHT_MID, W - B, (H - B) / 2},
      {"CENTER", LV_ALIGN_CENTER, (W - B) / 2, (H - B) / 2},
  };
  for (size_t i = 0; i < sizeof(anchors) / sizeof(anchors[0]); ++i) {
    const Anchor& a = anchors[i];
    for (int laid = 0; laid < 2; ++laid) {
      lv_obj_t* parent = nullptr;
      lv_obj_t* scr = glass(W, H, &parent);
      lv_obj_t* bird = canary_mark_create(parent, B);
      if (a.align == LV_ALIGN_DEFAULT) {
        lv_obj_set_pos(bird, DX, DY);
      } else {
        lv_obj_align(bird, a.align, DX, DY);
      }
      if (laid) lv_obj_update_layout(scr);
      canary_mark_mood(CanaryMood::Idle);
      const Span s = watch(scr, bird, 3000);
      char what[64];
      std::snprintf(what, sizeof(what), "%s, first mood %s a layout pass",
                    a.name, laid ? "after" : "before");
      check_seat(what, s, a.x + DX, a.y + DY);
    }
  }
  std::printf("  %d anchors x {before, after} a layout pass\n",
              (int)(sizeof(anchors) / sizeof(anchors[0])));
}

// 5. A rebuild: a new bird on a new screen records its own base.
static void test_new_bird_new_base() {
  std::printf("a rebuilt face's bird:\n");
  lv_obj_t* a_page = nullptr;
  lv_obj_t* a_scr = glass(800, 480, &a_page);
  lv_obj_t* a = canary_mark_create(a_page, 72);
  lv_obj_align(a, LV_ALIGN_LEFT_MID, 223, -44);
  canary_mark_mood(CanaryMood::Idle);
  check_seat("dash (LEFT_MID 223, -44)", watch(a_scr, a, 2000), 223, 160);
  lv_obj_del(a_scr);
  lv_obj_t* b_page = nullptr;
  lv_obj_t* b_scr = glass(800, 480, &b_page);
  lv_obj_t* b = canary_mark_create(b_page, 72);
  lv_obj_align(b, LV_ALIGN_LEFT_MID, 18, 0);
  canary_mark_mood(CanaryMood::Asleep);
  // Asleep breathes 1 px: y1 spans the seat +-1.
  const Span s = watch(b_scr, b, 6000);
  CHECK(s.x_lo == 18 && s.x_hi == 18 && s.y_lo == 203 && s.y_hi == 205,
        "nightstand7 (LEFT_MID 18, 0), asleep: drawn x %d..%d y %d..%d, "
        "placed at x 18 y 204", s.x_lo, s.x_hi, s.y_lo, s.y_hi);
}

// 6. A host's own animation on the bird (F222). The nightlight's tumble, as
//    nightlight_ui_tumble() starts it right after nightlight_ui_create():
//    700 ms of translate from the edge that was up to 0; the face's update
//    sets the first mood a frame later. Seat: the landscape perch on the
//    320x180 glass (RIGHT_MID, -(96 - 93) / 2, -(180 / 12) for a 93 px bird),
//    which the native runtime-turn boot (canary-local/emulator/test/
//    runtime_turn.sh) reads as x 226, y 27..31 on LVGL 8.4.
static void host_tx(void* var, int32_t v) {
  lv_obj_set_style_translate_x((lv_obj_t*)var, (lv_coord_t)v, 0);
}
static void host_ty(void* var, int32_t v) {
  lv_obj_set_style_translate_y((lv_obj_t*)var, (lv_coord_t)v, 0);
}

static void slide(lv_obj_t* bird, lv_anim_exec_xcb_t cb, int from) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, bird);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, from, 0);
  lv_anim_set_time(&a, 700);
  lv_anim_start(&a);
}

// Running anims on `var` (any exec callback but `except`), or on any child.
static int anims_on(const void* var, lv_anim_exec_xcb_t only = nullptr,
                    lv_anim_exec_xcb_t except = nullptr) {
  int n = 0;
  for (const lv_anim_t* a : fake_lvgl::anims()) {
    if (a->var != var) continue;
    if (only != nullptr && a->exec_cb != only) continue;
    if (except != nullptr && a->exec_cb == except) continue;
    n++;
  }
  return n;
}
static int anims_on_children(const lv_obj_t* o) {
  int n = 0;
  for (const lv_obj_t* c : o->children) n += anims_on(c);
  return n;
}

static void test_host_animation_survives_moods() {
  std::printf("a host's animation on the bird (the nightlight's tumble):\n");
  const int SEAT_X = 226, SEAT_Y = 29;
  {
    // The first mood after the tumble began, and a second mood mid-slide.
    lv_obj_t* content = nullptr;
    lv_obj_t* scr = glass(320, 180, &content);
    lv_obj_t* bird = canary_mark_create(content, 93);
    lv_obj_align(bird, LV_ALIGN_RIGHT_MID, -1, -15);
    slide(bird, host_tx, 160);  // a quarter turn: in over the right edge
    canary_mark_mood(CanaryMood::Idle);
    CHECK(anims_on(bird, host_tx) == 1,
          "the first mood stopped the host's slide (%d running)",
          anims_on(bird, host_tx));
    const Span early = watch(scr, bird, 300);
    CHECK(early.x_hi > SEAT_X, "the slide starts off the perch (x %d..%d)",
          early.x_lo, early.x_hi);
    canary_mark_mood(CanaryMood::Worried);
    CHECK(anims_on(bird, host_tx) == 1,
          "a mood change mid-slide stopped the host's slide (%d running)",
          anims_on(bird, host_tx));
    watch(scr, bird, 600);  // the slide's last 400 ms, and then some
    const Span s = watch(scr, bird, 3000);
    std::printf("  after the slide: drawn x %d..%d y %d..%d\n", s.x_lo, s.x_hi,
                s.y_lo, s.y_hi);
    check_seat("landed after two moods", s, SEAT_X, SEAT_Y);
    lv_obj_del(scr);
  }
  {
    // Hidden mid-slide: the slide finishes behind the curtain, and the next
    // on-stage mood shows the bird on its perch, not where the hide caught it.
    lv_obj_t* content = nullptr;
    lv_obj_t* scr = glass(320, 180, &content);
    lv_obj_t* bird = canary_mark_create(content, 93);
    lv_obj_align(bird, LV_ALIGN_RIGHT_MID, -1, -15);
    canary_mark_mood(CanaryMood::Idle);
    slide(bird, host_ty, -160);  // a half turn: it falls from above
    watch(scr, bird, 200);
    canary_mark_mood(CanaryMood::Hidden);
    CHECK(anims_on(bird, host_ty) == 1,
          "going off stage stopped the host's slide (%d running)",
          anims_on(bird, host_ty));
    watch(scr, bird, 800);
    canary_mark_mood(CanaryMood::Idle);
    check_seat("hidden mid-slide, back on stage", watch(scr, bird, 3000),
               SEAT_X, SEAT_Y);
    lv_obj_del(scr);
  }
  {
    // ...while a mood change still stops all of the mark's own motion: a hop
    // that a mood change left running would bounce over the new breath, and
    // a wing flourish would keep flapping into the new pose.
    lv_obj_t* content = nullptr;
    lv_obj_t* scr = glass(320, 180, &content);
    lv_obj_t* bird = canary_mark_create(content, 93);
    lv_obj_align(bird, LV_ALIGN_RIGHT_MID, -1, -15);
    canary_mark_mood(CanaryMood::Happy);  // the hop
    canary_mark_react(CanaryReact::Greeting);  // one slow wing lift
    CHECK(anims_on(bird) == 1 && anims_on_children(bird) == 1,
          "Happy and a greeting run the hop and the wing (%d on the bird, %d "
          "on its parts)", anims_on(bird), anims_on_children(bird));
    slide(bird, host_tx, 160);
    canary_mark_mood(CanaryMood::Idle);
    CHECK(anims_on(bird, nullptr, host_tx) == 1,
          "after the mood change the mark runs one motion on the bird, its "
          "breath (%d)", anims_on(bird, nullptr, host_tx));
    CHECK(anims_on_children(bird) == 0,
          "the mood change stopped the wing flourish (%d running)",
          anims_on_children(bird));
    CHECK(anims_on(bird, host_tx) == 1, "...and left the host's slide (%d)",
          anims_on(bird, host_tx));
    canary_mark_mood(CanaryMood::Hidden);
    CHECK(anims_on(bird, nullptr, host_tx) == 0,
          "off stage the mark runs nothing on the bird (%d)",
          anims_on(bird, nullptr, host_tx));
    lv_obj_del(scr);
  }
}

int main() {
  test_first_mood_before_layout();
  test_first_mood_after_layout();
  test_rebase_walk();
  test_every_anchor();
  test_new_bird_new_base();
  test_host_animation_survives_moods();
  if (g_fail == 0) {
    std::printf("ALL CANARY MARK SEAT TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
