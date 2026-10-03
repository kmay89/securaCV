// canary-local/emulator/test/glass_turn_test.cpp — the turned dash glass,
// natively (F184). Built and run by glass_turn.sh against the LVGL 8.4
// checkout build.sh pins; g++ only, no emsdk.
//
// The browser emulator compiles the dash glass against LVGL 8.4, where a
// saved portrait rotation used to change nothing LVGL drew: only the 9.x
// branch of lvgl_port_set_rotation() turned the display, so a turned dash
// laid a 480x800 face out on an 800x480 canvas. This test links the REAL
// ui/lvgl_port.cpp (dash config) and the emulator's REAL display HAL
// (src/emu_hal_display.cpp, through test/native/emscripten.h), and holds:
//
//  1. the turn: lvgl_port_set_rotation() turns LVGL's logical canvas to
//     480x800 for either portrait and back to 800x480, and a landscape boot
//     (rotation 0) leaves the driver exactly as lvgl_port_init() registered it;
//  2. the glass: at every quarter turn the HAL's framebuffer — what the page
//     reads off its canvas — is the logical frame, pixel for pixel equal to
//     the same scene rendered by a plain display of the logical size, at the
//     size emu_fb_width()/emu_fb_height() report, and the HAL tells the page
//     (js_display_ready) each time the glass changes shape;
//  3. the finger: a logical point fed through lvgl_port_touch_feed() comes
//     out of LVGL's own pointer processing (indev_pointer_proc, which turns
//     every sample by the display's rotation) as that same point.
//
// Prints "ALL GLASS TURN TESTS PASSED" on success.
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "canary/glass_settings.h"
#include "canary/log.h"
#include "canary/ui/lvgl_port.h"

// ── The bench this test stands in for ───────────────────────────────────
namespace {
uint32_t g_now = 0;
int g_display_ready_calls = 0;
int g_fail = 0;
int g_checks = 0;
}  // namespace

extern "C" uint32_t millis(void) { return g_now; }
extern "C" void emu_native_em_js(const char* name) {
  if (strcmp(name, "js_display_ready") == 0) g_display_ready_calls++;
}
void EmuSerial::write_str(const char*) {}
EmuSerial Serial;
namespace canary {
LogSink g_log_sink = nullptr;
}

// The emulator HAL's exports — what the page reads.
extern "C" uint8_t* emu_fb_ptr(void);
extern "C" int emu_fb_width(void);
extern "C" int emu_fb_height(void);
namespace canary::hal {
bool display_init();
}

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

// ── The reference: a plain display of the logical size ──────────────────
namespace {
constexpr int kNativeW = 800, kNativeH = 480;
constexpr int kBufPx = kNativeW * 80;  // lvgl_port's dash draw buffer
lv_disp_draw_buf_t s_ref_dbuf;
lv_disp_drv_t s_ref_drv;
lv_color_t s_ref_buf[kBufPx];
uint16_t s_ref_fb[kNativeW * kNativeH];
lv_disp_t* s_ref = nullptr;
lv_disp_t* s_port = nullptr;

void ref_flush(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* px) {
  const int w = d->hor_res;
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++, px++)
      s_ref_fb[y * w + x] = px->full;
  lv_disp_flush_ready(d);
}

// The HAL's RGB565 -> RGBA8888 (low-bit replication), for the comparison.
void rgba(uint16_t c, uint8_t out[4]) {
  const uint8_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
  out[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
  out[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
  out[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
  out[3] = 255;
}

// A scene with every kind of pixel the onboarding draws: anti-aliased text
// in three faces (one cut to "..."), a rounded card with a border, a halo
// arc with rounded caps, a QR code, a translucent band — placed off-center
// so a wrong turn or a mirrored axis cannot read as equal.
void build_scene(lv_obj_t* scr) {
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
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
  lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
  lv_obj_set_width(body, 190);
  lv_label_set_text(body, "Basement-Mesh-Extender-Office-5G");
  lv_obj_set_pos(body, 200, 40);

  lv_obj_t* small = lv_label_create(scr);
  lv_obj_set_style_text_font(small, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(small, lv_color_hex(0xC8C0AA), 0);
  lv_label_set_text(small, "password p7Rm2Kqf");
  lv_obj_set_pos(small, 30, 430);

  lv_obj_t* qr = lv_qrcode_create(scr, 120, lv_color_black(), lv_color_white());
  const char* payload = "WIFI:T:WPA;S:SecuraCV-A7K2;P:p7Rm2Kqf;;";
  lv_qrcode_update(qr, payload, (uint32_t)strlen(payload));
  lv_obj_set_pos(qr, 180, 240);

  lv_obj_t* band = lv_obj_create(scr);
  lv_obj_remove_style_all(band);
  lv_obj_set_size(band, 440, 36);
  lv_obj_set_pos(band, 6, 380);
  lv_obj_set_style_bg_color(band, lv_color_hex(0x3355AA), 0);
  lv_obj_set_style_bg_opa(band, LV_OPA_50, 0);
}

void refresh(lv_disp_t* d) {
  lv_obj_invalidate(lv_disp_get_scr_act(d));
  lv_refr_now(d);
}

const char* rot_name(uint8_t rot) { return canary::glass::rotation_name(rot); }

// ── 1 + 2: the turn and the glass at one quarter turn ───────────────────
void check_turn(uint8_t rot) {
  const int calls_before = g_display_ready_calls;
  const int turn_before = (int)lv_disp_get_rotation(s_port);
  canary::ui::lvgl_port_set_rotation(rot);
  const bool side = canary::glass::rotation_is_portrait(rot);
  const int lw = side ? kNativeH : kNativeW, lh = side ? kNativeW : kNativeH;
  CHECK(lv_disp_get_hor_res(s_port) == lw && lv_disp_get_ver_res(s_port) == lh,
        "%s: LVGL's canvas is %dx%d, want %dx%d", rot_name(rot),
        (int)lv_disp_get_hor_res(s_port), (int)lv_disp_get_ver_res(s_port), lw,
        lh);
  CHECK(canary::ui::lvgl_port_width() == lw &&
            canary::ui::lvgl_port_height() == lh,
        "%s: lvgl_port reports %dx%d", rot_name(rot),
        (int)canary::ui::lvgl_port_width(),
        (int)canary::ui::lvgl_port_height());

  // The same scene on a plain display of the logical size.
  s_ref_drv.hor_res = (lv_coord_t)lw;
  s_ref_drv.ver_res = (lv_coord_t)lh;
  lv_disp_drv_update(s_ref, &s_ref_drv);
  build_scene(lv_disp_get_scr_act(s_ref));
  build_scene(lv_disp_get_scr_act(s_port));
  refresh(s_ref);
  refresh(s_port);

  CHECK(emu_fb_width() == lw && emu_fb_height() == lh,
        "%s: the HAL's framebuffer is %dx%d, want the turned glass's %dx%d",
        rot_name(rot), emu_fb_width(), emu_fb_height(), lw, lh);
  if ((int)lv_disp_get_rotation(s_port) != turn_before) {
    CHECK(g_display_ready_calls > calls_before,
          "%s: the glass changed shape and the HAL never told the page",
          rot_name(rot));
  }
  const uint8_t* fb = emu_fb_ptr();
  CHECK(fb != nullptr, "no framebuffer");
  if (fb == nullptr || emu_fb_width() != lw || emu_fb_height() != lh) return;
  int diff = 0, fx = -1, fy = -1;
  for (int y = 0; y < lh; y++) {
    for (int x = 0; x < lw; x++) {
      uint8_t want[4];
      rgba(s_ref_fb[y * lw + x], want);
      if (memcmp(fb + ((size_t)y * lw + x) * 4, want, 4) != 0) {
        if (diff == 0) { fx = x; fy = y; }
        diff++;
      }
    }
  }
  CHECK(diff == 0,
        "%s: %d of %d pixels on the glass differ from the scene rendered "
        "unturned at %dx%d (first at %d,%d)",
        rot_name(rot), diff, lw * lh, lw, lh, fx, fy);
  printf("  %-17s canvas %dx%d, glass %dx%d, %d px differ\n", rot_name(rot), lw,
         lh, emu_fb_width(), emu_fb_height(), diff);
}

// ── 3: the finger at one quarter turn ───────────────────────────────────
void check_touch(uint8_t rot) {
  canary::ui::lvgl_port_set_rotation(rot);
  lv_indev_t* in = lv_indev_get_next(nullptr);
  CHECK(in != nullptr, "no pointer device registered");
  if (in == nullptr) return;
  const int lw = canary::ui::lvgl_port_width();
  const int lh = canary::ui::lvgl_port_height();
  const int pts[][2] = {{0, 0}, {lw - 1, 0}, {0, lh - 1}, {lw - 1, lh - 1},
                        {37, 401}, {lw / 2 + 3, lh / 3}};
  int bad = 0;
  for (const auto& p : pts) {
    canary::ui::lvgl_port_touch_feed(true, (int16_t)p[0], (int16_t)p[1]);
    lv_indev_read_timer_cb(in->driver->read_timer);
    lv_point_t got;
    lv_indev_get_point(in, &got);
    if (got.x != p[0] || got.y != p[1]) {
      if (bad == 0) {
        printf("  FAIL detail: %s fed %d,%d, LVGL read %d,%d\n", rot_name(rot),
               p[0], p[1], (int)got.x, (int)got.y);
      }
      bad++;
    }
  }
  canary::ui::lvgl_port_touch_feed(false, 0, 0);
  lv_indev_read_timer_cb(in->driver->read_timer);
  CHECK(bad == 0, "%s: %d fed points came out of LVGL's pointer elsewhere",
        rot_name(rot), bad);
}
}  // namespace

int main() {
  CHECK(canary::hal::display_init(), "display_init failed");
  CHECK(canary::ui::lvgl_port_init(), "lvgl_port_init failed");
  s_port = lv_disp_get_default();
  CHECK(s_port != nullptr, "no display");
  if (s_port == nullptr) return 1;
  CHECK(emu_fb_width() == kNativeW && emu_fb_height() == kNativeH,
        "the glass boots %dx%d", emu_fb_width(), emu_fb_height());

  // A landscape boot (main.cpp hands rotation 0 straight through) leaves the
  // driver as registered: no software rotation, no turn.
  canary::ui::lvgl_port_set_rotation(canary::glass::ROT_LANDSCAPE);
  CHECK(!s_port->driver->sw_rotate &&
            lv_disp_get_rotation(s_port) == LV_DISP_ROT_NONE,
        "a landscape boot touched the driver (sw_rotate %d, rotated %d)",
        (int)s_port->driver->sw_rotate, (int)lv_disp_get_rotation(s_port));

  // The reference display, registered after the port's (which stays default).
  lv_disp_draw_buf_init(&s_ref_dbuf, s_ref_buf, nullptr, kBufPx);
  lv_disp_drv_init(&s_ref_drv);
  s_ref_drv.hor_res = kNativeW;
  s_ref_drv.ver_res = kNativeH;
  s_ref_drv.flush_cb = ref_flush;
  s_ref_drv.draw_buf = &s_ref_dbuf;
  s_ref = lv_disp_drv_register(&s_ref_drv);
  CHECK(s_ref != nullptr && lv_disp_get_default() == s_port,
        "reference display registration");

  printf("glass turn (LVGL %d.%d.%d, dash config):\n", LVGL_VERSION_MAJOR,
         LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
  using namespace canary::glass;
  for (uint8_t rot : {ROT_LANDSCAPE, ROT_PORTRAIT, ROT_PORTRAIT_INV,
                      ROT_LANDSCAPE_INV, ROT_PORTRAIT, ROT_LANDSCAPE}) {
    check_turn(rot);
  }
  for (uint8_t rot = 0; rot < 4; rot++) check_touch(rot);

  if (g_fail) {
    printf("%d of %d glass turn checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  printf("ALL GLASS TURN TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
