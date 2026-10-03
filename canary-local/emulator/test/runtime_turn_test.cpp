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

constexpr double kTurnAt = 7000;        // ms after power-on: the face is up
constexpr double kFirstRead = 60;       // ms after the ask: two frames on
constexpr double kSettleFrom = 1500;    // ms after the ask: the 700 ms tumble is long done
constexpr double kSettleFor = 6000;     // ms: past a whole breath, even asleep (2 x 2.8 s)
constexpr double kNoon = 1781524800.0;  // 2026-06-15 12:00:00 UTC: daylight, no quiet hours

struct Case {
  int from;  // rotation worn from power-on
  int to;    // rotation asked for at kTurnAt; -1: a reference boot that never turns
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
};

bool g_verbose = false;
Case g_case = {0, -1};
int g_out_fd = -1;
double g_wall = 0;  // the virtual wall clock, ms
Seen g_seen;
bool g_asked = false;
uint32_t g_rng = 0x2545F491u;

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
  }
  if (!g_asked) return;
  const double t = g_wall - kTurnAt;
  int x = 0, y = 0, w = 0, h = 0, shown = 0;
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
    if (g_seen.samples == 0) {
      g_seen.x_lo = g_seen.x_hi = x;
      g_seen.y_lo = g_seen.y_hi = y;
      g_seen.w = w;
      g_seen.h = h;
    }
    g_seen.samples++;
    if (x < g_seen.x_lo) g_seen.x_lo = x;
    if (x > g_seen.x_hi) g_seen.x_hi = x;
    if (y < g_seen.y_lo) g_seen.y_lo = y;
    if (y > g_seen.y_hi) g_seen.y_hi = y;
  }
  if (t >= kSettleFrom + kSettleFor) finish();
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
  }

  for (int from = 0; from < 4; ++from) {
    for (int to = 0; to < 4; ++to) {
      if (from == to) continue;
      Seen s;
      if (!run_case({from, to}, &s)) {
        g_fail++;
        continue;
      }
      const Seen& r = ref[to];
      int w = 0, h = 0;
      shape_of(to, &w, &h);
      const int delta = (to - from) & 3;
      printf("turned %d -> %d: glass %dx%d, first frames x %d y %d, then x %d..%d y %d..%d\n", from,
             to, s.fb_w, s.fb_h, s.first_x, s.first_y, s.x_lo, s.x_hi, s.y_lo, s.y_hi);
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
