/**
 * @file lv_conf.h
 * @brief LVGL v8 configuration for canary-display ("Quiet Glass" UI).
 *
 * Deliberately short: lv_conf_internal.h supplies defaults for everything
 * not set here — only the decisions live in this file.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* RGB565, native byte order: Arduino_GFX's draw16bitRGBBitmap handles the
 * SPI byte order on the watch, and the dash framebuffer is native LE. */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Widget/heap pool in internal RAM. ~120 objects across both faces; 64 KiB
 * leaves generous headroom (lv_mem_monitor is visible in debug builds). */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64U * 1024U)

/* Tick straight from millis() — no timer ISR to maintain. */
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
