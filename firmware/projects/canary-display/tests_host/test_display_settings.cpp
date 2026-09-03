// Host test for the pure display-settings geometry + brightness helpers
// (include/canary/glass_settings.h): orientation dims, the touch un-rotation
// (round-tripped against the forward render rotation), and the rendered
// brightness scrim. No Arduino, no LVGL, no board.
//
// Prints "ALL DISPLAY SETTINGS TESTS PASSED" on success (a CI grep makes a
// silent pass impossible to fake). Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//     firmware/projects/canary-display/tests_host/test_display_settings.cpp -o t && ./t

#include "canary/glass_settings.h"

#include <cstdio>
#include <string>

using namespace canary::glass;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// The forward render rotation (logical -> native panel) that the touch map
// must invert. Kept here, in the test, as the independent oracle.
static void forward(uint8_t rot, int PW, int PH, int lx, int ly,
                    int* px, int* py) {
  switch (rot & 3) {
    case ROT_PORTRAIT:      *px = PW - 1 - ly; *py = lx;          break;  // 90 CW
    case ROT_LANDSCAPE_INV: *px = PW - 1 - lx; *py = PH - 1 - ly; break;  // 180
    case ROT_PORTRAIT_INV:  *px = ly;          *py = PH - 1 - lx; break;  // 270
    default:                *px = lx;          *py = ly;          break;  // 0
  }
}

// ── Orientation classification + dims ────────────────────────────────────
static void test_dims() {
  CHECK(!rotation_is_portrait(ROT_LANDSCAPE), "landscape is not portrait");
  CHECK(rotation_is_portrait(ROT_PORTRAIT), "portrait is portrait");
  CHECK(!rotation_is_portrait(ROT_LANDSCAPE_INV), "180 is not portrait");
  CHECK(rotation_is_portrait(ROT_PORTRAIT_INV), "270 is portrait");

  int w = 0, h = 0;
  rotation_logical_dims(ROT_LANDSCAPE, 800, 480, &w, &h);
  CHECK(w == 800 && h == 480, "landscape keeps native dims");
  rotation_logical_dims(ROT_PORTRAIT, 800, 480, &w, &h);
  CHECK(w == 480 && h == 800, "portrait swaps to 480x800");
  rotation_logical_dims(ROT_LANDSCAPE_INV, 800, 480, &w, &h);
  CHECK(w == 800 && h == 480, "180 keeps native dims");
  rotation_logical_dims(ROT_PORTRAIT_INV, 800, 480, &w, &h);
  CHECK(w == 480 && h == 800, "270 swaps to 480x800");
}

// ── Touch mapping inverts the render rotation, exactly, at every rotation ─
static void test_touch_roundtrip() {
  const int PW = 800, PH = 480;
  for (uint8_t rot = 0; rot < 4; rot++) {
    int LW = 0, LH = 0;
    rotation_logical_dims(rot, PW, PH, &LW, &LH);
    // Sweep a lattice of logical points; forward to the panel, then the HAL's
    // touch map must bring them back unchanged.
    for (int lx = 0; lx < LW; lx += 17) {
      for (int ly = 0; ly < LH; ly += 13) {
        int px = 0, py = 0;
        forward(rot, PW, PH, lx, ly, &px, &py);
        CHECK(px >= 0 && px < PW && py >= 0 && py < PH,
              "forward lands inside the panel");
        int bx = 0, by = 0;
        rotation_map_touch(rot, PW, PH, px, py, &bx, &by);
        CHECK(bx == lx && by == ly, "touch un-rotates to the logical point");
      }
    }
  }
}

static void test_touch_corners() {
  const int PW = 800, PH = 480;
  int x = 0, y = 0;
  // Landscape identity.
  rotation_map_touch(ROT_LANDSCAPE, PW, PH, 10, 20, &x, &y);
  CHECK(x == 10 && y == 20, "landscape touch is identity");
  // Portrait: the panel's top-left corner is the logical bottom-left.
  rotation_map_touch(ROT_PORTRAIT, PW, PH, 0, 0, &x, &y);
  CHECK(x == 0 && y == PW - 1, "portrait maps panel origin to logical (0, 799)");
}

// ── Brightness scrim ─────────────────────────────────────────────────────
static void test_brightness() {
  CHECK(bright_pct_clamp(120) == BRIGHT_PCT_MAX, "clamp above 100");
  CHECK(bright_pct_clamp(10) == BRIGHT_PCT_MIN, "clamp below 50");
  CHECK(bright_pct_clamp(70) == 70, "clamp in-range passes through");

  CHECK(bright_scrim_opa(100) == 0, "100% is a clear scrim");
  CHECK(bright_scrim_opa(50) == 127, "50% floor is a half scrim");
  CHECK(bright_scrim_opa(40) == 127, "below floor snaps to the floor scrim");
  // Monotone: dimmer setting => more opaque scrim.
  for (int p = BRIGHT_PCT_MIN; p < BRIGHT_PCT_MAX; p += 5) {
    CHECK(bright_scrim_opa((uint8_t)p) > bright_scrim_opa((uint8_t)(p + 5)),
          "scrim opacity is monotone in brightness");
  }
}

// ── Names ────────────────────────────────────────────────────────────────
static void test_names() {
  CHECK(std::string(rotation_name(ROT_LANDSCAPE)) == "landscape", "0 name");
  CHECK(std::string(rotation_name(ROT_PORTRAIT)) == "portrait", "90 name");
  CHECK(std::string(rotation_name(ROT_LANDSCAPE_INV)) == "landscape flipped",
        "180 name");
  CHECK(std::string(rotation_name(ROT_PORTRAIT_INV)) == "portrait flipped",
        "270 name");
}

// ── The LVGL 9 pointer feed inverts LVGL's own indev rotation ────────────
// LVGL 9 rotates every pointer sample by the display rotation itself
// (lv_display_rotate_point, quoted here from lvgl v9.5 as the independent
// oracle — NATIVE dims, not the rotated ones). The settings panel is fed
// the HAL's already-logical point, so lvgl_port hands LVGL
// rotation_to_lvgl_indev(logical) and LVGL's rotation must return the
// logical point unchanged, at every quarter turn, across the whole panel.
static void lvgl9_rotate_point(uint8_t rot, int hor_res, int ver_res,
                               int* x, int* y) {
  const int ox = *x, oy = *y;
  switch (rot & 3) {
    case ROT_PORTRAIT:      *x = ver_res - oy - 1; *y = ox;               break;
    case ROT_LANDSCAPE_INV: *x = hor_res - ox - 1; *y = ver_res - oy - 1; break;
    case ROT_PORTRAIT_INV:  *x = oy;               *y = hor_res - ox - 1; break;
    default: break;
  }
}

static void test_lvgl_indev_feed() {
  const int PW = 800, PH = 480;
  for (uint8_t rot = 0; rot < 4; rot++) {
    int LW = 0, LH = 0;
    rotation_logical_dims(rot, PW, PH, &LW, &LH);
    for (int lx = 0; lx < LW; lx += 19) {
      for (int ly = 0; ly < LH; ly += 11) {
        int fx = 0, fy = 0;
        rotation_to_lvgl_indev(rot, PW, PH, lx, ly, &fx, &fy);
        CHECK(fx >= 0 && fx < PW && fy >= 0 && fy < PH,
              "the fed point lies inside the native panel");
        lvgl9_rotate_point(rot, PW, PH, &fx, &fy);
        CHECK(fx == lx && fy == ly,
              "LVGL's own rotation returns the logical point unchanged");
      }
    }
  }
  // Rotation 0 is the identity — the fed point IS the logical point.
  int x = 0, y = 0;
  rotation_to_lvgl_indev(ROT_LANDSCAPE, PW, PH, 123, 45, &x, &y);
  CHECK(x == 123 && y == 45, "landscape feeds the point through untouched");
}

int main() {
  test_dims();
  test_touch_roundtrip();
  test_lvgl_indev_feed();
  test_touch_corners();
  test_brightness();
  test_names();
  if (g_fail == 0) {
    std::printf("ALL DISPLAY SETTINGS TESTS PASSED\n");
    return 0;
  }
  std::printf("%d DISPLAY SETTINGS TEST(S) FAILED\n", g_fail);
  return 1;
}
