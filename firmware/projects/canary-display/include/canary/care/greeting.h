// include/canary/care/greeting.h — the first-meet story, pure model.
//
// The glass already introduces itself: splash.cpp plays the first-meeting
// scene (story::kHello in firmware/common/story/story_scripts.h) off the
// same NVS "scv-hello" bit — presence before speech, a handful of typed
// lines in a speech bubble, then the wordmark, so the name lands after
// the friend does. That is shipped and this file does NOT replace it.
//
// What this adds is the part that script has no way to say, plus the
// guardrails it has no way to enforce:
//
//   * Two beats it lacks — the self-identification flex and the fleet-as-
//     company line (the "there are more of us" idea, introduced as company
//     rather than as coverage).
//   * The honesty rules, mechanical instead of remembered. The splash
//     script is tested data now (test_story.cpp bans severity words and
//     glyphs Montserrat can't draw), but these runtime-fact lines never
//     pass through it — here THEIR copy is the same kind of tested surface.
//
// NOTE ON WIRING: this model is deliberately not called from splash yet.
// splash_play() runs before the face is built, so on a true first boot
// there is no Wi-Fi association — no IP and no fleet count to recite. The
// self-identification beat belongs at the onboarding success moment
// (onboard_ui.h's ObStage::Success), the first instant those facts exist.
// Converting the storyboard to render from this model is a follow-up
// against real display flavors.
//
// The arc, deliberately shaped like a person being introduced rather than a
// product demoing features:
//
//   Hello      a beat of welcome, nothing asked of you
//   Introduce  the show-off: it recites its own salted Hardware ID and its
//              address, precisely, because it CAN and is a little proud of
//              it — never the raw MAC (Invariant III / F-03)
//   Nickname   the walk-back — that was a lot, call me <pseudonym>. The
//              joke is at its own expense, which is the only kind a device
//              in your bedroom should ever make
//   Purpose    what it does, in the shortest honest words we have
//   Fleet      it is not alone (or it is, and says so) — the network idea,
//              introduced as company rather than as coverage
//   Settle     hands the stage to the normal glass and never returns
//
// THE RULES IT CANNOT BREAK (they are enforced here, not by the caller
// remembering):
//
//   1. First meet only. A device you've lived with does not re-introduce
//      itself; splash.cpp already persists "have we met" and passes it in.
//   2. It never runs over trouble, and it ABORTS if trouble arrives
//      mid-story. Charm during a real alarm is the one thing the character
//      system has always refused (bird_mood.h's "never cute during a real
//      alarm"), and an introduction is the cutest thing there is.
//   3. It words no alarm, ever. Every line here is ambient copy under the
//      Voice rule (character.h §6): a Character may rephrase contentment,
//      never trouble. Nothing in this file describes a severity, a badge,
//      or a link state.
//   4. Plain ASCII — the built-in Montserrat tables carry no emoji, and
//      these lines must read at 3 m like everything else on the glass.
//
// Deterministic and Arduino-free (time is injected) so the whole arc is
// host-tested: the beats, the dwell, the abort, and the copy.
#pragma once
#include <stdint.h>
#include <stdio.h>

namespace canary::care {

enum class GreetBeat : uint8_t {
  None = 0,   // not running
  Hello,
  Introduce,
  Nickname,
  Purpose,
  Fleet,
  Settle,     // the hand-off beat: caller cross-fades to the normal UI
  Done,       // finished or aborted; never runs again this boot
};

// What the device knows about itself when it introduces itself. All borrowed
// pointers owned by the caller (NVS/WiFi statics), all optional: a missing
// fact degrades the line instead of printing a hole.
struct SelfIntro {
  // The SALTED, MAC-free handle from device_pseudonym.h — the same
  // "Hardware ID" the boot banner shows. Never the raw hardware MAC:
  // firmware MUST NOT surface it (Invariant III / F-03), and the pseudonym
  // header goes so far as to not include esp_mac.h at all. That rule wins
  // over the joke, and costs it nothing — a salted hex handle is exactly as
  // gloriously unmemorable as a MAC, which is the entire gag.
  const char* hardware_id = nullptr;   // "3f7a9e21b4c5d6e8"
  const char* ip = nullptr;            // "192.168.1.44" — LAN-local, not a global ID
  const char* nickname = nullptr;      // the friendly name — "Pip"
  uint8_t fleet_size = 0;              // Canaries it can see, including itself
};

class Greeting {
 public:
  // Dwell per beat. Long enough to read at a glance and across a room,
  // short enough that the whole introduction is over in about fifteen
  // seconds — a story, not a splash screen someone has to wait out.
  static constexpr uint32_t HELLO_MS     = 1800;
  static constexpr uint32_t INTRODUCE_MS = 3200;   // the longest: it's a mouthful on purpose
  static constexpr uint32_t NICKNAME_MS  = 2600;
  static constexpr uint32_t PURPOSE_MS   = 2800;
  static constexpr uint32_t FLEET_MS     = 3000;
  static constexpr uint32_t SETTLE_MS    = 600;

  // Offer the story. Returns true if it started. `met_before` comes from the
  // persisted hello bit; `calm` is the caller's honesty gate (a quiet fleet
  // and healthy links — the same gate ambient life uses).
  bool begin(uint32_t now_ms, bool met_before, bool calm) {
    // Once only, per boot. Done means done — whether it finished, was
    // skipped, or was aborted by trouble. Without this guard a caller that
    // re-offers the story (a re-entered splash, a reconnect) could restart
    // an introduction the fleet had already interrupted, which is exactly
    // the "never cute during a real alarm" rule sneaking back in through
    // the front door.
    if (beat_ != GreetBeat::None) return false;
    if (met_before || !calm) {
      beat_ = GreetBeat::Done;   // a device you know doesn't re-introduce itself
      return false;
    }
    beat_ = GreetBeat::Hello;
    since_ms_ = now_ms;
    return true;
  }

  // Advance. `calm` is re-checked EVERY tick, not just at the start: if the
  // fleet needs the stage part-way through the introduction, the story stops
  // where it is. Trouble outranks charm, always.
  GreetBeat step(uint32_t now_ms, bool calm) {
    if (beat_ == GreetBeat::None || beat_ == GreetBeat::Done) return beat_;
    if (!calm) {                      // rule 2 — abort, do not resume
      beat_ = GreetBeat::Done;
      return beat_;
    }
    const uint32_t dwell = dwell_for(beat_);
    if ((uint32_t)(now_ms - since_ms_) < dwell) return beat_;
    since_ms_ = now_ms;
    beat_ = next_after(beat_);
    return beat_;
  }

  // True while the story owns the screen — the caller suppresses its normal
  // render and the ambient-life scheduler for exactly this long.
  bool running() const {
    return beat_ != GreetBeat::None && beat_ != GreetBeat::Done;
  }

  GreetBeat beat() const { return beat_; }

  // Let a tap end it. An introduction you can't skip is a cutscene, and a
  // device that traps you is not the one you want by your bed.
  void skip() { beat_ = GreetBeat::Done; }

  // The line for a beat, composed into the caller's buffer. Returns the
  // buffer for convenient inline use; writes an empty string for beats that
  // carry no copy (Settle/Done/None).
  //
  // Every string below is ambient copy: welcome, self-description, and
  // company. None of it names a severity, and none of it can be reached
  // while anything is wrong (step() aborts first).
  static const char* line(GreetBeat beat, const SelfIntro& me,
                          char* out, size_t cap) {
    if (!out || cap == 0) return "";
    out[0] = '\0';
    switch (beat) {
      case GreetBeat::Hello:
        snprintf(out, cap, "Hello.");
        break;
      case GreetBeat::Introduce:
        // The show-off beat. Over-precise on purpose — this is the device
        // proving it knows exactly what it is before it asks to be trusted.
        if (me.hardware_id && me.ip) {
          snprintf(out, cap, "I am %s, at %s.", me.hardware_id, me.ip);
        } else if (me.hardware_id) {
          snprintf(out, cap, "I am %s.", me.hardware_id);
        } else {
          snprintf(out, cap, "I am one of the quiet ones.");
        }
        break;
      case GreetBeat::Nickname:
        // The walk-back: the joke lands on itself, never on the user.
        if (me.nickname && me.nickname[0]) {
          snprintf(out, cap, "That's a lot. Call me %s.", me.nickname);
        } else {
          snprintf(out, cap, "That's a lot. Just call me your Canary.");
        }
        break;
      case GreetBeat::Purpose:
        // The invariant, said as charm. It is the whole product in six
        // words and it happens to be literally true.
        snprintf(out, cap, "I watch. I never record.");
        break;
      case GreetBeat::Fleet:
        // Company, not coverage. One device says so honestly rather than
        // implying a network it doesn't have.
        if (me.fleet_size > 1) {
          snprintf(out, cap, "There are %u of us. We keep watch together.",
                   (unsigned)me.fleet_size);
        } else {
          snprintf(out, cap, "It's just me so far. I'll keep watch.");
        }
        break;
      case GreetBeat::Settle:
      case GreetBeat::Done:
      case GreetBeat::None:
        break;
    }
    return out;
  }

  // Total run time of the story, for a caller that wants to size a timeout.
  static constexpr uint32_t total_ms() {
    return HELLO_MS + INTRODUCE_MS + NICKNAME_MS + PURPOSE_MS + FLEET_MS + SETTLE_MS;
  }

 private:
  static uint32_t dwell_for(GreetBeat b) {
    switch (b) {
      case GreetBeat::Hello:     return HELLO_MS;
      case GreetBeat::Introduce: return INTRODUCE_MS;
      case GreetBeat::Nickname:  return NICKNAME_MS;
      case GreetBeat::Purpose:   return PURPOSE_MS;
      case GreetBeat::Fleet:     return FLEET_MS;
      case GreetBeat::Settle:    return SETTLE_MS;
      default:                   return 0;
    }
  }

  static GreetBeat next_after(GreetBeat b) {
    switch (b) {
      case GreetBeat::Hello:     return GreetBeat::Introduce;
      case GreetBeat::Introduce: return GreetBeat::Nickname;
      case GreetBeat::Nickname:  return GreetBeat::Purpose;
      case GreetBeat::Purpose:   return GreetBeat::Fleet;
      case GreetBeat::Fleet:     return GreetBeat::Settle;
      case GreetBeat::Settle:    return GreetBeat::Done;
      default:                   return GreetBeat::Done;
    }
  }

  GreetBeat beat_ = GreetBeat::None;
  uint32_t since_ms_ = 0;
};

}  // namespace canary::care
