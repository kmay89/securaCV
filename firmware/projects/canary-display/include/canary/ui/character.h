// include/canary/ui/character.h — the curated Character ring
// (docs/hardware/display_character.md).
//
// A Character bundles four coordinated dimensions into one named,
// pre-validated look: ground & chrome palette, type feel (which enabled
// Montserrat size fills each role), the bird's temperament, and the
// voice (ambient copy only). It is NOT a license to change what anything
// means: the
// semantic set (col_ok/warn/alert/signed) stays at timeline-card parity
// and every ncol_* stays with the night engine — a Character restyles
// the room, never the alarms, and night outranks it.
#pragma once
#include <lvgl.h>
#include <stdint.h>

namespace canary::ui {

// QuietGlass MUST stay 0: it is the default and the safe fallback — an
// old or corrupt settings blob degrades to it by construction.
enum class Character : uint8_t {
  QuietGlass = 0,
  Heirloom   = 1,
  Aqua       = 2,
  Neon       = 3,
  Count      = 4,
};

// Ground & chrome tiers plus the single decorative accent, raw 0xRRGGBB
// (theme.cpp turns them into lv_color_t at the choke point).
struct Palette {
  uint32_t bg, surface, edge, text, muted, faint, accent;
};

// Which enabled Montserrat size fills each type role (theme.h's roles).
struct TypeLadder {
  const lv_font_t* hero;
  const lv_font_t* title;
  const lv_font_t* body;
  const lv_font_t* label;
  const lv_font_t* caption;
  const lv_font_t* clock;
};

// The bird's temperament: three bounded scalars layered UNDER the mood
// engine (display_character.md §5) — cadence, never vocabulary.
struct Temperament {
  float breath;    // breath half-period scale
  float flourish;  // idle-flourish cadence scale
  float hop;       // hop amplitude scale
};

// Wave 2 — the Voice: per-Character register for AMBIENT copy only
// (display_character.md §6). A Character may rephrase contentment, never
// trouble: severity words, badge text, link labels, event copy, and every
// degraded-state instruction ("waiting for wifi", "no canaries yet") are
// invariant — they are diagnosis and guidance, not mood. Plain ASCII by
// rule: the built-in Montserrat tables carry no emoji, and the glance
// contract wants words that read at 3 m, not glyphs.
struct Voice {
  const char* all_quiet;      // calm-fleet hero word ("All quiet")
  const char* all_quiet_low;  // its inline register, under a time hero
  const char* hello_again;    // returning-boot splash tagline
};

struct CharacterDef {
  const char* name;
  const char* caption;
  Palette pal;
  TypeLadder type;
  Temperament temp;
  Voice voice;
};

// Applies a Character: clamps, sets it active (theme.h reads it from
// here on), and pushes its temperament to the bird.
void character_apply(Character c);

Character active_character();
const CharacterDef& active_character_def();   // theme.cpp's choke point
const Voice& active_voice();                  // ambient copy, never alarms

const char* character_name(Character c);
const char* character_caption(Character c);
uint8_t character_count();                    // (uint8_t)Character::Count

// The ring (the picker's flip-through) runs in display order, not enum
// order; stepping wraps, so "just keep flipping" always comes home.
Character character_at_ring(uint8_t ring_pos);
uint8_t character_ring_pos(Character c);
Character character_ring_step(Character c, int dir);

}  // namespace canary::ui
