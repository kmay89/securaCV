// firmware/common/story/story.h — the Canary's performance engine.
//
// WHAT THIS IS
//   The bird already has feelings (`care/bird_mood.h`), poses and one-shot
//   reactions (`ui/canary_mark.h`), a temperament (`ui/character.h`) and now a
//   light-language (`color/plumage.h`). What it has never had is a way to say
//   something ORDERED — a sequence of moments with timing, where the pose, the
//   words and the light are choreographed together instead of each drifting on
//   its own clock.
//
//   That is all this is: a beat sheet and a clock. A `Scene` is a list of
//   `Beat`s; a `Beat` is one moment — a pose for the bird, a gesture for the
//   light, a tone, and (optionally) a line of copy that types on. `StoryTeller`
//   walks them and answers one question per frame: what should be on stage
//   right now.
//
//   It lives in `firmware/common/` and knows nothing about LVGL, Arduino or
//   any particular glass, because the point is that the same performance runs
//   on the 1.47" stick, the 7" bedside glass, the watch and the phone. A
//   surface supplies a renderer; the character is shared.
//
// WHY A BEAT SHEET AND NOT A STATE MACHINE
//   Because the thing being built is a performance, and performances are
//   written, reviewed and rewritten as text. Keeping the script as data
//   (`story_scripts.h`) means the writing can be edited by someone who is not
//   editing the renderer, the timing is visible in one place, and the whole
//   arc is host-testable — which is the only way a joke about MAC addresses
//   ever gets to be the same joke twice.
//
// THE RULES IT ENFORCES (these are the product's, not this file's)
//   1. THE TRUTH PREEMPTS THE STORY. `interrupt()` on attention drops the
//      scene instantly and it does NOT resume. `display_living_canary.md`
//      rule 5: alarms get instrument-grade UI, bird-free. A device that
//      finished its charming sentence while a Tamper was live would be a toy.
//   2. THE SCRIPT MAY ONLY EVER SAY AMBIENT THINGS. Diagnosis, severity
//      words, link states and instructions are invariant copy owned by the
//      UI — exactly the line `ui/character.h`'s Voice already draws ("a
//      Character may rephrase contentment, never trouble"). A Scene is
//      contentment, charm and explanation. It is never a diagnosis.
//   3. THE USER OUTRANKS THE STORYBOARD. A tap completes the typing line;
//      the next tap moves on; a hold leaves. Nobody is ever trapped in a
//      cutscene.
//   4. NIGHT IS SACRED. Scenes marked `day_only` will not start during quiet
//      hours. A hallway lamp at 3 a.m. does not do bits.
//
// Pure integer logic, no allocation, no clock reads — the caller feeds `now`.
// Host-tested in firmware/tests_host/test_story.cpp.
#pragma once
#include <stdint.h>

namespace canary::story {

// What the bird is doing during a beat. These are requests to whatever mark
// engine is on this surface (`canary_mark_react`, a SwiftUI phase, a CSS
// class); the engine maps them, and may legitimately ignore one it has no
// pose for. Deliberately a small vocabulary — a big one is how a character
// stops being recognizable.
enum class Pose : uint8_t {
  Hold = 0,   // stay as you are
  Enter,      // hop onto the stage
  Settle,     // land, fold, breathe
  Tilt,       // the curious head-cock: "it sees you"
  Lean,       // lean IN — conspiratorial, the setup to a joke
  Preen,      // a small self-satisfied fuss
  Hop,        // a bright little hop
  Stretch,    // one slow high wing stretch
  Ruffle,     // the full-body shake: delight
  Look,       // glance off-stage, toward what is being talked about
  Alert,      // upright, still, watching — NOT a pose that speaks
  Exit,       // leave the stage to something more important
};

// What the light does during a beat, in the look engine's own terms
// (`color/look_engine.h` Motion + `color/plumage.h`). The renderer maps these
// onto whatever channels it has: the glass wash, the WS2812, a phone's accent.
enum class Gesture : uint8_t {
  None = 0,
  Breathe,    // the resting swell
  Rise,       // a band of light climbing — the plumage phrase
  Pulse,      // one deliberate beat, for a punchline
  Sweep,      // travel across the palette
  Narrow,     // pull IN and dim: the held breath before a reveal
  Warm,       // settle to the warm end — reassurance
  Bloom,      // open out and brighten: delight, or an arrival
};

// How a beat is meant to land. The renderer uses it for pacing and type
// weight; the light engine uses it to pick a speed. It is the closest thing
// here to stage direction.
enum class Tone : uint8_t {
  Calm = 0,
  Warm,
  Playful,
  Sly,        // the setup
  Earnest,    // the promise
  Bright,     // the payoff
};

// One moment.
//
// `line` is ambient copy or nullptr for a WORDLESS beat — and wordless beats
// are the whole reason this reads as a character rather than a slideshow. The
// first meeting opens on an empty stage and a bird that arrives and looks at
// you before it says anything, because presence before speech is what makes
// the speech land.
//
// A line may contain ONE `%s`, filled from `StoryTeller::subject()`. That is
// the device's own pseudonym and nothing else — see the note there.
struct Beat {
  const char* line;
  Pose pose;
  Gesture gesture;
  Tone tone;
  uint16_t hold_ms;   // how long to sit AFTER the line finishes typing
};

struct Scene {
  const char* id;
  const Beat* beats;
  uint8_t n;
  bool skippable;   // a tap advances (true for anything with words)
  bool day_only;    // never starts during quiet hours
};

// What the surface should have on stage this instant.
struct Frame {
  const Beat* beat = nullptr;  // nullptr = nothing playing
  uint8_t chars = 0;           // how much of `line` has typed on
  bool line_done = false;      // the line is fully typed
  bool scene_done = false;     // the last beat has finished
  uint8_t index = 0;           // which beat, for progress dots
  uint8_t count = 0;
};

class StoryTeller {
 public:
  // Typing speed. Slow enough to read at a glance, fast enough that a
  // four-line scene is not a hostage situation. Punctuation gets an extra
  // breath, which is most of what makes typed text feel spoken rather than
  // printed.
  static constexpr uint16_t kCharMs = 42;
  static constexpr uint16_t kPunctMs = 260;

  // Start a scene. Returns false (and changes nothing) if `night` and the
  // scene is day_only, or if a scene is already running.
  bool play(const Scene* s, uint32_t now_ms, bool night);

  // The truth preempting the story. Drops the scene immediately; it does NOT
  // resume, because a performance that picked up where it left off after an
  // alarm would be telling the user the alarm was an interruption to the
  // charm rather than the other way round.
  void interrupt();

  // The user outranks the storyboard: the first tap completes the typing
  // line, the next moves on, and past the last beat the scene ends.
  void tap(uint32_t now_ms);

  bool active() const { return scene_ != nullptr; }
  const Scene* scene() const { return scene_; }

  // What to render now. Also advances the beat clock, so call it every frame.
  Frame frame(uint32_t now_ms);

  // The ONE runtime substitution a line may carry (`%s`). It is the device's
  // pseudonym — `common/identity/device_pseudonym.h`, which reads no hardware
  // MAC — and that is not an arbitrary restriction: the only reason the
  // script is allowed to name anything is to make the point that this is the
  // only name it has.
  void set_subject(const char* s) { subject_ = s ? s : ""; }
  const char* subject() const { return subject_; }

  // Expand `beat->line` into `out` with `%s` filled. Returns the full
  // expanded length (which is what `chars` counts against), so a renderer can
  // type on the expanded string rather than the raw one — otherwise the
  // typing stalls on the literal "%s".
  uint16_t expand(const Beat* b, char* out, uint16_t cap) const;

 private:
  uint32_t beat_started_ms(uint32_t now) const;
  uint16_t typed_len(const Beat* b, uint32_t elapsed) const;
  uint16_t line_ms(const Beat* b) const;

  const Scene* scene_ = nullptr;
  uint8_t idx_ = 0;
  uint32_t started_ = 0;      // when the CURRENT beat started
  bool forced_done_ = false;  // a tap completed this beat's typing
  const char* subject_ = "";
};


// ── Implementation ────────────────────────────────────────────────────────
// Header-only, deliberately. `splash.cpp` plays a scene and compiles into
// roughly twenty PlatformIO envs, several of which do not resolve
// firmware/common as an LDF library and have to name every shared .cpp in
// their build_src_filter by hand (see the long note in
// envs/platformio/canary-display.ini). A pure model with no TU to link cannot
// be forgotten by one of them — which is exactly the failure mode the rest of
// the pure models here (lantern.h, hallway.h, ambient_life.h, boot_button.h)
// avoid the same way.

namespace detail {

inline bool is_punct(char c) {
  return c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':';
}

}  // namespace detail

// The expanded length of a line, with `%s` replaced by the subject. Kept in
// one place so the typing clock and the renderer can never disagree about how
// long the line is — a mismatch there shows up as text that finishes typing
// before it is finished, which reads as a glitch rather than as speech.
inline uint16_t StoryTeller::expand(const Beat* b, char* out, uint16_t cap) const {
  uint16_t w = 0;
  if (b == nullptr || b->line == nullptr) {
    if (out && cap) out[0] = '\0';
    return 0;
  }
  for (const char* p = b->line; *p; p++) {
    if (p[0] == '%' && p[1] == 's') {
      for (const char* q = subject_; q && *q; q++) {
        if (out && w + 1 < cap) out[w] = *q;
        w++;
      }
      p++;   // skip the 's'
      continue;
    }
    if (out && w + 1 < cap) out[w] = *p;
    w++;
  }
  if (out && cap) out[w < cap ? w : cap - 1] = '\0';
  return w;
}

// How long this beat's line takes to type, punctuation breaths included.
inline uint16_t StoryTeller::line_ms(const Beat* b) const {
  if (b == nullptr || b->line == nullptr) return 0;
  char buf[192];
  const uint16_t n = expand(b, buf, sizeof(buf));
  const uint16_t shown = n < sizeof(buf) - 1 ? n : (uint16_t)(sizeof(buf) - 1);
  uint32_t ms = (uint32_t)shown * kCharMs;
  for (uint16_t i = 0; i < shown; i++)
    if (detail::is_punct(buf[i])) ms += kPunctMs;
  return (uint16_t)(ms > 60000 ? 60000 : ms);
}

// How many characters have typed on at `elapsed` into the beat.
inline uint16_t StoryTeller::typed_len(const Beat* b, uint32_t elapsed) const {
  if (b == nullptr || b->line == nullptr) return 0;
  char buf[192];
  const uint16_t n = expand(b, buf, sizeof(buf));
  const uint16_t shown = n < sizeof(buf) - 1 ? n : (uint16_t)(sizeof(buf) - 1);
  uint32_t t = 0;
  for (uint16_t i = 0; i < shown; i++) {
    t += kCharMs;
    if (detail::is_punct(buf[i])) t += kPunctMs;
    if (elapsed < t) return i;   // this character has not landed yet
  }
  return n;
}

inline bool StoryTeller::play(const Scene* s, uint32_t now_ms, bool night) {
  if (s == nullptr || s->n == 0) return false;
  if (scene_ != nullptr) return false;         // never talk over yourself
  if (night && s->day_only) return false;      // night is sacred
  scene_ = s;
  idx_ = 0;
  started_ = now_ms;
  forced_done_ = false;
  return true;
}

inline void StoryTeller::interrupt() {
  scene_ = nullptr;
  idx_ = 0;
  forced_done_ = false;
}

inline void StoryTeller::tap(uint32_t now_ms) {
  if (scene_ == nullptr || !scene_->skippable) return;
  const Beat* b = &scene_->beats[idx_];

  // First tap completes the line rather than skipping it — the reader who
  // taps because they read faster than the typing should get the whole
  // sentence, not the next one.
  if (b->line != nullptr && !forced_done_ &&
      typed_len(b, now_ms - started_) < expand(b, nullptr, 0)) {
    forced_done_ = true;
    return;
  }

  if (idx_ + 1 >= scene_->n) {
    interrupt();
    return;
  }
  idx_++;
  started_ = now_ms;
  forced_done_ = false;
}

inline Frame StoryTeller::frame(uint32_t now_ms) {
  Frame f;
  if (scene_ == nullptr) return f;

  const Beat* b = &scene_->beats[idx_];
  const uint32_t elapsed = now_ms - started_;
  const uint16_t full = expand(b, nullptr, 0);
  const uint16_t typing = line_ms(b);

  const bool typed_out = forced_done_ || elapsed >= typing;
  const uint16_t chars = forced_done_ ? full : typed_len(b, elapsed);

  // The beat is over once the line has typed AND its hold has elapsed. A
  // forced (tapped) line still gets its hold, so a reader who taps ahead does
  // not get a scene that flickers past — they get it at reading speed.
  const uint32_t beat_ms =
      (uint32_t)(forced_done_ ? 0 : typing) + (uint32_t)b->hold_ms;
  const uint32_t since = forced_done_ ? elapsed - 0 : elapsed;

  if (typed_out && since >= beat_ms) {
    if (idx_ + 1 >= scene_->n) {
      f.beat = b;
      f.chars = (uint8_t)(full > 255 ? 255 : full);
      f.line_done = true;
      f.scene_done = true;
      f.index = idx_;
      f.count = scene_->n;
      interrupt();
      return f;
    }
    idx_++;
    started_ = now_ms;
    forced_done_ = false;
    b = &scene_->beats[idx_];
  }

  f.beat = b;
  f.chars = (uint8_t)(chars > 255 ? 255 : chars);
  f.line_done = typed_out;
  f.index = idx_;
  f.count = scene_->n;
  return f;
}

}  // namespace canary::story
