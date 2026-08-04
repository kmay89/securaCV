/*
 * SecuraCV — the animated "wake" (host-testable, no Arduino).
 *
 * The animation with a job: instead of eye-candy, the boot self-test's ten
 * probes light up [..] -> [OK]/[!!] one at a time, so the animation *is* the
 * health check. It reveals the REAL per-probe results (name + pass/fail +
 * duration) from diag_get_selftest(), then converges to the static identity
 * card. On a confirmed ANSI terminal each reveal repaints the fixed-height block
 * in place (cursor-up + overwrite — every row is a fixed width, so no clearing
 * is needed and nothing tears). On the ASCII floor there is no cursor control,
 * so it degrades to printing the finished checklist once. Both end identically.
 *
 * Pure composition here; the timing / skip / Serial live in the firmware glue.
 * Compiles hosted for tests_host with -Wall -Wextra -Werror.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */
#ifndef SECURACV_CONSOLE_WAKE_H
#define SECURACV_CONSOLE_WAKE_H

#include "ui/console_theme.h"

namespace scene {

enum class ProbeState : uint8_t { Pending = 0, Running, Pass, Fail };

struct WakeProbe {
  const char* label;
  ProbeState state;
  uint16_t ms;
};

// Fixed-width 4-char marker so rows always align; the WORD carries the meaning
// (OK / !!), color is only a bonus (WCAG 1.4.1 / the ambient-display rule).
inline const char* probe_marker(ProbeState s) {
  switch (s) {
    case ProbeState::Pending: return "[..]";
    case ProbeState::Running: return "[~~]";
    case ProbeState::Pass:    return "[OK]";
    case ProbeState::Fail:    return "[!!]";
  }
  return "[..]";
}

inline int probe_color(ProbeState s) {
  switch (s) {
    case ProbeState::Pass:    return COL_MOSS;
    case ProbeState::Fail:    return COL_AMBER;
    case ProbeState::Running: return COL_BRAND;
    default:                  return COL_DIM;
  }
}

// One probe row: "[OK] NVS read/write                      12ms" — marker + label
// left, duration right, colored by state.
inline void probe_row(const Renderer& r, int inner, const WakeProbe& p) {
  const int contentw = inner - 2;
  char ms[16];
  if (p.state == ProbeState::Pass || p.state == ProbeState::Fail)
    snprintf(ms, sizeof ms, "%ums", (unsigned)p.ms);
  else ms[0] = '\0';
  const int mslen = (int)strlen(ms);

  char left[160];
  snprintf(left, sizeof left, "%s %s", probe_marker(p.state), p.label ? p.label : "?");
  int maxleft = contentw - mslen;
  if (maxleft < 0) maxleft = 0;
  if ((int)strlen(left) > maxleft) left[maxleft] = '\0';

  char full[200];
  snprintf(full, sizeof full, "%-*s%s", maxleft, left, ms);
  row(r, inner, probe_color(p.state), full);
}

// The fixed-height block: title rule + n probe rows + a status row + bottom rule.
inline int wake_height(int n) { return n + 3; }

// Draw the whole block. `done` freezes the title/status; until then the status
// shows how many have reported (never the final score, so the reveal isn't
// spoiled). Health is the real report score, shown only when done.
inline void wake_frame(const Renderer& r, int inner, const WakeProbe* probes,
                       int n, int health, bool done) {
  int reported = 0, passed = 0;
  for (int i = 0; i < n; ++i) {
    if (probes[i].state == ProbeState::Pass) { reported++; passed++; }
    else if (probes[i].state == ProbeState::Fail) reported++;
  }
  hrule(r, done ? "self-test  (complete)" : "self-test  (running)", inner, 0);
  for (int i = 0; i < n; ++i) probe_row(r, inner, probes[i]);

  char st[96];
  int sc;
  if (done) {
    snprintf(st, sizeof st, "Health    %d%%   %d/%d probes passed", health, passed, n);
    sc = (health >= 100) ? COL_MOSS : COL_AMBER;
  } else {
    snprintf(st, sizeof st, "Health    checking...   %d/%d reported", reported, n);
    sc = COL_DIM;
  }
  row(r, inner, sc, st);
  hrule(r, "", inner, 2);
}

// Cursor control for the in-place repaint — all gated on caps.ansi, so on the
// ASCII floor these emit nothing (the driver prints sequentially instead).
inline void cursor_up(const Renderer& r, int n) {
  if (!r.caps.ansi || n <= 0) return;
  char b[16];
  snprintf(b, sizeof b, "\x1b[%dA", n);
  r.put(b);
  r.put("\r");
}
inline void hide_cursor(const Renderer& r) { if (r.caps.ansi) r.put("\x1b[?25l"); }
inline void show_cursor(const Renderer& r) { if (r.caps.ansi) r.put("\x1b[?25h"); }

}  // namespace scene

#endif  // SECURACV_CONSOLE_WAKE_H
