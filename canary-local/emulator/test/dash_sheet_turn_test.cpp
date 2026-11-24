// canary-local/emulator/test/dash_sheet_turn_test.cpp — the dash turned by
// the phone under an open settings sheet (F370), natively.
//
// runtime_turn.sh builds this driver with build.sh's whole dash TU list
// (src/main.cpp and the firmware, the emulator's src/*.cpp and shims, LVGL
// 8.4, Ed25519) and the dash's defines, with g++, the way it builds
// runtime_turn_test.cpp for the nightlight. The driver plays the page as that
// one does: every call into it (EM_JS) lands here, the clock is virtual
// (emscripten_sleep advances it in 5 ms steps), the entropy is seeded and the
// wall date is fixed at a June noon.
//
// Each case is one boot of a provisioned dash, landscape (800x480), in its
// own process. At 7 s the driver opens the settings sheet (the 620x440
// centered sheet over the poster) and at 8 s turns the glass to portrait the
// way the phone's /api/set does (net/glass_web.cpp: the stored rotation and
// settings_mark_dirty(), nothing that touches LVGL; main.cpp's render hands
// it to the port on its next pass, within a second). The sheet must then be laid out for the frame
// the glass wears: its host the whole 480x800 column, and the Orientation
// row (the root page) or the Orientation page's check naming the turn worn,
// Portrait. Before F370 settings_ui_tick never laid an open sheet out again,
// so the 620x440 sheet sat in the 480x800 frame saying Landscape until the
// next navigation.
//
//  * root:   the sheet open on its root page; the Orientation row's value.
//  * page:   the Orientation row tapped first (a real tap through the touch
//            HAL and LVGL), so the Orientation page is open; its check.
//  * finger: a finger down on the poster beside the sheet when the turn
//            lands, dragged 40 px at once (a drag, past the 12 px slop, is
//            never the stationary hold that closes the sheet) and held 2 s,
//            past main.cpp's next render, which turns the port under it:
//            while it is down nothing is rebuilt (a rebuild would pull the
//            list out from under a scroll), and on the lift the sheet is
//            laid out for the turn.
//
//   dash_sheet_turn_test            every case, a line per case, exit 1 on a failure
//   dash_sheet_turn_test -v         the same with the firmware's serial log on stderr
//
// This proves the sources build.sh hands em++ for the dash, built with g++
// (64-bit), not the dist's bytes.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "lvgl.h"
#include "canary/glass_settings.h"
#include "canary/ui/lvgl_port.h"
#include "canary/ui/settings_ui.h"

extern "C" {
// ── the emulator's exports this page uses ──
void emu_power_on(void);
void emu_nvs_preseed_hex(const char*, const char*, const char*);
void emu_set_lan(const char*);
void emu_set_tz(const char*);
void emu_seed(unsigned);
void emu_epoch_offset(double);
void emu_touch(int down, int x, int y);
}
int emu_entry();  // emu_main.cpp's main(), renamed by runtime_turn.sh

namespace {

constexpr double kOpenAt = 7000;       // ms: the face is up; open the sheet
constexpr double kTapAt = 7600;        // the page case taps the Orientation row
constexpr double kTapFor = 120;        // ...held this long (LVGL reads every 30 ms)
constexpr double kTurnAt = 8000;       // the phone turns the glass
constexpr double kDragAt = 50;         // the finger case: dragged this long after the turn
constexpr double kFingerFor = 2000;    // ...down this long over the turn (render runs >= 1 Hz)
constexpr double kHeldRead = 1700;     // ...and read this far into it, still down
constexpr double kReadAfter = 1500;    // ms after the turn (or the lift): read the sheet
constexpr double kNoon = 1781524800.0; // 2026-06-15 12:00:00 UTC

enum class Kind { Root, Page, Finger };

// What the sheet looked like at one read.
struct Look {
  int valid;              // the sheet was open and its host found
  int canvas_w, canvas_h; // LVGL's logical canvas (the frame the glass wears)
  int rot;                // lvgl_port_rotation()
  int host_w, host_h;     // the sheet's host (its first child of the screen)
  int on_page;            // the page case: the Orientation page is open
  char value[24];         // root: the Orientation row's value
  char checked[24];       // page: the row the check sits on
};

struct Seen {
  int reached_end;
  int opened;     // settings_ui_active() after the open
  Look before;    // just before the turn
  Look held;      // the finger case, the finger still down
  Look after;     // kReadAfter after the turn (the lift, in the finger case)
  int active_end; // the sheet still open at the end (no hold closed it)
};

bool g_verbose = false;
Kind g_kind = Kind::Root;
int g_out_fd = -1;
double g_wall = 0;
Seen g_seen;
uint32_t g_rng = 0x2545F491u;
int g_step = 0;  // the next scheduled action

const char* label_text(lv_obj_t* o) {
  return (o && lv_obj_check_type(o, &lv_label_class)) ? lv_label_get_text(o) : nullptr;
}

// The first label under `o` whose text is `text`, depth first.
lv_obj_t* find_label(lv_obj_t* o, const char* text) {
  if (o == nullptr) return nullptr;
  const char* t = label_text(o);
  if (t && strcmp(t, text) == 0) return o;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) {
    lv_obj_t* f = find_label(lv_obj_get_child(o, (int32_t)i), text);
    if (f) return f;
  }
  return nullptr;
}

// The row a title label sits in: [title, value?, chevron or check?]
// (settings_ui.cpp's mk_title_value, row_nav, row_check).
lv_obj_t* row_of(const char* title) {
  lv_obj_t* l = find_label(lv_scr_act(), title);
  return l ? lv_obj_get_parent(l) : nullptr;
}

void read_look(Look* k) {
  memset(k, 0, sizeof(*k));
  k->canvas_w = canary::ui::lvgl_port_width();
  k->canvas_h = canary::ui::lvgl_port_height();
  k->rot = canary::ui::lvgl_port_rotation();
  if (!canary::ui::settings_ui_active()) return;
  lv_obj_t* scr = lv_scr_act();
  if (scr == nullptr || lv_obj_get_child_cnt(scr) == 0) return;
  lv_obj_update_layout(scr);
  lv_obj_t* host = lv_obj_get_child(scr, 0);
  k->valid = find_label(host, "Settings") != nullptr || find_label(host, "Orientation") != nullptr;
  k->host_w = lv_obj_get_width(host);
  k->host_h = lv_obj_get_height(host);
  // The Orientation page: four check rows; the check is LV_SYMBOL_OK.
  const char* rows[4] = {"Landscape", "Portrait", "Landscape, Flipped", "Portrait, Flipped"};
  k->on_page = find_label(host, "Landscape, Flipped") != nullptr;
  // The root's Orientation row: its value is the row's second label (on the
  // Orientation page the word is the navigation bar's title instead).
  if (lv_obj_t* r = k->on_page ? nullptr : row_of("Orientation")) {
    if (lv_obj_get_child_cnt(r) >= 2) {
      const char* v = label_text(lv_obj_get_child(r, 1));
      if (v) snprintf(k->value, sizeof k->value, "%s", v);
    }
  }
  for (const char* t : rows) {
    lv_obj_t* r = row_of(t);
    if (r && find_label(r, LV_SYMBOL_OK)) snprintf(k->checked, sizeof k->checked, "%s", t);
  }
}

// The center of the root's Orientation row, on the glass (landscape: the
// panel's own pixels are LVGL's).
bool orientation_row_center(int* x, int* y) {
  lv_obj_t* r = row_of("Orientation");
  if (r == nullptr) return false;
  lv_obj_update_layout(lv_scr_act());
  lv_area_t a;
  lv_obj_get_coords(r, &a);
  *x = (a.x1 + a.x2) / 2;
  *y = (a.y1 + a.y2) / 2;
  return true;
}

void finish() {
  g_seen.reached_end = 1;
  g_seen.active_end = canary::ui::settings_ui_active() ? 1 : 0;
  const ssize_t n = write(g_out_fd, &g_seen, sizeof(g_seen));
  _exit(n == (ssize_t)sizeof(g_seen) ? 0 : 5);
}

// The phone's turn, as net/glass_web.cpp's /api/set makes it on the dash
// (gs.rotation = v; settings_mut() = gs; settings_mark_dirty()): the stored
// rotation and its debounced commit, and nothing that touches LVGL.
void phone_turn() {
  canary::glass::settings_mut().rotation = 1;
  canary::glass::settings_mark_dirty();
}

// Every 5 ms of virtual time: the case's script, in order.
void beat() {
  const double t = g_wall;
  switch (g_step) {
    case 0:
      if (t < kOpenAt) return;
      canary::ui::settings_ui_open();
      g_seen.opened = canary::ui::settings_ui_active() ? 1 : 0;
      g_step = g_kind == Kind::Page ? 1 : 3;
      return;
    case 1: {  // the page case: tap the Orientation row
      if (t < kTapAt) return;
      int x = 0, y = 0;
      if (!orientation_row_center(&x, &y)) finish();
      emu_touch(1, x, y);
      g_step = 2;
      return;
    }
    case 2:
      if (t < kTapAt + kTapFor) return;
      emu_touch(0, 0, 0);
      g_step = 3;
      return;
    case 3:
      if (t < kTurnAt) return;
      read_look(&g_seen.before);
      if (g_kind == Kind::Finger) emu_touch(1, 20, 20);  // the poster, beside the sheet
      phone_turn();
      g_step = g_kind == Kind::Finger ? 4 : 7;
      return;
    case 4:
      if (t < kTurnAt + kDragAt) return;
      emu_touch(1, 20, 60);  // a drag: past the slop, never the hold that closes the sheet
      g_step = 5;
      return;
    case 5:
      if (t < kTurnAt + kHeldRead) return;
      read_look(&g_seen.held);
      g_step = 6;
      return;
    case 6:
      if (t < kTurnAt + kFingerFor) return;
      emu_touch(0, 0, 0);
      g_step = 7;
      return;
    case 7: {
      const double from = g_kind == Kind::Finger ? kTurnAt + kFingerFor : kTurnAt;
      if (t < from + kReadAfter) return;
      read_look(&g_seen.after);
      finish();
    }
  }
}

std::string hex(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) {
    o += d[c >> 4];
    o += d[c & 15];
  }
  return o;
}

void preseed(const char* ns, const char* key, const std::string& v) {
  emu_nvs_preseed_hex(ns, key, hex(v).c_str());
}

[[noreturn]] void boot(Kind k, int fd) {
  g_kind = k;
  g_out_fd = fd;
  memset(&g_seen, 0, sizeof(g_seen));
  emu_seed(1234);
  emu_set_tz("UTC0");
  emu_epoch_offset(kNoon - (double)time(nullptr));
  const std::string lan = hex("HomeNet") + " -52 1 1 " + hex("correct-horse") + "\n" +
                          hex("Corner Cafe Guest") + " -83 0 0 -";
  emu_set_lan(lan.c_str());
  preseed("securacv", "wifi_ssid", "HomeNet");
  preseed("securacv", "wifi_pass", "correct-horse");
  preseed("securacv", "mqtt_host", "hub.local");
  preseed("securacv", "mqtt_user", "fleet");
  preseed("securacv", "mqtt_pass", "fleet");
  preseed("scv-hello", "met", std::string("\x01", 1));
  emu_power_on();
  emu_entry();
  _exit(6);  // the firmware's loop never returns
}

const char* kind_name(Kind k) {
  return k == Kind::Root ? "root" : k == Kind::Page ? "page" : "finger";
}

bool run_case(Kind k, Seen* out) {
  int p[2];
  if (pipe(p) != 0) return false;
  fflush(nullptr);
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    close(p[0]);
    boot(k, p[1]);
  }
  close(p[1]);
  memset(out, 0, sizeof(*out));
  size_t got = 0;
  while (got < sizeof(*out)) {
    const ssize_t n = read(p[0], (char*)out + got, sizeof(*out) - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  close(p[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (got != sizeof(*out) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    printf("  the %s case ended early (status %d, %zu bytes)\n", kind_name(k), status, got);
    return false;
  }
  return true;
}

int g_fail = 0;
int g_checks = 0;

#define CHECK(cond, ...)                                  \
  do {                                                    \
    g_checks++;                                           \
    if (!(cond)) {                                        \
      printf("  FAIL (%s:%d): ", __FILE__, __LINE__);     \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
      g_fail++;                                           \
    }                                                     \
  } while (0)

void print_look(const char* when, const Look& k) {
  printf("  %s: canvas %dx%d (turn %d), sheet %s %dx%d, Orientation row \"%s\", check on \"%s\"%s\n",
         when, k.canvas_w, k.canvas_h, k.rot, k.valid ? "open" : "NOT FOUND", k.host_w, k.host_h,
         k.value, k.checked, k.on_page ? " (the Orientation page)" : "");
}

}  // namespace

extern "C" {
// ── the page's side of every EM_JS (onboard.test.js's A81 holds each of
//    these to the EM_JS it stands in for, as runtime_turn_test.cpp's) ──
void js_serial_write(const char* s) {
  if (g_verbose) fputs(s, stderr);
}
void js_backlight(int, int, int) {}
void js_tone(int, int) {}
void js_nvs_write(const char*, const char*, const char*) {}
void js_nvs_remove(const char*, const char*) {}
void js_reboot_request(void) {
  fprintf(stderr, "the firmware asked for a reboot\n");
  _exit(3);
}
double js_entropy(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return (double)(g_rng >> 8) / (double)(1u << 24);
}
void js_mqtt_out(const char*, const char*, int) {}
void js_mqtt_sub(const char*) {}
void js_mqtt_conn(int, const char*, const char*) {}
void js_http_response(int, int, const char*, const char*, const char*) {}
void js_radio_event(const char*, const char*) {}
void js_udp_send(int, const char*, int, const char*) {}
void js_display_ready(int, int, int, int) {}
void js_flush(int, int, int, int, int) {}
void js_backlight_apply(int, int) {}
void js_net_event(const char*, const char*) {}

void emscripten_sleep(unsigned int ms) {
  unsigned left = ms ? ms : 1;
  while (left) {
    const unsigned d = left > 5 ? 5 : left;
    g_wall += d;
    left -= d;
    beat();
  }
}
double emscripten_get_now(void) { return g_wall; }
}

int main(int argc, char** argv) {
  g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
  setvbuf(stdout, nullptr, _IOLBF, 0);

  for (Kind k : {Kind::Root, Kind::Page, Kind::Finger}) {
    Seen s;
    if (!run_case(k, &s)) {
      g_fail++;
      continue;
    }
    printf("the dash's sheet, %s case:\n", kind_name(k));
    print_look("before the phone's turn", s.before);
    if (k == Kind::Finger) print_look("the finger still down", s.held);
    print_look("after", s.after);
    CHECK(s.opened, "%s: the sheet did not open", kind_name(k));
    // Before: the landscape sheet, centered over the poster, naming the turn.
    CHECK(s.before.valid && s.before.canvas_w == 800 && s.before.canvas_h == 480 &&
              s.before.host_w == 620 && s.before.host_h == 440,
          "%s: before the turn, want the 620x440 sheet on the 800x480 canvas", kind_name(k));
    if (k == Kind::Page) {
      CHECK(s.before.on_page && strcmp(s.before.checked, "Landscape") == 0,
            "page: the tap should open the Orientation page, checked on Landscape (got %s, \"%s\")",
            s.before.on_page ? "the page" : "no page", s.before.checked);
    } else {
      CHECK(strcmp(s.before.value, "Landscape") == 0,
            "%s: before the turn the Orientation row should say Landscape (\"%s\")", kind_name(k),
            s.before.value);
    }
    if (k == Kind::Finger) {
      // The port turned under the finger, and the sheet waits for the lift.
      CHECK(s.held.canvas_w == 480 && s.held.canvas_h == 800 && s.held.rot == 1,
            "finger: the glass should wear the turn while the finger is down (canvas %dx%d, turn %d)",
            s.held.canvas_w, s.held.canvas_h, s.held.rot);
      CHECK(s.held.valid && s.held.host_w == 620 && s.held.host_h == 440 &&
                strcmp(s.held.value, "Landscape") == 0,
            "finger: nothing is rebuilt under a finger (sheet %dx%d, row \"%s\"; want the 620x440 "
            "sheet as it was)",
            s.held.host_w, s.held.host_h, s.held.value);
    }
    // After: the column the glass wears, and the turn it wears named.
    CHECK(s.after.canvas_w == 480 && s.after.canvas_h == 800 && s.after.rot == 1,
          "%s: after the turn the glass should wear portrait (canvas %dx%d, turn %d)", kind_name(k),
          s.after.canvas_w, s.after.canvas_h, s.after.rot);
    CHECK(s.after.valid && s.after.host_w == 480 && s.after.host_h == 800,
          "%s: after the turn the sheet should be laid out for the 480x800 column (it is %dx%d)",
          kind_name(k), s.after.host_w, s.after.host_h);
    if (k == Kind::Page) {
      CHECK(s.after.on_page && strcmp(s.after.checked, "Portrait") == 0,
            "page: after the turn the Orientation page should be open, checked on Portrait (got %s, "
            "\"%s\")",
            s.after.on_page ? "the page" : "no page", s.after.checked);
    } else {
      CHECK(strcmp(s.after.value, "Portrait") == 0,
            "%s: after the turn the Orientation row should say Portrait (\"%s\")", kind_name(k),
            s.after.value);
    }
    CHECK(s.active_end, "%s: the sheet closed (a drag is not the stationary hold)", kind_name(k));
  }

  printf("%d checks, %d failed\n", g_checks, g_fail);
  if (g_fail) {
    printf("DASH SHEET TURN TEST FAILED\n");
    return 1;
  }
  printf("ALL DASH SHEET TURN TESTS PASSED\n");
  return 0;
}
