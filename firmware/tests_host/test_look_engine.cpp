// firmware/tests_host/test_look_engine.cpp — host tests for the Nightstand
// color + look engine (firmware/common/color). Pure integer math, so the
// whole pipeline is exercised without the Arduino/LVGL toolchain.
//
// Build/run via the tests_host Makefile (g++ -std=c++17 -Wall -Wextra -Werror).
#include "color/color_engine.h"
#include "color/look_engine.h"

#include <cstdio>
#include <cstdlib>

using namespace canary::color;

static int g_fail = 0;
#define CHECK(cond, msg)                                              \
  do {                                                               \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; }         \
  } while (0)

static bool near(int a, int b, int tol) { return (a - b <= tol) && (b - a <= tol); }

static void test_gamma() {
  printf("gamma...\n");
  CHECK(gamma8(0) == 0, "gamma(0)=0");
  CHECK(gamma8(255) == 255, "gamma(255)=255");
  bool mono = true;
  for (int i = 1; i < 256; i++) if (gamma8((uint8_t)i) < gamma8((uint8_t)(i - 1))) mono = false;
  CHECK(mono, "gamma monotonic non-decreasing");
  // Gamma darkens the midtones (2.2 curve): gamma(128) well below 128.
  CHECK(gamma8(128) < 80, "gamma(128) pulls midtone down");
}

static void test_hsv() {
  printf("hsv...\n");
  Rgb red = hsv_to_rgb(0, 255, 255);
  CHECK(red.r == 255 && red.g == 0 && red.b == 0, "h=0 -> pure red");
  Rgb grn = hsv_to_rgb(120, 255, 255);
  CHECK(grn.r == 0 && grn.g == 255 && grn.b == 0, "h=120 -> pure green");
  Rgb blu = hsv_to_rgb(240, 255, 255);
  CHECK(blu.r == 0 && blu.g == 0 && blu.b == 255, "h=240 -> pure blue");
  Rgb white = hsv_to_rgb(200, 0, 255);
  CHECK(white.r == 255 && white.g == 255 && white.b == 255, "s=0 -> white");
  Rgb black = hsv_to_rgb(123, 200, 0);
  CHECK(black.r == 0 && black.g == 0 && black.b == 0, "v=0 -> black");
  // Hue wraps.
  Rgb wrap = hsv_to_rgb(360, 255, 255);
  CHECK(wrap.r == 255 && wrap.g == 0 && wrap.b == 0, "h=360 wraps to red");
}

static void test_warmth() {
  printf("warmth...\n");
  Rgb mid = {128, 128, 128};
  Rgb warm = apply_warmth(mid, 100);
  CHECK(warm.r > mid.r && warm.b < mid.b, "warm lifts R, trims B");
  Rgb cool = apply_warmth(mid, -100);
  CHECK(cool.r < mid.r && cool.b > mid.b, "cool trims R, lifts B");
  Rgb none = apply_warmth(mid, 0);
  CHECK(none.r == 128 && none.g == 128 && none.b == 128, "warmth 0 = identity");
}

static void test_palette() {
  printf("palette...\n");
  Hsv stops[2] = {{0, 255, 255}, {180, 255, 255}};  // red .. cyan
  Hsv a = palette_sample(stops, 2, 0);
  CHECK(a.h == 0, "t=0 -> first stop");
  Hsv mid = palette_sample(stops, 2, 64);   // ~quarter of the loop = mid of seg 0
  CHECK(mid.h > 0 && mid.h < 180, "midpoint hue between stops");
  // Single stop is a constant.
  Hsv one = palette_sample(stops, 1, 200);
  CHECK(one.h == 0, "single stop constant");
}

static void test_breath() {
  printf("breath...\n");
  uint8_t lo = breath(0, 1000);          // trough
  uint8_t hi = breath(1000, 1000);       // peak (half period)
  CHECK(lo < 20, "breath trough near 0");
  CHECK(hi > 235, "breath peak near 255");
  // Symmetric-ish: quarter and three-quarter near the mid.
  uint8_t q = breath(500, 1000);
  CHECK(near(q, 128, 40), "breath quarter near mid");
}

static void test_scenes() {
  printf("scenes...\n");
  CHECK(kSceneCount == 11, "eleven scenes");
  for (uint8_t i = 0; i < kSceneCount; i++) {
    CHECK(kScenes[i].n_stops >= 1 && kScenes[i].n_stops <= 4, "stop count sane");
    CHECK(kScenes[i].id != nullptr && kScenes[i].name != nullptr, "scene named");
  }
}

static void test_rainbow() {
  printf("rainbow...\n");
  // Find the rainbow scene by id (position is not part of the contract).
  int idx = -1;
  for (uint8_t i = 0; i < kSceneCount; i++) {
    if (kScenes[i].id && kScenes[i].id[0] == 'r' && kScenes[i].id[1] == 'a')
      idx = i;
  }
  CHECK(idx >= 0, "rainbow scene present");
  if (idx < 0) return;
  LookParams p;
  p.scene_idx = (uint8_t)idx;
  // Over one sweep loop the wheel must actually cycle: collect the dominant
  // channel across phases and expect all three primaries to lead at least
  // once (a warm-only or fixed-hue palette can't do that).
  bool r_led = false, g_led = false, b_led = false;
  for (uint32_t t = 0; t < 16000; t += 250) {
    Rgb c = led_color(t, p, Sev::Ok, false);
    if (c.r > c.g && c.r > c.b) r_led = true;
    if (c.g > c.r && c.g > c.b) g_led = true;
    if (c.b > c.r && c.b > c.g) b_led = true;
  }
  CHECK(r_led && g_led && b_led, "rainbow cycles through all three primaries");
  // Honesty holds for the fun scene too: Alert is red, not the wheel.
  Rgb alert = led_color(4000, p, Sev::Alert, false);
  CHECK(alert.r > alert.g && alert.r > alert.b, "rainbow never dresses an alert");
  Rgb dark = led_color(4000, p, Sev::Ok, /*safe_dark=*/true);
  CHECK(dark.r == 0 && dark.g == 0 && dark.b == 0, "rainbow respects safe_dark");
}

static void test_led_honesty() {
  printf("led honesty...\n");
  LookParams p;  // defaults: dawn scene, day, gamma on
  p.scene_idx = 0;

  // safe_dark ALWAYS black, whatever the scene.
  Rgb dark = led_color(1234, p, Sev::Ok, /*safe_dark=*/true);
  CHECK(dark.r == 0 && dark.g == 0 && dark.b == 0, "safe_dark -> black");

  // Ok + day: the beacon is lit with the scene color (non-black at some phase).
  bool any_lit = false;
  for (uint32_t t = 0; t < 4000; t += 100) {
    Rgb c = led_color(t, p, Sev::Ok, false);
    if (c.r + c.g + c.b > 20) any_lit = true;
  }
  CHECK(any_lit, "ok/day beacon is lit");

  // Warn: the honest override -> amber (R and G present, B ~0), NOT the scene.
  Rgb warn = led_color(1000, p, Sev::Warn, false);
  CHECK(warn.r > warn.b && warn.g > warn.b, "warn -> amber-ish (R,G > B)");

  // Alert: red dominates.
  Rgb alert = led_color(1000, p, Sev::Alert, false);
  CHECK(alert.r > alert.g && alert.r > alert.b, "alert -> red dominant");

  // The alert override is scene-independent: pick a blue scene (calm) and it
  // must still be red, not blue.
  LookParams pc = p; pc.scene_idx = 3;  // Deep Calm (indigo)
  Rgb alert2 = led_color(1000, pc, Sev::Alert, false);
  CHECK(alert2.r > alert2.b, "alert overrides even a blue scene");
}

static void test_wash() {
  printf("wash...\n");
  LookParams p;
  Rgb stops[6];
  wash_stops(500, p, Sev::Ok, false, stops, 6);
  int sum = 0;
  for (auto& s : stops) sum += s.r + s.g + s.b;
  CHECK(sum > 0, "ok wash is non-black");

  wash_stops(500, p, Sev::Ok, /*safe_dark=*/true, stops, 6);
  sum = 0; for (auto& s : stops) sum += s.r + s.g + s.b;
  CHECK(sum == 0, "safe_dark wash is fully black");

  // Alert wash reads red-dominant across the field.
  wash_stops(500, p, Sev::Alert, false, stops, 6);
  bool red_field = true;
  for (auto& s : stops) if (!(s.r >= s.b)) red_field = false;
  CHECK(red_field, "alert wash is red-dominant top to bottom");
}

int main() {
  printf("== look engine host tests ==\n");
  test_gamma();
  test_hsv();
  test_warmth();
  test_palette();
  test_breath();
  test_scenes();
  test_rainbow();
  test_led_honesty();
  test_wash();
  if (g_fail) { printf("\n%d CHECK(s) FAILED\n", g_fail); return 1; }
  printf("\nAll look-engine checks passed.\n");
  return 0;
}
