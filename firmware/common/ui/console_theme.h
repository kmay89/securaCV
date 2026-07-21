/*
 * SecuraCV — console scene engine (host-testable, no Arduino).
 *
 * Composes framed, optionally-coloured text panels for the USB-serial console,
 * with ONE hard rule that keeps it from ever becoming garbage on the wrong
 * terminal:
 *
 *   The safe floor is 7-bit ASCII, no colour, no cursor control (caps_ascii()).
 *   Escape bytes and box-drawing glyphs are emitted ONLY when the terminal has
 *   proven it can handle them (a Device-Attributes / cursor-position reply —
 *   see console_probe in the firmware glue). We never emit an escape we didn't
 *   confirm. This matters concretely: our own browser flasher's serial monitor
 *   (canary-local flash-core.js looksLikeGarbage) treats ESC bytes as "wrong
 *   baud", so an unconfirmed ANSI banner would make our own tool distrust the
 *   device. Default-ASCII is the contract, host-tested.
 *
 * Layout math is exact because *content is ASCII* (device ids, hex, randomart
 * glyphs are all 7-bit, so byte length == column width); only the BORDER glyphs
 * differ between the Unicode and ASCII tiers, and those are placed one column
 * at a time. So panels align in both tiers.
 *
 * Colour never carries meaning alone (AMBIENT_DISPLAY_STANDARD / WCAG 1.4.1):
 * every state that gets a colour also gets a word.
 *
 * Compiles hosted for tests_host with -Wall -Wextra -Werror.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */
#ifndef SECURACV_CONSOLE_THEME_H
#define SECURACV_CONSOLE_THEME_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace scene {

// ── capability, decided once at connect ─────────────────────────────────────
enum class ColorDepth : uint8_t { None = 0, Ansi16, Ansi256 };

struct Caps {
  bool ansi;         // cursor control + SGR allowed
  bool unicode;      // box-drawing glyphs allowed (else ASCII + - |)
  ColorDepth color;
  int width;         // columns (safe default 80)
  int height;        // rows (safe default 24)
};

// THE floor. What we emit when a terminal hasn't proven anything.
inline Caps caps_ascii() { return Caps{false, false, ColorDepth::None, 80, 24}; }
// Confirmed-capable terminal.
inline Caps caps_full(int w, int h) {
  return Caps{true, true, ColorDepth::Ansi256, w > 0 ? w : 80, h > 0 ? h : 24};
}

// 256-colour palette indices, matching the brand + the ambient-display states.
enum Color { COL_NONE = -1, COL_BRAND = 39, COL_MOSS = 42, COL_AMBER = 214,
             COL_DIM = 245, COL_TEXT = 255 };

// The engine composes text and pushes it to a sink one chunk at a time —
// Serial.print in firmware, a string collector in tests. Keeps the engine pure.
typedef void (*emit_fn)(void* ctx, const char* s);

struct Renderer {
  emit_fn emit;
  void* ctx;
  Caps caps;

  void put(const char* s) const { if (emit && s) emit(ctx, s); }
  void nl() const { put("\r\n"); }                       // CRLF: raw terminals need it

  void fg(int idx) const {
    if (!caps.ansi || caps.color == ColorDepth::None || idx < 0 || idx > 255) return;
    char b[24];
    snprintf(b, sizeof b, "\x1b[38;5;%dm", idx);
    put(b);
  }
  void bold() const { if (caps.ansi) put("\x1b[1m"); }
  void reset() const { if (caps.ansi) put("\x1b[0m"); }
};

// ── border glyph set ────────────────────────────────────────────────────────
struct Box { const char *h, *v, *tl, *tr, *bl, *br, *ml, *mr; };
inline Box box_for(const Caps& c) {
  if (c.unicode) return Box{"─","│","┌","┐","└","┘","├","┤"};
  return Box{"-", "|", "+", "+", "+", "+", "+", "+"};
}

// ── primitives ──────────────────────────────────────────────────────────────
// Total visible width of a panel is (inner + 2): the two vertical bars bracket
// `inner` interior columns.

// A horizontal rule with an optional inline title. kind: 0 top, 1 mid, 2 bottom.
inline void hrule(const Renderer& r, const char* title, int inner, int kind) {
  const Box b = box_for(r.caps);
  const char* L = kind == 0 ? b.tl : (kind == 2 ? b.bl : b.ml);
  const char* R = kind == 0 ? b.tr : (kind == 2 ? b.br : b.mr);
  r.fg(COL_BRAND);
  r.put(L);
  int used = 0;
  if (title && *title) {
    r.put(b.h); r.put(" ");
    r.bold(); r.fg(COL_TEXT); r.put(title); r.fg(COL_BRAND);
    r.put(" ");
    used = 2 + (int)strlen(title) + 1;    // h + space + title + space
  }
  for (int i = used; i < inner; ++i) r.put(b.h);
  r.put(R);
  r.reset();
  r.nl();
}

// Left-align `text` (ASCII) inside a panel row, coloured `fg` (COL_NONE = plain),
// padded/truncated to the interior. One space of margin on each side.
inline void row(const Renderer& r, int inner, int fgc, const char* text) {
  const Box b = box_for(r.caps);
  int contentw = inner - 2;
  if (contentw < 0) contentw = 0;
  char buf[512];
  int n = 0;
  if (text) { n = (int)strlen(text); if (n > contentw) n = contentw; memcpy(buf, text, n); }
  buf[n] = '\0';

  r.fg(COL_BRAND); r.put(b.v); r.reset();
  r.put(" ");
  if (fgc >= 0) r.fg(fgc);
  r.put(buf);
  if (fgc >= 0) r.reset();
  for (int i = n; i < contentw; ++i) r.put(" ");
  r.put(" ");
  r.fg(COL_BRAND); r.put(b.v); r.reset(); r.nl();
}

// printf-style row.
inline void rowf(const Renderer& r, int inner, int fgc, const char* fmt, ...) {
  char buf[512];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  row(r, inner, fgc, buf);
}

// A blank interior row.
inline void row_blank(const Renderer& r, int inner) { row(r, inner, COL_NONE, ""); }

// Center an ASCII string into `dst` (>= width+1) padded with spaces to `width`.
inline void center_into(char* dst, size_t cap, const char* s, int width) {
  int n = s ? (int)strlen(s) : 0;
  if (n > width) n = width;
  int left = (width - n) / 2;
  int o = 0;
  for (int i = 0; i < left && (size_t)o + 1 < cap; ++i) dst[o++] = ' ';
  for (int i = 0; i < n && (size_t)o + 1 < cap; ++i) dst[o++] = s[i];
  for (int i = left + n; i < width && (size_t)o + 1 < cap; ++i) dst[o++] = ' ';
  dst[o] = '\0';
}

}  // namespace scene

#endif  // SECURACV_CONSOLE_THEME_H
