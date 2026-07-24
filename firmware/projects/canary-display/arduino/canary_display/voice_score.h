#pragma once
#include <stddef.h>
#include <stdint.h>

// ── Canary Voice — the acoustic signature core ──────────────────────────
//
// This is the *pure* half of the display's sound engine (the LEDC streamer
// that plays it lives in src/hal/chime.cpp). No Arduino, no LEDC, no float:
// just the score tables and the integer audio math, so the whole grammar is
// host-tested (tests_host/test_voice_score.cpp) the same way fleet_model and
// mode_registry are. The hardware is a single passive piezo driven by one
// LEDC channel — one square-wave voice, monophonic. Everything "beautiful"
// here is what you can honestly pull off on that: shaped envelopes so notes
// breathe instead of click, glissando so a note can *chirp*, warble so it
// can *trill* like the bird it's named for, and a palette built from a warm
// major-pentatonic so nothing ever clashes — except the intruder alarm,
// which is deliberately dissonant and near piezo resonance because a Tier-1
// alert must never be mistaken for anything pretty (the Owlet lesson, and
// IEC 60601-1-8's whole point).
//
// The result is a *family*: every interaction and status sound shares one
// timbre and one scale, differentiated only by contour and category — an
// acoustic signature that is recognizably ours, the way a marque has a
// startup chime. Alerts keep the safety-critical grammar (§5 of the
// trailblazer spec) unchanged in pitch/tempo.
//
// Loudness model (peak_duty): one honest volume knob, 0..4. Category ceilings
// keep a UI blip from ever being as loud as a fault; night silences the
// gentle categories entirely; the Tier-1 alert keeps a floor so the one
// sound that is allowed to break the night can never be turned all the way
// down. See peak_duty() for the exact ladder.

namespace canary::hal {

// ── The palette ─────────────────────────────────────────────────────────
// One enum for every voice the display has — interactions and alerts alike.
// The Chime enum (chime.h) is a thin back-compat alias onto the four alert
// members here, so existing callers keep compiling untouched.
enum class Voice : uint8_t {
  // Alert grammar (trailblazer spec §5 — pitch/tempo frozen for safety):
  Alarm,      // Tier 1: Alert/Tamper. 10 fast pulses, alt 2.6/3.1 kHz.
  Warn,       // Tier 2: fault (witness lost, chain failed). 3 slow 1.8 kHz.
  AllClear,   // resolution: falling two-tone 1.3 -> 0.9 kHz.
  Sunrise,    // wake chirp: soft ascending warbled pair (nightstand wave).

  // Signature identity — the brand's own voice:
  Boot,       // power-on "the canary wakes": rising warbled chirp.
  Heartbeat,  // once-a-minute all-verified swell: a barely-there low breath.
  JoinSuccess,// onboarding joined the fleet: a bright pentatonic run up.

  // Interactions (optional; off at night; a soft ceiling by construction):
  Tap,        // touch/wake: one soft high pip.
  PageTurn,   // paging a witness: two light rising pips.
  AckConfirm, // long-press acknowledged: a warm resolved rise.
  MuteOn,     // per-witness mute engaged: a gentle fall (going quiet).
  MuteOff,    // mute lifted: the same pair, ascending (coming back).

  Count
};

enum class VoiceCat : uint8_t {
  Interaction,  // UI feedback — quietest ceiling, silent at night, opt-out.
  Ambient,      // the heartbeat — near-silent, silent at night.
  Notice,       // Boot/Join/AllClear/Warn — attenuated at night.
  Wake,         // the morning alarm you armed — sounds through the night.
  Alert,        // Tier-1 — full, breaks the night, keeps a floor at any volume.
};

// ── Envelope shapes ─────────────────────────────────────────────────────
// The amplitude contour of a single note. All of them ramp their edges so a
// note starts and ends near zero — that de-click is the single biggest
// reason this sounds like an instrument and not a beeper.
enum VoiceEnv : uint8_t {
  ENV_SOFT = 0,  // eased attack + release; the default, elegant voice.
  ENV_PLUCK,     // instant attack, gentle decay; short pips (Tap/Page).
  ENV_SWELL,     // slow breath in, hold, slow breath out; the heartbeat.
  ENV_HARD,      // near-rectangular but edge-de-clicked; alert urgency.
};

// One note. freq_end == 0 means "hold f0" (no glissando). warble_permille is
// vibrato depth as thousandths of the pitch (0 = a pure tone); warble_hz is
// its rate (0 defaults to WARBLE_HZ_DFLT). freq_hz == 0 is a rest.
struct Tone {
  uint16_t freq_hz;         // start pitch (Hz); 0 = rest
  uint16_t freq_end_hz;     // glissando target; 0 = flat
  uint16_t ms;              // duration
  uint8_t  env;             // VoiceEnv
  uint8_t  warble_permille; // vibrato depth (0 = none)
  uint16_t warble_hz;       // vibrato rate (0 = default)
};

struct Phrase {
  const Tone* tones;
  uint8_t     count;
};

// ── The warm palette (major pentatonic, piezo-audible octaves) ──────────
// A small piezo is weak below ~1 kHz and harsh at its ~3 kHz resonance, so
// the pleasant motifs live in C6..E7 where the element is both audible and
// sweet. Pentatonic {C D E G A} has no minor seconds — you cannot write an
// ugly interval in it, which is exactly why every non-alarm sound draws from
// here. (The alert tones are intentionally *outside* this set.)
constexpr uint16_t NOTE_C6 = 1047, NOTE_D6 = 1175, NOTE_E6 = 1319;
constexpr uint16_t NOTE_G6 = 1568, NOTE_A6 = 1760;
constexpr uint16_t NOTE_C7 = 2093, NOTE_D7 = 2349, NOTE_E7 = 2637;

constexpr uint16_t WARBLE_HZ_DFLT = 13;  // canary trill rate

// ── Scores ──────────────────────────────────────────────────────────────
// Kept deliberately short (most < 500 ms): a signature earns its place by
// being recognizable and *over*, never by lingering. Rests are freq 0.

// Tier 1 — the intruder/tamper alarm. Pitch and tempo are the frozen
// IEC-60601-1-8 high-priority shape (fast burst, wide pitch jump); only the
// edges are de-clicked. This is the one voice allowed to be jarring.
constexpr Tone SCORE_ALARM[] = {
    {2600, 0, 70, ENV_HARD, 0, 0}, {0, 0, 40, ENV_HARD, 0, 0},
    {3100, 0, 70, ENV_HARD, 0, 0}, {0, 0, 40, ENV_HARD, 0, 0},
    {2600, 0, 70, ENV_HARD, 0, 0}, {0, 0, 40, ENV_HARD, 0, 0},
    {3100, 0, 70, ENV_HARD, 0, 0}, {0, 0, 40, ENV_HARD, 0, 0},
    {2600, 0, 70, ENV_HARD, 0, 0}, {0, 0, 40, ENV_HARD, 0, 0},
    {3100, 0, 70, ENV_HARD, 0, 0}, {0, 0, 200, ENV_HARD, 0, 0},
    {2600, 0, 70, ENV_HARD, 0, 0}, {0, 0, 40, ENV_HARD, 0, 0},
    {3100, 0, 70, ENV_HARD, 0, 0}, {0, 0, 40, ENV_HARD, 0, 0},
    {2600, 0, 70, ENV_HARD, 0, 0}, {0, 0, 40, ENV_HARD, 0, 0},
    {3100, 0, 70, ENV_HARD, 0, 0},
};

// Tier 2 — a fault. Three slow, softer pulses at 1.8 kHz: distinct tempo AND
// pitch from the alarm so a dead battery never reads as a break-in.
constexpr Tone SCORE_WARN[] = {
    {1800, 0, 140, ENV_SOFT, 0, 0}, {0, 0, 240, ENV_SOFT, 0, 0},
    {1800, 0, 140, ENV_SOFT, 0, 0}, {0, 0, 240, ENV_SOFT, 0, 0},
    {1800, 0, 140, ENV_SOFT, 0, 0},
};

// All-clear — a falling two-tone (spec pitches 1.3 -> 0.9 kHz), softened.
// Resolution has a sound, so silence keeps meaning "nothing new."
constexpr Tone SCORE_ALL_CLEAR[] = {
    {1300, 0, 160, ENV_SOFT, 0, 0}, {0, 0, 60, ENV_SOFT, 0, 0},
    {900, 0, 240, ENV_SOFT, 0, 0},
};

// Sunrise — the wake chirp: two ascending warbled notes, a small dawn
// phrase. Rising = morning; the security voices never rise.
constexpr Tone SCORE_SUNRISE[] = {
    {NOTE_C6, NOTE_E6, 150, ENV_SOFT, 22, 12}, {0, 0, 90, ENV_SOFT, 0, 0},
    {NOTE_E6, NOTE_G6, 190, ENV_SOFT, 22, 12},
};

// Boot — "the canary wakes." A rising warbled chirp that resolves up an
// octave: our startup signature. Short, warm, unmistakable.
constexpr Tone SCORE_BOOT[] = {
    {NOTE_C6, NOTE_E6, 130, ENV_SOFT, 26, 14}, {0, 0, 30, ENV_SOFT, 0, 0},
    {NOTE_G6, NOTE_C7, 170, ENV_SOFT, 26, 16},
};

// Heartbeat — the once-a-minute all-verified pulse. A single low note that
// swells and fades: felt more than heard. Its absence is the information.
constexpr Tone SCORE_HEARTBEAT[] = {
    {NOTE_C6, 0, 420, ENV_SWELL, 8, 6},
};

// JoinSuccess — you're in the fleet. A bright pentatonic arpeggio up to the
// octave: the one unabashedly happy sound, earned once at onboarding.
constexpr Tone SCORE_JOIN[] = {
    {NOTE_C6, 0, 90, ENV_SOFT, 0, 0}, {0, 0, 15, ENV_SOFT, 0, 0},
    {NOTE_E6, 0, 90, ENV_SOFT, 0, 0}, {0, 0, 15, ENV_SOFT, 0, 0},
    {NOTE_G6, 0, 90, ENV_SOFT, 0, 0}, {0, 0, 15, ENV_SOFT, 0, 0},
    {NOTE_C7, 0, 170, ENV_SOFT, 12, 14},
};

// Tap — one soft high pip. The lightest possible "heard you."
constexpr Tone SCORE_TAP[] = {
    {NOTE_A6, 0, 42, ENV_PLUCK, 0, 0},
};

// PageTurn — two light rising pips: the sound of a page moving forward.
constexpr Tone SCORE_PAGE[] = {
    {NOTE_E6, 0, 34, ENV_PLUCK, 0, 0}, {0, 0, 22, ENV_PLUCK, 0, 0},
    {NOTE_A6, 0, 34, ENV_PLUCK, 0, 0},
};

// AckConfirm — long-press acknowledged. A warm perfect-fifth rise that
// *resolves*: the interaction equivalent of a nod. Consonant, settled, done.
constexpr Tone SCORE_ACK[] = {
    {NOTE_C6, 0, 95, ENV_SOFT, 0, 0}, {0, 0, 18, ENV_SOFT, 0, 0},
    {NOTE_G6, 0, 165, ENV_SOFT, 10, 12},
};

// MuteOn / MuteOff — one gesture, two directions. Falling = going quiet;
// the same interval ascending = coming back. Mirror pair by design.
constexpr Tone SCORE_MUTE_ON[] = {
    {NOTE_G6, 0, 70, ENV_SOFT, 0, 0}, {0, 0, 12, ENV_SOFT, 0, 0},
    {NOTE_D6, 0, 120, ENV_SOFT, 0, 0},
};
constexpr Tone SCORE_MUTE_OFF[] = {
    {NOTE_D6, 0, 70, ENV_SOFT, 0, 0}, {0, 0, 12, ENV_SOFT, 0, 0},
    {NOTE_G6, 0, 120, ENV_SOFT, 0, 0},
};

// Look up the score + category for a Voice. Pure; returns an empty phrase for
// an out-of-range value (defensive — a bad enum makes no sound, never UB).
inline Phrase voice_phrase(Voice v) {
#define CD_PHRASE(arr) Phrase{arr, (uint8_t)(sizeof(arr) / sizeof(arr[0]))}
  switch (v) {
    case Voice::Alarm:       return CD_PHRASE(SCORE_ALARM);
    case Voice::Warn:        return CD_PHRASE(SCORE_WARN);
    case Voice::AllClear:    return CD_PHRASE(SCORE_ALL_CLEAR);
    case Voice::Sunrise:     return CD_PHRASE(SCORE_SUNRISE);
    case Voice::Boot:        return CD_PHRASE(SCORE_BOOT);
    case Voice::Heartbeat:   return CD_PHRASE(SCORE_HEARTBEAT);
    case Voice::JoinSuccess: return CD_PHRASE(SCORE_JOIN);
    case Voice::Tap:         return CD_PHRASE(SCORE_TAP);
    case Voice::PageTurn:    return CD_PHRASE(SCORE_PAGE);
    case Voice::AckConfirm:  return CD_PHRASE(SCORE_ACK);
    case Voice::MuteOn:      return CD_PHRASE(SCORE_MUTE_ON);
    case Voice::MuteOff:     return CD_PHRASE(SCORE_MUTE_OFF);
    default:                 return Phrase{nullptr, 0};
  }
#undef CD_PHRASE
}

inline VoiceCat voice_category(Voice v) {
  switch (v) {
    case Voice::Alarm:       return VoiceCat::Alert;
    case Voice::Sunrise:     return VoiceCat::Wake;
    case Voice::Heartbeat:   return VoiceCat::Ambient;
    case Voice::Tap:
    case Voice::PageTurn:
    case Voice::AckConfirm:
    case Voice::MuteOn:
    case Voice::MuteOff:     return VoiceCat::Interaction;
    default:                 return VoiceCat::Notice;  // Warn/AllClear/Boot/Join
  }
}

// ── Envelope: amplitude over the life of a note ─────────────────────────
// Returns 0..256 (256 == full). Every shape returns ~0 at the very edges so
// there is no on/off click; the shapes differ in how they get there. Pure
// integer, monotone at the edges, safe for any t (clamped to [0,dur]).
inline uint16_t voice_env_amp(uint8_t env, uint16_t t_ms, uint16_t dur_ms) {
  if (dur_ms == 0) return 0;
  if (t_ms > dur_ms) t_ms = dur_ms;
  switch (env) {
    case ENV_PLUCK: {
      // Instant attack (first 4 ms), then a linear decay to a soft tail.
      constexpr uint16_t ATT = 4, TAIL = 48;  // tail = 48/256 ~= 19%
      uint16_t up = t_ms < ATT ? (uint16_t)(256u * t_ms / ATT) : 256;
      uint32_t dec = 256u - (uint32_t)(256u - TAIL) * t_ms / dur_ms;
      uint16_t a = up < (uint16_t)dec ? up : (uint16_t)dec;
      return a;
    }
    case ENV_SWELL: {
      // Slow raised ramp in over the first ~45%, hold, ramp out over the
      // last ~45%: a breath. The 10% hold keeps the peak honest.
      uint16_t ramp = (uint16_t)((uint32_t)dur_ms * 45 / 100);
      if (ramp == 0) ramp = 1;
      if (t_ms < ramp) return (uint16_t)(256u * t_ms / ramp);
      uint16_t rel_start = (uint16_t)(dur_ms - ramp);
      if (t_ms > rel_start)
        return (uint16_t)(256u * (dur_ms - t_ms) / ramp);
      return 256;
    }
    case ENV_HARD: {
      // Rectangular, but the first/last 4 ms ramp so even the alarm doesn't
      // click (a click reads as a fault, not urgency).
      constexpr uint16_t E = 4;
      if (t_ms < E) return (uint16_t)(256u * t_ms / E);
      if (t_ms > dur_ms - E && dur_ms > E)
        return (uint16_t)(256u * (dur_ms - t_ms) / E);
      return 256;
    }
    case ENV_SOFT:
    default: {
      // Eased attack + release. Attack/release length scales with the note
      // but is capped so long notes still speak promptly.
      uint16_t edge = (uint16_t)((uint32_t)dur_ms * 30 / 100);
      if (edge > 45) edge = 45;
      if (edge == 0) edge = 1;
      if (t_ms < edge) return (uint16_t)(256u * t_ms / edge);
      if (t_ms > dur_ms - edge && dur_ms > edge)
        return (uint16_t)(256u * (dur_ms - t_ms) / edge);
      return 256;
    }
  }
}

// ── Pitch over the life of a note: glissando + warble ───────────────────
// A tiny signed sine LUT (quarter-wave, 0..90 deg, scaled to 1000) drives the
// warble without float or <math.h> — deterministic on-device and in tests.
inline int32_t voice_sin1000(uint32_t phase_deg) {
  static const int16_t Q[16] = {  // sin(0..90] in 6-deg steps, x1000
      0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000};
  phase_deg %= 360;
  int sign = 1;
  if (phase_deg >= 180) { phase_deg -= 180; sign = -1; }
  if (phase_deg > 90) phase_deg = 180 - phase_deg;   // fold to 0..90
  uint32_t idx = phase_deg / 6;
  if (idx > 15) idx = 15;
  return sign * (int32_t)Q[idx];
}

// Instantaneous pitch at t within a note. Glissando lerps freq -> freq_end;
// warble adds a ± depth vibrato. Returns 0 for a rest. Pure.
inline uint16_t voice_freq_at(const Tone& n, uint16_t t_ms) {
  if (n.freq_hz == 0) return 0;
  uint16_t dur = n.ms ? n.ms : 1;
  if (t_ms > dur) t_ms = dur;
  int32_t base = n.freq_hz;
  if (n.freq_end_hz != 0 && n.freq_end_hz != n.freq_hz) {
    base += ((int32_t)n.freq_end_hz - (int32_t)n.freq_hz) * t_ms / dur;
  }
  if (n.warble_permille != 0) {
    uint16_t rate = n.warble_hz ? n.warble_hz : WARBLE_HZ_DFLT;
    uint32_t deg = ((uint32_t)t_ms * rate * 360u / 1000u);  // rate cycles/sec
    int32_t wob = base * (int32_t)n.warble_permille / 1000 * voice_sin1000(deg) / 1000;
    base += wob;
  }
  if (base < 40) base = 40;             // stay in a drivable band
  if (base > 6000) base = 6000;
  return (uint16_t)base;
}

// ── The volume model ────────────────────────────────────────────────────
// One knob (vol 0..4: Off, Soft, Low, Medium, Full) becomes a *peak duty*
// 0..128 (a passive piezo is loudest as duty approaches 50% == 128/255; the
// engine writes an 8-bit duty). Category ceilings mean a UI blip is never as
// loud as a fault; night silences the gentle categories; the alert keeps a
// floor so the one sound that breaks the night can't be dialed to nothing.
// `ramp` (0/1/2) is the alarm-escalation sub-level (wake soft, escalate
// honestly) — pass 2 for everything else.
//
// Returns 0 for "make no sound." Pure — this is the whole loudness policy in
// one testable function.
inline uint8_t voice_peak_duty(VoiceCat cat, uint8_t vol, bool night,
                               uint8_t ramp) {
  if (vol > 4) vol = 4;
  if (ramp > 2) ramp = 2;

  // Base peak per volume step (duty 0..128). Echoes the old RAMP_DUTY ladder
  // so a bench-tuned "full" is unchanged.
  static const uint8_t BASE[5] = {0, 46, 74, 102, 128};
  // Category ceiling (num/128). Interaction/Ambient sit well under a fault.
  static const uint8_t CEIL[5] = {72, 60, 108, 116, 128};   // by VoiceCat order
  // Night factor (num/128): the gentle categories go fully silent; Wake and
  // Alert sound through.
  static const uint8_t NIGHT[5] = {0, 0, 60, 128, 128};
  // Escalation ramp (num/128).
  static const uint8_t RAMP[3] = {60, 96, 128};

  const uint8_t ci = (uint8_t)cat;
  uint32_t d = BASE[vol];
  d = d * CEIL[ci] / 128;
  if (night) d = d * NIGHT[ci] / 128;
  d = d * RAMP[ramp] / 128;

  // The Tier-1 floor: an Alert is always audible, at any volume — even Off,
  // even mid-ramp. This is the "one sound that always speaks" made literal.
  if (cat == VoiceCat::Alert) {
    const uint8_t floor = night ? 96 : 80;
    if (d < floor) d = floor;
  }
  if (d > 128) d = 128;
  return (uint8_t)d;
}

}  // namespace canary::hal
