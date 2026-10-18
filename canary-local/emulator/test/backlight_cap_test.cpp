// canary-local/emulator/test/backlight_cap_test.cpp — the board's backlight
// ceiling, in the twin's display HAL (sweep A61). Built and run by
// glass_turn.sh against the emulator's REAL display HAL
// (src/emu_hal_display.cpp, through test/native/emscripten.h), once in the
// dash config and once in the nightlight's; g++ only, no emsdk.
//
// The C3-LCD-1.47 (TFT_USE_EXIO, the nightlight's pin map) writes its
// backlight as one byte into Waveshare's I2C expander, and
// display_1in47.cpp's exio_backlight() clips that byte at
// (255 * CD_BL_MAX_PCT) / 100 under every caller: backlight_set() hands it
// the day ladder's level, backlight_night_set() the 13-bit night duty
// compressed to a byte (clamped to 8191, >> 5, never rounded to zero while a
// glow was asked for). board_byte_*() below are those two paths, restated
// here and held to the board's source by bench.test.js. For every level and
// every night duty 0..9000 this holds the twin to them:
//   * day: the level the twin applies (emu_backlight_level, what the page
//     glows at) is the byte the board writes;
//   * night (sweep A79): the duty the twin applies is the byte the board
//     writes for the duty asked, on the 13-bit scale (byte << 5). So the
//     twin's night floor has the board's steps, 32 duties apart, and no more
//     of them than the cap's byte allows plus dark; a glow asked under 32
//     glows at the first step, never dark and never dimmer than the glass;
//     a duty on a step passes as asked; and a duty the board clips is held
//     at the cap's step. The twin's OWN duty is held, not only the byte the
//     board would make of it: comparing bytes alone passes a twin that never
//     clipped (the board's own clip, applied to the twin's duty, hides it,
//     A61's review) and one that never steps (>> 5 of an unstepped duty is
//     the same byte);
//   * emu_backlight_cap_pct() names the ceiling: CD_BL_MAX_PCT on the
//     expander board, 100 on a board with none.
// In the dash config (no expander, no ceiling) every level and duty passes
// through as asked, as the twin always passed them.
//
// Prints "ALL BACKLIGHT CAP TESTS PASSED" on success.
#include <Arduino.h>
#include <config.h>
#include "pins.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "canary/log.h"

// ── The bench this test stands in for ───────────────────────────────────
namespace {
int g_backlight_calls = 0;
int g_fail = 0;
int g_checks = 0;
}  // namespace

extern "C" uint32_t millis(void) { return 0; }
// The page's side of the HAL's EM_JS calls (test/native/emscripten.h makes
// each a plain declaration the test defines, sweep A64): the backlight call
// is counted, the shape and flush calls are not this test's to read. A call
// the HAL adds fails the link here too, naming it.
extern "C" void js_display_ready(int, int, int, int) {}
extern "C" void js_flush(int, int, int, int, int) {}
extern "C" void js_backlight_apply(int, int) { g_backlight_calls++; }
void EmuSerial::write_str(const char*) {}
EmuSerial Serial;
namespace canary {
LogSink g_log_sink = nullptr;
}

// The emulator HAL: its two sinks and what the page reads back.
namespace canary::hal {
void backlight_set(uint8_t level);
void backlight_night_set(uint16_t duty13);
}  // namespace canary::hal
extern "C" int emu_backlight_level(void);
extern "C" int emu_backlight_night_duty(void);
extern "C" int emu_backlight_cap_pct(void);

#define CHECK(cond, ...)                                    \
  do {                                                      \
    g_checks++;                                             \
    if (!(cond)) {                                          \
      g_fail++;                                             \
      if (g_fail <= 12) {                                   \
        printf("  FAIL (%s:%d): ", __FILE__, __LINE__);     \
        printf(__VA_ARGS__);                                \
        printf("\n");                                       \
      }                                                     \
    }                                                       \
  } while (0)

namespace {
#if defined(TFT_USE_EXIO) && TFT_USE_EXIO
constexpr bool kExpander = true;
#ifdef CD_BL_MAX_PCT
constexpr unsigned kCapPct = CD_BL_MAX_PCT;
#else
constexpr unsigned kCapPct = 100;  // display_1in47.cpp's default
#endif
#else
constexpr bool kExpander = false;
constexpr unsigned kCapPct = 100;
#endif

// display_1in47.cpp's exio_backlight(): kCap and the clip.
constexpr unsigned kCap = (255u * kCapPct) / 100u;
unsigned exio(unsigned duty8) { return duty8 > kCap ? kCap : duty8; }

// display_1in47.cpp's backlight_set() on the expander path.
unsigned board_byte_day(unsigned level) { return exio(level); }

// display_1in47.cpp's backlight_night_set() on the expander path: the
// 13-bit duty clamped to NIGHT_DUTY_MAX, compressed to a byte, never to zero
// while a glow was asked for, then clipped.
unsigned board_byte_night(unsigned duty13) {
  if (duty13 > 8191) duty13 = 8191;
  unsigned d8 = duty13 >> 5;
  if (d8 == 0 && duty13 > 0) d8 = 1;
  return exio(d8);
}
// The byte the board writes for that duty before its clip.
unsigned board_unclipped_night(unsigned duty13) {
  if (duty13 > 8191) duty13 = 8191;
  unsigned d8 = duty13 >> 5;
  if (d8 == 0 && duty13 > 0) d8 = 1;
  return d8;
}
// The duty the twin is to hold for a night duty asked (sweep A79): the byte
// the board writes for it, on the 13-bit scale. The cap's step is the
// highest: kCap << 5 (4064 for the nightlight's 127; 8160 with no cap).
unsigned twin_night(unsigned duty13) { return board_byte_night(duty13) << 5; }
constexpr unsigned kNightTop13 = kCap << 5;
}  // namespace

int main() {
  printf("%s config: %s, ceiling %u%% (byte %u)\n", kExpander ? "expander (C3-LCD-1.47)" : "no-expander",
         kExpander ? "the HAL clips at the expander's register" : "no ceiling", kCapPct, kCap);

  CHECK(emu_backlight_cap_pct() == (int)kCapPct, "emu_backlight_cap_pct() says %d, the board's ceiling is %u",
        emu_backlight_cap_pct(), kCapPct);

  // The day ladder, every level.
  int day_clipped = 0;
  for (unsigned level = 0; level <= 255; ++level) {
    const int calls = g_backlight_calls;
    canary::hal::backlight_set((uint8_t)level);
    const int got = emu_backlight_level();
    const int want = kExpander ? (int)board_byte_day(level) : (int)level;
    if (want != (int)level) day_clipped++;
    CHECK(got == want, "day: backlight_set(%u) glows at %d, the board writes %d", level, got, want);
    CHECK(emu_backlight_night_duty() == -1, "day: backlight_set(%u) leaves the night profile on", level);
    CHECK(g_backlight_calls == calls + 1, "day: backlight_set(%u) told the page %d times", level,
          g_backlight_calls - calls);
  }

  // The night floor, every duty the profile can ask (and past its scale on
  // the expander board, which clamps it).
  int night_clipped = 0;
  int night_stepped = 0;           // duties the expander's byte moves off the duty asked
  static bool step_seen[8192];     // the night duties the twin applied
  const unsigned top = kExpander ? 9000u : 8191u;
  for (unsigned duty = 0; duty <= top; ++duty) {
    const int calls = g_backlight_calls;
    canary::hal::backlight_night_set((uint16_t)duty);
    const int got = emu_backlight_night_duty();
    CHECK(g_backlight_calls == calls + 1, "night: backlight_night_set(%u) told the page %d times", duty,
          g_backlight_calls - calls);
    CHECK(got >= 0 && got <= 8191, "night: backlight_night_set(%u) applies %d, off the 13-bit scale", duty, got);
    if (got < 0 || got > 8191) continue;
    step_seen[got] = true;
    if (!kExpander) {
      CHECK(got == (int)duty, "night: backlight_night_set(%u) applies %d, not the duty asked", duty, got);
      continue;
    }
    CHECK(board_byte_night((unsigned)got) == board_byte_night(duty),
          "night: backlight_night_set(%u) applies %d, which the board writes as byte %u; it writes %u for the duty asked",
          duty, got, board_byte_night((unsigned)got), board_byte_night(duty));
    // The twin's own duty, not only the board's byte of it: the step that
    // byte stands for, so neither an unstepped duty nor one past the cap
    // hides behind the board's own >> 5 and clip.
    CHECK(got == (int)twin_night(duty),
          "night: backlight_night_set(%u) applies %d, not %u (the board's byte %u, << 5)", duty, got,
          twin_night(duty), board_byte_night(duty));
    CHECK((got & 31) == 0, "night: backlight_night_set(%u) applies %d, between two of the expander's steps", duty, got);
    CHECK(duty == 0 || got > 0, "night: backlight_night_set(%u) goes dark; the board raises a glow asked to byte 1", duty);
    CHECK(((unsigned)got >> 5) <= kCap, "night: backlight_night_set(%u) applies %d, whose byte %u is past the cap's %u",
          duty, got, (unsigned)got >> 5, kCap);
    if (board_unclipped_night(duty) > kCap || duty > 8191) {
      night_clipped++;
    } else if (got != (int)duty) {
      night_stepped++;
    } else {
      // A duty the board neither clips nor steps passes as asked: one on a
      // step (the calibrated floor's whole duties of 32 pass through).
      CHECK((duty & 31) == 0, "night: backlight_night_set(%u) passes between two steps unchanged", duty);
    }
  }

  if (kExpander && kCapPct < 100) {
    // The ceiling bites where it should: a full day ladder and a full night
    // duty both glow at the cap's byte, not past it.
    canary::hal::backlight_set(255);
    CHECK(emu_backlight_level() == (int)kCap, "a full day ladder glows at %d, not the cap's %u", emu_backlight_level(), kCap);
    canary::hal::backlight_night_set(8191);
    CHECK(board_byte_night((unsigned)emu_backlight_night_duty()) == kCap,
          "a full night duty glows past the cap (%d)", emu_backlight_night_duty());
    CHECK(emu_backlight_night_duty() == (int)kNightTop13 && kNightTop13 < 8191,
          "a full night duty applies %d, not the cap's step %u", emu_backlight_night_duty(), kNightTop13);
    CHECK(day_clipped == (int)(255 - kCap) && night_clipped > 0,
          "the cap clipped %d day levels (want %u) and %d night duties", day_clipped, 255 - kCap, night_clipped);
  } else if (!kExpander) {
    CHECK(day_clipped == 0 && night_clipped == 0, "no ceiling, yet %d day levels and %d night duties were clipped",
          day_clipped, night_clipped);
  }
  // The night floor's steps (sweep A79): on the expander, dark and one step
  // per byte up to the cap's, 32 duties apart, the dimmest glow at 32, the
  // brightest at the cap's step; and the twin stepped duties the glass steps.
  int steps = 0;
  int lowest_glow = -1;
  for (int d = 0; d <= 8191; ++d) {
    if (!step_seen[d]) continue;
    steps++;
    if (d > 0 && lowest_glow < 0) lowest_glow = d;
  }
  if (kExpander) {
    CHECK(steps == (int)kCap + 1 && lowest_glow == 32 && step_seen[kNightTop13] && night_stepped > 0,
          "the expander's night floor has %d steps (want %u: dark and bytes 1..%u), its dimmest glow at %d (want "
          "32), %s its cap's step %u, and %d duties stepped",
          steps, kCap + 1, kCap, lowest_glow, step_seen[kNightTop13] ? "reaching" : "never reaching", kNightTop13,
          night_stepped);
  } else {
    CHECK(steps == 8192 && night_stepped == 0, "no expander, yet the night floor has %d of 8192 duties (%d stepped)",
          steps, night_stepped);
  }

  printf("%d checks, %d failed (%d day levels clipped; %d night duties clipped, %d stepped, %d steps)\n", g_checks,
         g_fail, day_clipped, night_clipped, night_stepped, steps);
  if (g_fail) {
    printf("BACKLIGHT CAP TEST FAILED\n");
    return 1;
  }
  printf("ALL BACKLIGHT CAP TESTS PASSED\n");
  return 0;
}
