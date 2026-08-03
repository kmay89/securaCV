// firmware/common/color/plumage.h — the plumage: birdsong rendered as light.
//
// The look engine (look_engine.h) gives the lamp a *palette*. Plumage gives it
// a *voice*. It is the layer that makes a hallway nightlight read as a living
// thing rather than a color fade: the lamp rests, and every so often it says
// something — a short phrase of light built the way birdsong is built, out of
// syllables.
//
// THE LANGUAGE
//   Real song is phrases of syllables, and the syllable types are what make a
//   species recognizable. We render five, mapping the two things a note has
//   (pitch and amplitude) onto the two things light has (hue and brightness):
//
//     Chirp    a fast rise and a quick decay — one bright point, gone
//     Trill    the same note repeated fast, a shimmer inside one syllable
//     Warble   pitch wanders while the amplitude stays gentle — the lyrical one
//     Whistle  slow in, long hold, slow out — the calm, carrying note
//     Churr    low and rough, almost muttered; the bird talking to itself
//     Rest     silence between syllables — in a LAMP this is the resting glow,
//              never darkness (a hallway light that blinks out is a fault, not
//              a phrase)
//
//   PITCH selects a position along the active scene's palette, so a "high"
//   note is further around the same palette the lamp is already wearing — the
//   song can never introduce a color the scene does not own. AMPLITUDE drives
//   an envelope that is *added* to the resting lamp, so the light only ever
//   swells; it does not chop.
//
//   On the tall 172x320 glass a syllable is a BAND THAT RISES: the note's glow
//   travels bottom-to-top over its duration, so a phrase reads as light moving
//   up the pane the way a call rises. That is the pattern — motion with a
//   grammar behind it, not a scrolling gradient.
//
// THE PERSONALITY
//   `Voice` is four traits — chatter, boldness, pitch, restlessness — derived
//   from the device id, so every Canary speaks a little differently and two on
//   one hallway never fall into lockstep, while each stays exactly reproducible
//   across its own reboots. Same trick, and same reason, as `ambient_life`'s
//   seeded cadence: reproducible is what makes it host-testable.
//
// THE HONESTY INVARIANT (the reason this file delegates instead of deciding)
//   Plumage may only ever dress the CALM. It is a lantern layer: its light is
//   scene light, never semantic light, so it can never say "safe" by glowing.
//   Rather than restate that rule and risk it drifting, the two render entry
//   points below hand `worst >= Warn` and `safe_dark` straight to
//   look_engine's `led_color()` / `wash_stops()` — the song simply does not
//   exist on those paths. There is no way to call plumage and get a decorated
//   alarm, because the decoration code never runs.
//
// Integer / fixed-point throughout, like the rest of firmware/common/color:
// the C6 has no FPU and this runs every loop pass. Host-tested in
// firmware/tests_host/test_plumage.cpp.
#pragma once
#include <stdint.h>
#include "color/look_engine.h"

namespace canary::color {

// The syllables of the light-language. Rest is the resting glow between
// syllables, not darkness.
enum class Syllable : uint8_t {
  Rest = 0,
  Chirp,
  Trill,
  Warble,
  Whistle,
  Churr,
};

// A device's personality, all 0..255. Derived from the device id; stable for
// the life of the device.
struct Voice {
  uint8_t chatter;       // how often it speaks (gap between phrases)
  uint8_t boldness;      // how far the song swells above the resting glow
  uint8_t pitch;         // where in the palette this bird's voice sits
  uint8_t restlessness;  // how far pitch wanders inside a phrase
};

// Derive the voice. Pure and total: any id yields traits in a musical middle
// band (never a mute bird, never a strobe), so a bad id cannot produce an
// unpleasant device.
Voice voice_from_id(uint32_t device_id);

// One syllable of a phrase.
struct Note {
  Syllable kind;
  uint8_t pitch;   // palette position 0..255
  uint8_t amp;     // peak swell 0..255 (before boldness/depth scaling)
  uint16_t dur_ms;
};

// A phrase is short by design — a bird says a thing and stops.
inline constexpr uint8_t kMaxNotes = 6;

struct Phrase {
  Note notes[kMaxNotes];
  uint8_t n = 0;
  uint32_t total_ms = 0;
};

// The song: schedules phrases, and answers "what is the lamp saying right
// now" as a (pitch, amp) pair. Owns no color — the render entry points below
// turn its two numbers into light using the active scene.
class Plumage {
 public:
  // How long a newly-lit (or newly-ungated) lamp collects itself before it
  // says anything, and the shortest gap the scheduler will ever place between
  // two phrases. Exposed because the host tests assert against them.
  static constexpr uint32_t kSettleMs = 8000;
  static constexpr uint32_t kMinGapMs = 22000;

  // Seed the personality and the phrase clock. Safe to call again (re-seeds).
  void begin(uint32_t device_id, uint32_t now_ms);

  const Voice& voice() const { return voice_; }

  // Advance the clock. `gated` shuts the mouth: while it is true no phrase
  // starts and the clock simply HOLDS, so re-opening the gate never releases
  // a stored-up burst of speech (the ambient_life rule — a lamp that has been
  // quiet for an hour must not make up for it). A phrase already in flight
  // when the gate shuts is allowed to finish its sentence rather than being
  // cut off mid-syllable, which reads as a glitch.
  // `night` lengthens the gaps: a hallway lamp stirs, it does not perform.
  void tick(uint32_t now_ms, bool night, bool gated);

  // True while a phrase is in flight.
  bool speaking(uint32_t now_ms) const;

  // The whole language in two numbers, for this instant.
  //   `pitch` — palette position 0..255 (the note's color)
  //   `amp`   — swell above the resting glow, 0..255 (0 = resting)
  // Returns {voice pitch, 0} when silent, so a caller that ignores
  // `speaking()` still gets a sane resting lamp.
  void sample(uint32_t now_ms, uint8_t* pitch, uint8_t* amp) const;

  // Where the current syllable's glow sits on a vertical glass: 0 = bottom,
  // 255 = top. It RISES across the syllable, which is what makes a phrase
  // read as light climbing the pane. Meaningless (returns 0) while silent.
  uint8_t head(uint32_t now_ms) const;

  // Introspection for tests and the settings surface.
  const Phrase& phrase() const { return phrase_; }
  uint32_t next_phrase_at() const { return next_at_; }

 private:
  void compose(uint32_t now_ms, bool night);
  // The note covering `elapsed` and how far into it (0..255); nullptr in a
  // rest. One walk, so sample() and head() cannot disagree about where a
  // syllable starts.
  const Note* locate(uint32_t elapsed, uint8_t* progress) const;
  uint32_t rnd();
  uint8_t rnd8(uint8_t lo, uint8_t hi);   // inclusive

  Voice voice_{};
  Phrase phrase_{};
  uint32_t seed_ = 1;
  uint32_t start_ms_ = 0;   // current phrase start
  uint32_t next_at_ = 0;    // when the next phrase is due
  bool seeded_ = false;
};

// ── Render ─────────────────────────────────────────────────────────────────
// Both entry points take the same (worst, safe_dark) the look engine does and
// hand those cases straight back to it, so the honest override is the SAME
// code path it has always been — see the invariant note at the top.
//
// `depth` scales how far the song may swell the lamp (0..255). The hallway
// preset keeps it low at night on purpose.

// The beacon color for this instant, with the song on top of the scene.
Rgb plumage_led(uint32_t now_ms, const LookParams& p, Sev worst, bool safe_dark,
                const Plumage& song, uint8_t depth);

// The glass field: `count` bands top->bottom. The resting field is the look
// engine's wash; the note rides it as a band of light traveling bottom-to-top
// over the syllable, added (never subtracted), so the lamp cannot dip.
void plumage_bands(uint32_t now_ms, const LookParams& p, Sev worst,
                   bool safe_dark, const Plumage& song, uint8_t depth,
                   Rgb* out, uint8_t count);

}  // namespace canary::color
