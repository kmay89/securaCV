// Host test for the pure display-settings geometry + brightness helpers
// (include/canary/glass_settings.h): orientation dims, the touch un-rotation
// (round-tripped against the turn LVGL draws, on both majors), the rendered
// brightness scrim, and the standalone-weather location wheels (wheel
// positions <-> stored tenths, and the cell caption). No Arduino, no LVGL,
// no board.
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

// Where the glass DRAWS a logical pixel (logical -> native panel): the turn
// the touch map must invert, kept here, in the test, as the independent
// oracle. On LVGL 9 lvgl_port.cpp's flush_cb places each rendered area with
// lv_display_rotate_area(), quoted from lvgl v9.5.0 as written there
// (disp->hor_res / ver_res are the panel's NATIVE width and height) and
// applied to a 1x1 area. F205: the old oracle sent ROT_PORTRAIT to
// (W-1-ly, lx), LVGL's 270, so the HAL's taps followed a turn no LVGL draws.
struct Area { int x1, y1, x2, y2; };
static void lvgl9_rotate_area(uint8_t rot, int hor_res, int ver_res, Area* area) {
  const int w = area->x2 - area->x1 + 1;
  const int h = area->y2 - area->y1 + 1;
  switch (rot & 3) {
    case ROT_PORTRAIT:  // LV_DISPLAY_ROTATION_90
      area->y2 = ver_res - area->x1 - 1;
      area->x1 = area->y1;
      area->x2 = area->x1 + h - 1;
      area->y1 = area->y2 - w + 1;
      break;
    case ROT_LANDSCAPE_INV:  // LV_DISPLAY_ROTATION_180
      area->y2 = ver_res - area->y1 - 1;
      area->y1 = area->y2 - h + 1;
      area->x2 = hor_res - area->x1 - 1;
      area->x1 = area->x2 - w + 1;
      break;
    case ROT_PORTRAIT_INV:  // LV_DISPLAY_ROTATION_270
      area->x1 = hor_res - area->y2 - 1;
      area->y2 = area->x2;
      area->x2 = area->x1 + h - 1;
      area->y1 = area->y2 - w + 1;
      break;
    default: break;
  }
}

static void forward(uint8_t rot, int PW, int PH, int lx, int ly,
                    int* px, int* py) {
  Area a = {lx, ly, lx, ly};
  lvgl9_rotate_area(rot, PW, PH, &a);
  *px = a.x1;
  *py = a.y1;
}

// The same pixel through LVGL 8.4's sw_rotate (lv_refr.c draw_buf_rotate,
// quoted from lvgl v8.4.0: the area arithmetic of its 90/270 branch, where
// a rendered row band becomes a native column band, and draw_buf_rotate_180),
// the turn the emulator's LVGL 8 glass draws. Applied to a 1x1 area.
static void lvgl8_draw_buf_rotate(uint8_t rot, int hor_res, int ver_res,
                                  int lx, int ly, int* px, int* py) {
  Area a = {lx, ly, lx, ly};
  const int area_w = a.x2 - a.x1 + 1;
  const int init_y_off = a.y1;
  switch (rot & 3) {
    case ROT_PORTRAIT:  // LV_DISP_ROT_90
      a.y2 = ver_res - a.x1 - 1;
      a.y1 = a.y2 - area_w + 1;
      a.x1 = init_y_off;
      a.x2 = init_y_off + area_w - 1;
      break;
    case ROT_PORTRAIT_INV:  // LV_DISP_ROT_270
      a.y1 = a.x1;
      a.y2 = a.y1 + area_w - 1;
      a.x2 = hor_res - 1 - init_y_off;
      a.x1 = a.x2 - area_w + 1;
      break;
    case ROT_LANDSCAPE_INV: {  // draw_buf_rotate_180
      int tmp_coord = a.y2;
      a.y2 = ver_res - a.y1 - 1;
      a.y1 = ver_res - tmp_coord - 1;
      tmp_coord = a.x2;
      a.x2 = hor_res - a.x1 - 1;
      a.x1 = hor_res - tmp_coord - 1;
      break;
    }
    default: break;
  }
  *px = a.x1;
  *py = a.y1;
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

// ── Touch mapping inverts the drawn turn, exactly, at every rotation ─────
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

// Both LVGL majors draw the same quarter turns on the dash (the emulator's
// LVGL 8 glass and the shipped LVGL 9 builds), so one touch map serves both.
static void test_majors_draw_one_turn() {
  const int PW = 800, PH = 480;
  for (uint8_t rot = 0; rot < 4; rot++) {
    int LW = 0, LH = 0;
    rotation_logical_dims(rot, PW, PH, &LW, &LH);
    for (int lx = 0; lx < LW; lx += 11) {
      for (int ly = 0; ly < LH; ly += 9) {
        int p9x = 0, p9y = 0, p8x = 0, p8y = 0;
        forward(rot, PW, PH, lx, ly, &p9x, &p9y);
        lvgl8_draw_buf_rotate(rot, PW, PH, lx, ly, &p8x, &p8y);
        CHECK(p9x == p8x && p9y == p8y,
              "LVGL 8's sw_rotate draws the pixel where LVGL 9's area turn does");
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
  // Portrait (LVGL 90): the content's top-left sits at the panel's
  // bottom-left, so the panel's top-left corner is the logical top-right.
  rotation_map_touch(ROT_PORTRAIT, PW, PH, 0, 0, &x, &y);
  CHECK(x == PH - 1 && y == 0, "portrait maps panel origin to logical (479, 0)");
  rotation_map_touch(ROT_PORTRAIT, PW, PH, 0, PH - 1, &x, &y);
  CHECK(x == 0 && y == 0, "portrait: the panel's bottom-left is the logical origin");
  // Portrait flipped (LVGL 270): the panel's top-left is the logical bottom-left.
  rotation_map_touch(ROT_PORTRAIT_INV, PW, PH, 0, 0, &x, &y);
  CHECK(x == 0 && y == PW - 1, "portrait flipped maps panel origin to logical (0, 799)");
  rotation_map_touch(ROT_PORTRAIT_INV, PW, PH, PW - 1, 0, &x, &y);
  CHECK(x == 0 && y == 0, "portrait flipped: the panel's top-right is the logical origin");
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

// The HAL's un-rotation IS LVGL 9's own pointer rotation: a raw panel touch
// mapped by rotation_map_touch() lands where lv_display_rotate_point() would
// put the same sample, at every quarter turn, across the whole panel (the
// fed pointer device and the HAL's taps agree on what is under the finger).
static void test_touch_is_lvgl_indev_rotation() {
  const int PW = 800, PH = 480;
  for (uint8_t rot = 0; rot < 4; rot++) {
    for (int px = 0; px < PW; px += 13) {
      for (int py = 0; py < PH; py += 7) {
        int hx = 0, hy = 0;
        rotation_map_touch(rot, PW, PH, px, py, &hx, &hy);
        int lx = px, ly = py;
        lvgl9_rotate_point(rot, PW, PH, &lx, &ly);
        CHECK(hx == lx && hy == ly,
              "rotation_map_touch agrees with lv_display_rotate_point");
      }
    }
  }
}

// ── The LVGL 8 pointer feed inverts LVGL 8's indev rotation too ───────────
// The browser emulator compiles the dash glass against LVGL 8.4, and
// lvgl_port.cpp feeds its pointer device through rotation_to_lvgl_indev on
// both majors. LVGL 8 rotates a pointer sample in indev_pointer_proc (quoted
// here from lvgl v8.4.0 as written there: the half turn first for 180 and
// 270, then the quarter turn for 90 and 270, in the driver's NATIVE dims), so
// the same feed must come back unchanged through v8's arithmetic as well.
static void lvgl8_indev_pointer_proc(uint8_t rot, int hor_res, int ver_res,
                                     int* x, int* y) {
  const int r = rot & 3;
  if (r == ROT_LANDSCAPE_INV || r == ROT_PORTRAIT_INV) {
    *x = hor_res - *x - 1;
    *y = ver_res - *y - 1;
  }
  if (r == ROT_PORTRAIT || r == ROT_PORTRAIT_INV) {
    const int tmp = *y;
    *y = *x;
    *x = ver_res - tmp - 1;
  }
}

static void test_lvgl8_indev_feed() {
  const int PW = 800, PH = 480;
  for (uint8_t rot = 0; rot < 4; rot++) {
    int LW = 0, LH = 0;
    rotation_logical_dims(rot, PW, PH, &LW, &LH);
    for (int lx = 0; lx < LW; lx += 23) {
      for (int ly = 0; ly < LH; ly += 7) {
        int fx = 0, fy = 0;
        rotation_to_lvgl_indev(rot, PW, PH, lx, ly, &fx, &fy);
        lvgl8_indev_pointer_proc(rot, PW, PH, &fx, &fy);
        CHECK(fx == lx && fy == ly,
              "LVGL 8's own rotation returns the logical point unchanged");
      }
    }
  }
}

// ── The on-glass location wheels ─────────────────────────────────────────
// The Location page (settings → weather → location) edits a coordinate as
// hemisphere · degrees · tenths wheels per axis. These helpers are the only
// path from a wheel position to the stored tenths, so the properties that
// keep the privacy promise true live here: every producible value is ON the
// 0.1° grid and inside the range glass_settings.cpp's sanitizer accepts, the
// pole and the antimeridian clamp rather than wrap, and no wheel needs more
// options than the panel's 8-bit dispatch value can carry.
static void test_wx_wheels_examples() {
  // The canonical cell every wx test uses: 37.4 N, 122.4 W.
  CHECK(wx_wheel_to_tenths(0, 37, 4, WX_LAT_MAX_DEG) == 374, "37.4 N -> 374");
  CHECK(wx_wheel_to_tenths(1, 122, 4, WX_LON_MAX_DEG) == -1224, "122.4 W -> -1224");
  uint8_t h = 9, d = 9, t = 9;
  wx_tenths_to_wheel(374, WX_LAT_MAX_DEG, &h, &d, &t);
  CHECK(h == 0 && d == 37 && t == 4, "374 -> N 37 .4");
  wx_tenths_to_wheel(-1224, WX_LON_MAX_DEG, &h, &d, &t);
  CHECK(h == 1 && d == 122 && t == 4, "-1224 -> W 122 .4");

  // Zero: both hemispheres of 0.0 are the same point, and it reads N / E.
  CHECK(wx_wheel_to_tenths(0, 0, 0, WX_LAT_MAX_DEG) == 0, "0.0 N is 0");
  CHECK(wx_wheel_to_tenths(1, 0, 0, WX_LAT_MAX_DEG) == 0, "0.0 S is 0 too");
  wx_tenths_to_wheel(0, WX_LON_MAX_DEG, &h, &d, &t);
  CHECK(h == 0 && d == 0 && t == 0, "0 reads E 0 .0");

  // The poles: 90.0 exactly; any tenth past the pole clamps to the pole.
  CHECK(wx_wheel_to_tenths(0, 90, 0, WX_LAT_MAX_DEG) == 900, "north pole");
  CHECK(wx_wheel_to_tenths(0, 90, 5, WX_LAT_MAX_DEG) == 900, "90.5 N clamps to 90.0");
  CHECK(wx_wheel_to_tenths(1, 90, 9, WX_LAT_MAX_DEG) == -900, "90.9 S clamps to the south pole");
  // The antimeridian: 180.0, and 180.x clamps rather than wrapping to -179.x.
  CHECK(wx_wheel_to_tenths(0, 180, 0, WX_LON_MAX_DEG) == 1800, "antimeridian east");
  CHECK(wx_wheel_to_tenths(0, 180, 3, WX_LON_MAX_DEG) == 1800, "180.3 E clamps to 180.0");
  CHECK(wx_wheel_to_tenths(1, 180, 9, WX_LON_MAX_DEG) == -1800, "180.9 W clamps to 180.0 W");

  // Garbage wheel positions clamp instead of escaping the grid.
  CHECK(wx_wheel_to_tenths(0, 200, 0, WX_LAT_MAX_DEG) == 900, "deg past the pole clamps");
  CHECK(wx_wheel_to_tenths(0, 12, 12, WX_LAT_MAX_DEG) == 129, "tenth past 9 clamps to 9");
  CHECK(wx_wheel_to_tenths(0, -3, -3, WX_LAT_MAX_DEG) == 0, "negative wheel positions clamp to 0");
  // Stored values outside the range (a blob the sanitizer would have
  // rejected anyway) land on the nearest edge, never on a wrong sky.
  wx_tenths_to_wheel(950, WX_LAT_MAX_DEG, &h, &d, &t);
  CHECK(h == 0 && d == 90 && t == 0, "950 clamps to the north pole");
  wx_tenths_to_wheel(-1900, WX_LON_MAX_DEG, &h, &d, &t);
  CHECK(h == 1 && d == 180 && t == 0, "-1900 clamps to 180.0 W");
}

static void test_wx_wheels_grid() {
  // Round trip over the whole grid: every storable tenth survives
  // tenths -> wheels -> tenths unchanged.
  for (int v = -900; v <= 900; v++) {
    uint8_t h, d, t;
    wx_tenths_to_wheel(v, WX_LAT_MAX_DEG, &h, &d, &t);
    CHECK(wx_wheel_to_tenths(h, d, t, WX_LAT_MAX_DEG) == v, "latitude round-trips");
  }
  for (int v = -1800; v <= 1800; v++) {
    uint8_t h, d, t;
    wx_tenths_to_wheel(v, WX_LON_MAX_DEG, &h, &d, &t);
    CHECK(wx_wheel_to_tenths(h, d, t, WX_LON_MAX_DEG) == v, "longitude round-trips");
  }
  // Every position the wheels can take is inside the sanitizer's range
  // (glass_settings.cpp: -900..900 / -1800..1800) — the mirror of the
  // clamp that turns an out-of-range blob back into "unset".
  for (int h = 0; h <= 1; h++)
    for (int d = 0; d <= WX_LAT_MAX_DEG; d++)
      for (int t = 0; t <= 9; t++) {
        const int v = wx_wheel_to_tenths(h, d, t, WX_LAT_MAX_DEG);
        CHECK(v >= -900 && v <= 900, "every latitude wheel position is storable");
      }
  for (int h = 0; h <= 1; h++)
    for (int d = 0; d <= WX_LON_MAX_DEG; d++)
      for (int t = 0; t <= 9; t++) {
        const int v = wx_wheel_to_tenths(h, d, t, WX_LON_MAX_DEG);
        CHECK(v >= -1800 && v <= 1800, "every longitude wheel position is storable");
      }
  // The panel packs a wheel's position into 8 bits: no wheel may carry
  // more than 256 options, and the widest one here is the 0..180 degrees.
  CHECK(WX_LAT_MAX_DEG + 1 <= 256, "the latitude degrees wheel fits 8 bits");
  CHECK(WX_LON_MAX_DEG + 1 <= 256, "the longitude degrees wheel fits 8 bits");
}

static void test_wx_cell_text() {
  char b[32];
  CHECK(std::string(wx_cell_text(b, sizeof(b), 374, -1224)) == "37.4 N, 122.4 W",
        "the canonical cell");
  CHECK(std::string(wx_cell_text(b, sizeof(b), 0, 0)) == "0.0 N, 0.0 E", "zero reads N / E");
  CHECK(std::string(wx_cell_text(b, sizeof(b), -900, 1800)) == "90.0 S, 180.0 E",
        "the pole and the antimeridian");
  CHECK(std::string(wx_cell_text(b, sizeof(b), -5, 15)) == "0.5 S, 1.5 E",
        "sub-degree tenths keep their leading zero");
  // One decimal, always — the exact precision the forecast query carries.
  CHECK(std::string(wx_cell_text(b, sizeof(b), 900, -1800)).find('.') != std::string::npos,
        "one decimal is printed");
  // A short buffer truncates and terminates; it never overruns.
  char s[8];
  s[7] = 'X';
  wx_cell_text(s, 7, 374, -1224);
  CHECK(s[6] == '\0' && s[7] == 'X', "a small cap is NUL-terminated inside the cap");
}

int main() {
  test_dims();
  test_touch_roundtrip();
  test_majors_draw_one_turn();
  test_touch_is_lvgl_indev_rotation();
  test_lvgl_indev_feed();
  test_lvgl8_indev_feed();
  test_touch_corners();
  test_brightness();
  test_names();
  test_wx_wheels_examples();
  test_wx_wheels_grid();
  test_wx_cell_text();
  if (g_fail == 0) {
    std::printf("ALL DISPLAY SETTINGS TESTS PASSED\n");
    return 0;
  }
  std::printf("%d DISPLAY SETTINGS TEST(S) FAILED\n", g_fail);
  return 1;
}
