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

// ── Envelope hysteresis ─────────────────────────────────────────────────────
// RMS scalar in, loud/quiet out. Two thresholds so hum near one level can't
// chatter edges into the cadence detector (the WAP's on/off pattern).

struct Envelope {
  uint16_t on_threshold = 900;
  uint16_t off_threshold = 600;
  bool loud = false;

  bool update(uint16_t rms) {
    if (!loud && rms >= on_threshold) loud = true;
    else if (loud && rms < off_threshold) loud = false;
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
  // Beep-duration windows (ms), generous for room acoustics + DSP latency.
  static constexpr uint16_t T3_BEEP_MIN = 300, T3_BEEP_MAX = 900;
  static constexpr uint16_t T4_BEEP_MIN = 40, T4_BEEP_MAX = 280;
  // A gap this long ends the current beep group.
  static constexpr uint16_t GROUP_GAP_MS = 1100;
  // A silence this long means the alarm stopped; cycles reset.
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

}  // namespace mic
}  // namespace io
}  // namespace canary

#endif  // CANARY_IO_MIC_LOGIC_H
