/**
 * @file lv_conf.h
 * @brief LVGL configuration for canary-display ("Quiet Glass" UI) —
 *        serves BOTH majors (v8.4 on the PlatformIO/core-2 path, v9.x on
 *        the Arduino core-3 path).
 *
 * One file works because lv_conf_internal.h (either major) defaults every
 * key not set here and ignores keys it doesn't know: the v8-only entries
 * below are inert under v9 (which gets its tick from lvgl_port.cpp's
 * lv_tick_set_cb and its byte order from the default RGB565 format), and
 * every shared key keeps its name across the majors.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* RGB565, native byte order: Arduino_GFX's draw16bitRGBBitmap handles the
 * SPI byte order on the watch, and the dash framebuffer is native LE.
 * (LV_COLOR_16_SWAP is v8-only; v9 renders LE RGB565 by default.) */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Widget/heap pool in internal RAM. ~120 objects across both faces; 64 KiB
 * leaves generous headroom. (v8: LV_MEM_CUSTOM 0; v9's builtin allocator
 * reads the same LV_MEM_SIZE.) */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64U * 1024U)

/* Tick straight from millis() — no timer ISR to maintain. (v8-only; the
 * v9 port registers the same source at runtime via lv_tick_set_cb.) */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF 130

/* No default theme: every style on these faces is deliberate (Quiet Glass
 * sets each color/radius/shadow itself; theme defaults would fight it). */
#define LV_USE_THEME_DEFAULT 0

/* Type scale (display_ux_design.md §Design language). Montserrat ships
 * anti-aliased with the FontAwesome symbol set merged in (LV_SYMBOL_*). */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_LOG 0

/* Proof-on-Glass: bundled QR generator (trailblazer spec §1). */
#define LV_USE_QRCODE 1

#endif /* LV_CONF_H */
