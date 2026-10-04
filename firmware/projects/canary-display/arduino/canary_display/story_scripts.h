// firmware/common/story/story_scripts.h — the Canary's scripts.
//
// The engine (story.h) is a beat sheet and a clock. This is the writing.
// It is kept as data, in one file, on purpose: the arc should be readable and
// editable as a script, by someone who is not editing a renderer.
//
// ── THE HOUSE RULES FOR ANYTHING WRITTEN HERE ────────────────────────────
//
//  1. IT MUST BE TRUE. This is the whole reason the character works. The bird
//     is a gauge (`display_living_canary.md` rule 1) and its charm is only
//     charming while every claim it makes is one a log line could back. The
//     joke below lands *because* it is literally true: `device_pseudonym.h`
//     reads no hardware MAC, by construction, with the include deliberately
//     absent. If that ever stopped being true the joke would have to go — and
//     that is the correct incentive.
//
//  2. AMBIENT ONLY, NEVER DIAGNOSIS. Severity words, link states, event copy
//     and every instruction stay invariant and belong to the UI. Nothing here
//     may tell a user what is wrong or what to do about it. Charm, warmth and
//     explanation only — the same line `ui/character.h`'s Voice already draws.
//
//  3. NO FALSE MODESTY AND NO SWAGGER. The bird is competent and warm. It can
//     be pleased with itself for a beat; it does not brag, and it never
//     performs concern it does not have.
//
//  4. SHORT LINES. This is read at a glance, at three meters, on a 172 px
//     wide pane. Two clauses is a long sentence here.
//
//  5. PLAIN ASCII. The built-in Montserrat tables carry no emoji, and the
//     glance contract wants words that read across a room, not glyphs.
#pragma once
#include "story.h"

namespace canary::story {

// ── THE FIRST MEETING ─────────────────────────────────────────────────────
//
// The arc, and why it is shaped like this:
//
//   PRESENCE, then speech. It opens on an empty stage. The bird arrives,
//   settles, and LOOKS at you before a word is typed. This is the single
//   most important thing in the scene and it costs two silent beats.
//
//   THE SETUP. It leans in and offers to tell you what it noticed about you.
//   Every instinct built by every other device in this category says the next
//   line is going to be impressive and slightly threatening. The light pulls
//   in and dims — the held breath.
//
//   THE PUNCHLINE IS "NOTHING." Then ONE itemized line of the nothing — the
//   specific things it went out of its way not to learn are exactly the
//   things everything else in your house collects first — and one flex: it
//   could have, everything else in this house did.
//
//   THE WALK-BACK. One warm line doing double duty: the joke's button, and
//   the handover of the only name it has — a pseudonym it made up itself.
//
//   THEN THE ACTUAL JOB. The color language in one line, the fleet in one —
//   the part the user needs to retain, landing right after the laugh.
//
//   THE PROMISE, last, in the bird's own voice, because it is the bird's to
//   keep.
//
//   Each of those things is said exactly once. A first meeting is charming
//   in inverse proportion to how long it holds the stage: the chuckle, the
//   language, the promise — then hand the glass back.
inline constexpr Beat kHelloBeats[] = {
    // Presence before speech.
    {nullptr, Pose::Enter,  Gesture::Bloom,   Tone::Bright,  520},
    {nullptr, Pose::Settle, Gesture::Breathe, Tone::Calm,    420},
    {nullptr, Pose::Tilt,   Gesture::None,    Tone::Warm,    680},

    {"Oh! Hello.",
     Pose::Hop,    Gesture::Bloom,   Tone::Bright,  520},
    {"I'm your canary.",
     Pose::Settle, Gesture::Breathe, Tone::Warm,    700},

    // The setup.
    {"Want to know what I worked out about you?",
     Pose::Lean,   Gesture::Narrow,  Tone::Sly,     700},
    {nullptr, Pose::Hold, Gesture::Narrow, Tone::Sly, 900},

    // The punchline.
    {"Nothing.",
     Pose::Tilt,   Gesture::Pulse,   Tone::Playful, 800},
    {"No MAC, no IP, no idea who's on your wifi.",
     Pose::Preen,  Gesture::Rise,    Tone::Playful, 850},
    {"I could have. Everything else in this house did.",
     Pose::Look,   Gesture::Sweep,   Tone::Sly,     900},

    // The walk-back.
    {"Not that kind of bird. Call me %s.",
     Pose::Ruffle, Gesture::Warm,    Tone::Warm,    900},

    // The actual job.
    {"Dark means all is well. If I glow, look at me.",
     Pose::Tilt,   Gesture::Pulse,   Tone::Earnest, 950},
    {"Add another of me and we compare notes.",
     Pose::Hop,    Gesture::Bloom,   Tone::Bright,  850},

    // The promise.
    {"What I see stays here. That's a promise, not a setting.",
     Pose::Settle, Gesture::Warm,    Tone::Earnest, 1100},
};

inline constexpr Scene kHello = {
    "hello", kHelloBeats, (uint8_t)(sizeof(kHelloBeats) / sizeof(Beat)),
    /*skippable=*/true, /*day_only=*/false};

// ── THE RETURNING HELLO ───────────────────────────────────────────────────
// Every later boot. `display_living_canary.md`: a returning boot must never
// feel slower than home. Three beats, one of them silent.
inline constexpr Beat kHelloAgainBeats[] = {
    {nullptr, Pose::Enter,  Gesture::Bloom,   Tone::Bright, 380},
    {"Hello again.",
     Pose::Tilt,   Gesture::Breathe, Tone::Warm,   600},
    {nullptr, Pose::Settle, Gesture::Breathe, Tone::Calm,   300},
};
inline constexpr Scene kHelloAgain = {
    "hello_again", kHelloAgainBeats,
    (uint8_t)(sizeof(kHelloAgainBeats) / sizeof(Beat)),
    /*skippable=*/true, /*day_only=*/false};

// ── IDLE VIGNETTES ────────────────────────────────────────────────────────
// The Flipper property: a device that is plugged in and idle should
// occasionally be caught DOING something. These are one- and two-beat scenes,
// released by `care/ambient_life.h`'s ration — minutes apart, and only onto a
// calm, lit, idle glass. They are day_only: a hallway lamp at 3 a.m. does not
// do bits (`display_living_canary.md` rule 3, night is sacred).
//
// Most are wordless. The bird being visibly alive is the point; a device that
// talks every four minutes is a device that gets unplugged.

inline constexpr Beat kIdlePreenBeats[] = {
    {nullptr, Pose::Preen, Gesture::Breathe, Tone::Calm, 900},
    {nullptr, Pose::Settle, Gesture::Breathe, Tone::Calm, 400},
};
inline constexpr Scene kIdlePreen = {
    "idle_preen", kIdlePreenBeats, 2, false, true};

inline constexpr Beat kIdleGlanceBeats[] = {
    {nullptr, Pose::Look, Gesture::Sweep, Tone::Calm, 700},
    {nullptr, Pose::Settle, Gesture::Breathe, Tone::Calm, 400},
};
inline constexpr Scene kIdleGlance = {
    "idle_glance", kIdleGlanceBeats, 2, false, true};

inline constexpr Beat kIdleCheckBeats[] = {
    {"Checking on the others.",
     Pose::Look, Gesture::Rise, Tone::Calm, 1400},
    {nullptr, Pose::Settle, Gesture::Breathe, Tone::Calm, 400},
};
inline constexpr Scene kIdleCheck = {
    "idle_check", kIdleCheckBeats, 2, false, true};

inline constexpr Beat kIdleQuietBeats[] = {
    {"All quiet. Carry on.",
     Pose::Settle, Gesture::Warm, Tone::Warm, 1500},
};
inline constexpr Scene kIdleQuiet = {"idle_quiet", kIdleQuietBeats, 1, false, true};

inline constexpr Beat kIdleStillHereBeats[] = {
    {nullptr, Pose::Stretch, Gesture::Breathe, Tone::Calm, 800},
    {"Still here.", Pose::Tilt, Gesture::Warm, Tone::Warm, 1200},
};
inline constexpr Scene kIdleStillHere = {
    "idle_still_here", kIdleStillHereBeats, 2, false, true};

// The rare one. Gated on trust (a long-healthy system is VISIBLY different
// from a day-one system — `display_living_canary.md`, and trust is earned by
// security hygiene, not by grinding).
inline constexpr Beat kIdleProudBeats[] = {
    {nullptr, Pose::Ruffle, Gesture::Bloom, Tone::Bright, 700},
    {"Quiet week. I'll take it.",
     Pose::Preen, Gesture::Warm, Tone::Playful, 1400},
};
inline constexpr Scene kIdleProud = {
    "idle_proud", kIdleProudBeats, 2, false, true};

// The idle pool, in the order a caller should weight them: the wordless ones
// are common, the speaking ones rare. `story_pick_idle()` below does the
// weighting so every surface rations them the same way.
inline constexpr const Scene* kIdlePool[] = {
    &kIdlePreen, &kIdleGlance, &kIdleStillHere,
    &kIdleCheck, &kIdleQuiet,  &kIdleProud,
};
inline constexpr uint8_t kIdlePoolCount =
    (uint8_t)(sizeof(kIdlePool) / sizeof(kIdlePool[0]));

// Pick an idle vignette. `r` is any per-device random draw (the caller's
// seeded xorshift — same one `ambient_life` uses, so two Canaries on one
// shelf do not perform the same bit at the same moment). `trust_days` gates
// the rare one.
//
// The weighting is the point: two thirds of the time the bird does something
// WORDLESS. A device that speaks every time you look at it stops being
// company and starts being a chatbot on your wall.
inline const Scene* story_pick_idle(uint32_t r, uint16_t trust_days) {
  const uint32_t roll = r % 100u;
  if (roll < 30) return &kIdlePreen;
  if (roll < 55) return &kIdleGlance;
  if (roll < 70) return &kIdleStillHere;
  if (roll < 84) return &kIdleCheck;
  if (roll < 96) return &kIdleQuiet;
  return trust_days >= 7 ? &kIdleProud : &kIdlePreen;
}

// ── THE APPROACH TO AN ALARM ──────────────────────────────────────────────
//
// The one place personality and honesty could genuinely fight, so it is worth
// being exact about how they are separated.
//
// `display_living_canary.md` rule 5 is not negotiable: alarms get
// instrument-grade UI, bird-free. The bird does not stand in front of a live
// alert making it charming, and nothing in this file will ever be shown on an
// alarm screen.
//
// What has character is the APPROACH — the second and a half between "the
// glass is calm" and "the glass is an instrument". Cutting straight from a
// warm idle lamp to a red panel is a jump scare, and a jump scare is a device
// people unplug. So the bird gets one beat to change posture and the light
// gets one beat to pull in, and then the bird LEAVES and the truth owns the
// stage.
//
// That is the whole scene: two beats, no words, and an exit. The words on an
// alarm belong to the UI, which knows what is actually wrong.
inline constexpr Beat kAlertApproachBeats[] = {
    // Upright, still, watching. The posture change alone is legible across a
    // room and costs nothing in credibility.
    {nullptr, Pose::Alert, Gesture::Narrow, Tone::Calm, 420},
    // ...and off, handing the glass to the instrument.
    {nullptr, Pose::Exit,  Gesture::Pulse,  Tone::Calm, 260},
};
inline constexpr Scene kAlertApproach = {
    "alert_approach", kAlertApproachBeats, 2,
    /*skippable=*/false,   // you may not tap past the truth arriving
    /*day_only=*/false};   // and it runs at 3 a.m. exactly as it does at noon

}  // namespace canary::story
