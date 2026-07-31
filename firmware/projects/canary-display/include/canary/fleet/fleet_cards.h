#pragma once
#include <stdint.h>
#include "canary/fleet/fleet_model.h"

// Canary Cards on the glass — the firmware side of the standardized
// widget-card layer (docs/standard/CANARY_CARDS.md, schema v1; reference JS
// renderer canary-local/assets/canary-cards.js).
//
// The rule, borrowed from Home Assistant: **one entity, one card**. A witness
// publishes the same MQTT entity set it announces to HA; every surface renders
// those entities through one small card vocabulary. Presence buckets, range
// bands, breathing locks and BPM numerics had nowhere type-aware to land on
// the glass — they got squeezed into last_event or dropped (fleet_model.h's
// generic Witness). This layer gives them cards.
//
// Pure C++ (no Arduino/LVGL/JSON): build_cards() is a projection from the
// dependency-free Witness onto an ordered, fixed-capacity CardSet the UI walks
// with a tiny per-kind renderer. Host-tested in tests_host/test_fleet_cards.cpp,
// pinned to the same ordering / kinds / privacy classes the JS senseCards()
// emits, so the two surfaces cannot drift.
//
// Design invariants (mirrors the JS + the doc):
//   - null renders as "—" (unknown), never as zero — a stalled radar is
//     *unknown presence*, not *no presence* (AD-Core §2.1, silence != safety).
//   - a card whose entity is compiled out of a build renders `absent` ("not in
//     this build"), not silently missing — the presence-only BPM story.
//   - the trust badge never overclaims (reuses the model's Badge enum).

namespace canary::fleet {

// Card kinds (schema v1), in the doc's order.
enum class CardKind : uint8_t { Binary, Stat, Band, Sparkline, Event, Trust };

// Severity accent — the card's own small ladder (maps to Quiet Glass colours
// at render time). None = no accent (neutral/idle).
enum class CardSev : uint8_t { None, Ok, Notice, Warn, Alert };

// Privacy-class chip (design doc §2): P0 coarse claim, P1 opt-in wellbeing
// numeric, P2 never-leaves-device (rendered only as its coarse derivative).
enum class CardPrivacy : uint8_t { None, P0, P1, P2 };

// One card descriptor. POD, pointer-light: title/unit/labels/icon are static
// string literals; `ev`/`band_options` point into caller-owned memory (the
// Witness, which outlives a render pass, and static option tables here). Kept
// flat rather than a union so it stays trivially copyable and host-testable.
struct Card {
  CardKind    kind = CardKind::Binary;
  const char* id = "";           // HA object_id suffix (lowercase slug)
  const char* title = "";
  const char* icon = "";         // icon id (matches the JS ICONS vocabulary)
  CardSev     sev = CardSev::None;
  CardPrivacy privacy = CardPrivacy::None;
  bool        absent = false;    // entity compiled out of this build

  // binary
  bool        b_known = false;   // false => "—"
  bool        b_state = false;
  const char* on_label = "on";
  const char* off_label = "off";

  // stat / sparkline
  bool        num_known = false; // false => "—"
  long        num = 0;
  const char* unit = "";
  uint8_t     num_decimals = 0;  // fixed-point: num is scaled by 10^decimals
                                 // (pH 7.4 -> num=74, num_decimals=1). 0 =
                                 // integer, so existing stat cards are unchanged.

  // band: an ordered vocabulary + the lit slot (-1 => "—")
  const char* const* band_options = nullptr;
  uint8_t     band_count = 0;
  int8_t      band_sel = -1;

  // event
  const char* ev = nullptr;      // last event name, or nullptr => "—"
  bool        signed_known = false;
  bool        signed_flag = false;

  // trust
  uint32_t    chain = 0;
  Badge       badge = Badge::Unknown;
};

// A witness's card set. Fixed capacity — no heap; built on demand into a
// stack instance when a detail page renders, never stored per witness.
struct CardSet {
  static constexpr int CAP = 12;
  Card cards[CAP];
  int  count = 0;
};

// Does this witness have a type-aware card set? (Drives whether a detail page
// renders cards or falls back to the generic field list.) Today: canary-sense
// witnesses — any device that has published a radar state row, or advertises
// device_type "canary-sense".
bool has_cards(const Witness& w);

// Build the ordered card set for a witness. Dispatches on device_type; unknown
// types yield count 0 (caller keeps its existing rendering). now/limits are
// accepted for parity with the model's time-driven severities (unused today
// but part of the contract so callers wire them from the start).
void build_cards(const Witness& w, uint32_t now, const FleetLimits& limits,
                 CardSet& out);

// Render helpers shared by the LVGL surfaces + host tests.
const char* card_kind_name(CardKind k);      // "binary" / "stat" / ...
const char* card_privacy_name(CardPrivacy p); // "" / "P0" / "P1" / "P2"

// Format one card's VALUE by kind into buf ("—" when unknown, the label/number/
// lit-slot/event/badge otherwise). This is where the LVGL surfaces "learn the
// kinds": each kind projects its value differently, exactly like the JS
// renderer. Returns strlen written. Pure ASCII (LVGL symbol substitution is the
// caller's job). An `absent` card formats as "· not in build".
size_t format_card_value(const Card& c, char* buf, size_t cap);

// Format an entire card set as a compact single-line strip for a witness
// detail row — "Presence present · Occupants 1 · Range near · ♥ 68" — skipping
// absent cards and (optionally) trust/event which the surrounding UI already
// shows. This is the text realization of the card grid the wall dash and watch
// render today; a full per-kind LVGL grid is the documented follow-on. Returns
// the number of cards included. `skip_shown` drops last_event + chain (the
// glance/dash chrome already renders those).
int format_card_strip(const CardSet& set, bool skip_shown,
                      char* buf, size_t cap);

}  // namespace canary::fleet
