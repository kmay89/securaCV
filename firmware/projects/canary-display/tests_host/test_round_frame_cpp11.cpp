// Host test for the Round Frame engine's C++11 constraint — compiled with
// -std=gnu++11 ON PURPOSE (see the Makefile rule), unlike every other host
// test here. The Arduino parity builds ride esp32 core 2.0.17, which
// compiles the flat sketch as gnu++11: a constexpr body there is a single
// return statement. The engine's header is written expression-form to honor
// that, and this test is the tripwire — a well-meaning "cleanup" back into
// loops or locals builds fine under C++17 (PlatformIO, emulator, the other
// host tests) and then fails only in the Arduino CI rows. This file fails
// FIRST, on the developer's machine, with a message that says why.
//
// The static_asserts double as the compile-time-evaluation proof: the
// commission_ui QR budget and the splash bubble guard need these functions
// callable in constant expressions under C++11, not just C++17.
//
// Prints "ROUND FRAME C++11 CONTRACT HOLDS" on success. Build (repo root):
//
//   g++ -std=gnu++11 -Wall -Wextra -I firmware/projects/canary-display/include
//     firmware/projects/canary-display/tests_host/test_round_frame_cpp11.cpp -o t && ./t

#include "canary/ui/round_frame_core.h"

#include <cstdio>

using namespace canary::ui::roundframe;

// Compile-time: the values the C++17 suite pins, evaluated under C++11.
static_assert(isqrt(115 * 115) == 115, "isqrt in a constant expression");
static_assert(half_chord_at(100, 60) == 80, "3-4-5 triple at compile time");
static_assert(chord(56, 14) == 190, "list-row chord at compile time");
static_assert(row_stack_y(240, 4, 32, 0, 0) == 56, "row stack at compile time");
static_assert(inscribed_square(240, 0) == 169,
              "the commission_ui QR budget's inscribed square");
static_assert(polar_dx(90, 100) == 100 && polar_dy(0, 100) == -100,
              "polar helpers at compile time");

int main() {
  // Runtime spot-checks so the binary asserts something too (a pure
  // static_assert TU would "pass" even if never run).
  int fail = 0;
  if (isqrt(2147483647) != 46340) {
    std::printf("  FAIL: INT32_MAX isqrt\n");
    fail++;
  }
  for (int d = 1; d <= 115; d++) {
    if (half_chord_at(115, d) > half_chord_at(115, d - 1)) {
      std::printf("  FAIL: chord widened toward the rim\n");
      fail++;
      break;
    }
  }
  if (fail == 0) {
    std::printf("ROUND FRAME C++11 CONTRACT HOLDS\n");
    return 0;
  }
  return 1;
}
