// canary-local/emulator/test/glass_turn_lvgl9_test.cpp — the turned dash
// glass on the LVGL the dash builds ship (F225). Built and run by
// glass_turn_lvgl9.sh against the LVGL 9.x release the display's Arduino
// profiles pin (sketch.yaml), compiled with the display's own lv_conf.h;
// g++ only, no emsdk, no ESP32.
//
// The dash family (the dash, its variants, dash7, nightstand7) builds LVGL
// 9.5, whose lv_display_set_rotation() swaps the logical resolution and
// turns nothing it renders: in partial render mode every area reaches the
// flush callback as a logical area over a logical-width buffer, and
// ui/lvgl_port.cpp's 9.x flush_cb turns it into the panel's native 800x480
// framebuffer (F205). The host test (tests_host/test_lvgl_port_turn) holds
// that flush against fake_lvgl9/lvgl.h, which QUOTES LVGL 9.5's rotation
// code; glass_turn.sh holds the emulator against LVGL 8.4. This test is the
// one that runs the real renderer: it links the REAL ui/lvgl_port.cpp (dash
// config, its LVGL 9 branch) against the real library and a recording
// Arduino_GFX over the native framebuffer (native9/), and holds:
//
//  1. the turn: lvgl_port_set_rotation() hands LVGL the quarter turn for
//     each glass_settings rotation (ROT_PORTRAIT is LVGL's 90), the logical
//     canvas follows (480x800 portrait), the touch layer is told, and a
//     landscape boot leaves the display unturned;
//  2. the glass: at every quarter turn a scene rendered through the port
//     (anti-aliased text in three faces, one cut to "...", a rounded card
//     with a border, a halo arc with rounded caps, a QR code, a translucent
//     band and the splash's turned tail square) lands on the panel with no
//     flushed pixel past its edge and every native pixel painted, each one
//     showing, pixel for pixel, the logical pixel that the same scene
//     rendered on a PLAIN display of the logical size puts there under the
//     quarter turn the port draws — 90: (ly, H-1-lx), 180: (W-1-lx, H-1-ly),
//     270: (W-1-ly, lx), the arithmetic glass_settings.h states and the
//     library's own lv_display_rotate_area() computes (held here too); the
//     turn's own refresh, with nothing rebuilt, repaints the whole panel;
//  3. the finger: rotation_map_touch() (display_dash.cpp's GT911 path) sends
//     every native pixel to the logical pixel drawn there, and a logical
//     point fed through lvgl_port_touch_feed() comes out of LVGL's own
//     pointer processing (lv_indev_read: indev_pointer_proc turns every
//     sample by the display's rotation) as that same point;
//  4. the tail: the splash's speech-bubble tail styles (a 12 px square,
//     radius 2, border 1, turned 450) draw a diamond on LVGL 9.5 — the path
//     LVGL 8.4 refuses on this lv_conf (F185) — hung from the square's
//     top-left corner (the default pivot), not drawn unturned;
//  5. the refusal: a glass whose turn buffer could not be allocated refuses
//     a portrait turn, says so, and keeps drawing the landscape scene
//     upright, pixel for pixel.
//
// Prints "ALL LVGL 9 GLASS TURN TESTS PASSED" on success.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <config.h>
#include "pins.h"

#include <stdio.h>
#include <string.h>

#include <vector>

#include "canary/glass_settings.h"
#include "canary/log.h"
#include "canary/ui/lvgl_port.h"

#if LVGL_VERSION_MAJOR != 9
#error "glass_turn_lvgl9_test.cpp builds against LVGL 9 (glass_turn_lvgl9.sh)"
#endif
#ifndef CD_FLAVOR_DASH
#error "glass_turn_lvgl9_test.cpp builds the dash config (glass_turn_lvgl9.sh)"
#endif

// ── The bench this test stands in for ───────────────────────────────────
namespace {
uint32_t g_now = 0;
int g_fail = 0;
int g_checks = 0;
constexpr int PW = LCD_WIDTH, PH = LCD_HEIGHT;  // the dash panel's native scan
Arduino_GFX g_panel(PW, PH);
int g_touch_rot = -1, g_touch_w = 0, g_touch_h = 0;
}  // namespace

extern "C" uint32_t millis(void) { return g_now; }
Native9Serial Serial;
namespace canary {
LogSink g_log_sink = nullptr;
}
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
      g_fail++;                                             \
      printf("  FAIL (%s:%d): ", __FILE__, __LINE__);       \
      printf(__VA_ARGS__);                                  \
      printf("\n");                                         \
    }                                                       \
  } while (0)

using namespace canary::glass;

namespace {

// ── The reference: a plain display of the logical size ──────────────────
// Its draw buffer is the port's size (lvgl_port.cpp: 800 x 80 rows x 2 B),
// so both render the same bands.
constexpr size_t kBufBytes = (size_t)PW * 80 * 2;
alignas(4) uint8_t s_ref_buf[kBufBytes];
std::vector<uint16_t> s_ref_fb;
int s_ref_w = 0, s_ref_h = 0;
lv_display_t* s_ref = nullptr;
lv_display_t* s_port = nullptr;

static_assert(LV_COLOR_DEPTH == 16, "the display's lv_conf renders RGB565");
static_assert(LV_DRAW_BUF_STRIDE_ALIGN == 1, "the reference reads packed RGB565 rows");

void ref_flush(lv_display_t* d, const lv_area_t* a, uint8_t* px_map) {
  const uint16_t* px = reinterpret_cast<const uint16_t*>(px_map);
  for (int32_t y = a->y1; y <= a->y2; y++)
    for (int32_t x = a->x1; x <= a->x2; x++, px++)
      if (x >= 0 && y >= 0 && x < s_ref_w && y < s_ref_h) s_ref_fb[(size_t)y * s_ref_w + x] = *px;
  lv_display_flush_ready(d);
}

void make_reference() {
  s_ref = lv_display_create(PW, PH);
  lv_display_set_flush_cb(s_ref, ref_flush);
  lv_display_set_buffers(s_ref, s_ref_buf, nullptr, (uint32_t)sizeof(s_ref_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
}

void size_reference(int w, int h) {
  lv_display_set_resolution(s_ref, w, h);
  s_ref_w = w;
  s_ref_h = h;
  s_ref_fb.assign((size_t)w * h, 0);
}

// The splash's tail square (splash.cpp), placed where every glass shape
// has room: its box's top-left corner.
constexpr int kTailX = 420, kTailY = 60, kTailSide = 12;
constexpr uint32_t kBg = 0x101418;

// Every kind of pixel the onboarding and the splash draw, inside the 480 px
// both of the dash's shapes share and off-center, so a wrong turn or a
// mirrored axis cannot read as equal.
void build_scene(lv_obj_t* scr) {
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_t* card = lv_obj_create(scr);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 170, 110);
  lv_obj_set_pos(card, 14, 22);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xF4EEDC), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0xFFD44F), 0);

  lv_obj_t* ring = lv_arc_create(scr);
  lv_obj_set_size(ring, 300, 300);
  lv_obj_align(ring, LV_ALIGN_TOP_LEFT, 90, 150);
  lv_arc_set_rotation(ring, 270);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_angles(ring, 0, 290);
  lv_obj_set_style_arc_width(ring, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ring, lv_color_hex(0x5A6470), LV_PART_MAIN);
  lv_obj_set_style_arc_color(ring, lv_color_hex(0xE8E2D0), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_KNOB);

  lv_obj_t* title = lv_label_create(scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_36, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE8E2D0), 0);
  lv_label_set_text(title, "Scan with your phone");
  lv_obj_set_pos(title, 20, 140);

  lv_obj_t* body = lv_label_create(scr);
  lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(body, lv_color_hex(0x9AA4AE), 0);
  lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(body, 190);
  lv_label_set_text(body, "Basement-Mesh-Extender-Office-5G");
  lv_obj_set_pos(body, 200, 40);

  lv_obj_t* small = lv_label_create(scr);
  lv_obj_set_style_text_font(small, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(small, lv_color_hex(0xC8C0AA), 0);
  lv_label_set_text(small, "password p7Rm2Kqf");
  lv_obj_set_pos(small, 30, 430);

  lv_obj_t* qr = lv_qrcode_create(scr);
  lv_qrcode_set_size(qr, 120);
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, lv_color_white());
  const char* payload = "WIFI:T:WPA;S:SecuraCV-A7K2;P:p7Rm2Kqf;;";
  lv_qrcode_update(qr, payload, (uint32_t)strlen(payload));
  lv_obj_set_pos(qr, 180, 240);

  lv_obj_t* band = lv_obj_create(scr);
  lv_obj_remove_style_all(band);
  lv_obj_set_size(band, 440, 36);
  lv_obj_set_pos(band, 6, 380);
  lv_obj_set_style_bg_color(band, lv_color_hex(0x3355AA), 0);
  lv_obj_set_style_bg_opa(band, LV_OPA_50, 0);

  // splash.cpp's tail, style for style: the square LVGL 9.5 turns through a
  // transform layer (LVGL 8.4 refuses that layer on this lv_conf, F185).
  lv_obj_t* tail = lv_obj_create(scr);
  lv_obj_set_size(tail, kTailSide, kTailSide);
  lv_obj_set_style_bg_color(tail, lv_color_hex(0xF4EEDC), 0);
  lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(tail, lv_color_hex(0xC8C0AA), 0);
  lv_obj_set_style_border_width(tail, 1, 0);
  lv_obj_set_style_radius(tail, 2, 0);
  lv_obj_set_style_transform_rotation(tail, 450, 0);
  lv_obj_clear_flag(tail, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(tail, kTailX, kTailY);
}

void refresh(lv_display_t* d) {
  lv_obj_invalidate(lv_display_get_screen_active(d));
  lv_refr_now(d);
}

lv_display_rotation_t lvgl_rotation_for(uint8_t rot) {
  switch (rot & 3) {
    case ROT_PORTRAIT:      return LV_DISPLAY_ROTATION_90;
    case ROT_LANDSCAPE_INV: return LV_DISPLAY_ROTATION_180;
    case ROT_PORTRAIT_INV:  return LV_DISPLAY_ROTATION_270;
    default:                return LV_DISPLAY_ROTATION_0;
  }
}

// Where the port draws logical (lx, ly) on the native panel: the quarter
// turn glass_settings.h states, written out here so a wrong turn in the
// port, or in this test's reading of LVGL, cannot agree with itself.
void native_of(lv_display_rotation_t r, int lx, int ly, int* px, int* py) {
  switch (r) {
    case LV_DISPLAY_ROTATION_90:  *px = ly;          *py = PH - 1 - lx; break;
    case LV_DISPLAY_ROTATION_180: *px = PW - 1 - lx; *py = PH - 1 - ly; break;
    case LV_DISPLAY_ROTATION_270: *px = PW - 1 - ly; *py = lx;          break;
    default:                      *px = lx;          *py = ly;          break;
  }
}

// RGB565 as r,g,b for a readable failure.
void say565(const char* what, uint16_t c) {
  printf("    %s 0x%04x (r%d g%d b%d)\n", what, c, (c >> 11) & 31, (c >> 5) & 63, c & 31);
}

// ── 4: the tail on the plain display ────────────────────────────────────
void check_tail(const char* tag) {
  const uint16_t bg = lv_color_to_u16(lv_color_hex(kBg));
  int ink = 0, x0 = 9999, x1 = -9999, y0 = 9999, y1 = -9999;
  for (int y = kTailY - 4; y <= kTailY + kTailSide * 2; y++) {
    for (int x = kTailX - kTailSide; x <= kTailX + kTailSide * 2; x++) {
      if (x < 0 || y < 0 || x >= s_ref_w || y >= s_ref_h) continue;
      if (s_ref_fb[(size_t)y * s_ref_w + x] == bg) continue;
      ink++;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
  }
  // A 12 px square turned 45 degrees about its top-left corner spans
  // 12*sqrt(2) = 17 px each way: x from kTailX-8.5 to kTailX+8.5, y from
  // kTailY to kTailY+17. Drawn unturned it would fill x kTailX..kTailX+11.
  const int cx2 = x0 + x1;  // twice the ink's center
  CHECK(ink >= 100, "%s: the tail draws %d px of ink (a 12 px diamond is ~144)", tag, ink);
  CHECK(ink > 0 && cx2 >= 2 * kTailX - 2 && cx2 <= 2 * kTailX + 2 && y0 >= kTailY - 1 &&
            y0 <= kTailY + 1 && y1 >= kTailY + 15 && y1 <= kTailY + 18 && x1 - x0 >= 15,
        "%s: the tail's ink spans x %d..%d, y %d..%d; a diamond hung from the box's top-left "
        "corner (%d,%d) spans x %d..%d, y %d..%d",
        tag, x0, x1, y0, y1, kTailX, kTailY, kTailX - 9, kTailX + 9, kTailY, kTailY + 17);
  const uint16_t corner = s_ref_fb[(size_t)(kTailY + 1) * s_ref_w + kTailX + kTailSide - 1];
  CHECK(corner == bg, "%s: the unturned square's top-right corner (%d,%d) is background", tag,
        kTailX + kTailSide - 1, kTailY + 1);
  printf("  %-17s tail ink %d px at x %d..%d, y %d..%d (box at %d,%d)\n", tag, ink, x0, x1, y0,
         y1, kTailX, kTailY);
}

// ── 1 + 2: the turn and the glass at one quarter turn ───────────────────
void check_turn(uint8_t rot) {
  const char* tag = rotation_name(rot);
  g_touch_rot = -1;
  canary::ui::lvgl_port_set_rotation(rot);
  const lv_display_rotation_t lr = lvgl_rotation_for(rot);
  int lw = 0, lh = 0;
  rotation_logical_dims(rot, PW, PH, &lw, &lh);
  CHECK(canary::ui::lvgl_port_rotation() == rot, "%s: the port wears the turn", tag);
  CHECK(lv_display_get_rotation(s_port) == lr, "%s: LVGL is handed quarter turn %d, got %d", tag,
        (int)lr, (int)lv_display_get_rotation(s_port));
  CHECK(lv_display_get_horizontal_resolution(s_port) == lw &&
            lv_display_get_vertical_resolution(s_port) == lh,
        "%s: LVGL's canvas is %dx%d, want %dx%d", tag,
        (int)lv_display_get_horizontal_resolution(s_port),
        (int)lv_display_get_vertical_resolution(s_port), lw, lh);
  CHECK(canary::ui::lvgl_port_width() == lw && canary::ui::lvgl_port_height() == lh,
        "%s: lvgl_port reports %dx%d", tag, (int)canary::ui::lvgl_port_width(),
        (int)canary::ui::lvgl_port_height());
  CHECK(g_touch_rot == rot && g_touch_w == PW && g_touch_h == PH,
        "%s: the touch layer is told the turn and the native size (got %d, %dx%d)", tag,
        g_touch_rot, g_touch_w, g_touch_h);

  // The table above is LVGL's own: lv_display_rotate_area() on every 1x1
  // area of the turned canvas.
  long table_off = 0;
  for (int ly = 0; ly < lh; ly++) {
    for (int lx = 0; lx < lw; lx++) {
      lv_area_t a = {lx, ly, lx, ly};
      lv_display_rotate_area(s_port, &a);
      int px = 0, py = 0;
      native_of(lr, lx, ly, &px, &py);
      if (a.x1 != px || a.y1 != py || a.x2 != px || a.y2 != py) table_off++;
    }
  }
  CHECK(table_off == 0, "%s: lv_display_rotate_area() puts %ld logical pixels elsewhere", tag,
        table_off);

  // The same scene on a plain display of the logical size.
  size_reference(lw, lh);
  build_scene(lv_display_get_screen_active(s_ref));
  refresh(s_ref);

  // The turn alone repaints the whole glass: the panel keeps scanning the
  // old frame, so a turn that left any native pixel unpainted would leave
  // the previous orientation there. The scene already on the port is laid
  // out in absolute positions, so after the turn it is the same scene.
  static bool s_scene_up = false;
  if (s_scene_up) {
    g_panel.clear_marks();
    lv_refr_now(s_port);  // no rebuild, no invalidation of our own
    long left = 0, stale = 0;
    for (int ly = 0; ly < lh; ly++) {
      for (int lx = 0; lx < lw; lx++) {
        int px = 0, py = 0;
        native_of(lr, lx, ly, &px, &py);
        if (!g_panel.painted[(size_t)py * PW + px]) left++;
        else if (g_panel.fb[(size_t)py * PW + px] != s_ref_fb[(size_t)ly * lw + lx]) stale++;
      }
    }
    CHECK(left == 0 && stale == 0 && g_panel.off_panel == 0,
          "%s: the turn's own refresh leaves %ld native pixels unpainted and %ld unlike the "
          "turned scene (%ld off the panel)",
          tag, left, stale, g_panel.off_panel);
  }

  // Then the scene built afresh through the port onto a panel whose marks
  // are cleared, so this frame alone counts.
  build_scene(lv_display_get_screen_active(s_port));
  s_scene_up = true;
  g_panel.clear_marks();
  refresh(s_port);

  long unpainted = 0;
  for (uint8_t p : g_panel.painted)
    if (!p) unpainted++;
  CHECK(g_panel.blits > 0, "%s: the port flushed nothing", tag);
  CHECK(g_panel.off_panel == 0, "%s: %ld flushed pixels fall off the %dx%d panel", tag,
        g_panel.off_panel, PW, PH);
  CHECK(unpainted == 0, "%s: %ld native pixels are never painted", tag, unpainted);
  long diff = 0;
  int fx = -1, fy = -1;
  uint16_t want0 = 0, got0 = 0;
  for (int ly = 0; ly < lh; ly++) {
    for (int lx = 0; lx < lw; lx++) {
      int px = 0, py = 0;
      native_of(lr, lx, ly, &px, &py);
      const uint16_t want = s_ref_fb[(size_t)ly * lw + lx];
      const uint16_t got = g_panel.fb[(size_t)py * PW + px];
      if (want != got) {
        if (diff == 0) {
          fx = lx;
          fy = ly;
          want0 = want;
          got0 = got;
        }
        diff++;
      }
    }
  }
  CHECK(diff == 0,
        "%s: %ld of %d pixels on the glass differ from the scene rendered on a plain %dx%d "
        "display (first at logical %d,%d)",
        tag, diff, lw * lh, lw, lh, fx, fy);
  if (diff) {
    say565("want", want0);
    say565("got ", got0);
  }

  // 3, the arithmetic half: a raw touch at every native pixel maps to the
  // logical pixel drawn there.
  long touch_off = 0;
  for (int py = 0; py < PH; py++) {
    for (int px = 0; px < PW; px++) {
      int tx = 0, ty = 0;
      rotation_map_touch(rot, PW, PH, px, py, &tx, &ty);
      int bx = -1, by = -1;
      if (tx >= 0 && ty >= 0 && tx < lw && ty < lh) native_of(lr, tx, ty, &bx, &by);
      if (bx != px || by != py) touch_off++;
    }
  }
  CHECK(touch_off == 0, "%s: a raw touch lands off the pixel drawn under it at %ld of %d pixels",
        tag, touch_off, PW * PH);
  printf("  %-17s canvas %dx%d, %ld blits, %ld off the panel, %ld unpainted, %ld px differ\n",
         tag, lw, lh, g_panel.blits, g_panel.off_panel, unpainted, diff);
  check_tail(tag);
}

// ── 3: the finger through LVGL's own pointer processing ─────────────────
void check_pointer(uint8_t rot) {
  const char* tag = rotation_name(rot);
  canary::ui::lvgl_port_set_rotation(rot);
  lv_obj_clean(lv_display_get_screen_active(s_port));  // nothing to press
  lv_indev_t* in = lv_indev_get_next(nullptr);
  CHECK(in != nullptr, "no pointer device registered");
  if (in == nullptr) return;
  const int lw = canary::ui::lvgl_port_width(), lh = canary::ui::lvgl_port_height();
  long moved = 0, n = 0;
  int bx = -1, by = -1, gx = -1, gy = -1;
  for (int ly = 0; ly < lh; ly += 23) {
    for (int lx = 0; lx < lw; lx += 17) {
      canary::ui::lvgl_port_touch_feed(true, (int16_t)lx, (int16_t)ly);
      lv_indev_read(in);
      lv_point_t got = {0, 0};
      lv_indev_get_point(in, &got);
      n++;
      if (got.x != lx || got.y != ly) {
        if (moved == 0) {
          bx = lx;
          by = ly;
          gx = (int)got.x;
          gy = (int)got.y;
        }
        moved++;
      }
    }
  }
  const int corners[4][2] = {{0, 0}, {lw - 1, 0}, {0, lh - 1}, {lw - 1, lh - 1}};
  for (const auto& p : corners) {
    canary::ui::lvgl_port_touch_feed(true, (int16_t)p[0], (int16_t)p[1]);
    lv_indev_read(in);
    lv_point_t got = {0, 0};
    lv_indev_get_point(in, &got);
    n++;
    if (got.x != p[0] || got.y != p[1]) moved++;
  }
  canary::ui::lvgl_port_touch_feed(false, 0, 0);
  lv_indev_read(in);
  CHECK(moved == 0,
        "%s: %ld of %ld fed points came out of LVGL's pointer elsewhere (first: fed %d,%d, read "
        "%d,%d)",
        tag, moved, n, bx, by, gx, gy);
}

// ── 5: a glass with no turn buffer ──────────────────────────────────────
void check_refused_turn() {
  lv_deinit();
  native9::heap_requests() = 0;
  native9::heap_grant_limit() = 1;  // the draw buffer, and nothing after it
  Serial.clear();
  CHECK(canary::ui::lvgl_port_init(), "the glass comes up without a turn buffer");
  CHECK(Serial.saw("Rotation buffer allocation FAILED"),
        "the missing turn buffer is logged at bring-up");
  s_port = lv_display_get_default();
  make_reference();
  CHECK(s_port != nullptr && lv_display_get_default() == s_port,
        "the port's display stays the default");
  if (s_port == nullptr) return;
  for (uint8_t rot : {ROT_PORTRAIT, ROT_LANDSCAPE_INV, ROT_PORTRAIT_INV}) {
    const char* tag = rotation_name(rot);
    Serial.clear();
    canary::ui::lvgl_port_set_rotation(rot);
    CHECK(Serial.saw("staying landscape"), "%s refused: the refusal is logged", tag);
    CHECK(canary::ui::lvgl_port_rotation() == ROT_LANDSCAPE &&
              lv_display_get_rotation(s_port) == LV_DISPLAY_ROTATION_0 &&
              lv_display_get_horizontal_resolution(s_port) == PW &&
              lv_display_get_vertical_resolution(s_port) == PH && g_touch_rot == ROT_LANDSCAPE,
          "%s refused: the port, LVGL and the touch layer stay landscape", tag);
    size_reference(PW, PH);
    build_scene(lv_display_get_screen_active(s_ref));
    refresh(s_ref);
    build_scene(lv_display_get_screen_active(s_port));
    g_panel.clear_marks();
    refresh(s_port);
    long diff = 0, unpainted = 0;
    for (size_t i = 0; i < g_panel.fb.size(); i++) {
      if (!g_panel.painted[i]) unpainted++;
      if (g_panel.fb[i] != s_ref_fb[i]) diff++;
    }
    CHECK(g_panel.off_panel == 0 && unpainted == 0 && diff == 0,
          "%s refused: the glass shows the landscape scene upright (%ld off the panel, %ld "
          "unpainted, %ld px differ)",
          tag, g_panel.off_panel, unpainted, diff);
    printf("  %-17s refused (no turn buffer): %ld px differ from the landscape scene\n", tag,
           diff);
  }
  native9::heap_grant_limit() = -1;
}

}  // namespace

int main() {
  printf("glass turn (LVGL %d.%d.%d, the real renderer, dash config):\n", LVGL_VERSION_MAJOR,
         LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
  CHECK(canary::ui::lvgl_port_init(), "lvgl_port_init failed");
  CHECK(!Serial.saw("FAILED"), "the glass came up with both buffers");
  s_port = lv_display_get_default();
  CHECK(s_port != nullptr, "no display");
  if (s_port == nullptr) return 1;
  CHECK(lv_display_get_horizontal_resolution(s_port) == PW &&
            lv_display_get_vertical_resolution(s_port) == PH,
        "the port's display is the panel's native %dx%d", PW, PH);

  // A landscape boot (main.cpp hands rotation 0 straight through) leaves
  // LVGL unturned.
  canary::ui::lvgl_port_set_rotation(ROT_LANDSCAPE);
  CHECK(lv_display_get_rotation(s_port) == LV_DISPLAY_ROTATION_0,
        "a landscape boot turned LVGL (%d)", (int)lv_display_get_rotation(s_port));

  // The reference display, created after the port's (which stays default).
  make_reference();
  CHECK(lv_display_get_default() == s_port, "the port's display stays the default");

  for (uint8_t rot : {ROT_LANDSCAPE, ROT_PORTRAIT, ROT_PORTRAIT_INV, ROT_LANDSCAPE_INV,
                      ROT_PORTRAIT, ROT_LANDSCAPE}) {
    check_turn(rot);
  }
  for (uint8_t rot = 0; rot < 4; rot++) check_pointer(rot);
  check_refused_turn();

  if (g_fail) {
    printf("%d of %d LVGL 9 glass turn checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  printf("ALL LVGL 9 GLASS TURN TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
