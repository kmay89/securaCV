// canary/companion/haptic_voice.h — how the companion speaks with a motor and a
// speaker, and what it does when it has neither. Pure, host-testable: this file
// picks WHAT to play; the HAL decides how.
//
// Design: docs/design/canary_companion.md §5.
//
// ── Honest degradation is the whole point of this file ───────────────────────
//
// The watch board's own registration is blunt about this
// (firmware/boards/waveshare-esp32s3-amoled206/README.md): there is no haptic
// motor anywhere on it and none in the vendor tree, so a DRV2605L + LRA on the
// exposed I²C port at 0x5A is required hardware for anything whose premise is a
// signal you feel. And `HAS_SPEAKER` is 1 only because the codec and the
// PA-enable line exist — whether a transducer is actually fitted varies by
// board revision, which is why the runtime probes instead of assuming.
//
// So this engine is built around a fact most firmware pretends away: **at
// authoring time we do not know which output channels exist.** A device that
// claims it buzzed and did not is exactly the surprise that leaves someone
// waiting for an answer that already arrived — which is the failure the
// project's own failure-semantics doc names as the worst kind.
//
// The contract here: every Utterance names channels it PREFERS, `voice_plan()`
// intersects that with what actually probed present, and the result always
// includes a channel that exists — falling back to the glass, which is the one
// output every one of these boards is guaranteed to have.

#ifndef CANARY_COMPANION_HAPTIC_VOICE_H
#define CANARY_COMPANION_HAPTIC_VOICE_H

#include <stdint.h>

namespace canary {
namespace companion {

// ── Channels ─────────────────────────────────────────────────────────────────

enum Channel : uint8_t {
  CH_NONE = 0,
  CH_HAPTIC = 1 << 0,  // DRV2605L + LRA, probed at 0x5A
  CH_SOUND = 1 << 1,   // ES8311 codec + PA enable, transducer probed
  CH_GLASS = 1 << 2,   // the panel itself: a flash, a pulse, a colour
};

// What the boot probe found. Everything defaults to absent, so a probe that
// never ran degrades to glass-only rather than to a confident lie.
struct VoiceCapabilities {
  bool haptic = false;
  bool sound = false;
  bool glass = true;  // the one channel that is always there
};

// ── The vocabulary ───────────────────────────────────────────────────────────
//
// Small on purpose. A device with forty distinct buzzes has taught its owner
// nothing; a device with six has taught them six things. Each entry below is a
// thing the wearer can learn to recognise without looking.

enum class Utterance : uint8_t {
  Tick = 0,      // a control acknowledged a touch — the smallest possible click
  Confirm,       // a setting committed, a care act landed
  Decline,       // the bird said no thank you (a full bird refusing seed)
  Greet,         // first wake of the day; the bird's hello
  Flourish,      // a rare earned moment (pet_model.h Bond milestones)
  WakeNudge,     // the gentle alarm's haptic phase
  Vigil,         // trouble in the fleet. The only utterance that may repeat.
};

// ── Night silence ────────────────────────────────────────────────────────────
//
// Night is sacred in this project (display_living_canary.md rule 3: "no
// flourishes, no sound, breath only"), and a wrist device makes that stricter
// rather than looser — it is in contact with a sleeping person.
//
// At night, sound is off entirely. Haptics survive for the two utterances that
// exist to reach a sleeping person on purpose: the wake alarm, and a real
// unacknowledged fleet problem. Everything else waits for morning.

inline bool utterance_may_sound_at_night(Utterance u) {
  (void)u;
  return false;  // nothing sounds at night. No exceptions, including Vigil.
}

inline bool utterance_may_buzz_at_night(Utterance u) {
  return u == Utterance::WakeNudge || u == Utterance::Vigil;
}

// ── Preferred channels ───────────────────────────────────────────────────────
//
// What each utterance WANTS, before reality is consulted.

inline uint8_t utterance_prefers(Utterance u) {
  switch (u) {
    case Utterance::Tick:      return CH_HAPTIC;
    case Utterance::Confirm:   return CH_HAPTIC | CH_GLASS;
    case Utterance::Decline:   return CH_GLASS;              // never a buzz — a
                                                             // refusal that buzzes
                                                             // reads as an error
    case Utterance::Greet:     return CH_HAPTIC | CH_GLASS;
    case Utterance::Flourish:  return CH_GLASS | CH_SOUND;
    case Utterance::WakeNudge: return CH_HAPTIC | CH_GLASS;
    case Utterance::Vigil:     return CH_HAPTIC | CH_SOUND | CH_GLASS;
  }
  return CH_GLASS;
}

// ── Intensity ────────────────────────────────────────────────────────────────
//
// 0..100. The DRV2605L's effect library is richer than this, but the mapping
// from "how strongly" to "which of 123 effects" belongs in the HAL where the
// motor's resonant frequency is known. This layer only says how much.

inline uint8_t utterance_strength(Utterance u) {
  switch (u) {
    case Utterance::Tick:      return 18;
    case Utterance::Confirm:   return 35;
    case Utterance::Decline:   return 0;
    case Utterance::Greet:     return 45;
    case Utterance::Flourish:  return 55;
    case Utterance::WakeNudge: return 60;
    case Utterance::Vigil:     return 80;
  }
  return 30;
}

// Night halves the strength of anything still permitted. A buzz calibrated for
// a wrist in a noisy kitchen is a startle against a sleeping wrist.
static constexpr uint8_t NIGHT_STRENGTH_NUM = 1;
static constexpr uint8_t NIGHT_STRENGTH_DEN = 2;

// ── The plan ─────────────────────────────────────────────────────────────────

struct VoicePlan {
  uint8_t channels = CH_NONE;  // what will ACTUALLY be driven
  uint8_t strength = 0;        // 0..100
  bool fell_back = false;      // a preferred channel was missing and we substituted
  bool suppressed = false;     // night rules removed a channel the utterance wanted
};

// Resolve an utterance against the hardware that actually exists and the hour.
//
// The invariant this function exists to hold: `plan.channels` never names a
// channel that is not present, and is never CH_NONE. Something always happens,
// and what happens is always something the device can really do.
inline VoicePlan voice_plan(Utterance u, const VoiceCapabilities& caps, bool night) {
  VoicePlan p;
  const uint8_t want = utterance_prefers(u);

  uint8_t got = CH_NONE;
  if ((want & CH_HAPTIC) && caps.haptic) {
    if (!night || utterance_may_buzz_at_night(u)) {
      got |= CH_HAPTIC;
    } else {
      p.suppressed = true;
    }
  }
  if ((want & CH_SOUND) && caps.sound) {
    if (!night || utterance_may_sound_at_night(u)) {
      got |= CH_SOUND;
    } else {
      p.suppressed = true;
    }
  }
  // The glass is subject to the night rule too, and this is the line people
  // get wrong: a "silent" flourish that still lights a 200-nit panel beside a
  // sleeping face is not silent. Night is sacred here (display_living_canary.md
  // rule 3: no flourishes, no sound, breath only), so a COURTESY utterance at
  // night gets no channel at all — not even a visual one.
  const bool courtesy_utterance =
      (u == Utterance::Tick || u == Utterance::Confirm || u == Utterance::Greet ||
       u == Utterance::Flourish);
  if ((want & CH_GLASS) && caps.glass) {
    if (!night || !courtesy_utterance) {
      got |= CH_GLASS;
    } else {
      p.suppressed = true;
    }
  }

  // Did a channel we wanted turn out not to exist? That is a fallback, and the
  // UI is expected to have SAID so at boot — "no motor fitted; knocks will be
  // seen, not felt" — rather than silently doing something else forever.
  if (((want & CH_HAPTIC) && !caps.haptic) || ((want & CH_SOUND) && !caps.sound)) {
    p.fell_back = true;
  }

  // The floor: never nothing. If night rules or missing hardware emptied the
  // plan, the glass carries it — except that at night the glass carrying a
  // Vigil is exactly right, and the glass carrying a Tick is not, so a
  // suppressed courtesy utterance is allowed to end up genuinely silent.
  if (got == CH_NONE) {
    if (!(night && courtesy_utterance) && caps.glass) got = CH_GLASS;
  }

  p.channels = got;
  p.strength = utterance_strength(u);
  if (night && p.strength > 0) {
    p.strength = static_cast<uint8_t>(p.strength * NIGHT_STRENGTH_NUM /
                                      NIGHT_STRENGTH_DEN);
  }
  return p;
}

// ── The boot line ────────────────────────────────────────────────────────────
//
// What the device SAYS about itself at first boot, so the gap between what it
// can do and what the wearer expects is closed on day one rather than during
// the first missed signal. Returns nullptr when everything is fitted and there
// is nothing to confess.
inline const char* voice_boot_note(const VoiceCapabilities& caps) {
  if (!caps.haptic && !caps.sound) {
    return "No motor and no speaker fitted — everything will be seen, not felt or heard.";
  }
  if (!caps.haptic) {
    return "No motor fitted — nudges will be seen and heard, not felt.";
  }
  if (!caps.sound) {
    return "No speaker fitted — nudges will be felt and seen, not heard.";
  }
  return nullptr;
}

}  // namespace companion
}  // namespace canary

#endif  // CANARY_COMPANION_HAPTIC_VOICE_H
