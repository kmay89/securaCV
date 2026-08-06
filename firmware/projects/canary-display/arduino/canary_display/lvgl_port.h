#pragma once
#include <stdint.h>

// LVGL bring-up: display driver bound to the flavor's panel through the
// existing HAL (canary/hal/display.h). Input stays with main.cpp's gesture
// policy — LVGL renders, it does not own touch. Call lvgl_port_init() once
// after display_init(); pump lv_timer_handler() from loop().

namespace canary::ui {

bool lvgl_port_init();

// ── Orientation (7"/dash RGB glass) ──────────────────────────────────────
// Software-rotate the whole UI to one of canary::glass::Rotation's four
// quarter turns. The panel keeps scanning its native landscape; LVGL renders
// the logical canvas rotated to match a wall-mounted-portrait or tall bedside
// device. A no-op on the SPI/round flavors (fixed glass). Also tells the HAL
// how to un-rotate raw touch so a tap still lands where the finger points.
void lvgl_port_set_rotation(uint8_t rot);
uint8_t lvgl_port_rotation();

// Logical canvas size after rotation — what a face should lay itself out in
// (800x480 landscape, 480x800 portrait). Faces read these, never LCD_WIDTH.
int16_t lvgl_port_width();
int16_t lvgl_port_height();

#ifdef CD_NIGHTLIGHT
// The nightlight's HARDWARE rotation: the panel re-addresses its own RAM
// (hal display_set_rotation does the MADCTL), so LVGL renders nothing
// rotated — it only needs the logical canvas swapped to the new shape.
// Call after display_set_rotation, then rebuild the face.
void lvgl_port_set_panel_rotation(uint8_t rot);
#endif

// ── Rendered brightness (binary-backlight glass) ─────────────────────────
// A full-glass black scrim on the top layer: opa 0 = clear, 255 = black.
// The sustained daytime dimming the CH422G backlight can't do in hardware
// (see canary::glass::bright_scrim_opa). Sits above every screen — including
// the settings sheet — so "the screen is the preview" holds while you drag.
// Cleared (opa 0) on an unacked alert by the caller: honesty outranks a dim.
void lvgl_port_set_dim(uint8_t opa);

}  // namespace canary::ui
