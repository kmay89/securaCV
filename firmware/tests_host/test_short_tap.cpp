// Host tests for firmware/common/io/short_tap.h — the debounced short press
// that opens the Bluetooth setup door for a minute on a board with a BOOT
// button.
//
//   * contact bounce (a press shorter than the debounce) is not a tap;
//   * a press between the debounce and the hold bound is one tap, reported
//     exactly once, on the release;
//   * a press held past the bound is somebody else's gesture: no tap, not
//     even on the release;
//   * the clock wrapping under a press changes nothing.

#include "../common/io/short_tap.h"

#include <cstdio>

using canary::io::ShortTap;

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

// Press at `down`, release at `up`, sampling every 10 ms; counts the taps.
static int press(ShortTap& t, uint32_t down, uint32_t up, uint32_t until) {
  int taps = 0;
  // Unsigned deltas throughout, so a window that straddles the wrap walks.
  for (uint32_t off = 0; off <= (uint32_t)(until - down); off += 10) {
    const uint32_t now = down + off;
    if (t.step((uint32_t)(now - down) < (uint32_t)(up - down), now)) ++taps;
  }
  return taps;
}

static void bounce_is_not_a_tap() {
  ShortTap t;
  CHECK(press(t, 1000, 1020, 1200) == 0, "a 20 ms blip is noise");
  CHECK(press(t, 2000, 2030, 2200) == 0, "30 ms: still under the debounce");
}

static void a_short_press_is_one_tap_on_the_release() {
  ShortTap t;
  CHECK(press(t, 1000, 1040, 1030) == 0, "nothing while still down");
  CHECK(t.step(false, 1040), "the release at 40 ms reports the tap");
  CHECK(!t.step(false, 1050), "and only once");
  CHECK(press(t, 2000, 2300, 2500) == 1, "a 300 ms press is one tap");
  CHECK(press(t, 3000, 3690, 3800) == 1, "690 ms: just inside the bound");
}

static void a_hold_is_not_a_tap() {
  ShortTap t;
  CHECK(press(t, 1000, 1700, 1900) == 0, "700 ms: a hold");
  CHECK(press(t, 2000, 4000, 4200) == 0, "two seconds: the WAP's reset gesture, not ours");
  // The next short press after a hold still counts.
  CHECK(press(t, 5000, 5200, 5400) == 1, "a tap after a hold");
}

static void idle_level_reports_nothing() {
  ShortTap t;
  for (uint32_t now = 0; now < 5000; now += 10) CHECK(!t.step(false, now), "idle");
}

static void a_press_across_the_wrap_is_still_a_tap() {
  ShortTap t;
  const uint32_t near_wrap = 0xFFFFFFFFu - 100;
  CHECK(press(t, near_wrap, near_wrap + 200, near_wrap + 300) == 1, "200 ms across the wrap: a tap");
  CHECK(press(t, 1000, 1900, 2000) == 0, "and a hold across nothing is a hold");
}

static void custom_bounds_apply() {
  ShortTap t;
  t.debounce_ms = 100;
  t.max_ms = 300;
  CHECK(press(t, 1000, 1080, 1200) == 0, "80 ms is under a 100 ms debounce");
  CHECK(press(t, 2000, 2200, 2400) == 1, "200 ms is a tap");
  CHECK(press(t, 3000, 3300, 3500) == 0, "300 ms is a hold under a 300 ms bound");
}

int main() {
  bounce_is_not_a_tap();
  a_short_press_is_one_tap_on_the_release();
  a_hold_is_not_a_tap();
  idle_level_reports_nothing();
  a_press_across_the_wrap_is_still_a_tap();
  custom_bounds_apply();

  if (g_failures) {
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
  }
  std::printf("ALL short_tap TESTS PASSED\n");
  return 0;
}
