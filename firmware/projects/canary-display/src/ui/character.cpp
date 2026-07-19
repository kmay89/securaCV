// src/ui/character.cpp — the Character table + active-look state. See
// character.h and docs/hardware/display_character.md.
//
// Pure data and a few lookups. Palettes carry computed contrast ratios in
// comments (WCAG 1.4.3: body text >= 4.5:1; Heirloom targets AAA) so a
// future edit can't quietly regress one.
#include <config.h>

#include "canary/ui/character.h"
#include "canary/ui/canary_mark.h"

namespace canary::ui {

namespace {

// Type ladders, per-flavor for the same bench finding theme.h's scale
// was: the dash reads from across a room, so it runs larger per role.
// Every size referenced here is enabled in lv_conf.h ({12,14,16,20,24,
// 28,36,48}). Heirloom steps one enabled size UP per role — the "large
// print" edition (display_character.md §4). The default ladder doubles
// for Neon's "compact": there is no smaller enabled font, and tighter
// than default would break the glance contract anyway.
#ifdef CD_FLAVOR_DASH
constexpr TypeLadder k_ladder_default = {
    &lv_font_montserrat_48,   // hero
    &lv_font_montserrat_36,   // title
    &lv_font_montserrat_24,   // body
    &lv_font_montserrat_20,   // label
    &lv_font_montserrat_16,   // caption
    &lv_font_montserrat_28,   // clock
};
constexpr TypeLadder k_ladder_heirloom = {
    &lv_font_montserrat_48,   // hero (48 is the enabled ceiling)
    &lv_font_montserrat_36,   // title
    &lv_font_montserrat_28,   // body
    &lv_font_montserrat_24,   // label
    &lv_font_montserrat_20,   // caption
    &lv_font_montserrat_36,   // clock
};
#else
constexpr TypeLadder k_ladder_default = {
    &lv_font_montserrat_48,   // hero
    &lv_font_montserrat_28,   // title
    &lv_font_montserrat_16,   // body
    &lv_font_montserrat_14,   // label
    &lv_font_montserrat_12,   // caption
    &lv_font_montserrat_20,   // clock
};
constexpr TypeLadder k_ladder_heirloom = {
    &lv_font_montserrat_48,   // hero (48 is the enabled ceiling)
    &lv_font_montserrat_36,   // title
    &lv_font_montserrat_20,   // body
    &lv_font_montserrat_16,   // label
    &lv_font_montserrat_14,   // caption
    &lv_font_montserrat_24,   // clock
};
#endif

// Indexed by (uint8_t)Character — QuietGlass first because it is enum 0,
// the default, and the safe fallback.
constexpr CharacterDef k_defs[(uint8_t)Character::Count] = {
    // [QuietGlass] — glass (2010s minimal): byte-for-byte today's look.
    {"Quiet Glass", "calm & minimal • the default",
     {
         0x000000,  // bg — true black: bezels disappear, night floors go low
         0x141414,  // surface — card/sheet ground
         0x262626,  // edge — hairline borders
         0xEDEDED,  // text — primary ink (~17.9:1 on bg)
         0x8A8A8A,  // muted — secondary ink (~5.9:1)
         0x4A4A4A,  // faint — tertiary/disabled
         0x03A9F4,  // accent — signal blue (= the signed hue, by design)
     },
     k_ladder_default,
     {1.0f, 1.0f, 1.0f},       // measured
     // Voice: today's words, byte-for-byte — the default never drifts.
     {"All quiet", "all quiet", "hello again"}},
    // [Heirloom] — warm machines (mid-century): big, warm, unhurried.
    {"Heirloom", "warm & roomy • easy reading",
     {
         0x0B0A08,  // bg — warm charcoal
         0x17140E,  // surface — aged panel
         0x2A241A,  // edge — bronze hairline
         0xF3E9D2,  // text — cream ink (~16.4:1 on bg — AAA)
         0xC9A77A,  // muted — tan ink (~8.8:1 — still AAA)
         0x6E5C3C,  // faint — deep brass
         0xC9A24B,  // accent — brass
     },
     k_ladder_heirloom,
     {1.15f, 1.30f, 0.90f},    // slow & sparing
     // Voice: mid-century courtesy — unhurried, complete sentences.
     {"All is well", "all is well", "welcome home"}},
    // [Aqua] — the millennium (early-2000s gloss): bright, still calm.
    {"Aqua", "bright & glossy • turn-of-century",
     {
         0x050912,  // bg — deep blue-black
         0x0C1524,  // surface — ocean panel
         0x1B2C42,  // edge — steel hairline
         0xE8F1F7,  // text — cool ink (~17.4:1 on bg)
         0x8FB0C4,  // muted — sea-glass ink
         0x4A6274,  // faint — deep water
         0x38C6FF,  // accent — glossy cyan (day-only; night outranks it)
     },
     k_ladder_default,
     {0.95f, 1.0f, 1.05f},     // friendly
     // Voice: millennium optimism — crisp and bright.
     {"All clear", "all clear", "welcome back"}},
    // [Neon] — now (Gen-Alpha energy): vivid and alive, still honest.
    {"Neon", "vivid & lively • high energy",
     {
         0x08060C,  // bg — near-black violet
         0x141019,  // surface — dim stage
         0x2A2035,  // edge — violet hairline
         0xF0EAF5,  // text — bright ink (~17.1:1 on bg)
         0x9A8FB0,  // muted — dusty lavender
         0x554A66,  // faint — deep violet
         0x22E0C8,  // accent — electric teal (day-only; night outranks it)
     },
     k_ladder_default,
     {0.90f, 0.75f, 1.20f},    // quick & springy
     // Voice: quick and casual — short words, no ceremony.
     {"All good", "all good", "hey again"}},
};

// Ring (flip-through) order — display order, not enum order: the warm
// age first, then the default, then the brighter ones. QuietGlass stays
// enum 0 (the fallback) wherever it sits on the ring.
constexpr Character k_ring[] = {Character::Heirloom, Character::QuietGlass,
                                Character::Aqua, Character::Neon};

// Valid before settings load, so the splash and any early draw wear the
// default.
Character s_active = Character::QuietGlass;

uint8_t clamp_idx(Character c) {
  const uint8_t i = (uint8_t)c;
  return i < (uint8_t)Character::Count ? i : (uint8_t)Character::QuietGlass;
}

}  // namespace

void character_apply(Character c) {
  const uint8_t i = clamp_idx(c);
  s_active = (Character)i;
  // Temperament rides along: a live bird re-paces at once; before any
  // bird exists this just stashes the scalars for the first one.
  const Temperament& t = k_defs[i].temp;
  canary_mark_temperament(t.breath, t.flourish, t.hop);
}

Character active_character() { return s_active; }

const CharacterDef& active_character_def() {
  return k_defs[clamp_idx(s_active)];
}

const Voice& active_voice() { return k_defs[clamp_idx(s_active)].voice; }

const char* character_name(Character c) { return k_defs[clamp_idx(c)].name; }

const char* character_caption(Character c) {
  return k_defs[clamp_idx(c)].caption;
}

uint8_t character_count() { return (uint8_t)Character::Count; }

Character character_at_ring(uint8_t ring_pos) {
  return k_ring[ring_pos % (uint8_t)Character::Count];
}

uint8_t character_ring_pos(Character c) {
  for (uint8_t i = 0; i < (uint8_t)Character::Count; i++)
    if (k_ring[i] == c) return i;
  return 0;
}

Character character_ring_step(Character c, int dir) {
  const int n = (int)(uint8_t)Character::Count;
  const int pos = ((int)character_ring_pos(c) + dir % n + n) % n;
  return k_ring[pos];
}

}  // namespace canary::ui
