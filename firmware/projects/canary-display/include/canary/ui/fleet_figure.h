// include/canary/ui/fleet_figure.h — a witness's own picture, on the glass.
//
// The last unwired surface of the fleet-figures system (FLEET_FIGURES.md
// §10): the phone, the wrist, the catalog and both flashers already draw
// every device from the one shared ledger, and the roll call on the glass
// still said "canary_n". This widget closes that gap with the same honesty
// rules the other surfaces keep:
//
//   · the picture comes from the generated art tier
//     (common/core/fleet_figures_art.h) — the same projector, camera and
//     palette as every other surface, never a hand-drawn icon;
//   · an unresolvable figure draws NOTHING (the widget reports it, the
//     caller keeps its generic marker) — a wrong picture is worse than no
//     picture;
//   · faces keep their physical-object colors in every Character — a case
//     is off-white in Neon too — and the night face hides figures rather
//     than recoloring them.
//
// Vector at render time: one multiply per vertex, no canvas buffer, no
// bitmap tiers — a 24 px chip and a 40 px row are the same bytes.
#pragma once
#include <lvgl.h>

namespace canary::ui {

// A transparent size_px x size_px object that paints the figure's art.
// With no art for figure_id (or a null id — an empty row slot) the widget
// is created HIDDEN: the slot keeps its size, nothing false is drawn, and a
// later fleet_figure_set can light it up.
lv_obj_t* fleet_figure_create(lv_obj_t* parent, const char* figure_id,
                              int size_px);

// Re-key a live widget (a recycled list row). An empty or unknown id hides
// it; a resolvable one shows it and repaints.
void fleet_figure_set(lv_obj_t* obj, const char* figure_id);

}  // namespace canary::ui
