#pragma once
#include <stdint.h>

// Severity-tiered chime engine (display_trailblazer_spec.md §5). LEDC tone
// on a passive piezo, fully non-blocking: chime_play() latches a pattern,
// chime_loop() advances it from the main loop. The grammar is the product:
// a dead battery must never sound like an intruder, and resolution gets a
// falling tone so silence keeps meaning "nothing new".
//
// The engine is always compiled (CI coverage); it only makes sound after
// chime_init() — which main.cpp calls solely under FEATURE_CHIME, i.e.
// when the piezo pad is actually populated.

namespace canary::hal {

enum class Chime : uint8_t {
  Tier1Alarm,   // Alert/Tamper: 10 fast pulses, alternating 2.6/3.1 kHz
  Tier2Warn,    // Warn: 3 slow pulses, 1.8 kHz
  AllClear,     // falling two-tone 1.3 -> 0.9 kHz
};

// pin < 0 disables. Uses its own LEDC channel (not the backlight's).
void chime_init(int pin);

// Latch a pattern (replaces any pattern in progress).
void chime_play(Chime c);

// Advance the pattern; call every loop pass. No-op when idle/uninitialized.
void chime_loop(uint32_t now_ms);

}  // namespace canary::hal
