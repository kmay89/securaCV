// src/ui/fleet_figure.cpp — draw a fleet figure's art tier. See the header.
//
// The art is pre-triangulated by the generator, so both LVGL majors reduce
// to "fill these triangles": v8 through lv_draw_triangle(draw_ctx, ...), v9
// through a triangle draw task on the event's layer. This TU carries both
// behind LVGL_VERSION_MAJOR — the same pattern lvgl_port.cpp and mk_qrcode
// keep, and for the same reason (the SPI/emulator line pins 8.4, the RGB
// dash family rides 9.x).
#include "flavor_config.h"
#include <lvgl.h>

#include "fleet_figure.h"
#include "fleet_figures_art.h"

namespace canary::ui {

namespace {

using canary::figures::FigureArt;

// The art pointer rides the object's user_data (defaulted on in both
// majors); nothing else on the glass uses user_data on these leaf objects.

void draw_cb(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  const FigureArt* art = (const FigureArt*)lv_obj_get_user_data(obj);
  if (!art) return;

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const int32_t w = coords.x2 - coords.x1 + 1;
  const int32_t h = coords.y2 - coords.y1 + 1;
  const int32_t size = w < h ? w : h;
  // Center the square art box inside the object's area.
  const int32_t ox = coords.x1 + (w - size) / 2;
  const int32_t oy = coords.y1 + (h - size) / 2;
  const int32_t S = canary::figures::kFigureArtScale;
  const auto px = [&](int16_t c) -> int32_t {
    return ((int32_t)c * size + S / 2) / S;
  };

#if LVGL_VERSION_MAJOR >= 9
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_draw_triangle_dsc_t dsc;
  lv_draw_triangle_dsc_init(&dsc);
  dsc.opa = LV_OPA_COVER;
  for (uint16_t f = 0; f < art->face_count; f++) {
    const auto& face = art->faces[f];
    dsc.color = lv_color_hex(face.rgb);
    for (uint16_t t = 0; t < face.tri_count; t++) {
      const int16_t* v = face.tris + t * 6;
      dsc.p[0].x = (lv_value_precise_t)(ox + px(v[0]));
      dsc.p[0].y = (lv_value_precise_t)(oy + px(v[1]));
      dsc.p[1].x = (lv_value_precise_t)(ox + px(v[2]));
      dsc.p[1].y = (lv_value_precise_t)(oy + px(v[3]));
      dsc.p[2].x = (lv_value_precise_t)(ox + px(v[4]));
      dsc.p[2].y = (lv_value_precise_t)(oy + px(v[5]));
      lv_draw_triangle(layer, &dsc);
    }
  }
#else
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_opa = LV_OPA_COVER;
  dsc.border_width = 0;
  lv_point_t p[3];
  for (uint16_t f = 0; f < art->face_count; f++) {
    const auto& face = art->faces[f];
    dsc.bg_color = lv_color_hex(face.rgb);
    for (uint16_t t = 0; t < face.tri_count; t++) {
      const int16_t* v = face.tris + t * 6;
      p[0].x = (lv_coord_t)(ox + px(v[0]));
      p[0].y = (lv_coord_t)(oy + px(v[1]));
      p[1].x = (lv_coord_t)(ox + px(v[2]));
      p[1].y = (lv_coord_t)(oy + px(v[3]));
      p[2].x = (lv_coord_t)(ox + px(v[4]));
      p[2].y = (lv_coord_t)(oy + px(v[5]));
      lv_draw_triangle(ctx, &dsc, p);
    }
  }
#endif
}

}  // namespace

lv_obj_t* fleet_figure_create(lv_obj_t* parent, const char* figure_id,
                              int size_px) {
  const FigureArt* art = canary::figures::figure_art(figure_id);
  lv_obj_t* obj = lv_obj_create(parent);
  lv_obj_set_size(obj, size_px, size_px);
  lv_obj_set_style_bg_opa(obj, LV_OPA_0, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(obj, (void*)art);
  lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  // A slot with no honest picture yet starts hidden — the caller lays out a
  // fixed-size slot either way, so rows never reflow as figures land.
  if (!art) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  return obj;
}

void fleet_figure_set(lv_obj_t* obj, const char* figure_id) {
  if (!obj) return;
  const FigureArt* art = canary::figures::figure_art(figure_id);
  const FigureArt* cur = (const FigureArt*)lv_obj_get_user_data(obj);
  if (art == cur) {
    // Same picture — but the SLOT may have been hidden while its row was
    // empty, and a recycled row with the same figure must come back
    // visible (Codex P2: the equal-art path kept a stale HIDDEN flag).
    if (art) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_set_user_data(obj, (void*)art);
  if (art) {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(obj);
  } else {
    // No honest picture for this id: hide, never guess.
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace canary::ui
