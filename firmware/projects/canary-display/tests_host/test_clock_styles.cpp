// Host test for the curated clock-face ring (canary/ui/clock_styles.h):
// the 12-hour conversion every drawn face shares (0 -> 12 AM and 12 -> 12 PM,
// the two everyone gets wrong), the wrap-safe flip-through step, the
// segment-family geometry table, and the ring's naming — including the
// mechanical check that no caption ever leaves the built-in font's glyph
// range (the LESSONS_LEARNED mojibake class). No Arduino, no LVGL, no board.
//
// Prints "ALL CLOCK STYLE TESTS PASSED" on success. Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//     firmware/projects/canary-display/tests_host/test_clock_styles.cpp -o t && ./t

#include "canary/ui/clock_styles.h"

#include <cstdio>
#include <cstring>

using namespace canary::ui;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// ── the 12-hour conversion ──────────────────────────────────────────────────

static void test_display_hour() {
  std::printf("clock_display_hour:\n");
  bool pm = true;

  // 24-hour mode passes the hour through untouched but still reports AM/PM
  // (the quiet marker may render even on a 24 h face's siblings).
  CHECK(clock_display_hour(0, false, &pm) == 0 && !pm, "24h: 0 stays 0, AM");
  CHECK(clock_display_hour(12, false, &pm) == 12 && pm, "24h: 12 stays 12, PM");
  CHECK(clock_display_hour(23, false, &pm) == 23 && pm, "24h: 23 stays 23, PM");

  // The two everyone gets wrong.
  CHECK(clock_display_hour(0, true, &pm) == 12 && !pm, "midnight is 12 AM");
  CHECK(clock_display_hour(12, true, &pm) == 12 && pm, "noon is 12 PM");

  // The ordinary hours on both sides of them.
  CHECK(clock_display_hour(1, true, &pm) == 1 && !pm, "01 -> 1 AM");
  CHECK(clock_display_hour(11, true, &pm) == 11 && !pm, "11 -> 11 AM");
  CHECK(clock_display_hour(13, true, &pm) == 1 && pm, "13 -> 1 PM");
  CHECK(clock_display_hour(23, true, &pm) == 11 && pm, "23 -> 11 PM");

  // A face that doesn't render the marker passes nullptr; must not crash.
  CHECK(clock_display_hour(15, true, nullptr) == 3, "nullptr pm is allowed");

  // A digit and its AM/PM word can never disagree: the marker is derived in
  // the same call as the digit, so sweep the whole day and cross-check.
  for (int hh = 0; hh < 24; hh++) {
    bool p = false;
    const int d = clock_display_hour(hh, true, &p);
    CHECK(d >= 1 && d <= 12, "12h digit stays in 1..12");
    CHECK(p == (hh >= 12), "pm tracks the real hour, not the drawn one");
  }
}

// ── the flip-through ring ───────────────────────────────────────────────────

static void test_step() {
  std::printf("clock_style_step:\n");
  const uint8_t n = clock_style_count();
  CHECK(n == (uint8_t)ClockStyle::Count, "count mirrors the enum");

  // Stepping wraps in both directions.
  CHECK(clock_style_step(n - 1, +1) == 0, "forward wrap: last -> first");
  CHECK(clock_style_step(0, -1) == n - 1, "backward wrap: first -> last");

  // One lap forward visits every style exactly once and comes home.
  bool seen[(int)ClockStyle::Count] = {};
  uint8_t s = 0;
  for (int i = 0; i < n; i++) {
    CHECK(!seen[s], "a lap never revisits a style");
    seen[s] = true;
    s = clock_style_step(s, +1);
  }
  CHECK(s == 0, "a full lap comes home");
  for (int i = 0; i < n; i++) CHECK(seen[i], "a lap visits every style");

  // +1 then -1 is identity from every start.
  for (uint8_t i = 0; i < n; i++)
    CHECK(clock_style_step(clock_style_step(i, +1), -1) == i, "step round-trips");

  // An out-of-range stored value (corrupt blob) still lands inside the ring.
  CHECK(clock_style_step(200, +1) < n, "corrupt value steps back into range");
}

// ── the geometry table and the ring's names ────────────────────────────────

static void test_table_and_names() {
  std::printf("seg_style + names:\n");

  // Segment MUST stay 0: the default and the safe fallback by construction.
  CHECK((uint8_t)ClockStyle::Segment == 0, "Segment is 0 (the safe fallback)");

  CHECK(seg_style((uint8_t)ClockStyle::Segment).t_pct == 100, "Segment stroke 100%");
  CHECK(seg_style((uint8_t)ClockStyle::Segment).ghost_day, "Segment keeps day ghosts");
  CHECK(seg_style((uint8_t)ClockStyle::Slab).t_pct == 150, "Slab is heavyset");
  CHECK(seg_style((uint8_t)ClockStyle::Hairline).t_pct == 55, "Hairline is thin");
  CHECK(!seg_style((uint8_t)ClockStyle::Hairline).ghost_day, "Hairline drops ghosts");
  CHECK(seg_style((uint8_t)ClockStyle::Calendar).t_pct == 84, "Calendar shares the hero");
  // Analog draws a dial, but its fallback geometry is the Segment's.
  CHECK(seg_style((uint8_t)ClockStyle::Analog).t_pct == 100, "Analog falls back to base");

  CHECK(clock_style_is_analog((uint8_t)ClockStyle::Analog), "Analog knows itself");
  CHECK(!clock_style_is_analog((uint8_t)ClockStyle::Slab), "Slab is not analog");
  CHECK(clock_style_is_calendar((uint8_t)ClockStyle::Calendar), "Calendar knows itself");

  // Out-of-range names degrade to the fallback, never to nullptr.
  CHECK(std::strcmp(clock_style_name(200), "Segment") == 0, "corrupt name -> Segment");

  // Every caption stays inside the built-in font's range: printable ASCII or
  // the exact 3-byte UTF-8 bullet (0xE2 0x80 0xA2) — the one non-ASCII glyph
  // the Montserrat build carries. This is the host-side twin of
  // check_display_glyphs.py, here so a bad byte fails before a push.
  for (uint8_t i = 0; i < clock_style_count(); i++) {
    CHECK(clock_style_name(i) != nullptr, "every style has a name");
    const unsigned char* c = (const unsigned char*)clock_style_caption(i);
    CHECK(c != nullptr, "every style has a caption");
    while (*c) {
      if (*c == 0xE2 && c[1] == 0x80 && c[2] == 0xA2) { c += 3; continue; }
      CHECK(*c >= 0x20 && *c <= 0x7F, "caption stays in the font's glyph range");
      c++;
    }
  }
}

int main() {
  test_display_hour();
  test_step();
  test_table_and_names();
  if (g_fail) {
    std::printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("ALL CLOCK STYLE TESTS PASSED\n");
  return 0;
}
