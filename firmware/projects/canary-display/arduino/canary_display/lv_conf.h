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

/* v9-only (v8 reads LV_MEM_CUSTOM above and ignores this key): route
 * lv_malloc through the C library so LVGL shares the ESP heap instead of a
 * fixed 64 KiB pool. Under v9 that pool also feeds draw tasks, layers, the
 * canvas/QR buffers and the image cache — and on the RGB glasses its worst
 * moment is exactly the setup wizard, where two full 800x480 screens are
 * alive at once (see onboard_ui.cpp's fade note). The builtin pool's
 * failure mode is LV_ASSERT_MALLOC's while(1) — a silent halt (and, with
 * the task watchdog armed, a panic loop). The heap turns that cliff into
 * a couple hundred KiB of headroom. */
#define LV_USE_STDLIB_MALLOC 1  /* LV_STDLIB_CLIB */

/* Tick straight from millis() — no timer ISR to maintain. (v8-only; the
 * v9 port registers the same source at runtime via lv_tick_set_cb.) */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF 130

/* Frame pacing (the motion engine, ui/motion.h): raise the refresh ceiling
 * from LVGL's stock 30 ms to 16 ms (~60 fps) on every glass with the head-
 * room for it. Dirty-region rendering means an idle face pays nothing for
 * the faster ceiling — the refresh timer finds nothing invalid and returns —
 * while eased micro-motion (digit morphs, the veil, the minute sweep) gets
 * twice the frames. A pass that runs long simply lands late (animations are
 * time-parameterized, so they stay time-correct and drop frames instead of
 * stretching), and the engine's governor watches real frame cost and parks
 * decorative motion if a glass cannot keep up. The lean C3/C6 builds keep
 * the stock 30 ms ceiling — fewer frames is that hardware's honest answer.
 * (Two names, one value: LV_DISP_DEF_REFR_PERIOD is the v8 key,
 * LV_DEF_REFR_PERIOD the v9 one; each major ignores the other's.) */
#ifndef CD_LEAN_BUILD
#define LV_DISP_DEF_REFR_PERIOD 16
#define LV_DEF_REFR_PERIOD 16
#endif

/* No default theme: every style on these faces is deliberate (Quiet Glass
 * sets each color/radius/shadow itself; theme defaults would fight it). */
#define LV_USE_THEME_DEFAULT 0

/* Type scale (display_ux_design.md §Design language). Montserrat ships
 * anti-aliased with the FontAwesome symbol set merged in (LV_SYMBOL_*). */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1   /* dash body (per-flavor scale, theme.h) */
#define LV_FONT_MONTSERRAT_28 1
/* The two room-scale sizes are the expensive ones (~170 KB together with the
 * merged symbol set). CD_LEAN_BUILD — the 4 MB C6 nightstand env — drops
 * them to fit the 0x1E0000 OTA A/B slot; its 1.47" bedside glass never draws
 * them (character.cpp's lean ladder caps at 28). Every roomier build keeps
 * them. */
#ifndef CD_LEAN_BUILD
#define LV_FONT_MONTSERRAT_36 1   /* dash title (per-flavor scale, theme.h) */
#define LV_FONT_MONTSERRAT_48 1
#endif
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_LOG 0

/* Proof-on-Glass: bundled QR generator (trailblazer spec §1). */
#define LV_USE_QRCODE 1

#endif /* LV_CONF_H */
