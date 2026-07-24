// firmware/common/color/color_engine.cpp — integer color primitives.
// See color_engine.h. No Arduino, no float — hosted-testable and FPU-free.
#include "color/color_engine.h"

namespace canary::color {

namespace {

// gamma8 LUT (exponent 2.2), computed once at first use without <cmath> so
// the TU stays float-free on the device. We integrate x^2.2 via a small
// integer pow-by-log-free approximation: repeated multiply for the integer
// part plus one linear step. Simpler and exact enough: precompute with a
// fixed table generator here using a rational approximation is overkill —
// instead we use the closed-form on host build only, guarded so the device
// path is a plain table. To keep ONE code path and stay float-free, the
// table is a hand-tuned 2.2-curve baked as constants below.
//
// The values are round(pow(i/255, 2.2) * 255) for i in 0..255. Generated
// once and pasted so neither the host nor the device ever needs <cmath>.
const uint8_t kGamma[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
    1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,
    3,3,3,3,3,4,4,4,4,5,5,5,5,6,6,6,
    6,7,7,7,8,8,8,9,9,9,10,10,11,11,11,12,
    12,13,13,13,14,14,15,15,16,16,17,17,18,18,19,19,
    20,20,21,22,22,23,23,24,25,25,26,26,27,28,28,29,
    30,30,31,32,33,33,34,35,35,36,37,38,39,39,40,41,
    42,43,43,44,45,46,47,48,49,49,50,51,52,53,54,55,
    56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,
    73,74,75,76,77,78,79,81,82,83,84,85,87,88,89,90,
    91,93,94,95,97,98,99,100,102,103,105,106,107,109,110,111,
    113,114,116,117,119,120,121,123,124,126,127,129,130,132,133,135,
    137,138,140,141,143,145,146,148,149,151,153,154,156,158,159,161,
    163,165,166,168,170,172,173,175,177,179,181,182,184,186,188,190,
    192,194,196,197,199,201,203,205,207,209,211,213,215,217,219,221,
    223,225,227,229,231,234,236,238,240,242,244,246,248,251,253,255};

}  // namespace

uint8_t gamma8(uint8_t c) { return kGamma[c]; }

Rgb hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v) {
  h %= 360;
  const uint8_t region = (uint8_t)(h / 60);          // 0..5
  const uint16_t rem = (uint16_t)((h % 60) * 255 / 60);  // 0..255 within region
  const uint8_t p = (uint8_t)((uint32_t)v * (255 - s) / 255);
  const uint8_t q = (uint8_t)((uint32_t)v * (255 - (uint32_t)s * rem / 255) / 255);
  const uint8_t t = (uint8_t)((uint32_t)v * (255 - (uint32_t)s * (255 - rem) / 255) / 255);
  switch (region) {
    case 0:  return {v, t, p};
    case 1:  return {q, v, p};
    case 2:  return {p, v, t};
    case 3:  return {p, q, v};
    case 4:  return {t, p, v};
    default: return {v, p, q};
  }
}

Rgb apply_warmth(Rgb c, int8_t warmth) {
  if (warmth == 0) return c;
  // ±45% on the extreme ends, split across the channels.
  const int32_t w = warmth;  // -100..100
  int32_t r = c.r + (int32_t)c.r * 45 * w / 10000;
  int32_t g = c.g + (int32_t)c.g * 12 * w / 10000;
  int32_t b = c.b - (int32_t)c.b * 45 * w / 10000;
  const auto clamp8 = [](int32_t x) -> uint8_t {
    return (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x));
  };
  return {clamp8(r), clamp8(g), clamp8(b)};
}

Rgb finish(Rgb c, int8_t warmth, bool gamma_on) {
  c = apply_warmth(c, warmth);
  if (gamma_on) { c.r = gamma8(c.r); c.g = gamma8(c.g); c.b = gamma8(c.b); }
  return c;
}

Hsv palette_sample(const Hsv* stops, uint8_t n, uint8_t t) {
  if (n == 0) return {0, 0, 0};
  if (n == 1) return stops[0];
  // Which segment: t in 0..255 maps across n segments (looping).
  const uint16_t f = (uint16_t)((uint16_t)t * n);   // 0..255*n
  const uint8_t seg = (uint8_t)((f >> 8) % n);
  const uint8_t frac = (uint8_t)(f & 0xFF);          // 0..255 within segment
  const Hsv& a = stops[seg];
  const Hsv& b = stops[(seg + 1) % n];
  // Shortest-path hue interpolation.
  int32_t dh = (int32_t)b.h - (int32_t)a.h;
  if (dh > 180) dh -= 360;
  if (dh < -180) dh += 360;
  int32_t h = (int32_t)a.h + dh * frac / 255;
  h %= 360; if (h < 0) h += 360;
  const uint8_t s = (uint8_t)((int32_t)a.s + ((int32_t)b.s - a.s) * frac / 255);
  const uint8_t v = (uint8_t)((int32_t)a.v + ((int32_t)b.v - a.v) * frac / 255);
  return {(uint16_t)h, s, v};
}

uint8_t breath(uint32_t now_ms, uint32_t half_ms) {
  if (half_ms == 0) return 255;
  const uint32_t period = half_ms * 2;
  const uint32_t ph = now_ms % period;                 // 0..period
  const uint32_t tri = ph < half_ms ? ph : (period - ph);  // 0..half_ms
  const uint32_t x = tri * 255 / half_ms;              // 0..255 linear
  // Integer smoothstep: x*x*(3 - 2x) with the 255 scaling folded in.
  // At x=255 -> 255, x=128 -> ~128, x=0 -> 0. Eases both ends.
  const uint32_t sm = (uint32_t)x * x * (765 - 2 * x) / 65025;
  return (uint8_t)(sm > 255 ? 255 : sm);
}

}  // namespace canary::color
