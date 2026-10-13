// canary-local/emulator/test/runtime_turn_test.cpp — the nightlight turned
// while it runs (F222), natively.
//
// runtime_turn.sh builds this driver with build.sh's whole nightlight TU list
// (src/main.cpp and the firmware, the emulator's src/*.cpp and shims, LVGL
// 8.4, Ed25519) with g++. The driver plays the page: every call into it
// (EM_JS) lands here, the clock is virtual (emscripten_sleep advances it in
// 5 ms steps, emscripten_get_now reads it), the entropy is seeded and the wall
// date is fixed at a June noon, so every boot is the same boot.
//
// Each case is one boot in its own process (fork), of a provisioned
// nightlight (Wi-Fi, broker, the first meeting behind it):
//  * a REFERENCE boot wears rotation R from power-on (emu_preset_rotation,
//    the nightlight's own scv-nl key) and never turns: where its companion
//    sits, breathing, is the perch nightlight_ui_create() seats on that
//    glass;
//  * a TURNED boot wears rotation P from power-on and, at 7 s, is asked for
//    R through nightlight_request_rotation() — the mailbox the app's picker
//    writes (glass_web.cpp) and main.cpp's loop drains into
//    nightlight_apply_orientation(): the panel turned, the face rebuilt into
//    the new shape, and the companion tumbling in from the edge that was up
//    (nightlight_ui_tumble()).
// For every P -> R the turned boot must announce R's shape, show its
// companion off its perch on the first frames after the turn (the tumble ran:
// a case whose turn never happened cannot pass), and then, over six seconds
// from 1.5 s after the turn, draw the companion exactly where the reference
// boot draws it: the same x, and the breath's span of y. Before F222 the
// first mood after a turn deleted the tumble's translate animations
// (canary_mark_mood()'s lv_anim_del(s_bird, nullptr)), so the bird stayed at
// the tumble's start offset, off the glass (x=386 on 320x180 after 0 -> 1).
//
// Every turn must also wear its Startle (F242) and hop with one owner of the
// bird's y (F244). The tumble owes the Startle to the bird's first mood, which
// the face's first update after the turn gives it (canary_mark_react() refuses
// the Hidden bird nightlight_ui_create() builds); the Startle is a hop, which
// lifts the bird's y offset (the style y the mark writes, not the tumble's
// translate) about 12 px off its seat. While the hop flies, every frame's y
// offset must be the one the hop wrote: the seat less the hop animation's
// current value, read off LVGL 8.4's own animation list, with no breath
// running beside it. Before F244 a breath ran beside every hop and, LVGL 8.4
// running its list newest first, its write landed after the hop's whenever
// its value moved. Each reference boot ends the same way with a hop over its
// running breath, mid-cycle: Joyful, which starts a hop the way the idle
// flourish does, unrationed. The turn comes at 7 s, inside the first 8 s of
// uptime: the mark's Tilt/Startle ration starts as "never reacted" (F279).
// Its clock used to start at 0 and refused both for the first 8 s, so a turn
// at 7 s refused the Startle on all 12 turns (these turns ran at 9 s for it).
//
// Every ask writes one serial line in the apply path (F285): a turned boot
// "[orient] app picker -> rotation R (was P)", and a reference boot, which
// asks at 7 s for the rotation it already wears, "[orient] app picker ->
// rotation R (already worn)" and is not turned. The driver reads the
// firmware's serial for "[orient]" lines and needs exactly that one.
//
//   runtime_turn_test            every case, prints a line per case, exit 1 on a failure
//   runtime_turn_test -v         the same with the firmware's serial log on stderr
//
// This proves the sources build.sh hands em++, built with g++ (64-bit), not
// the dist's bytes.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "lvgl.h"
#include "src/misc/lv_gc.h"  // LVGL 8.4's animation list, read below
#include "canary/ui/canary_mark.h"

namespace canary {
namespace care {
void nightlight_request_rotation(uint8_t rot);
}
namespace ui {
int16_t lvgl_port_width();
int16_t lvgl_port_height();
}
}  // namespace canary

extern "C" {
// ── the emulator's exports this page uses ──
void emu_power_on(void);
int emu_preset_rotation(int);
void emu_nvs_preseed_hex(const char*, const char*, const char*);
void emu_set_lan(const char*);
void emu_set_tz(const char*);
void emu_seed(unsigned);
void emu_epoch_offset(double);
int emu_fb_width(void);
int emu_fb_height(void);
const char* emu_mark_box(void);
}
int emu_entry();  // emu_main.cpp's main(), renamed by runtime_turn.sh

namespace {

constexpr double kTurnAt = 7000;        // ms after power-on: the face is up, and
                                        // inside the first 8 s of uptime (F279)
constexpr double kHopFor = 1000;        // ms after the ask: the Startle's 560 ms hop is over
constexpr double kFirstRead = 60;       // ms after the ask: two frames on
constexpr double kSettleFrom = 1500;    // ms after the ask: the 700 ms tumble is long done
constexpr double kSettleFor = 6000;     // ms: past a whole breath, even asleep (2 x 2.8 s)
constexpr double kNoon = 1781524800.0;  // 2026-06-15 12:00:00 UTC: daylight, no quiet hours

struct Case {
  int from;  // rotation worn from power-on
  int to;    // rotation asked for at kTurnAt; -1: a reference boot that never turns
  int seat_y = 0;  // a turned boot: the bird's y offset at rest on the new glass
};

// What one boot saw, written by the child to the parent through a pipe.
struct Seen {
  int reached_end;
  int shapes;                 // glass shapes announced (js_display_ready)
  int shape_w, shape_h;       // the last one
  int fb_w, fb_h;             // the framebuffer at the end
  int port_w, port_h;         // LVGL's logical canvas at the end
  int first_valid, first_x, first_y, first_shown;  // the bird kFirstRead after the ask
  int samples, hidden, missing;                    // over the settle window
  int x_lo, x_hi, y_lo, y_hi, w, h;
  int sy_lo, sy_hi;   // the bird's y offset (what the breath writes) over the settle window
  // Over kHopFor after the ask, every frame:
  int hop_frames;     // a hop (an anim on the bird with a ready callback) is running
  int hop_lo;         // the lowest y offset (the highest the bird rose)
  int beside;         // frames a breath (an endless anim on the bird) runs beside the hop
  int not_hops;       // frames the y offset is not the seat less the hop's value
  int first_not_hop;  // the first of those: ms after the ask, offset, hop value
  int not_hop_y, not_hop_v;
  int hop_apex_ms;    // when the bird rose highest (ms into the hop window)
  int hop_on_glass;   // hop frames whose drawn box lies wholly on the glass
  int orient_lines;   // "[orient]" lines on serial (F285)
  char orient[2][96]; // the first two of them
};

bool g_verbose = false;
Case g_case = {0, -1};
int g_out_fd = -1;
double g_wall = 0;  // the virtual wall clock, ms
Seen g_seen;
bool g_asked = false;
uint32_t g_rng = 0x2545F491u;

// The bird's y offset, and what LVGL 8.4's animation list holds on it: the
// hop (the mark's only anim with a ready callback, hop_done), its current
// value, and whether a breath (the mark's only endless anim) runs too. The
// tumble's translate anims end, and call nothing when they do.
struct Bird {
  bool ok;
  int y;
  bool hop, breath;
  int32_t hop_value;
};
Bird read_bird() {
  Bird b = {false, 0, false, false, 0};
  lv_obj_t* o = canary::ui::canary_mark_obj();
  if (o == nullptr) return b;
  b.ok = true;
  b.y = lv_obj_get_style_y(o, LV_PART_MAIN);
  for (void* n = _lv_ll_get_head(&LV_GC_ROOT(_lv_anim_ll)); n != nullptr;
       n = _lv_ll_get_next(&LV_GC_ROOT(_lv_anim_ll), n)) {
    const lv_anim_t* a = static_cast<const lv_anim_t*>(n);
    if (a->var != o) continue;
    if (a->ready_cb != nullptr) {
      b.hop = true;
      b.hop_value = a->current_value;
    }
    if (a->repeat_cnt == LV_ANIM_REPEAT_INFINITE) b.breath = true;
  }
  return b;
}

// The seat the turned glass gives the bird (its y offset at rest): the
// middle of the breath a boot at that rotation draws. Set in the child from
// the parent's reference reads before a turned boot.
int g_seat_y = 0;
bool g_joyful = false;  // a reference boot's Joyful hop has been asked for

bool read_box(int* x, int* y, int* w, int* h, int* shown);

// One frame of a hop window, `t` ms into it, on a bird whose y offset at
// rest is `seat`. Where the hop falls against the tumble's slide (its apex,
// and how many of its frames the whole bird is on the glass) is measured
// for the bench row, not held: the Startle starts with the face's first
// update, while the slide is still bringing the bird in.
void read_hop(double t, int seat) {
  const Bird b = read_bird();
  if (!b.ok || !b.hop) return;
  if (g_seen.hop_frames == 0 || b.y < g_seen.hop_lo) {
    g_seen.hop_lo = b.y;
    g_seen.hop_apex_ms = (int)t;
  }
  g_seen.hop_frames++;
  int x = 0, y = 0, w = 0, h = 0, shown = 0;
  if (read_box(&x, &y, &w, &h, &shown) && x >= 0 && y >= 0 &&
      x + w <= canary::ui::lvgl_port_width() && y + h <= canary::ui::lvgl_port_height())
    g_seen.hop_on_glass++;
  if (b.breath) g_seen.beside++;
  if (b.y != seat - (int)b.hop_value) {
    if (g_seen.not_hops == 0) {
      g_seen.first_not_hop = (int)t;
      g_seen.not_hop_y = b.y;
      g_seen.not_hop_v = (int)b.hop_value;
    }
    g_seen.not_hops++;
  }
}

bool read_box(int* x, int* y, int* w, int* h, int* shown) {
  const char* s = emu_mark_box();
  return s && sscanf(s, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"shown\":%d}", x, y, w, h, shown) == 5;
}

void finish() {
  g_seen.reached_end = 1;
  g_seen.fb_w = emu_fb_width();
  g_seen.fb_h = emu_fb_height();
  g_seen.port_w = canary::ui::lvgl_port_width();
  g_seen.port_h = canary::ui::lvgl_port_height();
  const ssize_t n = write(g_out_fd, &g_seen, sizeof(g_seen));
  _exit(n == (ssize_t)sizeof(g_seen) ? 0 : 5);
}

// Every 5 ms of virtual time.
void beat() {
  if (!g_asked && g_wall >= kTurnAt) {
    g_asked = true;
    if (g_case.to >= 0) canary::care::nightlight_request_rotation((uint8_t)g_case.to);
    else canary::care::nightlight_request_rotation((uint8_t)g_case.from);  // already worn
  }
  if (!g_asked) return;
  const double t = g_wall - kTurnAt;
  int x = 0, y = 0, w = 0, h = 0, shown = 0;
  if (g_case.to >= 0 && t < kHopFor) read_hop(t, g_seat_y);
  // A reference boot, once its perch is read, hops over its running breath:
  // Joyful, the trust milestone's song, which starts a hop the way the idle
  // flourish's hop does, mid-breath and unrationed.
  const double joy = kSettleFrom + kSettleFor;
  if (g_case.to < 0 && t >= joy) {
    if (!g_joyful) {
      g_joyful = true;
      canary::ui::canary_mark_react(canary::ui::CanaryReact::Joyful);
    }
    read_hop(t - joy, (g_seen.sy_lo + g_seen.sy_hi) / 2);
    if (t >= joy + kHopFor) finish();
    return;
  }
  if (!g_seen.first_valid && t >= kFirstRead) {
    if (read_box(&x, &y, &w, &h, &shown)) {
      g_seen.first_valid = 1;
      g_seen.first_x = x;
      g_seen.first_y = y;
      g_seen.first_shown = shown;
    }
  }
  if (t >= kSettleFrom && t < kSettleFrom + kSettleFor) {
    if (!read_box(&x, &y, &w, &h, &shown)) {
      g_seen.missing++;
      return;
    }
    if (!shown) g_seen.hidden++;
    const Bird b = read_bird();
    if (g_seen.samples == 0) {
      g_seen.x_lo = g_seen.x_hi = x;
      g_seen.y_lo = g_seen.y_hi = y;
      g_seen.sy_lo = g_seen.sy_hi = b.y;
      g_seen.w = w;
      g_seen.h = h;
    }
    if (b.y < g_seen.sy_lo) g_seen.sy_lo = b.y;
    if (b.y > g_seen.sy_hi) g_seen.sy_hi = b.y;
    g_seen.samples++;
    if (x < g_seen.x_lo) g_seen.x_lo = x;
    if (x > g_seen.x_hi) g_seen.x_hi = x;
    if (y < g_seen.y_lo) g_seen.y_lo = y;
    if (y > g_seen.y_hi) g_seen.y_hi = y;
  }
  if (g_case.to >= 0 && t >= kSettleFrom + kSettleFor) finish();
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

// One boot, in the child: never returns.
[[noreturn]] void boot(const Case& c, int fd) {
  g_case = c;
  g_seat_y = c.seat_y;
  g_out_fd = fd;
  memset(&g_seen, 0, sizeof(g_seen));
  emu_seed(1234);
  emu_set_tz("UTC0");
  emu_epoch_offset(kNoon - (double)time(nullptr));
  // The page's LAN (lanSpec's demo networks), and a unit that has met its
  // owner and joined it: Wi-Fi, a broker, the first meeting behind it.
  const std::string lan = hex("HomeNet") + " -52 1 1 " + hex("correct-horse") + "\n" +
                          hex("Corner Cafe Guest") + " -83 0 0 -";
  emu_set_lan(lan.c_str());
  preseed("securacv", "wifi_ssid", "HomeNet");
  preseed("securacv", "wifi_pass", "correct-horse");
  preseed("securacv", "mqtt_host", "hub.local");
  preseed("securacv", "mqtt_user", "fleet");
  preseed("securacv", "mqtt_pass", "fleet");
  preseed("scv-hello", "met", std::string("\x01", 1));
  if (!emu_preset_rotation(c.from)) {
    fprintf(stderr, "emu_preset_rotation(%d) refused\n", c.from);
    _exit(4);
  }
  emu_power_on();
  emu_entry();
  _exit(6);  // the firmware's loop never returns
}

bool run_case(const Case& c, Seen* out) {
  int p[2];
  if (pipe(p) != 0) return false;
  fflush(nullptr);
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    close(p[0]);
    boot(c, p[1]);
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
    printf("  boot %d%s%d ended early (status %d, %zu bytes)\n", c.from, c.to >= 0 ? "->" : "@",
           c.to >= 0 ? c.to : c.from, status, got);
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

// Arduino_GFX numbering, as io/orientation.h's Orient: odd = landscape on
// the 180x320 panel.
void shape_of(int rot, int* w, int* h) {
  const bool land = (rot & 1) != 0;
  *w = land ? 320 : 180;
  *h = land ? 180 : 320;
}

}  // namespace

extern "C" {
// ── the page's side of every EM_JS ──
void js_serial_write(const char* s) {
  if (g_verbose) fputs(s, stderr);
  // Whole lines, however the writes split them: keep each "[orient]" one.
  static std::string line;
  for (const char* p = s; p != nullptr && *p != '\0'; ++p) {
    if (*p != '\n') {
      line += *p;
      continue;
    }
    if (line.compare(0, 8, "[orient]") == 0) {
      if (g_seen.orient_lines < 2)
        snprintf(g_seen.orient[g_seen.orient_lines], sizeof g_seen.orient[0], "%s",
                 line.c_str());
      g_seen.orient_lines++;
    }
    line.clear();
  }
}
void js_backlight(int, int, int) {}
void js_tone(int, int) {}
void js_nvs_write(const char*, const char*, const char*) {}
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
void js_display_ready(int w, int h, int) {
  g_seen.shapes++;
  g_seen.shape_w = w;
  g_seen.shape_h = h;
}
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

  Seen ref[4];
  for (int r = 0; r < 4; ++r) {
    if (!run_case({r, -1}, &ref[r])) {
      g_fail++;
      continue;
    }
    const Seen& s = ref[r];
    int w = 0, h = 0;
    shape_of(r, &w, &h);
    printf("rotation %d from power-on: glass %dx%d, the companion at x %d..%d y %d..%d (%dx%d)\n", r,
           s.fb_w, s.fb_h, s.x_lo, s.x_hi, s.y_lo, s.y_hi, s.w, s.h);
    CHECK(s.fb_w == w && s.fb_h == h && s.port_w == w && s.port_h == h,
          "rotation %d from power-on: glass %dx%d, LVGL %dx%d, want %dx%d", r, s.fb_w, s.fb_h,
          s.port_w, s.port_h, w, h);
    CHECK(s.samples > 0 && s.hidden == 0 && s.missing == 0,
          "rotation %d from power-on: the companion on stage throughout (%d reads, %d hidden, %d "
          "missing)",
          r, s.samples, s.hidden, s.missing);
    CHECK(s.x_lo >= 0 && s.x_hi + s.w <= w && s.y_lo >= 0 && s.y_hi + s.h <= h,
          "rotation %d from power-on: the companion on the glass", r);
    CHECK(s.sy_hi - s.sy_lo == 4,
          "rotation %d from power-on: the companion breathes 2 px either side of its seat "
          "(y offset %d..%d)",
          r, s.sy_lo, s.sy_hi);
    // Joyful over the running breath: a hop that owns the bird's y (F244).
    const int seat = (s.sy_lo + s.sy_hi) / 2;
    printf("rotation %d, Joyful mid-breath: hop %d frames to y offset %d (seat %d), %d beside a "
           "breath, %d not the hop's\n",
           r, s.hop_frames, s.hop_lo, seat, s.beside, s.not_hops);
    CHECK(s.hop_frames > 0 && s.hop_lo <= seat - 8,
          "rotation %d, Joyful: the companion should hop (%d frames, y offset down to %d, seat "
          "%d)",
          r, s.hop_frames, s.hop_lo, seat);
    CHECK(s.beside == 0, "rotation %d, Joyful: a breath ran beside the hop for %d of its %d frames",
          r, s.beside, s.hop_frames);
    CHECK(s.not_hops == 0,
          "rotation %d, Joyful: in %d of the hop's %d frames the bird's y offset was not the "
          "hop's (first at %d ms: y offset %d, the hop's value %d from seat %d)",
          r, s.not_hops, s.hop_frames, s.first_not_hop, s.not_hop_y, s.not_hop_v, seat);
    // The ask for the rotation it wears writes one line and turns nothing
    // (the companion was on stage throughout, above).
    char want[96];
    snprintf(want, sizeof want, "[orient] app picker -> rotation %d (already worn)", r);
    printf("rotation %d, asked for itself: %d [orient] line(s): \"%s\"\n", r, s.orient_lines,
           s.orient_lines ? s.orient[0] : "");
    CHECK(s.orient_lines == 1 && strcmp(s.orient[0], want) == 0,
          "rotation %d, asked for itself: want one line \"%s\", got %d (\"%s\")", r, want,
          s.orient_lines, s.orient_lines ? s.orient[0] : "");
  }

  for (int from = 0; from < 4; ++from) {
    for (int to = 0; to < 4; ++to) {
      if (from == to) continue;
      Seen s;
      const Seen& r = ref[to];
      const int seat = (r.sy_lo + r.sy_hi) / 2;
      if (!run_case({from, to, seat}, &s)) {
        g_fail++;
        continue;
      }
      int w = 0, h = 0;
      shape_of(to, &w, &h);
      const int delta = (to - from) & 3;
      printf("turned %d -> %d: glass %dx%d, first frames x %d y %d, then x %d..%d y %d..%d; "
             "Startle hop %d frames to y offset %d (seat %d), %d beside a breath, %d not the hop's; "
             "apex at %d ms, the bird wholly on the glass in %d of them; serial \"%s\"\n",
             from, to, s.fb_w, s.fb_h, s.first_x, s.first_y, s.x_lo, s.x_hi, s.y_lo, s.y_hi,
             s.hop_frames, s.hop_lo, seat, s.beside, s.not_hops, s.hop_apex_ms, s.hop_on_glass,
             s.orient_lines ? s.orient[0] : "");
      CHECK(s.shape_w == w && s.shape_h == h && s.fb_w == w && s.fb_h == h && s.port_w == w &&
                s.port_h == h,
            "turned %d -> %d: announced %dx%d, glass %dx%d, LVGL %dx%d, want %dx%d", from, to,
            s.shape_w, s.shape_h, s.fb_w, s.fb_h, s.port_w, s.port_h, w, h);
      // The tumble ran: right after the turn the companion is off its perch,
      // on the side the turn's old "up" now faces (nightlight_ui_tumble():
      // a quarter turn clockwise enters over the right edge, a half turn
      // falls from above, three quarters over the left edge).
      const bool off_right = s.first_x > r.x_hi;
      const bool off_top = s.first_y < r.y_lo;
      const bool off_left = s.first_x < r.x_lo;
      CHECK(s.first_valid && s.first_shown &&
                (delta == 1 ? off_right : delta == 2 ? off_top : off_left),
            "turned %d -> %d: %.0f ms after the turn the companion should still be tumbling in "
            "(x %d y %d, perch x %d y %d..%d)",
            from, to, kFirstRead, s.first_x, s.first_y, r.x_lo, r.y_lo, r.y_hi);
      // ...and it lands on the perch a boot at that rotation seats it on.
      CHECK(s.samples > 0 && s.hidden == 0 && s.missing == 0,
            "turned %d -> %d: the companion on stage throughout (%d reads, %d hidden, %d missing)",
            from, to, s.samples, s.hidden, s.missing);
      CHECK(s.w == r.w && s.h == r.h, "turned %d -> %d: the companion %dx%d, a boot there %dx%d",
            from, to, s.w, s.h, r.w, r.h);
      CHECK(s.x_lo == r.x_lo && s.x_hi == r.x_hi && s.y_lo == r.y_lo && s.y_hi == r.y_hi,
            "turned %d -> %d: the companion lands at x %d..%d y %d..%d; a boot at rotation %d "
            "seats it at x %d..%d y %d..%d",
            from, to, s.x_lo, s.x_hi, s.y_lo, s.y_hi, to, r.x_lo, r.x_hi, r.y_lo, r.y_hi);
      // The turn wears its Startle (F242): a hop off the seat once the bird
      // has its first mood...
      CHECK(s.hop_frames > 0 && s.hop_lo <= seat - 8,
            "turned %d -> %d: the companion should hop with its Startle (%d frames of a hop, "
            "y offset down to %d, seat %d)",
            from, to, s.hop_frames, s.hop_lo, seat);
      // ...and the hop owns the bird's y while it flies (F244).
      CHECK(s.beside == 0,
            "turned %d -> %d: a breath ran beside the hop for %d of its %d frames", from, to,
            s.beside, s.hop_frames);
      CHECK(s.not_hops == 0,
            "turned %d -> %d: in %d of the hop's %d frames the bird's y offset was not the "
            "hop's (first at %d ms: y offset %d, the hop's value %d from seat %d)",
            from, to, s.not_hops, s.hop_frames, s.first_not_hop, s.not_hop_y, s.not_hop_v,
            seat);
      // ...and the commit says so on serial, naming who asked (F285).
      char want[96];
      snprintf(want, sizeof want, "[orient] app picker -> rotation %d (was %d)", to, from);
      CHECK(s.orient_lines == 1 && strcmp(s.orient[0], want) == 0,
            "turned %d -> %d: want one line \"%s\", got %d (\"%s\")", from, to, want,
            s.orient_lines, s.orient_lines ? s.orient[0] : "");
    }
  }

  printf("%d checks, %d failed\n", g_checks, g_fail);
  if (g_fail) {
    printf("RUNTIME TURN TEST FAILED\n");
    return 1;
  }
  printf("ALL RUNTIME TURN TESTS PASSED\n");
  return 0;
}
