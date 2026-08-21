// src/ui/character.cpp — the Character table + active-look state. See
// character.h and docs/hardware/display_character.md.
//
// Pure data and a few lookups. Palettes carry computed contrast ratios in
// comments (WCAG 1.4.3: body text >= 4.5:1; Heirloom targets AAA) so a
// future edit can't quietly regress one.
#include "flavor_config.h"

#include "character.h"
#include "canary_mark.h"

namespace canary::ui {

namespace {

// Type ladders, per-flavor for the same bench finding theme.h's scale
// was: the dash reads from across a room, so it runs larger per role.
// Every size referenced here is enabled in lv_conf.h ({12,14,16,20,24,
// 28,36,48}). Heirloom steps one enabled size UP per role — the "large
// print" edition (display_character.md §4). The default ladder doubles
// for Neon's "compact": there is no smaller enabled font, and tighter
// than default would break the glance contract anyway.
#if defined(CD_FLAVOR_DASH) || defined(CD_AMOLED_GLASS)
// The AMOLED 2.41 shares the dash's across-the-room ladder for a different
// reason: 450x600 in 2.41 inches is ~310 ppi, so the small-glass sizes
// would render physically tiny — the roles need the larger faces just to
// keep their designed physical height.
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
#elif defined(CD_LEAN_BUILD)
// The 4 MB C6 nightstand: lv_conf.h drops the 36/48 faces to fit the
// 0x1E0000 OTA slot, so the ladder's ceiling is 28. On a 172-px-wide
// bedside glass 28 still reads as the hero from arm's reach — the sizes
// removed were the across-the-room dash scale this board never plays at.
constexpr TypeLadder k_ladder_default = {
    &lv_font_montserrat_28,   // hero (28 is the lean ceiling)
    &lv_font_montserrat_24,   // title
    &lv_font_montserrat_16,   // body
    &lv_font_montserrat_14,   // label
    &lv_font_montserrat_12,   // caption
    &lv_font_montserrat_20,   // clock
};
constexpr TypeLadder k_ladder_heirloom = {
    &lv_font_montserrat_28,   // hero (28 is the lean ceiling)
    &lv_font_montserrat_28,   // title
    &lv_font_montserrat_20,   // body
    &lv_font_montserrat_16,   // label
    &lv_font_montserrat_14,   // caption
    &lv_font_montserrat_24,   // clock
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

// The canonical timeline-card semantic set — every dark Character shares
// these exact bytes (a state is the same color on the wall and in the
// app). Only a light ground may darken them within family (Almanac).
constexpr Semantics k_sem_canonical = {0x43A047, 0xFB8C00, 0xE53935,
                                       0x03A9F4};

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
     {"All quiet", "all quiet", "hello again"},
     k_sem_canonical},
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
     {"All is well", "all is well", "welcome home"},
     k_sem_canonical},
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
     {"All clear", "all clear", "welcome back"},
     k_sem_canonical},
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
     {"All good", "all good", "hey again"},
     k_sem_canonical},
    // [Almanac] — paper (the age of print): the one light ground (wave 3).
    // Ratios measured against BOTH paper tiers (WCAG 1.4.3); night
    // outranks it entirely — at night every glass shares the dark floor.
    {"Almanac", "paper & ink • the age of print",
     {
         0xF2EAD8,  // bg — warm paper
         0xE9DFC8,  // surface — deeper paper card
         0xC9BC9E,  // edge — ruled-line hairline
         0x2B2418,  // text — warm ink (~12.8:1 on bg — AAA)
         0x6A5F49,  // muted — faded ink (~5.2:1)
         0x9C9077,  // faint — pencil (tertiary, decorative)
         0x2F4A6E,  // accent — fountain-pen indigo (~7.5:1)
     },
     k_ladder_default,
     {1.10f, 1.15f, 0.90f},    // bookish calm
     // Voice: almanac weather-speak — fair skies, plain courtesy.
     {"All calm", "all calm", "good day"},
     // Semantics darkened WITHIN FAMILY for paper — same hue stories,
     // deeper stops (ok 5.4/4.9, warn 5.2/4.7, alert 5.5/5.0,
     // signed 6.2/5.6 : 1 on bg/surface; canonical amber is 1.98:1 on
     // paper — invisible, which is the dishonest option).
     {0x276B2B, 0x8F5300, 0xB71C1C, 0x01579B}},
    // [Terminal] — the CRT age: phosphor on near-black green. All chrome
    // is the phosphor family; the semantic set stays canonical (measured:
    // ok 6.1, warn 8.4, alert 4.7, signed 7.6 : 1 on bg).
    {"Terminal", "phosphor & glow • the CRT age",
     {
         0x030A04,  // bg — powered-down glass, green-black
         0x0A140B,  // surface — tube face
         0x1C3320,  // edge — scanline hairline
         0xB9F0C5,  // text — aged phosphor (~15.6:1 on bg — AAA)
         0x55B274,  // muted — dimmed phosphor (~7.6:1)
         0x2A5C3C,  // faint — afterglow
         0x36E06A,  // accent — the cursor's phosphor (~11.5:1)
     },
     k_ladder_default,
     {1.0f, 1.20f, 0.95f},     // machine-steady: flourishes are rare events
     // Voice: console register — status words, no ceremony.
     {"All nominal", "all nominal", "back online"},
     k_sem_canonical},
    // [Blueprint] — the drafting room: white ink on Prussian blue, chalk-
    // cyan accent. Semantics canonical (ok 5.2, warn 7.3, alert 4.1,
    // signed 6.5 : 1 on bg — all >=3:1 on surface too).
    {"Blueprint", "ink & cyan • the drafting room",
     {
         0x0D1B33,  // bg — Prussian blue paper
         0x142643,  // surface — title block
         0x2A4A78,  // edge — grid line
         0xEAF2FC,  // text — white ink (~15.2:1 on bg — AAA)
         0x9DB8DC,  // muted — construction line (~8.5:1)
         0x4C688F,  // faint — erased line
         0x7FD4FF,  // accent — chalk cyan (~10.5:1; day-only, night outranks)
     },
     k_ladder_default,
     {1.05f, 1.10f, 0.95f},    // the careful drafter
     // Voice: drafting-room register — the plan, holding.
     {"All to plan", "all to plan", "as you were"},
     k_sem_canonical},
};

// Ring (flip-through) order — display order, not enum order: the warm
// age first, then the default, then the brighter ones. QuietGlass stays
// enum 0 (the fallback) wherever it sits on the ring.
constexpr Character k_ring[] = {Character::Heirloom, Character::QuietGlass,
                                Character::Almanac, Character::Aqua,
                                Character::Blueprint, Character::Terminal,
                                Character::Neon};
// Every Character appears on the ring exactly once, enforced at compile
// time: a future wave that grows the enum but forgets the ring would
// otherwise index past the array (character_at_ring wraps by Count, not
// by the array's real length). An explicit [Count] bound would miss the
// too-few case — missing entries zero-fill into duplicate QuietGlass —
// so the assert checks the element count instead (review catch).
static_assert(sizeof(k_ring) / sizeof(k_ring[0]) ==
                  (size_t)(uint8_t)Character::Count,
              "every Character must appear on the ring exactly once");

// Valid before settings load, so the splash and any early draw wear the
// default.
Character s_active = Character::QuietGlass;
// Written by the render tick, read by the theme accessors — all on the
// loop task by construction (the faces, LVGL timers, settings, splash,
// and even the glass web server's handleClient run there; glass_web.cpp
// documents the same single-task property). No atomics until something
// off-loop reads the theme — and nothing does, by rule.
bool s_night = false;   // see character_set_night()

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

const Semantics& active_semantics() {
  // Night forces the dark floor (theme.cpp), so it forces the dark-ground
  // semantic bytes with it: Almanac's paper stops are ~3:1 on black —
  // right on paper, wrong in the dark. One policy, one place.
  return s_night ? k_sem_canonical : k_defs[clamp_idx(s_active)].sem;
}

void character_set_night(bool night) { s_night = night; }
bool character_night() { return s_night; }

const char* character_name(Character c) { return k_defs[clamp_idx(c)].name; }

const CharacterDef& character_def(Character c) { return k_defs[clamp_idx(c)]; }

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
