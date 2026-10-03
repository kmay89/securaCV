// Host test (F205): the dash glass's LVGL 9 flush turns what it draws, and a
// tap lands on what was drawn under it.
//
// LVGL 9.5's lv_display_set_rotation() swaps the logical resolution and
// turns nothing: in partial render mode every area reaches the flush
// callback as a LOGICAL area over a packed, logical-width buffer. The dash
// family's RGB framebuffer is the panel's native 800x480, so
// src/ui/lvgl_port.cpp's flush_cb must turn each area into it. This test
// compiles the REAL lvgl_port.cpp (dash config, LVGL 9 branch) against
// fake_lvgl9/lvgl.h — LVGL 9.5's display layer, its rotation arithmetic
// quoted from lvgl v9.5.0 — and a recording Arduino_GFX over the native
// framebuffer, then plays LVGL's part: it fills the draw buffer the port
// registered with each band of the logical canvas (two passes, one holding
// each pixel's logical x, one its logical y) and calls the flush callback
// the port registered. Reading the framebuffer back says, for every native
// pixel, which logical pixel the glass shows there. It holds, at every
// quarter turn:
//
//  1. the turn: lvgl_port_set_rotation() hands LVGL the quarter turn for
//     each glass_settings rotation (ROT_PORTRAIT is LVGL's 90) and the
//     logical canvas follows (480x800 portrait), the screen invalidated and
//     the touch layer told;
//  2. the glass: no flush reaches past the 800x480 panel, every native pixel
//     is painted, and each shows the logical pixel lv_display_rotate_area()
//     sends there — the turned face, not the top 480 rows of it unturned
//     (and "landscape, flipped" upside down, not upright); odd-sized areas
//     anywhere on the canvas land on their turned rectangle;
//  3. the finger: a raw touch at ANY native pixel, mapped by
//     rotation_map_touch() the way display_dash.cpp's touch_read() maps the
//     GT911's sample, is the logical pixel drawn there; and a logical point
//     fed to the port's pointer device comes out of LVGL's own pointer
//     rotation (lv_display_rotate_point) where it went in;
//  4. the memory: the turn buffer is the draw buffer's size from the same
//     tier (two 128,000 B PSRAM buffers; 25,600 B internal on the PSRAM-less
//     fallback, where the glass still turns), and a glass with no turn
//     buffer refuses a turn, says so, and stays landscape.
//
// Prints "ALL LVGL PORT TURN TESTS PASSED" on success.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "pins.h"
#include "canary/glass_settings.h"
#include "canary/log.h"
#include "canary/ui/lvgl_port.h"

FakeSerial Serial;
namespace canary {
LogSink g_log_sink = nullptr;
}

namespace {
const int PW = LCD_WIDTH, PH = LCD_HEIGHT;  // the dash panel's native scan
Arduino_GFX g_panel(PW, PH);
int g_touch_rot = -1, g_touch_w = 0, g_touch_h = 0;
int g_fail = 0;
int g_checks = 0;
}  // namespace

namespace canary::hal {
Arduino_GFX* gfx() { return &g_panel; }
void touch_set_rotation(uint8_t rot, int16_t native_w, int16_t native_h) {
  g_touch_rot = rot;
  g_touch_w = native_w;
  g_touch_h = native_h;
}
}  // namespace canary::hal

#define CHECK(cond, ...)                                    \
  do {                                                      \
    g_checks++;                                             \
    if (!(cond)) {                                          \
      std::printf("  FAIL (%s:%d): ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                             \
      std::printf("\n");                                    \
      g_fail++;                                             \
    }                                                       \
  } while (0)

using namespace canary::glass;

static lv_display_rotation_t lvgl_rotation_for(uint8_t rot) {
  switch (rot & 3) {
    case ROT_PORTRAIT:      return LV_DISPLAY_ROTATION_90;
    case ROT_LANDSCAPE_INV: return LV_DISPLAY_ROTATION_180;
    case ROT_PORTRAIT_INV:  return LV_DISPLAY_ROTATION_270;
    default:                return LV_DISPLAY_ROTATION_0;
  }
}

// Where LVGL puts a logical pixel on the native panel: its own
// lv_display_rotate_area() on a 1x1 area of a display of the panel's size.
static void lvgl_turn(lv_display_rotation_t r, int lx, int ly, int* px, int* py) {
  lv_display_t d;
  d.hor_res = PW;
  d.ver_res = PH;
  d.rotation = r;
  lv_area_t a = {lx, ly, lx, ly};
  lv_display_rotate_area(&d, &a);
  *px = a.x1;
  *py = a.y1;
}

// LVGL's part in partial render mode (lv_refr.c call_flush_cb): an area's
// logical pixels, packed into the registered draw buffer, handed to the
// registered flush callback.
template <class F>
static void flush_area(lv_display_t* d, lv_area_t a, F value) {
  const int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
  CHECK((size_t)w * h * 2 <= d->buf_size, "an area of %dx%d fits the %u B draw buffer", w, h,
        (unsigned)d->buf_size);
  if ((size_t)w * h * 2 > d->buf_size) return;  // LVGL never renders one larger
  uint16_t* px = reinterpret_cast<uint16_t*>(d->buf1);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) px[(size_t)y * w + x] = value(a.x1 + x, a.y1 + y);
  const int before = d->flush_ready;
  d->flushing = 1;
  d->flush_cb(d, &a, d->buf1);
  CHECK(d->flush_ready == before + 1 && d->flushing == 0,
        "flush_cb reports every flush ready exactly once");
}

// The whole logical canvas in full-width bands of as many rows as the draw
// buffer holds (lv_refr.c's get_max_row: buffer size / stride).
template <class F>
static void flush_canvas(lv_display_t* d, int lw, int lh, F value) {
  const int rows = (int)(d->buf_size / (uint32_t)(lw * 2));
  for (int y0 = 0; y0 < lh; y0 += rows) {
    flush_area(d, lv_area_t{0, y0, lw - 1, std::min(y0 + rows, lh) - 1}, value);
  }
}

// What the glass shows at each native pixel: the logical x and y drawn there
// (0xFFFF where nothing was).
struct Glass {
  std::vector<uint16_t> lx, ly;
  long off_panel = 0;
};
template <class Flush>
static Glass read_glass(Flush flush) {
  Glass g;
  g_panel.clear(0xFFFF);
  flush([](int x, int) { return (uint16_t)x; });
  g.lx = g_panel.fb;
  g.off_panel = g_panel.off_panel;
  g_panel.clear(0xFFFF);
  flush([](int, int y) { return (uint16_t)y; });
  g.ly = g_panel.fb;
  g.off_panel += g_panel.off_panel;
  return g;
}

// Holds 2 and 3 on a whole canvas drawn at rotation `rot`.
static void check_turned_glass(const char* tag, uint8_t rot) {
  lv_display_t* d = fake_lvgl9::display();
  const int lw = canary::ui::lvgl_port_width(), lh = canary::ui::lvgl_port_height();
  const lv_display_rotation_t lr = lvgl_rotation_for(rot);
  Glass g = read_glass([&](auto value) { flush_canvas(d, lw, lh, value); });
  long unpainted = 0, wrong = 0, touch_off = 0;
  for (int py = 0; py < PH; py++) {
    for (int px = 0; px < PW; px++) {
      const size_t i = (size_t)py * PW + px;
      if (g.lx[i] == 0xFFFF || g.ly[i] == 0xFFFF) { unpainted++; continue; }
      int ex = 0, ey = 0;
      lvgl_turn(lr, g.lx[i], g.ly[i], &ex, &ey);
      if (ex != px || ey != py) wrong++;
      int tx = 0, ty = 0;
      rotation_map_touch(rot, PW, PH, px, py, &tx, &ty);
      if (tx != g.lx[i] || ty != g.ly[i]) touch_off++;
    }
  }
  CHECK(g.off_panel == 0, "%s: %ld flushed pixels fall off the %dx%d panel", tag, g.off_panel, PW,
        PH);
  CHECK(unpainted == 0, "%s: %ld native pixels are never painted", tag, unpainted);
  CHECK(wrong == 0, "%s: %ld native pixels show a logical pixel LVGL's turn does not put there",
        tag, wrong);
  CHECK(touch_off == 0,
        "%s: a raw touch lands off the pixel drawn under it at %ld of %d native pixels", tag,
        touch_off, PW * PH);
}

// Odd-sized areas anywhere on the turned canvas land on their turned
// rectangle, pixel for pixel.
static void check_odd_areas(const char* tag, uint8_t rot) {
  lv_display_t* d = fake_lvgl9::display();
  const int lw = canary::ui::lvgl_port_width(), lh = canary::ui::lvgl_port_height();
  const lv_display_rotation_t lr = lvgl_rotation_for(rot);
  const lv_area_t areas[] = {
      {0, 0, 0, 0},                // one pixel at the origin
      {7, 13, 43, 23},             // 37x11 near the top-left
      {lw - 5, lh - 3, lw - 1, lh - 1},  // the far corner
      {lw / 2 - 4, 1, lw / 2 + 3, lh - 2},  // a tall strip (fits the smallest buffer)
  };
  for (const lv_area_t& a : areas) {
    lv_area_t want = a;
    lv_display_t dd;
    dd.hor_res = PW;
    dd.ver_res = PH;
    dd.rotation = lr;
    lv_display_rotate_area(&dd, &want);
    Glass g = read_glass([&](auto value) { flush_area(d, a, value); });
    const Arduino_GFX::Blit& b = g_panel.blits.back();
    CHECK(b.x == want.x1 && b.y == want.y1 && b.w == lv_area_get_width(&want) &&
              b.h == lv_area_get_height(&want),
          "%s: area (%d,%d)-(%d,%d) is blitted at its turned rectangle (%d,%d %dx%d), got (%d,%d "
          "%dx%d)",
          tag, a.x1, a.y1, a.x2, a.y2, want.x1, want.y1, lv_area_get_width(&want),
          lv_area_get_height(&want), b.x, b.y, b.w, b.h);
    long painted = 0, wrong = 0;
    for (int py = 0; py < PH; py++) {
      for (int px = 0; px < PW; px++) {
        const size_t i = (size_t)py * PW + px;
        if (g.lx[i] == 0xFFFF) continue;
        painted++;
        int ex = 0, ey = 0;
        lvgl_turn(lr, g.lx[i], g.ly[i], &ex, &ey);
        if (ex != px || ey != py) wrong++;
      }
    }
    const long n = (long)lv_area_get_width(&a) * lv_area_get_height(&a);
    CHECK(painted == n && wrong == 0 && g.off_panel == 0,
          "%s: area (%d,%d)-(%d,%d) paints its %ld pixels where LVGL turns them (painted %ld, "
          "misplaced %ld, off the panel %ld)",
          tag, a.x1, a.y1, a.x2, a.y2, n, painted, wrong, g.off_panel);
  }
}

// Hold 1, then 2 and 3, for one quarter turn.
static void check_rotation(uint8_t rot) {
  const char* tag = rotation_name(rot);
  lv_display_t* d = fake_lvgl9::display();
  const int inval_before = fake_lvgl9::screen()->invalidations;
  g_touch_rot = -1;
  canary::ui::lvgl_port_set_rotation(rot);
  int lw = 0, lh = 0;
  rotation_logical_dims(rot, PW, PH, &lw, &lh);
  CHECK(canary::ui::lvgl_port_rotation() == rot, "%s: the port wears the turn", tag);
  CHECK(canary::ui::lvgl_port_width() == lw && canary::ui::lvgl_port_height() == lh,
        "%s: the logical canvas is %dx%d", tag, lw, lh);
  CHECK(lv_display_get_rotation(d) == lvgl_rotation_for(rot),
        "%s: LVGL is handed quarter turn %d, got %d", tag, (int)lvgl_rotation_for(rot),
        (int)lv_display_get_rotation(d));
  CHECK(fake_lvgl9::screen()->invalidations > inval_before,
        "%s: the active screen is invalidated (the panel keeps the old frame)", tag);
  CHECK(g_touch_rot == rot && g_touch_w == PW && g_touch_h == PH,
        "%s: the touch layer is told the turn and the native size", tag);
  check_turned_glass(tag, rot);
  check_odd_areas(tag, rot);

  // The fed pointer device: a logical point, through the port's read_cb and
  // LVGL's own pointer rotation, comes out where it went in.
  lv_indev_t* in = &fake_lvgl9::st().indev;
  long moved = 0, n = 0;
  for (int ly = 0; ly < lh; ly += 23) {
    for (int lx = 0; lx < lw; lx += 17) {
      canary::ui::lvgl_port_touch_feed(true, (int16_t)lx, (int16_t)ly);
      lv_indev_data_t data = {};
      in->read_cb(in, &data);
      lv_display_rotate_point(in->disp, &data.point);
      n++;
      if (data.point.x != lx || data.point.y != ly || data.state != LV_INDEV_STATE_PRESSED)
        moved++;
    }
  }
  canary::ui::lvgl_port_touch_feed(false, 0, 0);
  CHECK(moved == 0, "%s: %ld of %ld fed points move through LVGL's pointer rotation", tag, moved,
        n);
}

// ── With PSRAM: the dash's arrangement ───────────────────────────────────
static void test_psram_glass() {
  fake_heap::reset();
  CHECK(canary::ui::lvgl_port_init(), "lvgl_port_init brings the glass up");
  lv_display_t* d = fake_lvgl9::display();
  CHECK(d && d->hor_res == PW && d->ver_res == PH && d->render_mode == LV_DISPLAY_RENDER_MODE_PARTIAL,
        "one partial-mode display of the panel's native size");
  const size_t draw = (size_t)PW * 80 * 2;
  CHECK(d && d->buf_size == draw, "the draw buffer is %zu B", draw);
  const auto& a = fake_heap::allocs();
  CHECK(a.size() == 2, "two capability allocations (draw buffer, turn buffer), got %zu", a.size());
  for (size_t i = 0; i < a.size(); i++)
    CHECK(a[i].bytes == draw && (a[i].caps & MALLOC_CAP_SPIRAM),
          "allocation %zu: %zu B from PSRAM (got %zu B, caps 0x%x)", i, draw, a[i].bytes,
          (unsigned)a[i].caps);
  for (uint8_t rot : {ROT_LANDSCAPE, ROT_PORTRAIT, ROT_LANDSCAPE_INV, ROT_PORTRAIT_INV,
                      ROT_PORTRAIT, ROT_LANDSCAPE}) {
    check_rotation(rot);
  }
}

// ── Without PSRAM: both buffers fall back to internal RAM, and still turn ─
static void test_internal_glass() {
  fake_heap::reset();
  fake_heap::refuse_spiram() = true;
  CHECK(canary::ui::lvgl_port_init(), "lvgl_port_init brings the glass up without PSRAM");
  lv_display_t* d = fake_lvgl9::display();
  const size_t draw = (size_t)PW * 16 * 2;
  CHECK(d && d->buf_size == draw, "the internal draw buffer is %zu B", draw);
  const auto& a = fake_heap::allocs();
  CHECK(a.size() == 1 && a[0].bytes == draw && (a[0].caps & MALLOC_CAP_INTERNAL) &&
            !(a[0].caps & MALLOC_CAP_SPIRAM),
        "the turn buffer falls back to %zu B of internal RAM", draw);
  for (uint8_t rot : {ROT_PORTRAIT, ROT_PORTRAIT_INV, ROT_LANDSCAPE_INV, ROT_LANDSCAPE}) {
    check_rotation(rot);
  }
}

// ── No turn buffer at all: the glass refuses the turn and stays landscape ─
static void test_no_turn_buffer() {
  fake_heap::reset();
  fake_heap::refuse_all() = true;
  Serial.text.clear();
  CHECK(canary::ui::lvgl_port_init(), "the glass still comes up (landscape needs no turn buffer)");
  CHECK(Serial.text.find("Rotation buffer allocation FAILED") != std::string::npos,
        "the missing turn buffer is logged at bring-up");
  lv_display_t* d = fake_lvgl9::display();
  for (uint8_t rot : {ROT_PORTRAIT, ROT_LANDSCAPE_INV, ROT_PORTRAIT_INV}) {
    Serial.text.clear();
    g_touch_rot = -1;
    canary::ui::lvgl_port_set_rotation(rot);
    CHECK(canary::ui::lvgl_port_rotation() == ROT_LANDSCAPE && canary::ui::lvgl_port_width() == PW &&
              canary::ui::lvgl_port_height() == PH,
          "%s refused: the port stays landscape at %dx%d", rotation_name(rot), PW, PH);
    CHECK(d && lv_display_get_rotation(d) == LV_DISPLAY_ROTATION_0,
          "%s refused: LVGL is left unturned", rotation_name(rot));
    CHECK(g_touch_rot == ROT_LANDSCAPE, "%s refused: the touch layer stays landscape",
          rotation_name(rot));
    CHECK(Serial.text.find("staying landscape") != std::string::npos,
          "%s refused: the refusal is logged", rotation_name(rot));
    check_turned_glass("refused turn", ROT_LANDSCAPE);
  }
}

int main() {
  test_psram_glass();
  test_internal_glass();
  test_no_turn_buffer();
  if (g_fail) {
    std::printf("%d of %d LVGL PORT TURN CHECK(S) FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("ALL LVGL PORT TURN TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
