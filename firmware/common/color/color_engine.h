// firmware/common/color/color_engine.h — board-agnostic color primitives.
//
// Everything here is INTEGER / fixed-point on purpose: the Nightstand's
// ESP32-C6 has no hardware FPU (RV32IMAC), so a float HSV→RGB per LED frame
// would be soft-float and slow. A gamma LUT + integer HSV keep the look
// engine cheap enough to run every loop pass on the single-core C6 and leave
// the S3 headroom to double-buffer. Pure hosted C++ — no Arduino, no float —
// so it is unit-tested on the host (firmware/tests_host/test_look_engine.cpp).
//
// Ranges are the natural byte ranges a WS2812 / RGB565 panel wants:
//   hue   0..359 (degrees, wraps)
//   sat   0..255
//   val   0..255
//   rgb   0..255 per channel
#pragma once
#include <stdint.h>

namespace canary::color {

struct Rgb { uint8_t r, g, b; };
struct Hsv { uint16_t h; uint8_t s; uint8_t v; };

// Perceptual gamma (exponent ~2.2) as a 256-entry LUT. A WS2812 driven with
// linear duty looks harsh and bands in the low end; gamma-correcting the
// output is the single biggest "color accuracy" win on these LEDs.
uint8_t gamma8(uint8_t c);

// Integer HSV→RGB (the standard sextant decomposition, all integer math).
Rgb hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v);

// White-balance shift, warmth in [-100, +100]: positive warms (lifts R, trims
// B), negative cools. Clamped. This is the "warmth" slider.
Rgb apply_warmth(Rgb c, int8_t warmth);

// The output finishing pipeline: warmth, then optional gamma. One call so the
// LED and the glass share exactly the same corrected color.
Rgb finish(Rgb c, int8_t warmth, bool gamma_on);

// Sample a looping multi-stop HSV palette at t (0..255 = one full loop),
// interpolating hue along the SHORTEST path (so gold→rose doesn't detour
// through cyan) and s/v linearly. n is the stop count (>=1).
Hsv palette_sample(const Hsv* stops, uint8_t n, uint8_t t);

// Breath easing: a smooth 0..255..0 triangle over `half_ms`, eased with an
// integer smoothstep so the in/out feels organic rather than linear. This is
// the shared waveform the LED, the wash, and the bird all breathe on.
uint8_t breath(uint32_t now_ms, uint32_t half_ms);

}  // namespace canary::color
