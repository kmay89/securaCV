// Microphone decision logic — pure, host-testable core (no Arduino, no I2S).
//
// The mic-bearing dash (Waveshare 4.3C, docs/hardware/display_mic_variant.md)
// hears ALARM PATTERNS, never speech: the runtime computes one envelope
// scalar per frame, this core turns loud/quiet EDGES into smoke-T3 / CO-T4
// cadence detections, and everything else about the audio dies in the
// runtime's frame buffer (zeroed after the RMS — the WAP's privacy-barrier
// design, securacv_audio.h). What crosses this boundary is booleans and
// milliseconds; speech reconstruction is structurally infeasible because
// nothing that could carry speech ever reaches here.
//
// Two invariants this core enforces by construction (host-tested):
//   1. The GATE is the single authority on the capture driver, and the
//      on-glass indicator IS the gate's running bit — there is no second
//      flag to desync. A runtime that starts the driver without lighting
//      the chip would have to bypass this type to do it.
//   2. OFF is the default and unset pins hard-block listening: armed alone
//      is never enough (a fresh board with -1 audio pins provably cannot
//      drive its mics).

#ifndef CANARY_IO_MIC_LOGIC_H
#define CANARY_IO_MIC_LOGIC_H

#include <stdint.h>

namespace canary {
namespace io {
namespace mic {

// ── The listening gate ──────────────────────────────────────────────────────

enum class Action : uint8_t { None, Start, Stop };

// One authority for "may the capture driver run". The runtime calls
// update() every pass and performs exactly the returned action — driver
// install/uninstall AND the indicator chip, in the same breath. The
// indicator has no state of its own: indicator_lit() == running.
struct Gate {
  bool armed = false;    // the household's opt-in (NVS-backed, default OFF)
  bool pins_ok = false;  // every AUDIO_PIN_I2S_* >= 0 (bench fills them)
  bool running = false;  // driver installed right now == chip lit right now

  Action update() {
    const bool want = armed && pins_ok;
    if (want && !running) {
      running = true;
      return Action::Start;
    }
    if (!want && running) {
      running = false;
      return Action::Stop;  // hard mute: the driver is UNINSTALLED
    }
    return Action::None;
  }
};

inline bool indicator_lit(const Gate& g) { return g.running; }

// ── Sensitivity presets (the one environment-tunable axis) ──────────────────
//
// The alarm CADENCE is fixed by the standards (below) — not tunable. What a
// room changes is only the LEVEL at which a sound counts as a "beep", and
// the honest way to set that is RELATIVE to the measured noise floor, not as
// an absolute RMS number:
//
//   A UL-listed alarm emits >= 85 dBA @ 10 ft (>= 79 dBA low-frequency,
//   UL 217/2034). Household ambient runs ~30 dBA (bedroom) to ~65 dBA (noisy
//   kitchen). So an alarm stands +14 dB above the room in the worst case and
//   +20..40 dB typically — a 5x..100x RMS margin. A threshold expressed as
//   "N dB over the tracked floor" therefore lands correctly REGARDLESS of the
//   ES7210's absolute gain, which is the one thing we cannot know without the
//   bench. The margins here are gain-independent physics; only `floor_min`
//   (the dead-silence clamp) is a soft bench anchor — and it fails SAFE when
//   set low (a low clamp keeps the detector sensitive; the runtime's SNAP
//   line prints the live floor so the bench sets it in one glance).
//
// Percent-of-floor thresholds: on = floor * on_pct/100 (e.g. 316 = +10 dB),
// off = floor * off_pct/100 (hysteresis, always < on_pct). Nuisance edges
// below the alarm cadence are harmless — the CadenceDetector rejects anything
// that isn't a T3/T4 group — so the presets can lean sensitive.

struct Sensitivity {
  uint16_t floor_min;   // dead-silence clamp on the tracked floor (bench anchor)
  uint16_t on_pct;      // on_threshold  = floor * on_pct  / 100  (dB over floor)
  uint16_t off_pct;     // off_threshold = floor * off_pct / 100  (< on_pct)
  uint8_t  floor_shift; // floor EMA speed: floor += (rms-floor) >> shift, quiet
};

// Bedroom / nightstand — sensitivity first: a faint, distant, or
// through-the-wall alarm must still wake you, and nuisance edges are cheap
// (you are right there). +9 dB on / +5 dB off, low clamp, medium tracking.
constexpr Sensitivity SENS_QUIET    = {100, 282, 178, 6};
// Living areas — the balanced default. +10 dB on / +6 dB off.
constexpr Sensitivity SENS_STANDARD = {180, 316, 200, 6};
// Kitchen / workshop — specificity first: clatter, tools and music shouldn't
// chatter the detector, and you are awake to hear a real one. +13 dB on /
// +8 dB off, higher clamp, slower tracking so a transient can't jerk the
// floor up and desensitize it.
constexpr Sensitivity SENS_NOISY    = {350, 450, 251, 7};

constexpr uint8_t SENS_COUNT = 3;
constexpr uint8_t SENS_DEFAULT_INDEX = 1;  // Standard

inline const Sensitivity& sensitivity_by_index(uint8_t i) {
  switch (i) {
    case 0:  return SENS_QUIET;
    case 2:  return SENS_NOISY;
    default: return SENS_STANDARD;
  }
}

inline const char* sensitivity_name(uint8_t i) {
  switch (i) {
    case 0:  return "quiet";
    case 2:  return "noisy";
    default: return "standard";
  }
}

// ── Envelope with an adaptive noise floor ───────────────────────────────────
// RMS scalar in, loud/quiet out. Thresholds ride a slowly-tracked noise
// floor (see Sensitivity): the floor follows ambient only WHILE QUIET (a beep
// can never inflate its own threshold) and is clamped to floor_min, so the
// detector self-calibrates to the room instead of trusting an absolute level.

struct Envelope {
  Sensitivity s = SENS_STANDARD;
  uint16_t floor = 0;
  bool loud = false;
  bool seeded = false;

  void set_profile(const Sensitivity& ns) {
    s = ns;
    floor = ns.floor_min;
    loud = false;
    seeded = true;
  }

  // Fall time constant (shared): the floor drops toward a quieter reading
  // ~8x faster than it rises. Fast fall is what makes the follower track the
  // room's QUIET level rather than its mean — the silence between beeps pulls
  // the floor straight back down, so an alarm can never inflate its own bar.
  static constexpr uint8_t FALL_SHIFT = 3;

  uint16_t noise_floor() const { return floor < s.floor_min ? s.floor_min : floor; }
  uint16_t on_threshold() const { return clamp16((uint32_t)noise_floor() * s.on_pct / 100); }
  uint16_t off_threshold() const { return clamp16((uint32_t)noise_floor() * s.off_pct / 100); }

  static uint16_t clamp16(uint32_t v) { return v > 0xFFFF ? 0xFFFF : (uint16_t)v; }

  bool update(uint16_t rms) {
    if (!seeded) { floor = s.floor_min; seeded = true; }
    // Asymmetric noise-floor follower: rise SLOWLY toward a louder reading (a
    // brief beep barely lifts it), fall FAST toward a quieter one (the gaps
    // pull it back), so the floor settles on the room's true ambient. A
    // standing alarm can't raise its own threshold — its gaps reset the floor
    // every cycle — while genuinely sustained room noise (no gaps) does.
    if (rms >= floor) {
      floor = (uint16_t)(floor + (((uint32_t)rms - floor) >> s.floor_shift));
    } else {
      floor = (uint16_t)(floor - (((uint32_t)floor - rms) >> FALL_SHIFT));
    }
    if (floor < s.floor_min) floor = s.floor_min;
    const uint32_t fl = floor;  // clamped above
    if (!loud && rms >= fl * s.on_pct / 100) loud = true;
    else if (loud && rms < fl * s.off_pct / 100) loud = false;
    return loud;
  }
};

// ── Alarm-cadence detection ─────────────────────────────────────────────────
//
// The two regulated household alarm grammars:
//   T3 (smoke — NFPA 72 / ISO 8201): three ~0.5 s beeps, ~0.5 s apart,
//       then a ~1.5 s pause, repeating.
//   T4 (CO — UL 2034): four ~0.1 s beeps, ~0.1 s apart, then a long
//       (~5 s) pause, repeating.
// Detection = beep COUNT per group + duration windows + a repeat: two
// consecutive matching groups raise the event (one group can be a horn
// honk; two on-grammar cycles are an alarm). Confidence grows with cycles.

enum class Event : uint8_t { None = 0, SmokeT3, CoT4 };

struct Detection {
  Event event = Event::None;
  uint8_t cycles = 0;      // consecutive matching groups
  uint8_t confidence = 0;  // 0..100, from cycle count
};

struct CadenceDetector {
  // Beep-duration windows (ms), derived from the standards' source timing
  // plus what the acoustic path adds: room reverb lengthens a beep's tail
  // above the release threshold, and 20 ms audio frames quantize each edge.
  //   T3 source beep = 0.5 s ±10% (450-550 ms) -> measured ~430-750 ms.
  //   T4 source beep = 0.1 s ±10 ms (90-110 ms) -> measured ~60-280 ms.
  // The windows stay DISJOINT (T3_MIN > T4_MAX by a 50 ms guard) so a
  // miscounted group can never cross-classify between the two grammars.
  static constexpr uint16_t T3_BEEP_MIN = 350, T3_BEEP_MAX = 800;
  static constexpr uint16_t T4_BEEP_MIN = 60, T4_BEEP_MAX = 300;
  // A gap this long ends the current beep group. It must sit ABOVE the
  // largest intra-group gap (T3's 0.5 s ±10% between beeps, ~570 ms measured)
  // and BELOW the smallest inter-group pause (T3's ~1.5 s, shortened by
  // reverb to ~1.3 s) so intra-gaps never split a group and inter-gaps always
  // close one. 900 ms sits mid-band with margin on both sides.
  static constexpr uint16_t GROUP_GAP_MS = 900;
  // A silence this long means the alarm stopped; cycles reset. It MUST exceed
  // the longest inter-group pause — T4's 5 s ±0.5 s — or a standing CO alarm
  // would reset its own streak between groups and never confirm. 12 s clears
  // that 5.5 s worst case with room to spare while still resetting promptly
  // once a real alarm goes quiet.
  static constexpr uint32_t RESET_GAP_MS = 12000;

  bool prev_loud = false;
  uint32_t edge_ms = 0;        // time of the last edge
  uint8_t beeps = 0;           // beeps in the current group
  uint16_t beep_min = 0xFFFF;  // duration extremes within the group
  uint16_t beep_max = 0;
  Event streak_event = Event::None;
  uint8_t streak = 0;

  // Feed the (hysteresis-clean) loud flag once per frame with the frame
  // time. Returns a detection when a group CLOSES and extends a streak to
  // >= 2 cycles; Event::None otherwise.
  Detection update(bool loud, uint32_t now) {
    Detection out;
    if (loud != prev_loud) {
      if (loud) {
        // Rising edge after a long-dead room: whatever streak an earlier
        // (stopped) alarm built must not lend credibility to new noise.
        if ((uint32_t)(now - edge_ms) >= RESET_GAP_MS) {
          streak = 0;
          streak_event = Event::None;
        }
      } else {
        // Falling edge: a beep just ended — measure it.
        const uint32_t dur32 = now - edge_ms;
        const uint16_t dur = dur32 > 0xFFFF ? 0xFFFF : (uint16_t)dur32;
        beeps = (uint8_t)(beeps < 255 ? beeps + 1 : beeps);
        if (dur < beep_min) beep_min = dur;
        if (dur > beep_max) beep_max = dur;
      }
      prev_loud = loud;
      edge_ms = now;
      return out;
    }
    if (!loud && beeps > 0) {
      const uint32_t quiet = now - edge_ms;
      if (quiet >= RESET_GAP_MS) {
        beeps = 0;
        beep_min = 0xFFFF;
        beep_max = 0;
        streak = 0;
        streak_event = Event::None;
      } else if (quiet >= GROUP_GAP_MS) {
        // Group closed: classify by count + duration window.
        Event e = Event::None;
        if (beeps == 3 && beep_min >= T3_BEEP_MIN && beep_max <= T3_BEEP_MAX) {
          e = Event::SmokeT3;
        } else if (beeps == 4 && beep_min >= T4_BEEP_MIN &&
                   beep_max <= T4_BEEP_MAX) {
          e = Event::CoT4;
        }
        if (e != Event::None && e == streak_event) {
          streak = (uint8_t)(streak < 255 ? streak + 1 : streak);
        } else {
          streak_event = e;
          streak = (uint8_t)(e == Event::None ? 0 : 1);
        }
        beeps = 0;
        beep_min = 0xFFFF;
        beep_max = 0;
        if (streak >= 2) {
          out.event = streak_event;
          out.cycles = streak;
          const unsigned c = 50u + 15u * streak;
          out.confidence = (uint8_t)(c > 95 ? 95 : c);
        }
      }
    }
    return out;
  }
};

inline const char* event_wire_name(Event e) {
  switch (e) {
    case Event::SmokeT3: return "acoustic_smoke_alarm";
    case Event::CoT4:    return "acoustic_co_alarm";
    default:             return "";
  }
}

// ── Loud-onset ("transient") detector ───────────────────────────────────────
//
// A door closing, a knock, someone crossing a quiet room: a sudden loud
// ONSET. This is the mechanism behind the opt-in "wake the screen on a sound"
// feature — a dark wall dash that lights to show the house the moment you walk
// in. It is ENVELOPE-ONLY, exactly like the alarm path: it watches the same
// RMS-vs-floor scalar (no samples, no new data crossing the privacy barrier)
// and never learns WHAT the sound was, only that the room got suddenly loud.
//
// Design: fire on the rising crossing of a threshold set well ABOVE the
// alarm's on-threshold — a real thump, not a TV murmur or speech — relative
// to the tracked noise floor (so it self-scales: a louder room needs a louder
// event). Refractory-gated so one door-close is one wake, and seeded on the
// first quiet frame so the driver-start transient can't fire it.
struct TransientDetector {
  // onset threshold = floor * rise_pct/100. 600 ≈ +15.6 dB over ambient —
  // clearly louder than the +10 dB an alarm beep clears, so speech/TV at
  // conversational level won't trip it. Tunable.
  uint16_t rise_pct = 600;
  uint32_t refractory_ms = 4000;  // one wake per event; ignore repeats 4 s
  bool prev_over = false;
  bool seeded = false;            // saw a below-threshold frame yet?
  bool has_fired = false;         // the refractory gap only applies BETWEEN wakes
  uint32_t last_fire_ms = 0;

  // Feed the frame's rms, the envelope's tracked noise floor, and the frame
  // time. Returns true once on a qualifying loud onset.
  bool update(uint16_t rms, uint16_t floor, uint32_t now) {
    const uint32_t thresh = (uint32_t)floor * rise_pct / 100;
    const bool over = rms >= thresh;
    bool fired = false;
    if (over && !prev_over && seeded &&
        (!has_fired || (uint32_t)(now - last_fire_ms) >= refractory_ms)) {
      fired = true;
      has_fired = true;
      last_fire_ms = now;
    }
    // Arm only after the room has been below threshold once, so a loud
    // capture-start glitch (or arming mid-noise) can't count as an onset.
    if (!over) seeded = true;
    prev_over = over;
    return fired;
  }
};

}  // namespace mic
}  // namespace io
}  // namespace canary

#endif  // CANARY_IO_MIC_LOGIC_H
