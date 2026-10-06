// doorbell_audio.h — the Canary Vision Doorbell's voice, as pure logic (no
// Arduino, no ESP-IDF, no floating point): host-tested by
// firmware/tests_host/test_doorbell_audio.cpp, streamed on the device by
// firmware/projects/canary-vision/src/doorbell_audio.cpp through one PWM pin
// into a class-D amplifier and a sealed driver (docs/hardware/
// canary_vision_doorbell_wiring.md, "The speaker").
//
// What it says and why (docs/hardware/canary_doorbell_research.md §4.3,
// §4.4, §5.6): a doorbell with no microphone and no feed still owes the
// visitor one thing the glow ring cannot give — a sound that says the
// house heard the press, loud enough for a street. So:
//
//   CHIME  two bell notes, high then low ("ding-dong"), on every SEALED
//          ring. It is the answer to the press, not a chime for the house
//          (the house rings through Home Assistant); it plays with the
//          glow's swell.
//   TICK   one short soft note on a press inside the holdoff: "still
//          heard, still one ring" — a jab never re-rings. Also the whole
//          answer to a ring the witness could not seal (no identity yet):
//          heard, but not the chime that claims a sealed record.
//   WAIT / LEAVE / NO   three short melodies the household can send from
//          Home Assistant (the Doorbell reply select): rising "we're
//          coming", falling "leave it", a low double "no thanks". They are
//          TONES, never speech: a Canary carries no audio in and renders no
//          voice out (§3, invariant I); these are the quick replies a
//          witness can honestly make.
//
// The synthesizer is two voices of a bell: a fundamental plus its octave,
// each with a 5 ms attack and an exponential decay, mixed, scaled by a
// perceptual volume law, and rendered as 8 kHz samples the device turns
// into a 10-bit PWM duty. Everything is integer: the ESP32-C3 has no FPU
// and the sample tick runs at 8000 Hz. Properties the host test holds:
// every phrase starts and ends in silence (no click), stays inside full
// scale at every volume, finishes within its stated length, and volume 0
// is silence.

#pragma once

#include <stdint.h>

namespace doorbell {

constexpr uint32_t kSampleRate = 8000;   // samples per second — chimes and tones, not music
constexpr uint8_t  kVolumeMin  = 0;      // 0 = silent (the glow still swells)
constexpr uint8_t  kVolumeMax  = 100;
constexpr uint8_t  kVolumeDefault = 60;

// ---------------------------------------------------------------------------
// The phrases
// ---------------------------------------------------------------------------

enum class Phrase : uint8_t {
  NONE = 0,
  CHIME,   // a sealed ring: ding-dong
  TICK,    // a repeat inside the holdoff, or an unsigned ring: one soft tick
  WAIT,    // reply: "we're coming" — rising three
  LEAVE,   // reply: "leave it" — falling three
  NO,      // reply: "no thanks" — a low double
};

struct Note {
  uint16_t at_ms;     // when the note starts, from the phrase's start
  uint16_t hz;        // fundamental
  uint16_t decay_ms;  // exponential decay time constant (to 1/e)
  uint8_t  amp;       // peak, 0..255 of full scale
};

constexpr uint8_t kMaxNotes = 4;

struct Score {
  Note    notes[kMaxNotes];
  uint8_t count;
  uint16_t length_ms;   // the phrase is over by here (every tail has decayed)
};

// Bell pitches: E5 / C5 is the classic two-tone door chime; the replies sit
// on the same C-major triad so they belong to one voice; NO drops to A3.
constexpr uint16_t kC5 = 523, kE5 = 659, kG5 = 784, kA3 = 220;

constexpr Score kScores[] = {
  /* NONE  */ {{{0, 0, 0, 0}}, 0, 0},
  // length_ms is at least the last note's start + 6 time constants, so the
  // tail has decayed past the engine's own silence floor before the cut
  /* CHIME */ {{{0, kE5, 320, 255}, {350, kC5, 420, 255}}, 2, 2900},
  /* TICK  */ {{{0, kE5, 40, 110}}, 1, 250},
  /* WAIT  */ {{{0, kC5, 160, 220}, {180, kE5, 160, 220}, {360, kG5, 320, 240}}, 3, 2300},
  /* LEAVE */ {{{0, kG5, 160, 220}, {250, kE5, 160, 220}, {500, kC5, 360, 240}}, 3, 2700},
  /* NO    */ {{{0, kA3, 140, 230}, {320, kA3, 220, 230}}, 2, 1700},
};
constexpr uint8_t kPhraseCount = sizeof(kScores) / sizeof(kScores[0]);

inline const Score& score_of(Phrase p) {
  const uint8_t i = (uint8_t)p;
  return kScores[i < kPhraseCount ? i : 0];
}

// ---------------------------------------------------------------------------
// The synthesizer
// ---------------------------------------------------------------------------

// A quarter wave of sin(), 65 points over 0..π/2, q15. The full wave folds
// out of it; no trig at runtime, no float anywhere.
constexpr int16_t kQuarterSine[65] = {
      0,   804,  1608,  2410,  3212,  4011,  4808,  5602,  6393,  7179,  7962,  8739,  9512,
  10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868,
  19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319,
  26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
  31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767,
};

// sin(phase) for a 16-bit phase (0..65535 = one turn), q15.
inline int16_t sine_q15(uint16_t phase) {
  const uint16_t quadrant = phase >> 14;          // 0..3
  uint16_t idx = (phase >> 8) & 63;               // 0..63 within the quadrant
  const uint16_t frac = phase & 255;
  if (quadrant & 1) idx = 64 - idx;               // 2nd/4th quadrant run backwards
  // linear interpolation between table points (direction follows the quadrant)
  const int32_t a = kQuarterSine[idx];
  const int32_t b = kQuarterSine[(quadrant & 1) ? (idx == 0 ? 0 : idx - 1) : (idx == 64 ? 64 : idx + 1)];
  const int32_t v = a + ((b - a) * (int32_t)frac) / 256;
  return (int16_t)((quadrant & 2) ? -v : v);
}

constexpr uint8_t kVoices = 2;

struct Voice {
  bool     on;
  uint16_t phase;        // fundamental
  uint16_t phase2;       // the octave partial
  uint16_t step;         // phase per sample
  uint32_t env;          // q24 envelope, 1<<24 = full
  uint32_t decay_mul;    // q24 per-sample multiplier
  uint16_t attack_left;  // samples of attack remaining
  uint8_t  amp;
};

struct VoiceState {
  Phrase   phrase;
  uint32_t sample;       // samples since the phrase started
  uint8_t  next_note;    // index of the next note to start
  Voice    v[kVoices];
  uint8_t  volume;       // 0..100
};

inline uint32_t ms_to_samples(uint32_t ms) { return (ms * kSampleRate) / 1000; }

// Perceptual volume: the ear hears amplitude roughly as its square root,
// so a slider that feels linear runs the amplitude as the square of its
// position. q15 of full scale.
inline uint16_t volume_q15(uint8_t pct) {
  const uint32_t p = pct > kVolumeMax ? kVolumeMax : pct;
  return (uint16_t)((p * p * 32767UL) / 10000UL);
}

inline uint8_t clamp_volume(int v) {
  return v < kVolumeMin ? kVolumeMin : v > kVolumeMax ? kVolumeMax : (uint8_t)v;
}

inline VoiceState audio_begin(uint8_t volume) {
  VoiceState s{};
  s.phrase = Phrase::NONE;
  s.volume = clamp_volume(volume);
  return s;
}

inline bool audio_active(const VoiceState* s) { return s->phrase != Phrase::NONE; }

// Start a phrase (replacing any in progress — the attack of the first note
// starts from the mixed level of the old one, so even a cut-off is not a
// click: the envelope is re-entered, never stepped).
inline void audio_play(VoiceState* s, Phrase p) {
  if (p == Phrase::NONE || (uint8_t)p >= kPhraseCount) return;
  s->phrase = p;
  s->sample = 0;
  s->next_note = 0;
  for (uint8_t i = 0; i < kVoices; ++i) s->v[i].on = false;
}

inline void audio_stop(VoiceState* s) {
  s->phrase = Phrase::NONE;
  for (uint8_t i = 0; i < kVoices; ++i) s->v[i].on = false;
}

// A per-sample decay multiplier for a time constant in ms: exp(-1/(tau·sr))
// in q24, from a small series (tau·sr >= 320 samples here, so the first two
// terms are within 0.1 %).
inline uint32_t decay_mul_q24(uint16_t decay_ms) {
  const uint32_t n = ms_to_samples(decay_ms);           // samples per time constant
  if (n == 0) return 0;
  const uint32_t one = 1UL << 24;
  // 1 - 1/n + 1/(2 n^2)
  return one - one / n + (one / n) / (2 * n);
}

constexpr uint16_t kAttackSamples = 40;   // 5 ms at 8 kHz: a bell is struck, not switched

inline void note_on(VoiceState* s, const Note& n) {
  // the free voice, else the quietest one
  uint8_t pick = 0;
  for (uint8_t i = 0; i < kVoices; ++i) if (!s->v[i].on) { pick = i; goto found; }
  for (uint8_t i = 1; i < kVoices; ++i) if (s->v[i].env < s->v[pick].env) pick = i;
found:
  Voice& v = s->v[pick];
  v.on = true;
  v.phase = 0;
  v.phase2 = 0;
  v.step = (uint16_t)((65536UL * n.hz) / kSampleRate);
  v.env = 0;
  v.decay_mul = decay_mul_q24(n.decay_ms);
  v.attack_left = kAttackSamples;
  v.amp = n.amp;
}

// One sample, q15, already scaled by the volume. Returns 0 and goes idle
// when the phrase is over.
inline int16_t audio_sample(VoiceState* s) {
  if (s->phrase == Phrase::NONE) return 0;
  const Score& sc = score_of(s->phrase);
  // start every note whose time has come
  while (s->next_note < sc.count && ms_to_samples(sc.notes[s->next_note].at_ms) <= s->sample) {
    note_on(s, sc.notes[s->next_note]);
    s->next_note++;
  }
  int32_t mix = 0;
  for (uint8_t i = 0; i < kVoices; ++i) {
    Voice& v = s->v[i];
    if (!v.on) continue;
    // the envelope: a linear attack to full, then the exponential decay
    if (v.attack_left) {
      v.env += (1UL << 24) / kAttackSamples;
      if (v.env > (1UL << 24)) v.env = 1UL << 24;
      v.attack_left--;
    } else {
      v.env = (uint32_t)(((uint64_t)v.env * v.decay_mul) >> 24);
      if (v.env < (1UL << 24) / 256) { v.on = false; continue; }    // below -48 dB: done (inaudible over a street)
    }
    // the bell: fundamental + the octave at 40 %, decaying twice as fast
    // (the partial's envelope is the square of the fundamental's)
    const int32_t f = sine_q15(v.phase);
    const int32_t o = sine_q15(v.phase2);
    const int32_t env_q15 = (int32_t)(v.env >> 9);                       // q24 -> q15
    const int32_t env2_q15 = (int32_t)(((uint64_t)env_q15 * env_q15) >> 15);
    int32_t tone = (f * env_q15 + ((o * env2_q15) * 2) / 5) >> 15;      // q15
    mix += (tone * v.amp) / 255;
    v.phase  += v.step;
    v.phase2 += v.step * 2;
  }
  s->sample++;
  // over: every voice decayed, or the score's stated length passed
  bool any = false;
  for (uint8_t i = 0; i < kVoices; ++i) any |= s->v[i].on;
  if ((!any && s->next_note >= sc.count) || s->sample > ms_to_samples(sc.length_ms)) {
    audio_stop(s);
    return 0;
  }
  // two voices at full scale would clip: 0.5 headroom each, then the volume
  if (mix > 32767) mix = 32767;
  if (mix < -32767) mix = -32767;
  return (int16_t)((mix * (int32_t)volume_q15(s->volume)) >> 15);
}

// A q15 sample as a 10-bit PWM duty around mid-scale (512): the DC the
// coupling capacitor blocks, the swing the amplifier hears.
inline uint16_t duty10(int16_t q15) {
  const int32_t d = 512 + ((int32_t)q15 >> 6);
  return (uint16_t)(d < 0 ? 0 : d > 1023 ? 1023 : d);
}

// The name a Home Assistant select sends, to a phrase (and back). constexpr,
// so the discovery payload's option literals can be held to these at
// compile time (same_name below).
constexpr const char* phrase_name(Phrase p) {
  switch (p) {
    case Phrase::CHIME: return "chime";
    case Phrase::TICK:  return "tick";
    case Phrase::WAIT:  return "we're coming";
    case Phrase::LEAVE: return "leave it";
    case Phrase::NO:    return "no thanks";
    default:            return "—";
  }
}

// compile-time string equality, for the static_asserts that hold a payload's
// literals to phrase_name()
constexpr bool same_name(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || same_name(a + 1, b + 1));
}

}  // namespace doorbell
