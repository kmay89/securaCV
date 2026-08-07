// firmware/common/color/plumage.cpp — the plumage engine. See plumage.h.
// Integer / fixed-point only (the C6 has no FPU and this runs every loop pass).
#include "color/plumage.h"

namespace canary::color {

namespace {

// ── Easing primitives ──────────────────────────────────────────────────────
// Smoothstep 3x^2 - 2x^3 on 0..255, done in the same 0..255 unit space so it
// composes with everything else here without a scale factor to get wrong.
uint8_t smoothstep8(uint8_t x) {
  const uint32_t x2 = ((uint32_t)x * x) / 255u;
  const uint32_t x3 = (x2 * x) / 255u;
  const int32_t s = (int32_t)(3u * x2) - (int32_t)(2u * x3);
  return (uint8_t)(s < 0 ? 0 : (s > 255 ? 255 : s));
}

// A smooth 0 -> 255 -> 0 over one cycle. Doubles as the oscillator inside a
// trill and the wander inside a warble.
uint8_t bell8(uint8_t t) {
  return t < 128 ? smoothstep8((uint8_t)(t * 2))
                 : smoothstep8((uint8_t)((255 - t) * 2));
}

// Attack/release envelope: `a` is the attack fraction of the note (0..255).
// A small `a` is a chirp's snap; a large one is a whistle's swell.
uint8_t env_ar(uint8_t t, uint8_t a) {
  if (a == 0) a = 1;
  if (t < a) return smoothstep8((uint8_t)((uint32_t)t * 255u / a));
  const uint8_t rem = (uint8_t)(255 - a);
  if (rem == 0) return 255;
  uint32_t u = (uint32_t)(t - a) * 255u / rem;
  if (u > 255) u = 255;
  return smoothstep8((uint8_t)(255 - u));
}

uint8_t add_sat(uint8_t a, uint8_t b) {
  const uint16_t s = (uint16_t)a + b;
  return (uint8_t)(s > 255 ? 255 : s);
}

Rgb add_sat(Rgb a, Rgb b) {
  return {add_sat(a.r, b.r), add_sat(a.g, b.g), add_sat(a.b, b.b)};
}

// Signed pitch offset, clamped into 0..255 (the palette loops, but a note that
// wrapped the whole wheel mid-syllable would read as a glitch, not a slur).
uint8_t pitch_shift(uint8_t base, int32_t delta) {
  int32_t v = (int32_t)base + delta;
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return (uint8_t)v;
}

// ── The syllables ──────────────────────────────────────────────────────────
// Each returns the amplitude envelope for progress `t` (0..255 through the
// note) and nudges the pitch. This is the whole light-language in one switch:
// keep it readable, because it is the thing people will want to tune.
void syllable_shape(Syllable k, uint8_t t, uint8_t restlessness,
                    uint8_t* env, int32_t* dpitch) {
  *dpitch = 0;
  switch (k) {
    case Syllable::Chirp:
      // Snap on, fall away. The pitch lifts slightly as it decays — the
      // upward flick that makes a chirp sound like a chirp.
      *env = env_ar(t, 40);
      *dpitch = (int32_t)t * 18 / 255;
      break;

    case Syllable::Trill: {
      // One note, repeated fast inside its own envelope. The repeat never
      // reaches zero (a lamp that chops reads as a fault), so it shimmers.
      const uint8_t body = env_ar(t, 70);
      const uint8_t osc = bell8((uint8_t)((uint32_t)t * 9u & 0xFFu));
      *env = (uint8_t)((uint32_t)body * (140u + (uint32_t)osc * 115u / 255u) / 255u);
      break;
    }

    case Syllable::Warble: {
      // The lyrical one: gentle amplitude, pitch wandering two full sweeps.
      *env = bell8(t);
      const uint8_t w = bell8((uint8_t)((uint32_t)t * 2u & 0xFFu));
      *dpitch = ((int32_t)w - 128) * (int32_t)restlessness / 255;
      break;
    }

    case Syllable::Whistle:
      // Slow in, long hold, slow out — the carrying note. Steady pitch.
      *env = env_ar(t, 110);
      break;

    case Syllable::Churr: {
      // Low, rough, muttered. Small fast flutter on a quiet body, pitched
      // down — the bird talking to itself rather than to the room.
      const uint8_t body = env_ar(t, 90);
      const uint8_t rough = bell8((uint8_t)((uint32_t)t * 17u & 0xFFu));
      *env = (uint8_t)((uint32_t)body * (170u + (uint32_t)rough * 85u / 255u) / 255u);
      *env = (uint8_t)((uint32_t)*env * 108u / 255u);
      *dpitch = -22;
      break;
    }

    case Syllable::Rest:
    default:
      *env = 0;
      break;
  }
}

}  // namespace

// ── Voice ──────────────────────────────────────────────────────────────────

Voice voice_from_id(uint32_t device_id) {
  // Avalanche the id so neighboring ids (a bench full of sequential Canaries)
  // get genuinely different personalities rather than adjacent ones.
  uint32_t h = device_id ? device_id : 0x9E3779B9u;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;

  Voice v;
  // Every trait lands in a musical middle band: there is no id that yields a
  // mute bird or a strobing one, so a bad id cannot make an unpleasant device.
  v.chatter = (uint8_t)(70 + (h & 0x7F));                // 70..197
  v.boldness = (uint8_t)(90 + ((h >> 8) & 0x7F));        // 90..217
  v.pitch = (uint8_t)((h >> 16) & 0xFF);                 // anywhere on the palette
  v.restlessness = (uint8_t)(40 + ((h >> 24) & 0x7F));   // 40..167
  return v;
}

// ── Plumage ────────────────────────────────────────────────────────────────

uint32_t Plumage::rnd() {
  // xorshift32 — the same reproducible-per-device trick ambient_life uses.
  seed_ ^= seed_ << 13;
  seed_ ^= seed_ >> 17;
  seed_ ^= seed_ << 5;
  return seed_;
}

uint8_t Plumage::rnd8(uint8_t lo, uint8_t hi) {
  if (hi <= lo) return lo;
  return (uint8_t)(lo + (rnd() % (uint32_t)(hi - lo + 1)));
}

void Plumage::begin(uint32_t device_id, uint32_t now_ms) {
  voice_ = voice_from_id(device_id);
  seed_ = device_id ? device_id : 0x1234567u;
  phrase_.n = 0;
  phrase_.total_ms = 0;
  start_ms_ = now_ms;
  seeded_ = true;
  // Do not speak the instant we boot: a device that greets an empty 3 a.m.
  // hallway on power-up is a device someone unplugs.
  next_at_ = now_ms + 12000u + (rnd() % 20000u);
}

void Plumage::compose(uint32_t now_ms, bool night) {
  const uint8_t n = rnd8(2, night ? 3 : 5);
  phrase_.n = n > kMaxNotes ? kMaxNotes : n;
  uint32_t total = 0;

  for (uint8_t i = 0; i < phrase_.n; i++) {
    Note& nt = phrase_.notes[i];

    // Weighted pick. At night the calm syllables dominate: a hallway lamp
    // stirs, it does not perform (the motion budget is law).
    const uint8_t roll = rnd8(0, 99);
    if (night) {
      nt.kind = roll < 45  ? Syllable::Whistle
              : roll < 70  ? Syllable::Warble
              : roll < 85  ? Syllable::Churr
              : roll < 95  ? Syllable::Chirp
                           : Syllable::Trill;
    } else {
      nt.kind = roll < 28  ? Syllable::Chirp
              : roll < 50  ? Syllable::Warble
              : roll < 68  ? Syllable::Trill
              : roll < 88  ? Syllable::Whistle
                           : Syllable::Churr;
    }

    // Whistles carry, chirps are quick. Everything is longer at night.
    const uint16_t base_ms =
        nt.kind == Syllable::Chirp    ? (uint16_t)rnd8(18, 34) * 10u
      : nt.kind == Syllable::Trill    ? (uint16_t)rnd8(34, 60) * 10u
      : nt.kind == Syllable::Whistle  ? (uint16_t)rnd8(80, 145) * 10u
      : nt.kind == Syllable::Churr    ? (uint16_t)rnd8(45, 80) * 10u
                                      : (uint16_t)rnd8(55, 95) * 10u;
    nt.dur_ms = night ? (uint16_t)((uint32_t)base_ms * 3u / 2u) : base_ms;

    // Pitch wanders around the voice's center by its restlessness.
    const int32_t span = (int32_t)voice_.restlessness / 2;
    nt.pitch = pitch_shift(voice_.pitch, (int32_t)rnd8(0, 255) * 2 * span / 255 - span);

    nt.amp = rnd8(150, 255);
    total += nt.dur_ms;

    // A rest between syllables — the pause is part of the phrase, and it is
    // what keeps the lamp from reading as a continuous animation.
    if (i + 1 < phrase_.n) total += 120u + rnd() % 220u;
  }

  phrase_.total_ms = total;
  start_ms_ = now_ms;
}

void Plumage::tick(uint32_t now_ms, bool night, bool gated) {
  if (!seeded_) begin(0, now_ms);

  if (speaking(now_ms)) return;   // let a sentence finish, even if gated

  if (gated) {
    // Hold the clock rather than banking time: re-opening the gate must not
    // release a stored-up burst of speech. The hold sits a settle period out,
    // not one millisecond, so a lamp that has just been lit collects itself
    // before it says anything — coming on and immediately performing reads as
    // a device reacting to being switched, which is not the impression a
    // hallway light should give.
    next_at_ = now_ms + kSettleMs;
    return;
  }

  if ((int32_t)(now_ms - next_at_) < 0) return;

  compose(now_ms, night);

  // Gap to the next phrase: chattier birds speak more often, and night more
  // than doubles every gap.
  uint32_t gap = kMinGapMs + (uint32_t)(255 - voice_.chatter) * 260u; // 22s..88s
  if (night) gap = gap * 9u / 4u;                                    // 50s..3.3min
  gap += rnd() % (gap / 3u + 1u);
  next_at_ = now_ms + phrase_.total_ms + gap;
}

bool Plumage::speaking(uint32_t now_ms) const {
  if (phrase_.n == 0 || phrase_.total_ms == 0) return false;
  return (uint32_t)(now_ms - start_ms_) < phrase_.total_ms;
}

// Walk the phrase to find the note covering `elapsed`, and how far into it we
// are (0..255). The composer records only the phrase total, so the rest time
// is redistributed evenly between notes here — one place, so `sample()` and
// `head()` can never disagree about where a syllable starts.
// Returns nullptr while in a rest or past the end.
const Note* Plumage::locate(uint32_t elapsed, uint8_t* progress) const {
  if (progress) *progress = 0;
  if (phrase_.n == 0) return nullptr;

  uint32_t note_total = 0;
  for (uint8_t i = 0; i < phrase_.n; i++) note_total += phrase_.notes[i].dur_ms;
  if (note_total == 0) return nullptr;

  const uint32_t rest_total =
      phrase_.total_ms > note_total ? phrase_.total_ms - note_total : 0;
  const uint32_t rest_each = phrase_.n > 1 ? rest_total / (phrase_.n - 1) : 0;

  uint32_t acc = 0;
  for (uint8_t i = 0; i < phrase_.n; i++) {
    const Note& nt = phrase_.notes[i];
    if (elapsed < acc + nt.dur_ms) {
      if (progress)
        *progress =
            nt.dur_ms ? (uint8_t)((elapsed - acc) * 255u / nt.dur_ms) : 0;
      return &nt;
    }
    acc += nt.dur_ms + rest_each;
    if (elapsed < acc) return nullptr;   // in the rest after this note
  }
  return nullptr;
}

void Plumage::sample(uint32_t now_ms, uint8_t* pitch, uint8_t* amp) const {
  if (pitch) *pitch = voice_.pitch;
  if (amp) *amp = 0;
  if (!speaking(now_ms)) return;

  uint8_t t = 0;
  const Note* nt = locate(now_ms - start_ms_, &t);
  if (nt == nullptr) return;   // a rest: the resting glow, not darkness

  uint8_t env = 0;
  int32_t dp = 0;
  syllable_shape(nt->kind, t, voice_.restlessness, &env, &dp);
  const uint8_t a = (uint8_t)((uint32_t)env * nt->amp / 255u);
  if (pitch) *pitch = pitch_shift(nt->pitch, dp);
  if (amp) *amp = (uint8_t)((uint32_t)a * voice_.boldness / 255u);
}

uint8_t Plumage::head(uint32_t now_ms) const {
  if (!speaking(now_ms)) return 0;
  uint8_t t = 0;
  if (locate(now_ms - start_ms_, &t) == nullptr) return 0;
  // Rise across the syllable, eased, so the climb decelerates at the top
  // instead of hitting the ceiling at speed.
  return smoothstep8(t);
}

// ── Render ─────────────────────────────────────────────────────────────────

namespace {

// The note's own light, as a color to ADD to the resting lamp. Drawn from the
// SAME scene palette the lamp is already wearing, so the song can never
// introduce a color the scene does not own — which is what keeps it from ever
// reading as a semantic (state) color.
Rgb note_light(const LookParams& p, uint8_t pitch, uint32_t swell) {
  // The owner's color if they picked one — otherwise the catalog scene.
  // Sampling kScenes directly here added the OLD scene's color on top of a
  // chosen hue during a note, so a picked color visibly drifted whenever the
  // lamp sang.
  const Scene sc = current_look(p);
  Hsv h = palette_sample(sc.stops, sc.n_stops, pitch);
  h.v = (uint8_t)((uint32_t)h.v * swell / 255u);
  return finish(hsv_to_rgb(h.h, h.s, h.v), p.warmth, p.gamma_on);
}

// How far the song may swell the lamp this frame, after depth, the user's
// brightness, and the night dim.
uint32_t swell_for(const LookParams& p, uint8_t amp, uint8_t depth) {
  uint32_t s = (uint32_t)amp * depth / 255u;
  s = s * p.brightness / 255u;
  if (p.night) s = s * 120u / 255u;
  return s > 255 ? 255 : s;
}

}  // namespace

Rgb plumage_led(uint32_t now_ms, const LookParams& p, Sev worst, bool safe_dark,
                const Plumage& song, uint8_t depth) {
  // The honest override and the safe-dark rule are look_engine's, not ours:
  // hand these straight back so there is exactly one implementation of them.
  if (worst >= Sev::Warn || safe_dark)
    return led_color(now_ms, p, worst, safe_dark);

  const Rgb base = led_color(now_ms, p, worst, safe_dark);
  if (depth == 0) return base;

  uint8_t pitch = 0, amp = 0;
  song.sample(now_ms, &pitch, &amp);
  if (amp == 0) return base;

  return add_sat(base, note_light(p, pitch, swell_for(p, amp, depth)));
}

void plumage_bands(uint32_t now_ms, const LookParams& p, Sev worst,
                   bool safe_dark, const Plumage& song, uint8_t depth,
                   Rgb* out, uint8_t count) {
  if (count == 0 || out == nullptr) return;

  // Resting field first — and on the honest paths, ONLY this.
  wash_stops(now_ms, p, worst, safe_dark, out, count);
  if (worst >= Sev::Warn || safe_dark || depth == 0) return;

  uint8_t pitch = 0, amp = 0;
  song.sample(now_ms, &pitch, &amp);
  if (amp == 0) return;

  const uint8_t head = song.head(now_ms);
  const uint32_t swell = swell_for(p, amp, depth);

  // The glow's vertical reach, in 0..255 glass units. Wide enough that on a
  // coarse band count it still reads as a soft body of light rather than a
  // single lit stripe.
  constexpr uint8_t kReach = 96;

  for (uint8_t i = 0; i < count; i++) {
    // out[0] is the TOP of the glass; the head rises from the bottom.
    const uint8_t pos =
        (uint8_t)((uint32_t)(count - 1 - i) * 255u / (count > 1 ? count - 1 : 1));
    const uint8_t d = pos > head ? (uint8_t)(pos - head) : (uint8_t)(head - pos);
    if (d >= kReach) continue;
    const uint8_t gain =
        smoothstep8((uint8_t)(255u - (uint32_t)d * 255u / kReach));
    const uint32_t local = swell * gain / 255u;
    if (local == 0) continue;
    out[i] = add_sat(out[i], note_light(p, pitch, local));
  }
}

}  // namespace canary::color
