/**
 * @file csi_dashboard_html.h
 * @brief The headline Sensing dashboard, served at "/" by canary_wap.ino's
 *        handle_ui() and aliased at /sense by csi_integration.cpp for
 *        backward compatibility — Phase 3 of the WiFi CSI Tool plan.
 *
 * One PROGMEM HTML asset that renders the full landing experience: the
 * pearlescent presence orb, the hero state plate, the ambient theme
 * engine, the 24-hour activity ribbon, the Today receipts sheet (slide-up),
 * and the living-waveform + calibration dock. All five surfaces speak to
 * the Phase-4 endpoints landed in PR #360:
 *
 *   GET  /api/csi/stream     polled @ 1 Hz; drives orb + waveform + state
 *   GET  /api/events/today   populates the Today receipts sheet
 *   POST /api/events/dismiss "That was nothing" swipe / button
 *   GET  /api/csi/window     P2-gated raw vector for the Tinker view
 *
 * Microcopy doctrine: every user-facing string lives in a single COPY
 * object so a translation pass is a one-file edit. Tooltip pattern is
 * uniform — frosted-glass callout pill, hover / long-press / focus.
 *
 * No JS framework, no chart library, no webfont fetch. System font
 * stack only. Vanilla canvas + CSS custom properties. Reduced-motion
 * preserved by collapsing animations to fades; dark mode follows OS.
 *
 * Served at / by handle_ui in canary_wap.ino. /sense kept as a
 * compatibility alias for tools that linked to it during Phase-3 staging.
 */

#ifndef SECURACV_CSI_DASHBOARD_HTML_H
#define SECURACV_CSI_DASHBOARD_HTML_H

#include "build_config.h"  // CANARY_WEB_ASSETS_GZIPPED
#include <Arduino.h>

// Source of truth for the headline dashboard. Compiled out in normal builds:
// the binary ships the gzip copy from web_assets_gz.h (CANARY_WEB_ASSETS_GZIPPED).
// Regenerate with gen_web_assets_gz.py after editing the HTML below.
#if !defined(CANARY_WEB_ASSETS_GZIPPED)
static const char CSI_DASHBOARD_HTML[] PROGMEM = R"DASHBOARD(<!doctype html>
<html lang="en" data-state="sensing">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#1a1605" media="(prefers-color-scheme: dark)">
<meta name="theme-color" content="#fffbec" media="(prefers-color-scheme: light)">
<title>Canary · Sensing</title>
<!-- PWA shell. The SW lives at /sw.js (scope /), the manifest gives the
     dashboard an installable identity for "Add to Home Screen". Both
     come from csi_integration.cpp's handle_sense_manifest / _sw. -->
<link rel="manifest" href="/manifest.webmanifest">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="Canary">
<style>
  /* ── Design tokens ──────────────────────────────────────────────────── */
  :root {
    --font-sans: -apple-system, BlinkMacSystemFont, "SF Pro Display", "SF Pro Text",
                 "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;

    /* Canary-yellow base palette. Warm cream chrome, not security-alarm
     * neon — the bird, not the coal mine. */
    --bg-base:        #fffbec;
    --bg-veil:        rgba(255,252,230,0.6);
    --fg:             #2a2310;
    --fg-soft:        #6f5f2e;
    --fg-mute:        #a89868;
    --hairline:       rgba(42,35,16,0.10);
    --shadow-card:    0 4px 24px rgba(180,130,30,0.10), 0 1px 3px rgba(180,130,30,0.06);

    /* Hero gradients keyed off room state via [data-state] on <html> */
    --orb-1:          #fff3b0;
    --orb-2:          #ffe26b;
    --orb-3:          #f0c319;
    --orb-glow:       rgba(240,195,25,0.6);
    --bg-1:           #fffbec;
    --bg-2:           #fff6d5;
    --bg-3:           #fffcef;
    --accent:         #f0c319;

    --orb-size:       min(72vmin, 420px);
    --pulse-dur:      4.3s; /* default inhale-exhale; replaced by JS at confirmed */

    --ease-soft:      cubic-bezier(.22,.94,.34,1.0);
    --ease-spring:    cubic-bezier(.34,1.56,.64,1.0);
  }

  @media (prefers-color-scheme: dark) {
    :root {
      /* Warm-dark palette — like a porch lantern, not a server rack. */
      --bg-base:    #1a1605;
      --bg-veil:    rgba(40,32,12,0.55);
      --fg:         #faefc4;
      --fg-soft:    #c9b987;
      --fg-mute:    #7a6f4d;
      --hairline:   rgba(255,240,200,0.10);
      --shadow-card:0 8px 28px rgba(0,0,0,0.5), 0 1px 3px rgba(0,0,0,0.4);

      --orb-1:      #b89a1f;
      --orb-2:      #806a0d;
      --orb-3:      #423605;
      --orb-glow:   rgba(255,220,80,0.45);
      --bg-1:       #1a1605;
      --bg-2:       #211b0a;
      --bg-3:       #1c1808;
    }
  }

  /* State-driven theme. All hues warm so the room feels lived-in, not
   * surveilled. JS sets <html data-state="..."> on each /api/csi/stream
   * tick. */
  html[data-state="empty"]    { --orb-1:#fff3b0;--orb-2:#ffe26b;--orb-3:#f0c319;--orb-glow:rgba(240,195,25,.55); }
  html[data-state="sensing"]  { --orb-1:#fff5d0;--orb-2:#f3e6b0;--orb-3:#d6c280;--orb-glow:rgba(214,194,128,.45); }
  html[data-state="subtle"]   { --orb-1:#ffe0c2;--orb-2:#ffc99a;--orb-3:#f9a86b;--orb-glow:rgba(249,168,107,.55); }
  html[data-state="quiet"]    { --orb-1:#e8f3b8;--orb-2:#cce478;--orb-3:#94b833;--orb-glow:rgba(148,184,51,.50); }
  html[data-state="active"]   { --orb-1:#fff099;--orb-2:#ffd83d;--orb-3:#e8a90a;--orb-glow:rgba(232,169,10,.65); }
  html[data-state="together"] { --orb-1:#ffd4b8;--orb-2:#ffaf7a;--orb-3:#e87a3a;--orb-glow:rgba(232,122,58,.55); }

  @media (prefers-color-scheme: dark) {
    html[data-state="empty"]    { --orb-1:#705a14;--orb-2:#4a3a08;--orb-3:#2a2105;--orb-glow:rgba(255,220,80,.45); }
    html[data-state="sensing"]  { --orb-1:#5e5230;--orb-2:#403718;--orb-3:#231e08;--orb-glow:rgba(220,200,140,.40); }
    html[data-state="subtle"]   { --orb-1:#7a4c2a;--orb-2:#522e15;--orb-3:#2c170b;--orb-glow:rgba(220,140,90,.50); }
    html[data-state="quiet"]    { --orb-1:#5a6e22;--orb-2:#3a4a10;--orb-3:#202806;--orb-glow:rgba(170,210,80,.50); }
    html[data-state="active"]   { --orb-1:#806808;--orb-2:#574504;--orb-3:#2e2502;--orb-glow:rgba(255,210,60,.65); }
    html[data-state="together"] { --orb-1:#80451f;--orb-2:#552c10;--orb-3:#2c1707;--orb-glow:rgba(232,140,80,.55); }
  }

  * { box-sizing: border-box; }

  html, body {
    margin: 0;
    padding: 0;
    height: 100%;
    -webkit-font-smoothing: antialiased;
    text-rendering: optimizeLegibility;
  }

  body {
    font-family: var(--font-sans);
    color: var(--fg);
    background:
      radial-gradient(circle at 18% 8%,  var(--bg-2) 0%, transparent 55%),
      radial-gradient(circle at 92% 88%, var(--bg-3) 0%, transparent 60%),
      var(--bg-base);
    transition: background-color 1.2s var(--ease-soft);
    min-height: 100vh;
    overflow-x: hidden;
  }

  /* ── Top bar ────────────────────────────────────────────────────────── */

  .topbar {
    position: sticky;
    top: 0;
    z-index: 50;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px max(env(safe-area-inset-left), 18px) 14px max(env(safe-area-inset-right), 18px);
    background: var(--bg-veil);
    -webkit-backdrop-filter: blur(28px) saturate(180%);
    backdrop-filter: blur(28px) saturate(180%);
    border-bottom: 1px solid var(--hairline);
  }
  .brand {
    font-weight: 600;
    letter-spacing: -0.01em;
    font-size: 17px;
    color: var(--fg);
  }
  .brand .device {
    color: var(--fg-mute);
    font-weight: 400;
    margin-left: 8px;
  }
  .topbar-actions { display: flex; gap: 8px; align-items: center; }
  .iconbtn {
    border: 1px solid var(--hairline);
    background: var(--bg-veil);
    color: var(--fg);
    padding: 8px 14px;
    border-radius: 10px;
    font: inherit;
    font-size: 14px;
    font-weight: 500;
    cursor: pointer;
    transition: transform .15s var(--ease-spring), background .25s;
  }
  .iconbtn:hover  { transform: translateY(-1px); }
  .iconbtn:active { transform: translateY(0); }
  .iconbtn[disabled] { opacity: .5; cursor: not-allowed; }

  /* ── Hero ───────────────────────────────────────────────────────────── */

  main { padding: 0 max(env(safe-area-inset-left), 18px) 0 max(env(safe-area-inset-right), 18px); }

  .hero {
    display: grid;
    grid-template-rows: auto auto;
    place-items: center;
    padding: clamp(24px, 6vh, 64px) 0 clamp(20px, 4vh, 48px);
    text-align: center;
  }

  .plate { margin-bottom: clamp(16px, 4vh, 36px); position: relative; }
  .plate .state {
    font-size: clamp(48px, 10vw, 88px);
    font-weight: 600;
    line-height: 1;
    letter-spacing: -0.025em;
    margin: 0;
    transition: opacity .4s var(--ease-soft), transform .4s var(--ease-soft);
  }
  .plate .sub {
    margin: 14px 0 0;
    font-size: clamp(15px, 2.4vw, 19px);
    color: var(--fg-soft);
    font-weight: 400;
  }
  .plate .meta {
    margin: 6px 0 0;
    font-size: 12px;
    color: var(--fg-mute);
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }
  .plate .help {
    position: absolute;
    top: 4px; right: -36px;
    width: 28px; height: 28px;
    border-radius: 50%;
    border: 1px solid var(--hairline);
    background: var(--bg-veil);
    color: var(--fg-soft);
    font-size: 14px;
    font-weight: 500;
    cursor: pointer;
    line-height: 26px;
    text-align: center;
  }

  /* Orb — pearlescent, layered, breathes via CSS animation tied to JS-set --pulse-dur */
  .orb-wrap {
    position: relative;
    width: var(--orb-size);
    height: var(--orb-size);
    display: grid;
    place-items: center;
  }
  .orb {
    position: absolute;
    inset: 0;
    border-radius: 50%;
    background:
      radial-gradient(circle at 30% 28%, var(--orb-1) 0%, var(--orb-2) 40%, var(--orb-3) 95%);
    box-shadow:
      0 0 60px var(--orb-glow),
      inset 0 -20px 60px rgba(0,0,0,0.18),
      inset 12px 14px 40px rgba(255,255,255,0.45);
    animation: breathe var(--pulse-dur) var(--ease-soft) infinite;
    transition: background 1.4s var(--ease-soft), box-shadow 1.4s var(--ease-soft);
  }
  /* second shell — only shown when state="together" via class .twinned */
  .orb-shell {
    position: absolute;
    inset: 6%;
    border-radius: 50%;
    background:
      radial-gradient(circle at 65% 70%, transparent 38%, var(--orb-glow) 70%, transparent 95%);
    opacity: 0;
    transition: opacity 1s var(--ease-soft);
    pointer-events: none;
  }
  .orb-wrap.twinned .orb-shell { opacity: 0.75; animation: counter-rotate 14s linear infinite; }

  /* Ripple layer — rendered when motion observed/confirmed */
  .ripple {
    position: absolute;
    border-radius: 50%;
    border: 2px solid var(--orb-glow);
    inset: 0;
    pointer-events: none;
    opacity: 0;
  }
  .ripple.go {
    animation: ripple 1.4s var(--ease-soft) forwards;
  }

  @keyframes breathe {
    0%   { transform: scale(0.985); filter: brightness(0.96); }
    18%  { transform: scale(1.000); filter: brightness(1.00); }
    52%  { transform: scale(1.030); filter: brightness(1.06); }
    74%  { transform: scale(1.005); filter: brightness(1.00); }
    100% { transform: scale(0.985); filter: brightness(0.96); }
  }
  @keyframes ripple {
    0%   { transform: scale(0.80); opacity: 0.55; }
    80%  { transform: scale(1.55); opacity: 0.0; }
    100% { transform: scale(1.65); opacity: 0.0; }
  }
  @keyframes counter-rotate {
    from { transform: rotate(0deg); }
    to   { transform: rotate(-360deg); }
  }

  @media (prefers-reduced-motion: reduce) {
    .orb { animation: none; }
    .orb-wrap.twinned .orb-shell { animation: none; }
    .ripple.go { animation: none; opacity: 0; }
    .plate .state { transition: opacity .25s; transform: none !important; }
    body { transition: none; }
  }

  /* ── 24-hour activity ribbon ────────────────────────────────────────── */

  .ribbon-card {
    margin: 0 auto clamp(18px, 4vh, 36px);
    max-width: 920px;
    padding: 14px 16px 12px;
    border-radius: 18px;
    background: var(--bg-veil);
    -webkit-backdrop-filter: blur(28px) saturate(180%);
    backdrop-filter: blur(28px) saturate(180%);
    border: 1px solid var(--hairline);
    box-shadow: var(--shadow-card);
  }
  .ribbon-head {
    display: flex; justify-content: space-between; align-items: center;
    font-size: 12px; color: var(--fg-mute);
    letter-spacing: 0.06em; text-transform: uppercase;
    margin-bottom: 8px;
  }
  #ribbon { width: 100%; height: 56px; display: block; border-radius: 8px; }

  /* ── Living waveform + calibration dock ─────────────────────────────── */

  .dock {
    margin: 0 auto clamp(28px, 6vh, 56px);
    max-width: 920px;
    padding: 16px 16px 18px;
    border-radius: 22px;
    background: var(--bg-veil);
    -webkit-backdrop-filter: blur(28px) saturate(180%);
    backdrop-filter: blur(28px) saturate(180%);
    border: 1px solid var(--hairline);
    box-shadow: var(--shadow-card);
    display: grid;
    gap: 14px;
  }
  #waveform { width: 100%; height: 96px; display: block; border-radius: 12px; }
  .waveform-legend {
    display: flex; gap: 16px; align-items: center;
    font-size: 12px; color: var(--fg-mute);
    letter-spacing: 0.04em;
  }
  .legend-dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; vertical-align: middle; }
  .legend-dot.motion    { background: var(--orb-3); }
  .legend-dot.breathing { background: var(--accent); }

  .controls {
    display: grid;
    gap: 10px;
    grid-template-columns: 1fr;
  }
  @media (min-width: 720px) {
    .controls { grid-template-columns: auto 1fr auto; align-items: center; }
  }
  .calibrate {
    border: 0;
    border-radius: 14px;
    padding: 14px 22px;
    background: linear-gradient(135deg, var(--orb-3), var(--accent));
    color: white;
    font: inherit;
    font-size: 16px;
    font-weight: 600;
    cursor: pointer;
    box-shadow: 0 4px 18px var(--orb-glow);
    transition: transform .2s var(--ease-spring), box-shadow .2s;
  }
  .calibrate:hover  { transform: translateY(-2px); box-shadow: 0 8px 24px var(--orb-glow); }
  .calibrate:active { transform: translateY(0); }
  .calibrate[data-running="1"] { opacity: .7; pointer-events: none; }

  /* iOS-style segmented control */
  .segmented {
    display: inline-flex;
    background: rgba(0,0,0,0.05);
    padding: 3px;
    border-radius: 12px;
    position: relative;
    user-select: none;
  }
  @media (prefers-color-scheme: dark) { .segmented { background: rgba(255,255,255,0.06); } }
  .segmented button {
    border: 0;
    background: transparent;
    color: var(--fg);
    padding: 8px 14px;
    border-radius: 9px;
    font: inherit;
    font-size: 14px;
    font-weight: 500;
    cursor: pointer;
    transition: color .25s;
    position: relative;
    z-index: 1;
  }
  .segmented button[aria-pressed="true"] {
    color: var(--fg);
  }
  .segmented .indicator {
    position: absolute;
    top: 3px; bottom: 3px;
    background: var(--bg-base);
    border-radius: 9px;
    box-shadow: 0 1px 3px rgba(0,0,0,0.10);
    transition: transform .3s var(--ease-spring), width .3s var(--ease-spring);
    z-index: 0;
  }
  @media (prefers-color-scheme: dark) {
    .segmented .indicator { background: rgba(255,255,255,0.12); }
  }

  .pet-row,
  .qh-row {
    display: inline-flex; gap: 10px; align-items: center;
    font-size: 14px; color: var(--fg-soft);
  }
  .qh-row .qh-times {
    display: inline-flex; gap: 6px; align-items: center;
    font-size: 13px;
  }
  .qh-row .qh-times input[type="time"] {
    border: 1px solid var(--hairline);
    background: var(--bg-veil);
    color: var(--fg);
    padding: 4px 6px;
    border-radius: 6px;
    font: inherit; font-size: 13px;
  }
  .qh-row .qh-arrow { color: var(--fg-mute); }
  .switch {
    position: relative; width: 44px; height: 26px;
    background: rgba(0,0,0,0.10); border-radius: 13px;
    transition: background .25s; cursor: pointer;
  }
  @media (prefers-color-scheme: dark) { .switch { background: rgba(255,255,255,0.10); } }
  .switch[aria-checked="true"] { background: var(--orb-3); }
  .switch::after {
    content: ""; position: absolute; left: 3px; top: 3px;
    width: 20px; height: 20px; background: white; border-radius: 50%;
    box-shadow: 0 1px 3px rgba(0,0,0,0.18);
    transition: transform .2s var(--ease-spring);
  }
  .switch[aria-checked="true"]::after { transform: translateX(18px); }

  details.tinker {
    grid-column: 1 / -1;
    border-top: 1px solid var(--hairline);
    padding-top: 12px;
    color: var(--fg-soft);
    font-size: 14px;
  }
  details.tinker summary {
    cursor: pointer; user-select: none; list-style: none; outline: none;
    padding: 4px 0; font-weight: 500;
  }
  details.tinker summary::-webkit-details-marker { display: none; }
  details.tinker[open] summary::after { content: " ▴"; }
  details.tinker:not([open]) summary::after { content: " ▾"; }
  .tinker-row {
    display: grid; gap: 12px; margin-top: 12px;
    grid-template-columns: 1fr;
  }
  @media (min-width: 720px) { .tinker-row { grid-template-columns: 1fr 1fr; } }
  .tinker-row label { display: flex; flex-direction: column; gap: 6px; }
  .tinker-row input[type="range"] { accent-color: var(--orb-3); }

  /* ── "How is it sensing?" reveal — under the waveform legend so a
     tester can see the live numbers behind the orb without leaving
     the page. Same disclosure pattern as .tinker; renders as a 3-
     column grid (label / raw / steady) on wide screens, single column
     on narrow. The "steady" column carries the smoothed value and a
     ±band annotation so the reader can tell, at a glance, whether
     the room is quiet (small band) or noisy (large band). */
  details.sense-detail {
    border-top: 1px solid var(--hairline);
    padding-top: 12px;
    color: var(--fg-soft);
    font-size: 13px;
  }
  details.sense-detail summary {
    cursor: pointer; user-select: none; list-style: none; outline: none;
    padding: 4px 0; font-weight: 500;
  }
  details.sense-detail summary::-webkit-details-marker { display: none; }
  details.sense-detail[open] summary::after { content: " ▴"; }
  details.sense-detail:not([open]) summary::after { content: " ▾"; }
  .sense-intro {
    margin: 6px 0 10px; color: var(--fg-mute); font-size: 12px; line-height: 1.5;
  }
  .sense-rows {
    display: grid; gap: 6px 14px; align-items: baseline;
    grid-template-columns: 1fr auto;
  }
  @media (min-width: 540px) {
    .sense-rows { grid-template-columns: 1fr auto auto; }
  }
  .sense-rows .label { color: var(--fg-mute); }
  .sense-rows .value { font-variant-numeric: tabular-nums; }
  .sense-rows .value.steady { color: var(--fg-soft); }
  /* "Loudest" / "How sure" / "Right now" / "Breath rate" / "Last event"
     have no third column, so we span the value across columns 2..end.
     Pulled out of inline style="grid-column:2/-1" attributes per
     Gemini's review on PR #423 — separates layout from content. */
  .sense-rows .value.full-width-value { grid-column: 2 / -1; }
  .sense-rows .band {
    color: var(--fg-mute); font-size: 11px; margin-left: 4px;
    font-variant-numeric: tabular-nums;
  }
  /* When .band has no content (steady reading, RMS rounds to 0), keep
     it from reserving a hairline of margin/space to the right of the
     steady value. :empty matches when the inner textContent is the
     empty string, which is exactly the state fmtBand() produces for a
     calm room. */
  .sense-rows .band:empty { margin-left: 0; }
  /* On the narrow layout, the third column folds under the second.
     We tag the band with a class so the grid-template-columns:1fr auto
     query still leaves it readable. */
  @media (max-width: 539px) {
    .sense-rows .value.steady::before { content: "  ·  "; color: var(--fg-mute); }
  }

  /* ── Sheets (Today + What-it-sees) ──────────────────────────────────── */

  .sheet-scrim {
    position: fixed; inset: 0;
    background: rgba(0,0,0,0.34);
    opacity: 0; pointer-events: none;
    transition: opacity .25s;
    z-index: 100;
  }
  .sheet-scrim.open { opacity: 1; pointer-events: auto; }
  .sheet {
    position: fixed; left: 0; right: 0; bottom: 0;
    max-height: 80vh;
    background: var(--bg-base);
    border-radius: 24px 24px 0 0;
    transform: translateY(100%);
    transition: transform .35s var(--ease-soft);
    overflow: hidden;
    display: flex; flex-direction: column;
    box-shadow: 0 -8px 40px rgba(0,0,0,0.18);
    z-index: 101;
  }
  .sheet.open { transform: translateY(0); }
  .sheet-grab {
    width: 38px; height: 4px; border-radius: 2px;
    background: var(--fg-mute); opacity: .4;
    margin: 10px auto 8px;
  }
  .sheet-head {
    padding: 6px 22px 14px;
    display: flex; align-items: center; justify-content: space-between;
    border-bottom: 1px solid var(--hairline);
  }
  .sheet-head h2 { margin: 0; font-size: 18px; letter-spacing: -0.01em; }
  /* Close button is the keyboard-accessible counterpart to the
     scrim's click-out — needed for screen-reader / keyboard users
     who can't tap outside. Sized to mirror .iconbtn so it doesn't
     visually fight the head layout. */
  .sheet-close {
    width: 32px; height: 32px; padding: 0;
    font-size: 22px; line-height: 1;
    margin-left: 8px;
  }
  .privacy-pill {
    font-size: 11px; color: var(--fg-mute);
    background: var(--bg-veil);
    border: 1px solid var(--hairline);
    padding: 5px 10px; border-radius: 999px;
    letter-spacing: 0.02em;
  }
  .privacy-pill.warm { color: #b87800; }
  /* Mic pill: same quiet-pill look as .privacy-pill, but always in the
     topbar — the mic's on/off state is privacy-relevant and must be
     visible at a glance, not buried in a sheet. Hidden until
     /api/audio/status confirms the mic is built into this firmware. */
  .mic-pill {
    font-size: 11px; color: var(--fg-mute);
    background: var(--bg-veil);
    border: 1px solid var(--hairline);
    padding: 5px 10px; border-radius: 999px;
    letter-spacing: 0.02em; cursor: pointer;
  }
  .mic-pill.muted { color: #b87800; }
  .sheet-body {
    flex: 1 1 auto;
    overflow-y: auto;
    padding: 14px 22px 30px;
  }
  .event-row {
    display: grid;
    grid-template-columns: 1fr auto;
    align-items: center;
    gap: 12px;
    padding: 12px 0;
    border-bottom: 1px solid var(--hairline);
    transition: opacity .25s, transform .25s var(--ease-soft);
  }
  .event-row.dismissed { opacity: 0.4; }
  .event-row .label { font-weight: 500; }
  .event-row .when  { color: var(--fg-soft); font-size: 13px; }
  .event-row .dismiss-btn {
    border: 1px solid var(--hairline);
    background: var(--bg-veil);
    color: var(--fg-soft);
    padding: 6px 10px; border-radius: 8px;
    font: inherit; font-size: 12px; cursor: pointer;
  }
  .summary-card {
    margin-top: 16px;
    padding: 14px 16px;
    border: 1px solid var(--hairline);
    border-radius: 14px;
    background: var(--bg-veil);
    color: var(--fg-soft);
  }
  .summary-card h3 { margin: 0 0 6px; font-size: 14px; color: var(--fg); letter-spacing: 0.04em; text-transform: uppercase; }

  .what-rows { display: grid; gap: 10px; }
  .what-row {
    display: grid; grid-template-columns: 1fr 1fr;
    gap: 12px; padding: 10px 0;
    border-bottom: 1px solid var(--hairline);
  }
  .what-row .scenario  { font-weight: 500; }
  .what-row .capability { color: var(--fg-soft); }
  .what-foot { margin-top: 14px; color: var(--fg-mute); font-size: 13px; }

  /* ── Fleet sheet ──────────────────────────────────────────────────────── */

  .fleet-section { margin-bottom: 24px; }
  .fleet-section:last-child { margin-bottom: 0; }
  .fleet-label {
    margin: 0 0 10px; font-size: 13px; font-weight: 600;
    text-transform: uppercase; letter-spacing: 0.04em;
    color: var(--fg-mute);
  }
  .fleet-hint {
    margin: 0 0 14px; font-size: 14px; color: var(--fg-soft); line-height: 1.45;
  }
  .fleet-peer-list { display: grid; gap: 8px; }
  .fleet-peer {
    display: grid; grid-template-columns: 1fr auto;
    align-items: center; gap: 12px;
    padding: 10px 14px;
    border: 1px solid var(--hairline);
    border-radius: 12px;
    background: var(--bg-veil);
  }
  .fleet-peer .name { font-weight: 500; }
  .fleet-peer .meta { font-size: 12px; color: var(--fg-mute); }
  .fleet-peer .rssi-bar {
    display: inline-flex; gap: 2px; align-items: flex-end; height: 16px;
  }
  .rssi-bar span {
    width: 3px; border-radius: 1px;
    background: var(--fg-mute); opacity: .25;
  }
  .rssi-bar span.on { opacity: 1; background: var(--accent); }
  .fleet-peer-empty { color: var(--fg-mute); font-size: 14px; padding: 8px 0; }
  .fleet-open {
    color: var(--accent); text-decoration: none; font-size: 14px;
    padding: 6px 10px; border: 1px solid var(--hairline); border-radius: 10px;
    white-space: nowrap;
  }
  .fleet-form { display: grid; gap: 12px; }
  .fleet-field {
    display: grid; gap: 4px;
    font-size: 13px; font-weight: 500; color: var(--fg-soft);
  }
  .fleet-field input {
    border: 1px solid var(--hairline);
    border-radius: 10px; padding: 10px 14px;
    background: var(--bg-veil);
    color: var(--fg); font: inherit; font-size: 15px;
    outline: none; transition: border-color .2s;
  }
  .fleet-field input:focus { border-color: var(--accent); }
  /* Password-shaped but NOT an account credential: mask without
     type=password so iOS never offers to invent a new password here
     (firmware/LESSONS_LEARNED.md, "iOS offers to invent a password"). */
  .pw-masked { -webkit-text-security: disc; text-security: disc; }
  .fleet-qr-wrap {
    margin-top: 18px; text-align: center;
  }
  .fleet-qr-img {
    display: inline-block; width: 200px; height: 200px;
    border-radius: 12px; overflow: hidden;
    background: #fff;
  }
  .fleet-qr-img svg { width: 100%; height: 100%; }
  .fleet-qr-caption {
    margin: 10px 0 0; font-size: 13px; color: var(--fg-mute);
  }

  /* ── Tooltips (one pattern, used everywhere via data-tip) ───────────── */

  .tip-bubble {
    position: absolute; z-index: 200;
    max-width: 240px; padding: 8px 12px;
    background: var(--fg);
    color: var(--bg-base);
    font-size: 12.5px; line-height: 1.35;
    border-radius: 10px;
    box-shadow: 0 6px 18px rgba(0,0,0,0.22);
    transform: translateY(-6px);
    opacity: 0; pointer-events: none;
    transition: opacity .15s, transform .15s var(--ease-soft);
  }
  .tip-bubble.show { opacity: 0.96; transform: translateY(0); }
  .tip-bubble::after {
    content: "";
    position: absolute; top: 100%; left: 50%;
    border: 5px solid transparent;
    border-top-color: var(--fg);
    transform: translateX(-50%);
  }

  /* ── Misc ──────────────────────────────────────────────────────────── */

  .visually-hidden {
    position: absolute; width: 1px; height: 1px;
    padding: 0; margin: -1px; overflow: hidden;
    clip: rect(0,0,0,0); border: 0;
  }
  /* ── First-run welcome overlay (4 cards) ───────────────────────────────
   *
   * Full-screen frosted-glass mask shown on the user's first visit, gated
   * by localStorage('csi.onboarding.done'). Teaches the four things from
   * the plan's "first-run explainer" before handing off to calibration:
   *
   *   1. "Your camera-free sixth sense" + optional Pets question.
   *   2. "WiFi waves, not video. No MACs, no cloud."
   *   3. "Best in the same room. Through one wall: motion only…"
   *   4. "Step out for 60 s." (Calibration handoff.)
   *
   * The orb, hero plate, and ambient theme keep rendering underneath the
   * overlay — when the user dismisses, the dashboard is already alive
   * without a load flash. */
  .welcome-mask {
    position: fixed; inset: 0;
    background: var(--bg-veil);
    -webkit-backdrop-filter: blur(36px) saturate(170%);
    backdrop-filter: blur(36px) saturate(170%);
    z-index: 110;
    opacity: 0; pointer-events: none;
    transition: opacity .35s var(--ease-soft);
    display: grid; place-items: center;
    padding: 24px;
  }
  body.is-onboarding .welcome-mask { opacity: 1; pointer-events: auto; }
  /* Card 3's "Learn more" opens the What-it-sees sheet over the welcome
   * mask. The welcome lives at z-index 110 and the sheet at 101, so we
   * need to step the welcome out of the way while the sheet is up. The
   * welcome stays in DOM (state preserved) and the user is returned
   * to it when the sheet is dismissed. */
  body.welcome-paused .welcome-mask { opacity: 0; pointer-events: none; }

  .welcome-card {
    width: min(440px, 100%);
    max-height: 80vh;
    background: var(--bg-base);
    border: 1px solid var(--hairline);
    border-radius: 22px;
    box-shadow: 0 18px 52px rgba(0,0,0,0.18), 0 2px 8px rgba(0,0,0,0.06);
    padding: 28px 26px 22px;
    display: grid;
    gap: 14px;
    text-align: center;
    transform: translateY(8px) scale(0.98);
    opacity: 0;
    transition: opacity .3s var(--ease-soft), transform .35s var(--ease-spring);
  }
  body.is-onboarding .welcome-card.show {
    opacity: 1; transform: translateY(0) scale(1);
  }
  .welcome-card .step {
    font-size: 11px; letter-spacing: 0.12em; color: var(--fg-mute);
    text-transform: uppercase;
  }
  .welcome-card h2 {
    font-size: clamp(22px, 5vw, 28px);
    font-weight: 600;
    letter-spacing: -0.02em;
    margin: 0;
  }
  .welcome-card p {
    margin: 0;
    color: var(--fg-soft);
    font-size: 16px;
    line-height: 1.45;
  }
  .welcome-card .learn-more {
    color: var(--orb-3);
    text-decoration: none;
    font-weight: 500;
    font-size: 14px;
  }
  .welcome-card .learn-more:hover { text-decoration: underline; }
  .welcome-card .pet-hint {
    font-size: 12px; color: var(--fg-mute);
    margin: 0 0 -4px;
  }
  .welcome-card .pet-choices {
    display: grid; grid-template-columns: repeat(2, 1fr); gap: 8px;
    margin-top: 4px;
  }
  .welcome-card .pet-choices button {
    position: relative;
    border: 1px solid var(--hairline);
    background: var(--bg-veil);
    color: var(--fg);
    padding: 10px 30px 10px 14px; /* right-pad for the check pip */
    border-radius: 10px;
    font: inherit; font-size: 14px; font-weight: 500;
    cursor: pointer;
    text-align: left;
    transition: transform .15s var(--ease-spring), border-color .2s, background .2s;
  }
  .welcome-card .pet-choices button:hover  { transform: translateY(-1px); border-color: var(--orb-3); }
  /* Empty checkbox pip in every chip — gets a check when selected. */
  .welcome-card .pet-choices button::after {
    content: "";
    position: absolute; right: 10px; top: 50%;
    width: 16px; height: 16px;
    transform: translateY(-50%);
    border: 1.5px solid var(--fg-mute);
    border-radius: 4px;
    background: transparent;
    transition: background .15s, border-color .15s;
  }
  .welcome-card .pet-choices button[aria-pressed="true"] {
    border-color: var(--orb-3);
    background: linear-gradient(135deg, rgba(140,158,255,0.10), rgba(30,197,177,0.10));
  }
  .welcome-card .pet-choices button[aria-pressed="true"]::after {
    background: var(--orb-3);
    border-color: var(--orb-3);
    /* SVG checkmark in white as a background-image on the pip */
    background-image: url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><path fill='none' stroke='white' stroke-width='2.4' stroke-linecap='round' stroke-linejoin='round' d='M3.5 8.5l3 3 6-6.5'/></svg>");
    background-repeat: no-repeat;
    background-position: center;
  }
  .welcome-card .actions {
    display: grid; grid-template-columns: auto 1fr; gap: 10px; margin-top: 8px;
  }
  .welcome-card .actions .skip {
    background: transparent;
    border: 0;
    color: var(--fg-mute);
    font: inherit; font-size: 14px;
    cursor: pointer;
    padding: 12px 8px;
  }
  .welcome-card .actions .skip:hover { color: var(--fg-soft); }
  .welcome-card .actions .primary {
    border: 0;
    border-radius: 12px;
    padding: 12px 18px;
    background: linear-gradient(135deg, var(--orb-3), var(--accent));
    color: white;
    font: inherit; font-size: 16px; font-weight: 600;
    cursor: pointer;
    box-shadow: 0 4px 14px var(--orb-glow);
    transition: transform .15s var(--ease-spring);
  }
  .welcome-card .actions .primary:hover { transform: translateY(-1px); }
  .welcome-card .dots {
    display: flex; gap: 6px; justify-content: center; margin-top: 4px;
  }
  .welcome-card .dot {
    width: 6px; height: 6px; border-radius: 50%;
    background: var(--fg-mute); opacity: 0.4;
    transition: opacity .25s, transform .25s var(--ease-spring);
  }
  .welcome-card .dot.active { opacity: 1; background: var(--orb-3); transform: scale(1.4); }

  @media (prefers-reduced-motion: reduce) {
    .welcome-card { transition: opacity .2s; transform: none !important; }
  }

  /* Calibration overlay that desaturates the page during baseline capture */
  .calibrating-mask {
    position: fixed; inset: 0;
    background: rgba(255,255,255,0.6);
    backdrop-filter: blur(6px) saturate(60%);
    -webkit-backdrop-filter: blur(6px) saturate(60%);
    z-index: 90; opacity: 0; pointer-events: none;
    transition: opacity .35s;
    display: grid; place-items: center;
    text-align: center;
  }
  body.is-calibrating .calibrating-mask { opacity: 1; pointer-events: auto; }
  .calibrating-mask .label { font-size: 18px; color: var(--fg); }
  .calibrating-mask .count { font-size: 56px; font-weight: 600; color: var(--fg); margin-top: 8px; letter-spacing: -0.02em; }
  /* The "ready" state replaces the running countdown with a small
   * proposed-vs-current diff and accept/cancel buttons. Same mask, two
   * mutually-exclusive panels controlled by data-state on the wrapper. */
  .calib-running, .calib-ready { display: none; }
  .calibrating-mask[data-state="running"] .calib-running { display: block; }
  .calibrating-mask[data-state="ready"]   .calib-ready   { display: block; }
  .calib-ready { max-width: 320px; margin: 0 auto; }
  .calib-ready h3 { font-size: 22px; font-weight: 500; margin: 0 0 8px; color: var(--fg); }
  .calib-ready .row { display: flex; justify-content: space-between; padding: 6px 0; font-size: 14px; color: var(--fg); }
  .calib-ready .row .now { color: var(--fg-mute); text-decoration: line-through; margin-right: 8px; }
  .calib-ready .row .next { font-weight: 500; color: var(--orb-3); }
  .calib-ready .actions { display: flex; gap: 12px; margin-top: 18px; justify-content: center; }
  .calib-ready button { font: inherit; padding: 10px 22px; border-radius: 12px; border: 0; cursor: pointer; }
  .calib-ready .accept { background: var(--orb-3); color: #1a1605; font-weight: 500; }
  .calib-ready .cancel { background: transparent; color: var(--fg-mute); border: 1px solid var(--fg-mute); }
  @media (prefers-color-scheme: dark) { .calibrating-mask { background: rgba(0,0,0,0.55); } }

  /* Forced-colors mode (Windows High Contrast, accessibility tools).
   * Browsers replace most custom colors with the user's chosen
   * system palette here, which collapses our state-encoded gradients
   * to a flat Canvas color and erases the orb's visual differentiation.
   * The dashboard already announces state textually via aria-live on
   * #state, so SR users still know what's happening — but sighted
   * forced-colors users need explicit borders + system colors so the
   * UI doesn't dissolve into a uniform field with invisible controls.
   *
   * Strategy:
   *   - Use CSS system colors (Canvas, CanvasText, Highlight,
   *     ButtonText) so the dashboard inherits the user's chosen
   *     palette instead of fighting it.
   *   - Add explicit 1-2px borders on interactive controls so they
   *     remain identifiable as controls when their backgrounds
   *     flatten.
   *   - Use Highlight / HighlightText for the active mode segment
   *     and pressed switches so state is preserved.
   *   - Outline-based focus rings stay visible because outlines are
   *     preserved in forced-colors mode (unlike box-shadow rings).
   *
   * Reference: WCAG 2.1 SC 1.4.11 (Non-text Contrast) + W3C CSS Color
   * Adjust Module §3 (forced-colors). */
  @media (forced-colors: active) {
    /* The orb is purely visual — but a sighted forced-colors user
       still expects to see SOMETHING in the hero. Render it as a
       bordered disc so the layout doesn't collapse, and let
       CanvasText form a clear edge against Canvas. */
    .orb {
      background: Canvas;
      border: 2px solid CanvasText;
      box-shadow: none;
    }
    .orb-shell, .ripple { display: none; }

    /* All interactive controls get explicit ButtonText borders so
       they remain identifiable when custom backgrounds flatten. */
    .iconbtn, .calibrate, .help, .switch, .sheet-close,
    .welcome-card .skip, .welcome-card .primary,
    .welcome-card .pet-choices button,
    .dismiss-btn {
      border: 1px solid ButtonText;
    }

    /* Active state for the mode segments and toggled switches uses
       Highlight so it visibly differs from the inactive treatment.
       HighlightText pairs with Highlight for legibility on the user's
       chosen accent color. */
    .segmented button[aria-pressed="true"],
    .switch[aria-checked="true"],
    .welcome-card .pet-choices button[aria-pressed="true"] {
      background: Highlight;
      color: HighlightText;
      border-color: Highlight;
    }

    /* Sheets and the welcome card need visible borders since their
       backgrounds and backdrop blur flatten away. */
    .sheet, .welcome-card {
      border: 2px solid CanvasText;
      background: Canvas;
    }

    /* Focus ring: switch from the project's box-shadow / colored-
       border idiom to a 2px outline since outlines are guaranteed
       to render in forced-colors mode. */
    :focus-visible {
      outline: 2px solid CanvasText;
      outline-offset: 2px;
    }

    /* The disconnected plate's dim treatment uses opacity which
       forced-colors UAs may flatten back to 1.0. Drop a visible
       GrayText cue so the "something's wrong" signal isn't lost. */
    body.disconnected #frameCount {
      color: GrayText;
    }
  }
</style>
</head>
<body>

<header class="topbar">
  <div class="brand">SecuraCV<span class="device" id="device-id">canary</span></div>
  <div class="topbar-actions">
    <button class="mic-pill" id="micPill" style="display:none"
            aria-label="Microphone on or muted — tap to switch">Mic on</button>
    <button class="iconbtn" id="todayBtn" data-tip="todayBtn">Today</button>
    <button class="iconbtn" id="fleetBtn" data-tip="fleetBtn">Fleet</button>
    <button class="iconbtn" id="settingsBtn" data-tip="settingsBtn">Settings</button>
  </div>
</header>

<main>
  <section class="hero">
    <div class="plate">
      <!-- aria-live on the headline so screen readers announce state
           transitions ("Empty", "Active", …) without the user having
           to refocus. polite (not assertive) because the canary's
           state changes shouldn't interrupt; atomic so the whole
           string is read on each change rather than diff'd. -->
      <h1 class="state" id="state" aria-live="polite" aria-atomic="true">Sensing…</h1>
      <p class="sub"   id="sub">Just getting a feel for the room.</p>
      <p class="meta"  id="meta">tentative</p>
      <button class="help" id="helpBtn" aria-label="What can the sensor see?" data-tip="helpBtn">?</button>
    </div>
    <!-- The orb is a pure visual mirror of the headline's state.
         aria-hidden keeps it out of the AT tree so screen-reader
         users hear "Empty" once instead of "Empty … decorative
         circle". -->
    <div class="orb-wrap" id="orbWrap" data-tip="orb" aria-hidden="true">
      <div class="orb"        id="orb"></div>
      <div class="orb-shell"  id="orbShell"></div>
      <div class="ripple"     id="ripple"></div>
    </div>
  </section>

  <section class="ribbon-card" aria-label="Last 24 hours activity">
    <div class="ribbon-head">
      <span>Last 24 hours</span>
      <span id="ribbonReadout" aria-live="polite">—</span>
    </div>
    <!-- Canvas is a visualization; the readout span carries the
         spoken summary. Hide the canvas itself from AT. -->
    <canvas id="ribbon" width="800" height="56" aria-hidden="true"></canvas>
  </section>

  <section class="dock">
    <canvas id="waveform" width="800" height="96" aria-hidden="true"></canvas>
    <div class="waveform-legend">
      <span><span class="legend-dot motion" aria-hidden="true"></span>motion</span>
      <span><span class="legend-dot breathing" aria-hidden="true"></span>breathing</span>
      <!-- frameCount doubles as the disconnect-message surface
           (PR #399). aria-live so the diagnostic ("Session ended.
           Tap to pair…", "No reply from the canary…") reaches
           screen-reader users without a manual refocus. -->
      <span style="margin-left:auto" id="frameCount" aria-live="polite" role="status">—</span>
    </div>

    <!-- Sense-detail reveal: the live numbers a tester / curious user
         wants to see behind the orb. Updated on every poll alongside
         the waveform. Wrapped in a <details> so the default state is
         collapsed and casual users never see it. role="region" with
         a stable aria-label so screen-reader users can land on it
         via heading navigation. -->
    <details class="sense-detail" id="senseDetail">
      <summary data-tip="senseDetail">How is it sensing?</summary>
      <p class="sense-intro">A peek at the live numbers behind the orb. The "steady" reading is what the canary trusts — the moment-to-moment number wobbles, especially in noisy rooms.</p>
      <div class="sense-rows" role="region" aria-label="Live sensing detail" aria-live="off">
        <span class="label">Movement</span>
        <span class="value" id="seMRaw">—</span>
        <!-- The "steady" cell holds two sibling spans (value + band) so
             the per-tick setText writes can target each independently.
             textContent on the outer container would replace both
             children. -->
        <span class="value steady"><span id="seMSteady">—</span><span class="band" id="seMBand"></span></span>

        <span class="label">Breath</span>
        <span class="value" id="seBRaw">—</span>
        <span class="value steady"><span id="seBSteady">—</span><span class="band" id="seBBand"></span></span>

        <span class="label">Loudest</span>
        <span class="value full-width-value" id="seDom">—</span>

        <span class="label">How sure</span>
        <span class="value full-width-value" id="seConf">—</span>

        <span class="label">Right now</span>
        <span class="value full-width-value" id="seState">—</span>

        <span class="label">Breath rate</span>
        <span class="value full-width-value" id="seBpm">—</span>

        <span class="label">Last event</span>
        <span class="value full-width-value" id="seEvent">—</span>
      </div>
    </details>

    <div class="controls">
      <button class="calibrate" id="calibrateBtn" data-tip="calibrate">Calibrate empty room</button>
      <div class="segmented" role="radiogroup" aria-label="Sensitivity mode" id="modeGroup">
        <span class="indicator" id="modeIndicator"></span>
        <button role="radio" data-mode="sensitive" aria-pressed="false" data-tip="sensitive">Sensitive</button>
        <button role="radio" data-mode="balanced"  aria-pressed="true"  data-tip="balanced">Balanced</button>
        <button role="radio" data-mode="quiet"     aria-pressed="false" data-tip="quiet">Quiet</button>
      </div>
      <div class="pet-row">
        <span>Pet Mode</span>
        <span class="switch" id="petSwitch" role="switch" aria-checked="false" tabindex="0" data-tip="petMode"></span>
      </div>
      <div class="qh-row">
        <span>Quiet hours</span>
        <span class="switch" id="qhSwitch" role="switch" aria-checked="false" tabindex="0" data-tip="quietHours"></span>
        <span class="qh-times" id="qhTimes" hidden>
          <input type="time" id="qhStart" value="23:00" data-tip="quietHoursStart">
          <span class="qh-arrow" aria-hidden="true">→</span>
          <input type="time" id="qhEnd"   value="07:00" data-tip="quietHoursEnd">
        </span>
      </div>

      <details class="tinker">
        <summary>Details</summary>
        <div class="tinker-row">
          <label>
            <span>Sensitivity <span data-tip="sensitivity">ⓘ</span></span>
            <input type="range" id="sensitivitySlider" min="0" max="100" value="50">
          </label>
          <label style="grid-column:1/-1">
            <span>Live numbers <span data-tip="rawVector">ⓘ</span></span>
            <canvas id="rawHeatmap" width="800" height="32" style="width:100%;height:32px;border-radius:6px;display:block;background:rgba(0,0,0,0.04)" aria-hidden="true"></canvas>
          </label>
        </div>
      </details>
    </div>
  </section>
</main>

<!-- Today receipts sheet.
     role/aria-modal upgrade the <aside> to a real modal dialog so
     screen readers announce it as such; openSheet (in JS) handles
     focus capture + restore + Tab cycling + ESC. The scrim is purely
     visual click-out; aria-hidden keeps it out of the AT tree. -->
<div class="sheet-scrim" id="todayScrim" aria-hidden="true"></div>
<aside class="sheet" id="todaySheet" role="dialog" aria-modal="true" aria-labelledby="todayTitle">
  <div class="sheet-grab" aria-hidden="true"></div>
  <div class="sheet-head">
    <h2 id="todayTitle">Today</h2>
    <span class="privacy-pill" id="privacyPill">Today: 0 bytes left the device</span>
    <span class="privacy-pill" id="sharePill">Sharing: off — nothing leaves unless you turn it on</span>
    <button class="iconbtn sheet-close" data-sheet="today" aria-label="Close Today">×</button>
  </div>
  <div class="sheet-body" id="todayBody">
    <p style="color:var(--fg-mute)">Loading…</p>
  </div>
</aside>

<!-- "What it can / can't see" sheet -->
<div class="sheet-scrim" id="whatScrim" aria-hidden="true"></div>
<aside class="sheet" id="whatSheet" role="dialog" aria-modal="true" aria-labelledby="whatTitle">
  <div class="sheet-grab" aria-hidden="true"></div>
  <div class="sheet-head">
    <h2 id="whatTitle">What the sensor can and can't see</h2>
    <button class="iconbtn sheet-close" data-sheet="what" aria-label="Close What the sensor can and can't see">×</button>
  </div>
  <div class="sheet-body" id="whatBody"></div>
</aside>

<!-- Fleet provisioning sheet -->
<div class="sheet-scrim" id="fleetScrim" aria-hidden="true"></div>
<aside class="sheet" id="fleetSheet" role="dialog" aria-modal="true" aria-labelledby="fleetTitle">
  <div class="sheet-grab" aria-hidden="true"></div>
  <div class="sheet-head">
    <h2 id="fleetTitle">Fleet</h2>
    <button class="iconbtn sheet-close" data-sheet="fleet" aria-label="Close Fleet">&#xd7;</button>
  </div>
  <div class="sheet-body" id="fleetBody">
    <section class="fleet-section" id="fleetPeersSection">
      <h3 class="fleet-label">Canaries on this network</h3>
      <div id="fleetPeerList" class="fleet-peer-list">
        <p style="color:var(--fg-mute)">Loading&hellip;</p>
      </div>
    </section>
    <section class="fleet-section">
      <h3 class="fleet-label">Provision a new Canary</h3>
      <p class="fleet-hint">Generate a QR code that a new Canary can scan to join your WiFi.</p>
      <div class="fleet-form">
        <label class="fleet-field">
          <span>Network name</span>
          <input type="text" id="fleetSsid" maxlength="32" autocomplete="off" spellcheck="false">
        </label>
        <label class="fleet-field">
          <span>Password</span>
          <input type="text" class="pw-masked" id="fleetPass" maxlength="63" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
        </label>
        <button class="calibrate" id="fleetGenBtn" style="margin-top:4px">Generate QR</button>
      </div>
      <div id="fleetQrWrap" class="fleet-qr-wrap" style="display:none">
        <div id="fleetQrImg" class="fleet-qr-img"></div>
        <p class="fleet-qr-caption">Point a new Canary's camera at this code.</p>
      </div>
    </section>
    <section class="fleet-section">
      <h3 class="fleet-label">Something wrong? Scan for the fix</h3>
      <p class="fleet-qr-caption">One code opens the Help Desk on the exact fix for this Canary's current state &mdash; safe mode, hub trouble, or a failing self-test.</p>
      <button class="calibrate" id="helpQrBtn" style="margin-top:4px">Show Help QR</button>
      <div id="helpQrWrap" class="fleet-qr-wrap" style="display:none">
        <div id="helpQrImg" class="fleet-qr-img"></div>
        <p class="fleet-qr-caption">Scan with your phone's camera.</p>
      </div>
    </section>
  </div>
</aside>

<!-- First-run welcome overlay -->
<div class="welcome-mask" id="welcomeMask">
  <!-- tabindex="-1" makes the card focusable via .focus() so render()
       can move focus to it on every step transition; AT then announces
       the dialog's accessible name (aria-labelledby) and the new card
       title without trapping the user on a non-interactive element
       (Tab cycles into the first interactive child). PR #402 review
       r3214577894. -->
  <div class="welcome-card" id="welcomeCard" role="dialog" aria-modal="true" aria-labelledby="welcomeTitle" tabindex="-1"></div>
</div>

<!-- Calibration overlay -->
<div class="calibrating-mask" id="calibratingMask" data-state="running">
  <div class="calib-running">
    <div class="label">Learning your empty room.</div>
    <div class="count" id="calibrateCount">10</div>
    <div class="label" style="margin-top:8px">Step out for ten seconds.</div>
  </div>
  <div class="calib-ready">
    <h3>Got it.</h3>
    <p class="label" style="margin-bottom:10px">Here is what the canary suggests:</p>
    <div class="row">
      <span>Motion</span>
      <span><span class="now" id="calibCurMotion">—</span><span class="next" id="calibNextMotion">—</span></span>
    </div>
    <div class="row">
      <span>Active</span>
      <span><span class="now" id="calibCurActive">—</span><span class="next" id="calibNextActive">—</span></span>
    </div>
    <div class="row">
      <span>Breathing</span>
      <span><span class="now" id="calibCurBreath">—</span><span class="next" id="calibNextBreath">—</span></span>
    </div>
    <div class="actions">
      <button class="cancel" id="calibCancelBtn">Keep current</button>
      <button class="accept" id="calibAcceptBtn">Use these</button>
    </div>
  </div>
</div>

<script>
"use strict";

/* ────────────────────────────────────────────────────────────────────────
 *  Safe localStorage accessors. On browsers configured to block all site
 *  data (Chrome "Block all cookies", some private/embedded webviews) ANY
 *  localStorage property access throws SecurityError. The dashboard reads
 *  localStorage at top level (setMode, sensitivity, pet mode, onboarding),
 *  so one such throw killed every binding declared after it — the orb froze
 *  on "Sensing…" with no polling. Routing all access through these helpers
 *  means a locked-down browser degrades to in-memory defaults instead of a
 *  dead page. Declared as hoisted functions so they precede every caller.
 * ──────────────────────────────────────────────────────────────────────── */
function lsGet(key) {
  try { return localStorage.getItem(key); } catch (_) { return null; }
}
function lsSet(key, val) {
  try { localStorage.setItem(key, val); } catch (_) { /* storage blocked */ }
}

/* ────────────────────────────────────────────────────────────────────────
 *  Auth helper — every CSI HTTP handler verifies an HttpOnly cv_session
 *  cookie set by handle_ui after the user opens the pair landing. Browsers
 *  send the cookie automatically with every same-origin fetch, so cvFetch
 *  is just a pass-through — no headers to set, no token to embed in the
 *  page (which used to leak via view-source on the SoftAP, see PR #392
 *  review r3213361582). If the cookie is missing or expired the request
 *  401s and pollStream's catch block paints the disconnect plate; the
 *  user reloads /, lands on the pair page, and re-pairs in one tap.
 * ──────────────────────────────────────────────────────────────────────── */
function cvFetch(url, opts) {
  /* Reserved as the single fetch surface so adding cookie variants
   * (refresh-after-401, retry-once, etc.) only touches one place. */
  return fetch(url, opts);
}

/* ────────────────────────────────────────────────────────────────────────
 *  Microcopy bank — all user-facing strings live here.
 *  Reading-grade target ≤ 6th. Localization is a one-file edit.
 * ──────────────────────────────────────────────────────────────────────── */
const COPY = {
  states: {
    /* Each subtitle pairs the room reading with one tiny canary beat.
     * The bird is a deadpan presence — head tilts, preens, hums along —
     * not a cartoon. Same data, more alive. */
    sensing:   { name: 'Sensing…',       sub: "Tilting its head, taking the room in." },
    empty:     { name: 'Empty',          sub: "Nobody home. The canary preens." },
    subtle:    { name: 'Subtle motion',  sub: "Something small stirs. Head turns." },
    quiet:     { name: 'Quiet',          sub: "Someone's here, very still. The canary hums." },
    quiet_bpm: { name: 'Quiet',          sub: "Someone's here, breathing about {bpm} a minute. The canary hums along." },
    active:    { name: 'Active',         sub: "Plenty going on. Canary's wide awake." },
    together:  { name: 'Together',       sub: "More than one. The canary perks up." },
    pet:       { name: 'Empty',          sub: "A small movement. Probably your pet — the canary doesn't fuss." },
  },
  tooltips: {
    orb:         "What the room feels like right now.",
    helpBtn:     "See what the sensor can and can't notice.",
    todayBtn:    "See everything that happened today.",
    fleetBtn:    "See your fleet and set up new Canaries.",
    settingsBtn: "Tweak how the sensor behaves.",
    calibrate:   "Step out for one minute. The canary learns your empty room by ear.",
    sensitive:   "Picks up small movements. Best for one quiet room.",
    balanced:    "Catches normal movement. Good for most homes.",
    quiet:       "Only big movements. Best with kids, pets, or open spaces.",
    petMode:     "Cats and small dogs breathe faster than people. Turn this on so they don't trigger 'someone's here'.",
    quietHours:      "Hide late-night events from the ribbon. Movement still folds into a gentle nightly summary.",
    quietHoursStart: "When quiet hours begin.",
    quietHoursEnd:   "When quiet hours end.",
    sensitivity: "Slide right to notice more. Slide left to ignore tiny movements.",
    rawVector:   "For tinkerers. Shows the live numbers behind the scenes.",
    breathAudio: "Play a soft breath sound that follows the rhythm in the room. Off by default.",
    ribbonCell:  "Tap a moment to see what was happening then.",
    senseDetail: "Live numbers behind the orb — what the canary is hearing right now.",
  },
  errors: {
    /* The pollStream catch block picks one of these based on the actual
     * failure mode so installers can self-diagnose without serial cable
     * (audit punch-list #9). frameCount footer is the surface; the
     * disconnect class on <body> dims the orb the same way regardless. */
    disconnect:  "Can't reach the canary. Move closer to your router and try again.",
    unavailable: "Sensing is offline. The canary's radio is not running.",
    unauthorized:"Session ended. Tap to pair the canary again.",
    serverError: "The canary hit a snag. It will try again.",
    timeout:     "No reply from the canary. Trying again…",
  },
  calibrate: {
    /* The overlay's static text + the button labels. The seconds-remaining
     * count is rendered into its own <div class="count"> sibling and is
     * NOT a placeholder in these strings. */
    label:    "The canary is learning your empty room.",
    stepOut:  "Step out for ten seconds.",
    btn:      "Calibrate empty room",
    done:     "Got it. The canary knows your room now.",
    error:    "Sensing is offline. Try again in a moment.",
  },
  today: {
    empty: "Quiet so far today. The canary is perched, head cocked.",
  },
  what: {
    title: "What the sensor can and can't see",
    rows: [
      { scenario: "Same room, person is still",          capability: "Both motion and breathing." },
      { scenario: "Same room, through one wall",         capability: "Motion is reliable. Breathing softens to 'subtle motion'." },
      { scenario: "One floor up or down (wood)",         capability: "Motion only. No breathing claims." },
      { scenario: "One floor up or down (concrete)",     capability: "Same floor only." },
      { scenario: "Two or more people present",          capability: "We say 'Together'. We can't count exactly." },
      { scenario: "Cats and small dogs",                 capability: "Their motion looks like ours. Pet Mode hides 'someone's here' for them." },
      { scenario: "People walking past your window",     capability: "May trigger motion. Calibrate when nobody's home so the sensor learns what's normal." },
    ],
    foot: "No camera. No microphone. No MAC addresses. Nothing leaves the device.",
  },
  welcome: {
    cards: [
      {
        step: "Step 1 of 4",
        title: "Your camera-free sixth sense for the home.",
        body: "This little canary listens to the WiFi waves bouncing around your room. When something moves, the waves shift. That's the whole trick.",
        primary: "Got it",
        skip: "Skip",
        // Card 1 also asks the optional Pets question — handled in JS.
        pets: true,
      },
      {
        step: "Step 2 of 4",
        title: "Listens to WiFi, never to your camera.",
        body: "No camera. No microphone. No MAC addresses stored. Nothing leaves the device.",
        primary: "Next",
        skip: "Skip",
      },
      {
        step: "Step 3 of 4",
        title: "Best in the same room.",
        body: "Through one wall: motion only, no breathing claims. Through a floor: depends on your home. We're honest about what the canary can and can't pick up.",
        learnMore: "What it can and can't see",
        primary: "Next",
        skip: "Skip",
      },
      {
        step: "Step 4 of 4",
        title: "One last thing — let's learn your empty room.",
        body: "Step out for a minute. The canary uses that minute to learn what 'empty' looks like, so it knows when somebody's actually here.",
        primary: "Calibrate now",
        skip: "Maybe later",
      },
    ],
    petQuestion: "Any pets at home?",
    petHint:     "Pick all that apply.",
    petChoices: [
      // Order matters: "None" first because it's the mutually-exclusive
      // escape hatch, then the specific pets the user can multi-select.
      { id: "none",  label: "No pets"    },
      { id: "cat",   label: "Cat"        },
      { id: "small", label: "Small dog"  },
      { id: "large", label: "Large dog"  },
    ],
  },
};

/* ────────────────────────────────────────────────────────────────────────
 *  Static-text hydration — the overlay's two <div class="label"> nodes
 *  and the calibrate button label live in HTML at parse time, before any
 *  JS runs, so they get baked-in placeholders. We replace those with
 *  COPY.calibrate.* on script load. The microcopy doctrine ("every
 *  user-facing string lives in COPY") then holds end-to-end.
 * ──────────────────────────────────────────────────────────────────────── */
(function hydrateStaticCopy() {
  const labels = document.querySelectorAll('#calibratingMask .label');
  if (labels.length >= 2) {
    labels[0].textContent = COPY.calibrate.label;
    labels[1].textContent = COPY.calibrate.stepOut;
  }
  const btn = document.getElementById('calibrateBtn');
  if (btn) btn.textContent = COPY.calibrate.btn;
})();

/* ────────────────────────────────────────────────────────────────────────
 *  Tooltip plumbing — one pattern, used everywhere.
 * ──────────────────────────────────────────────────────────────────────── */
(function tipModule() {
  const bubble = document.createElement('div');
  bubble.className = 'tip-bubble';
  bubble.id = 'tipBubble';
  document.body.appendChild(bubble);

  let openFor = null;
  let timeout = null;

  function show(target) {
    const key = target.dataset.tip;
    const text = COPY.tooltips[key];
    if (!text) return;
    bubble.textContent = text;
    target.setAttribute('aria-describedby', 'tipBubble');
    const r = target.getBoundingClientRect();
    bubble.style.left = (r.left + r.width/2 - bubble.offsetWidth/2) + 'px';
    bubble.style.top  = (window.scrollY + r.top - bubble.offsetHeight - 8) + 'px';
    bubble.classList.add('show');
    openFor = target;
    clearTimeout(timeout);
    timeout = setTimeout(hide, 5000);
  }
  function hide() {
    bubble.classList.remove('show');
    if (openFor) openFor.removeAttribute('aria-describedby');
    openFor = null;
  }
  document.addEventListener('mouseover', e => {
    const t = e.target.closest('[data-tip]');
    if (t) show(t);
  });
  document.addEventListener('mouseout', e => {
    if (e.target.closest('[data-tip]')) hide();
  });
  document.addEventListener('focusin', e => {
    const t = e.target.closest('[data-tip]');
    if (t) show(t);
  });
  document.addEventListener('focusout', hide);
  // Tap-and-hold for touch
  let touchTimer = null;
  document.addEventListener('touchstart', e => {
    const t = e.target.closest('[data-tip]');
    if (!t) return;
    touchTimer = setTimeout(() => show(t), 450);
  }, {passive: true});
  document.addEventListener('touchend', () => {
    clearTimeout(touchTimer);
    setTimeout(hide, 2000);
  });
  document.addEventListener('scroll', hide, true);
})();

/* ────────────────────────────────────────────────────────────────────────
 *  State + theming
 * ──────────────────────────────────────────────────────────────────────── */
const STATE_NAMES = ['empty','sensing','subtle','quiet','active','together'];

function setState(stateKey, opts) {
  opts = opts || {};
  const html = document.documentElement;
  if (!STATE_NAMES.includes(stateKey)) stateKey = 'sensing';
  if (html.dataset.state !== stateKey) html.dataset.state = stateKey;

  const isPet = opts.pet === true;
  const copyKey = isPet ? 'pet'
    : (stateKey === 'quiet' && opts.bpm) ? 'quiet_bpm'
    : stateKey;
  const tmpl = COPY.states[copyKey] || COPY.states.sensing;
  const name = tmpl.name;
  const sub  = tmpl.sub.replace('{bpm}', opts.bpm || '');

  const stateEl = document.getElementById('state');
  const subEl   = document.getElementById('sub');
  const metaEl  = document.getElementById('meta');

  if (stateEl.textContent !== name) {
    stateEl.style.opacity = '0';
    stateEl.style.transform = 'translateY(-6px)';
    setTimeout(() => {
      stateEl.textContent = name;
      stateEl.style.opacity = '1';
      stateEl.style.transform = 'translateY(0)';
    }, 240);
  }
  subEl.textContent = sub;
  metaEl.textContent = (opts.confidence || 'tentative') + (opts.dominant ? ' · ' + opts.dominant : '');

  const orbWrap = document.getElementById('orbWrap');
  orbWrap.classList.toggle('twinned', stateKey === 'together');
}

function pulseRipple() {
  const r = document.getElementById('ripple');
  r.classList.remove('go');
  void r.offsetWidth;
  r.classList.add('go');
}

function setBreathRate(bpm) {
  if (!bpm || bpm < 4 || bpm > 30) {
    document.documentElement.style.setProperty('--pulse-dur', '4.3s');
    return;
  }
  const dur = (60 / bpm).toFixed(2) + 's';
  document.documentElement.style.setProperty('--pulse-dur', dur);
}

/* ────────────────────────────────────────────────────────────────────────
 *  Live waveform — raw + smoothed + steady-state band
 *
 *  At 1 Hz polling the raw motion/breathing values jitter enough that
 *  the eye reads the line as noise. We apply an exponentially-weighted
 *  moving average (α = 0.30, ≈ 3-second time constant) so the bold
 *  "steady" line tracks the underlying reading without the jitter, and
 *  we shade a translucent ±1σ band around it computed from a rolling
 *  RMS of the residuals over the last 15 seconds.
 *
 *  Why EMA + RMS instead of a fancier filter:
 *    - O(1) update per sample for the EMA, O(N=15) for the band. At
 *      1 Hz that's free.
 *    - No model assumptions; works equally well for motion (impulse-y)
 *      and breathing (oscillatory).
 *    - The user-visible surface is a band, not a single number, so the
 *      reader sees noisiness directly — no false precision.
 *
 *  α tuning: 0.30 was picked so a step change reaches ~95% of the new
 *  level in ~10 seconds (one 24-hour ribbon bucket, basically). Lower
 *  α would lag visibly behind a real "someone walked in"; higher α
 *  reintroduces the jitter we're trying to hide.
 * ──────────────────────────────────────────────────────────────────────── */
const HISTORY_LEN = 60;
const SMOOTH_ALPHA = 0.30;       // EMA factor at 1 Hz; ~3s time constant
const BAND_WIN     = 15;         // residual-RMS window in samples (= seconds)

// Raw + smoothed history rings. Raw is what the device reported this
// second; smoothed is the EMA running over those raws. Both are length
// HISTORY_LEN so the canvas can draw them side by side without an
// off-by-one between the two ring positions.
const motionRawHist    = new Array(HISTORY_LEN).fill(0);
const motionSmoothHist = new Array(HISTORY_LEN).fill(0);
const breathRawHist    = new Array(HISTORY_LEN).fill(0);
const breathSmoothHist = new Array(HISTORY_LEN).fill(0);

// Persistent EMA accumulators + recent residuals. We keep just the
// last BAND_WIN residuals so the "wobble" band tracks the present
// noise floor, not an all-time average.
let motionEma = 0, breathEma = 0;
const motionResid = [];
const breathResid = [];

function pushHistory(motion, breathing) {
  // EMA update FIRST so the smoothed history we record is the value
  // the rest of the UI also reads via getSenseDetailSnapshot().
  motionEma = motionEma * (1 - SMOOTH_ALPHA) + motion    * SMOOTH_ALPHA;
  breathEma = breathEma * (1 - SMOOTH_ALPHA) + breathing * SMOOTH_ALPHA;

  motionRawHist.shift();    motionRawHist.push(motion);
  motionSmoothHist.shift(); motionSmoothHist.push(motionEma);
  breathRawHist.shift();    breathRawHist.push(breathing);
  breathSmoothHist.shift(); breathSmoothHist.push(breathEma);

  motionResid.push(motion    - motionEma);
  breathResid.push(breathing - breathEma);
  if (motionResid.length > BAND_WIN) motionResid.shift();
  if (breathResid.length > BAND_WIN) breathResid.shift();
}

// 1-σ-equivalent band width derived from the rolling residual RMS.
// Returns a number in the same 0..100 scale as motion/breathing so
// it can be painted directly on the waveform canvas without rescaling.
function residRms(arr) {
  if (arr.length === 0) return 0;
  let sumSq = 0;
  for (let i = 0; i < arr.length; i++) sumSq += arr[i] * arr[i];
  return Math.sqrt(sumSq / arr.length);
}

// Public accessor used by updateSenseDetail() — returns the freshest
// numbers without re-walking the whole history. Centralizing this so
// the reveal panel and the canvas can never drift out of sync.
function getSenseDetailSnapshot() {
  // pollStream() already coerces motion and breathing with Number()
  // before pushHistory() lands them, so no need to re-coerce here.
  return {
    motionRaw:    motionRawHist[HISTORY_LEN - 1],
    motionSteady: motionEma,
    motionBand:   residRms(motionResid),
    breathRaw:    breathRawHist[HISTORY_LEN - 1],
    breathSteady: breathEma,
    breathBand:   residRms(breathResid),
  };
}

function drawWaveform() {
  const canvas = document.getElementById('waveform');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const w = canvas.width = canvas.clientWidth * (window.devicePixelRatio||1);
  const h = canvas.height = canvas.clientHeight * (window.devicePixelRatio||1);
  ctx.clearRect(0,0,w,h);

  // Convert a 0..100 scalar to a canvas y-coord. Same mapping as the
  // original trace() so a value of 0 sits near the bottom and 100 near
  // the top, with a 4% breathing room top and bottom.
  const yFor = v => h - (v / 100) * (h * 0.92) - h * 0.04;

  // 1) Translucent band (±RMS) around the smoothed line. We render the
  //    band by walking forward along upperBand and backward along
  //    lowerBand to make a closed polygon. RMS is computed once from
  //    the most recent BAND_WIN residuals — applying it as a constant
  //    width across the visible window is the right call: the band is
  //    a "how noisy is the room right now" indicator, not a per-sample
  //    Bayesian credible interval.
  const mBand = residRms(motionResid);
  const bBand = residRms(breathResid);
  function band(smoothArr, sigma, fill) {
    if (sigma < 0.5) return;  // nothing useful to draw at sub-pixel widths
    if (smoothArr.length === 0) return;
    ctx.beginPath();
    // Seed the path with moveTo on the first upper sample. Without
    // this, the implicit canvas current point is (0,0), so the first
    // lineTo would draw an unintended segment from the origin and the
    // closed polygon would include a triangular wedge in the top-left
    // of the waveform (Codex P2 on the first revision of this PR).
    ctx.moveTo(0, yFor(smoothArr[0] + sigma));
    for (let i = 1; i < smoothArr.length; i++) {
      const x = (i / (HISTORY_LEN - 1)) * w;
      ctx.lineTo(x, yFor(smoothArr[i] + sigma));
    }
    for (let i = smoothArr.length - 1; i >= 0; i--) {
      const x = (i / (HISTORY_LEN - 1)) * w;
      ctx.lineTo(x, yFor(smoothArr[i] - sigma));
    }
    ctx.closePath();
    ctx.fillStyle = fill;
    ctx.fill();
  }

  // 2) Single-line trace. Used for both raw (faint) and smoothed
  //    (bold) — same mapping, different alphas / widths.
  function trace(arr, hue, alpha, width) {
    ctx.beginPath();
    for (let i = 0; i < arr.length; i++) {
      const x = (i / (HISTORY_LEN-1)) * w;
      const y = yFor(arr[i]);
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = hue;
    ctx.globalAlpha = alpha;
    ctx.lineWidth = width;
    ctx.lineCap = 'round'; ctx.lineJoin = 'round';
    ctx.stroke();
    ctx.globalAlpha = 1;
  }

  const motionColor = getComputedStyle(document.documentElement).getPropertyValue('--orb-3').trim() || '#1ec5b1';
  const breathColor = getComputedStyle(document.documentElement).getPropertyValue('--accent').trim() || '#8e9eff';
  const dpr = window.devicePixelRatio || 1;

  // Order back→front: band, raw faint, smoothed bold. That puts the
  // shaded band behind both lines (so the line you trust visually
  // sits on top of the noise envelope) and the raw line behind the
  // smoothed (so the bold trustworthy reading is visually dominant
  // even when the raw spikes off-band).
  band(motionSmoothHist, mBand, motionColor + '26');   // ~15% alpha
  band(breathSmoothHist, bBand, breathColor + '26');
  trace(motionRawHist,    motionColor, 0.30, 1.5 * dpr);
  trace(breathRawHist,    breathColor, 0.30, 1.5 * dpr);
  trace(motionSmoothHist, motionColor, 1.0,  2 * dpr);
  trace(breathSmoothHist, breathColor, 1.0,  2 * dpr);
}

function drawHeatmap() {
  const canvas = document.getElementById('rawHeatmap');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const w = canvas.width = canvas.clientWidth * (window.devicePixelRatio||1);
  const h = canvas.height = canvas.clientHeight * (window.devicePixelRatio||1);
  ctx.clearRect(0,0,w,h);
  if (!latestRawVector || latestRawVector.length === 0) return;
  const cellW = w / latestRawVector.length;
  for (let i = 0; i < latestRawVector.length; i++) {
    const v = latestRawVector[i];
    const mag = Math.min(127, Math.abs(v));
    const hue = v < 0 ? 200 : 30;
    ctx.fillStyle = `hsla(${hue}, 90%, ${30 + mag/3}%, ${0.3 + mag/200})`;
    ctx.fillRect(i*cellW, 0, cellW + 1, h);
  }
}

/* ────────────────────────────────────────────────────────────────────────
 *  24-hour ribbon
 * ──────────────────────────────────────────────────────────────────────── */
const RIBBON_BUCKETS = 96;
const ribbonData = new Array(RIBBON_BUCKETS).fill(0);
let ribbonHoverIdx = -1;

function drawRibbon() {
  const canvas = document.getElementById('ribbon');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const w = canvas.width = canvas.clientWidth * (window.devicePixelRatio||1);
  const h = canvas.height = canvas.clientHeight * (window.devicePixelRatio||1);
  ctx.clearRect(0,0,w,h);
  const cellW = w / RIBBON_BUCKETS;
  for (let i = 0; i < RIBBON_BUCKETS; i++) {
    const intensity = ribbonData[i] / 255;
    const hue = 220 - intensity * 180;  // cool when empty, warm when active
    const sat = 60 + intensity * 30;
    const lit = 50 + (1 - intensity) * 35;
    /* Quiet Hours buckets render at half opacity — same color hue as
     * waking hours so the eye still reads activity intensity, but the
     * row visibly recedes. Cool tint stays cool; warm tint stays warm,
     * just gentler. */
    const inQH = isBucketInQuietHours(i);
    const a = (0.25 + intensity * 0.65) * (inQH ? 0.5 : 1);
    ctx.fillStyle = `hsla(${hue}, ${sat}%, ${lit}%, ${a})`;
    ctx.fillRect(i*cellW, 0, cellW + 1, h);
  }
  // Highlight current bucket (last one)
  const nowIdx = currentBucketIdx();
  ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue('--accent').trim() || '#8e9eff';
  ctx.globalAlpha = 0.85;
  ctx.lineWidth = 2;
  ctx.strokeRect(nowIdx * cellW + 1, 1, cellW - 2, h - 2);
  ctx.globalAlpha = 1;
}

function currentBucketIdx() {
  const d = new Date();
  return Math.floor((d.getHours()*60 + d.getMinutes()) / 15);
}

function bucketLabel(idx) {
  const startMin = idx * 15;
  const endMin   = startMin + 15;
  const fmt = m => {
    let h = Math.floor(m / 60), mm = m % 60;
    const am = h < 12;
    h = h % 12; if (h === 0) h = 12;
    return `${h}:${mm.toString().padStart(2,'0')} ${am ? 'AM' : 'PM'}`;
  };
  return `${fmt(startMin)}–${fmt(endMin)}`;
}

(function ribbonHover() {
  const canvas = document.getElementById('ribbon');
  const readout = document.getElementById('ribbonReadout');
  function locate(e) {
    const r = canvas.getBoundingClientRect();
    const x = (e.touches ? e.touches[0].clientX : e.clientX) - r.left;
    const idx = Math.max(0, Math.min(RIBBON_BUCKETS-1, Math.floor(x / r.width * RIBBON_BUCKETS)));
    ribbonHoverIdx = idx;
    const intensity = Math.round(ribbonData[idx] / 2.55);
    readout.textContent = `${bucketLabel(idx)} · ${intensity}%`;
  }
  canvas.addEventListener('mousemove', locate);
  canvas.addEventListener('mouseleave', () => { ribbonHoverIdx = -1; readout.textContent = '—'; });
  canvas.addEventListener('touchstart', locate, {passive:true});
  canvas.addEventListener('touchmove',  locate, {passive:true});
})();

/* ────────────────────────────────────────────────────────────────────────
 *  Stream + Today fetchers
 * ──────────────────────────────────────────────────────────────────────── */
let lastEventId = 0;
let latestRawVector = null;
// Wall-clock millis when lastEventId most recently changed. Drives the
// "Last event · 7s ago" row in the sense-detail reveal.
let lastEventLandedAt = 0;
// Most recent /api/csi/stream payload — kept so updateSenseDetail() can
// re-render on the animation tick without us having to re-fetch.
let latestStreamJ = null;

// Plain-language labels for the device-side state names. Mirrors the
// COPY.states bank but kept here as a dedicated map because the
// sense-detail reveal labels several names (breathing_nearby) that
// don't have their own COPY entry — they share a state with another.
const SENSE_STATE_LABEL = {
  empty: 'Empty', subtle: 'Subtle motion', quiet: 'Quiet',
  active: 'Active', together: 'Together',
  breathing_nearby: 'Quiet', breathing_lost: 'Subtle motion',
  sensing: 'Sensing…',
};
// Same for confidence — we want capitalized, plain-language labels in
// the reveal, not the raw "tentative" / "likely" / "confirmed" strings.
const SENSE_CONF_LABEL = {
  tentative: 'Tentative', likely: 'Likely', confirmed: 'Confirmed',
};

// Format a ±band as a short, readable suffix. We round to whole
// numbers because the underlying motion/breathing scale is integer
// 0..100; sub-integer band widths read as "steady" rather than as
// noise. Returns an empty string for negligible bands so the row
// reads cleanly when the room is calm.
function fmtBand(v) {
  const r = Math.round(v);
  return r < 1 ? '' : `±${r}`;
}

// Seconds elapsed since the last event landed, formatted as "7s",
// "2m", "—". Returns "—" if no event has landed yet so the row is
// honest about a fresh boot rather than showing a misleading "0s".
function fmtAgo(landedAtMs) {
  if (!landedAtMs) return '—';
  const sec = Math.floor((Date.now() - landedAtMs) / 1000);
  if (sec < 0)    return '—';
  if (sec < 60)   return sec + 's';
  if (sec < 3600) return Math.floor(sec / 60) + 'm';
  return Math.floor(sec / 3600) + 'h';
}

// Update the live numbers in the sense-detail reveal. Cheap enough to
// run on every animation tick (the DOM writes only fire when the
// expressed value actually changes). Reads the freshest history via
// getSenseDetailSnapshot() so we never paint a value the canvas hasn't
// also been told about.
function updateSenseDetail() {
  const det = document.getElementById('senseDetail');
  // Skip the DOM walk entirely while the disclosure is collapsed —
  // saves a handful of textContent writes per frame on slow phones.
  if (!det || !det.open) return;

  const s = getSenseDetailSnapshot();
  const j = latestStreamJ;

  const setText = (id, txt) => {
    const el = document.getElementById(id);
    if (el && el.textContent !== txt) el.textContent = txt;
  };

  setText('seMRaw',    String(s.motionRaw));
  setText('seMSteady', String(Math.round(s.motionSteady)));
  setText('seMBand',   fmtBand(s.motionBand));
  setText('seBRaw',    String(s.breathRaw));
  setText('seBSteady', String(Math.round(s.breathSteady)));
  setText('seBBand',   fmtBand(s.breathBand));

  // Loudest signal: prefer the device's own assessment if it sent one.
  // Fall back to a simple compare so the row isn't blank during the
  // pre-first-event "ambient" phase. We compare smoothed values, not
  // raw, so a single noisy frame doesn't flip the label.
  let dom = '—';
  if (j && j.dominant_signal) {
    dom = (j.dominant_signal === 'motion') ? 'Movement'
        : (j.dominant_signal === 'breathing') ? 'Breath' : j.dominant_signal;
  } else if (s.motionSteady > s.breathSteady + 1) dom = 'Movement';
  else if (s.breathSteady > s.motionSteady + 1)   dom = 'Breath';
  else if (s.motionSteady < 1 && s.breathSteady < 1) dom = '—';
  else dom = 'Tied';
  setText('seDom', dom);

  setText('seConf',  j && j.confidence ? (SENSE_CONF_LABEL[j.confidence] || j.confidence) : '—');
  setText('seState', j && j.state      ? (SENSE_STATE_LABEL[j.state]      || j.state)     : '—');
  setText('seBpm',   j && j.bpm        ? `${Math.round(j.bpm)} a minute` : '—');

  // Last event row: show the id and how long ago it landed. We carry
  // the timestamp ourselves rather than trusting j.t, because j.t is
  // the device-side committed_ms relative to stream start, which doesn't
  // mean much without also knowing stream-start, and the wall-clock
  // delta is what an installer actually wants.
  if (lastEventId) {
    setText('seEvent', `#${lastEventId} · ${fmtAgo(lastEventLandedAt)} ago`);
  } else {
    setText('seEvent', '—');
  }
}

/* Map a fetch error / non-OK response to one of the COPY.errors strings.
 * Lets every poll surface the right diagnostic to the dashboard's
 * disconnect plate instead of the one-size-fits-all "can't reach"
 * (audit #9). status === 0 is our convention for "fetch threw" — the
 * browser's TypeError on network failure has no status field so we
 * stamp it ourselves before re-throwing. */
function disconnectMessageFor(status) {
  if (status === 401 || status === 403) return COPY.errors.unauthorized;
  if (status === 503)                   return COPY.errors.unavailable;
  if (status >= 500)                    return COPY.errors.serverError;
  if (status === 0)                     return COPY.errors.timeout;
  return COPY.errors.disconnect;
}

/* Apply the disconnected look + a diagnostic message keyed on the
 * status code we caught. When the failure is auth-related, also wire
 * the frameCount footer as a clickable target that reloads / so the
 * pair flow re-runs — saves the user from typing the URL into the
 * address bar again. */
function paintDisconnect(status) {
  document.body.classList.add('disconnected');
  const fc = document.getElementById('frameCount');
  fc.textContent = disconnectMessageFor(status);
  if (status === 401 || status === 403) {
    fc.style.cursor = 'pointer';
    fc.onclick = () => { window.location.href = '/'; };
  } else {
    fc.style.cursor = '';
    fc.onclick = null;
  }
}

async function pollStream() {
  try {
    const r = await cvFetch('/api/csi/stream', {cache: 'no-store'});
    if (!r.ok) {
      /* Surface the HTTP status in the thrown error so the catch block
       * can route to the right COPY string. Plain Error stringifies
       * its message for console; we tack the status on as a property
       * so the catch reads cleanly without parsing the message. */
      const e = new Error('stream not ok: ' + r.status);
      e.status = r.status;
      throw e;
    }
    const j = await r.json();

    // Device-side honest signal: when the radio HAL never came up
    // (chip lacks CSI, antenna fault), the firmware sends
    // status:"unavailable" instead of the normal sensing fallback.
    // Park the orb in the "sensing" theme but surface the disconnect
    // copy so installers see something is actually wrong.
    if (j.status === 'unavailable') {
      setState('sensing', {confidence: 'tentative'});
      paintDisconnect(503);  /* matches the on-wire HAL-unavailable path */
      return;
    }

    const motion    = Number(j.motion);
    const breathing = Number(j.breathing);
    pushHistory(motion, breathing);
    // Stash the freshest payload so the animation tick's call to
    // updateSenseDetail() can re-render between polls (the "ago"
    // counter ticks every second without a new fetch).
    latestStreamJ = j;

    // Signal-supply honesty: with (nearly) no CSI frames arriving the
    // device has no basis for ANY room claim — the firmware stops
    // ticking its modules and we park the orb on "Sensing…" with a
    // plain-language explanation instead of letting a stale "Empty"
    // stand. fps is frames-per-second in the last 1 s window.
    const supply = j.supply || null;
    const starved = supply && supply.fps < 2 &&
                    (supply.silent_ms < 0 || supply.silent_ms > 3000);
    if (starved) {
      setState('sensing', {confidence: 'tentative'});
      latestStreamJ = j;
      updateSenseDetail();
      document.getElementById('frameCount').textContent =
        'no WiFi signal to sense with — join your home WiFi or add a second Canary';
      document.body.classList.remove('disconnected');
      return;
    }

    const isEvent = (j.id !== undefined);
    if (isEvent) {
      // Map server state name -> our state key set
      const map = { empty: 'empty', subtle: 'subtle', quiet: 'quiet', active: 'active', together: 'together',
                    breathing_nearby: 'quiet', breathing_lost: 'subtle' };
      const stateKey = map[j.state] || 'sensing';
      const isPet = (window.PET_MODE && stateKey === 'subtle');
      setState(stateKey, {
        confidence: j.confidence,
        dominant:   j.dominant_signal || (motion > breathing ? 'motion' : 'breathing'),
        bpm:        (j.confidence === 'confirmed' && j.bpm) ? j.bpm : null,
        pet:        isPet,
      });
      if (j.bpm) setBreathRate(j.bpm); else setBreathRate(null);
      if (j.id !== lastEventId) {
        if (stateKey === 'active' || stateKey === 'subtle' || stateKey === 'together') pulseRipple();
        lastEventId = j.id;
        lastEventLandedAt = Date.now();   // drives the "Last event · Ns ago" row
      }
      // Push current bucket intensity
      const idx = currentBucketIdx();
      const combined = Math.min(255, Math.max(motion*2.55, ribbonData[idx]));
      ribbonData[idx] = combined;
    } else {
      setState('sensing', {confidence: j.confidence || 'tentative'});
    }
    updateSenseDetail();

    // Footer: live scalars plus the signal-supply chip. 8+ frames/s is
    // a healthy diet (AP beacons alone give ~10); 2–7 works but slower;
    // the starved case returned above.
    let supplyTxt = '';
    if (supply) {
      supplyTxt = supply.fps >= 8 ? ` · signal ${supply.fps}/s`
                                  : ` · weak signal ${supply.fps}/s`;
      if (supply.probe) supplyTxt += ' · probing';
    }
    document.getElementById('frameCount').textContent =
      `motion ${motion} · breathing ${breathing}${supplyTxt}`;
    /* Clear the disconnect plate AND the click-to-pair handler the
     * unauthorized branch may have wired in a previous tick. */
    document.body.classList.remove('disconnected');
    const fc = document.getElementById('frameCount');
    fc.style.cursor = '';
    fc.onclick = null;
  } catch (err) {
    /* fetch() throws TypeError on network failure with no .status
     * field; stamp 0 as "no response" so disconnectMessageFor maps
     * it to the timeout copy instead of the generic disconnect. */
    paintDisconnect(err.status || 0);
  }
}

async function fetchToday() {
  const body = document.getElementById('todayBody');
  body.innerHTML = '<p style="color:var(--fg-mute)">Loading…</p>';
  try {
    const r = await cvFetch('/api/events/today', {cache: 'no-store'});
    if (!r.ok) {
      const e = new Error('today fetch returned ' + r.status);
      e.status = r.status;
      throw e;
    }
    const j = await r.json();
    const events = j.events || [];
    if (events.length === 0) {
      body.innerHTML = '<p style="color:var(--fg-mute);padding:20px 0">' + COPY.today.empty + '</p>';
      return;
    }
    body.innerHTML = '';
    let activeCount = 0, quietCount = 0, anomalyCount = 0;
    for (const e of events) {
      // Server now exposes the row's privacy-class-tagged category from
      // csi_event (P0 events are "event", anomalies are "anomaly", and
      // ambient never persists). Tally the latter so users see if
      // anything unusual happened today.
      if (e.category === 'anomaly') anomalyCount++;
      const row = document.createElement('div');
      row.className = 'event-row' + (e.dismissed ? ' dismissed' : '');
      const stateLabel = ({
        empty: 'Empty', subtle: 'Subtle motion', quiet: 'Quiet', active: 'Active',
        together: 'Together', breathing_nearby: 'Quiet', breathing_lost: 'Subtle motion',
        smoke_alarm: 'Smoke alarm heard', co_alarm: 'CO alarm heard',
        knock: 'Knock heard', doorbell: 'Doorbell heard',
        glass_break: 'Glass break heard',
        mic_muted: 'Mic muted', mic_on: 'Mic turned on',
      })[e.state] || e.state;
      if (e.state === 'active') activeCount++;
      if (e.state === 'empty')  quietCount++;
      const dur = e.duration_sec ? Math.round(e.duration_sec / 60) + ' min' : '';
      const bucket = e.time_bucket;
      const bucketLabelStr = (bucket >= 0 && bucket < 144)
        ? `${Math.floor(bucket/6)}:${(bucket%6*10).toString().padStart(2,'0')}` : '';
      row.innerHTML = `
        <div>
          <div class="label">${stateLabel}</div>
          <div class="when">${bucketLabelStr}${dur ? ' · ' + dur : ''}${e.bundled > 1 ? ' · bundled ×'+e.bundled : ''}</div>
        </div>
        <button class="dismiss-btn" data-id="${e.id}">That was nothing</button>
      `;
      body.appendChild(row);
    }
    const summary = document.createElement('div');
    summary.className = 'summary-card';
    /* "unusual" works as both singular and plural here (the noun is
     * implicit), so we don't need the singular/plural ternary. Hide
     * the segment entirely on a 0-count day. */
    let anomalyLine = '';
    if (anomalyCount > 0) anomalyLine = ' · ' + anomalyCount + ' unusual';
    summary.innerHTML = `<h3>Today</h3>${activeCount} active · ${quietCount} quiet${anomalyLine}`;
    body.appendChild(summary);

    body.querySelectorAll('.dismiss-btn').forEach(btn => {
      btn.addEventListener('click', async () => {
        const id = +btn.dataset.id;
        await cvFetch('/api/events/dismiss', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({event_id: id}),
        });
        btn.closest('.event-row').classList.add('dismissed');
        btn.disabled = true;
      });
    });
  } catch (err) {
    /* Mirror pollStream's status-aware diagnostic so the Today sheet
     * tells the same story as the headline disconnect plate (audit
     * #9). Also paint the headline plate so the user has a single
     * pair-now affordance regardless of which fetch caught the
     * failure first. */
    const msg = disconnectMessageFor(err.status || 0);
    body.innerHTML = '<p style="color:var(--fg-mute)">' + msg + '</p>';
    paintDisconnect(err.status || 0);
  }
}

async function pollRawVector() {
  if (!document.querySelector('details.tinker[open]')) return;
  try {
    const r = await cvFetch('/api/csi/window', {cache: 'no-store'});
    if (!r.ok) return;  // 403 means privacy ceiling not raised; silently skip.
    const j = await r.json();
    if (j.v) latestRawVector = j.v;
    drawHeatmap();
  } catch {}
}

/* Privacy Budget pill — literal byte counter for outbound traffic.
 * /api/privacy-budget returns {bytes_today, ceiling, since_ms}. We
 * format human-friendly ("0 bytes" / "47 bytes" / "1.2 KB" / "3.4 MB"
 * / "1.1 GB") and warm-tint the pill if either bytes > 0 or the
 * privacy ceiling is above P0. Best-effort — a transient fetch
 * failure leaves the placeholder in place. */
function formatBytes(n) {
  if (n < 1024)               return n + ' bytes';
  if (n < 1024 * 1024)        return (n / 1024).toFixed(1) + ' KB';
  if (n < 1024 * 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + ' MB';
  return (n / (1024 * 1024 * 1024)).toFixed(1) + ' GB';
}

async function fetchPrivacyBudget() {
  const pill = document.getElementById('privacyPill');
  if (!pill) return;
  try {
    const r = await cvFetch('/api/privacy-budget', {cache: 'no-store'});
    if (!r.ok) return;
    const j = await r.json();
    /* The server reports `wired:false` while no off-device export path
     * actually feeds the byte counter. Until that's wired up the count
     * is structurally 0 regardless of activity, so claiming "0 bytes
     * left the device" would be a privacy guarantee we aren't actually
     * enforcing. Hide the pill instead of lying about it. */
    if (j.wired === false) {
      pill.style.display = 'none';
      return;
    }
    pill.style.display = '';
    /* DON'T use `j.bytes_today | 0` — JS bitwise ops coerce to
     * signed 32-bit, so any value ≥ 2 GB wraps negative (and then
     * formatBytes lies + the warm-tint check fails). The server's
     * counter saturates at UINT32_MAX (~4 GB), so we need full
     * unsigned 32-bit range. Number(...) keeps it as a JS double
     * which represents 32-bit unsigned exactly. */
    const bytes = Number(j.bytes_today) || 0;
    const ceiling = j.ceiling || 'p0';
    pill.textContent = 'Today: ' + formatBytes(bytes) + ' left the device';
    /* Warm tint when the local-first invariant is being challenged in
     * any way: either bytes left, or the user opted into a higher
     * privacy class (P1 / P2). Cool means "nothing's leaving and
     * nothing's been opted out of"; warm means "look at this." */
    if (bytes > 0 || ceiling !== 'p0') {
      pill.classList.add('warm');
    } else {
      pill.classList.remove('warm');
    }
  } catch {}
}

/* Outbound opt-in visibility: the device is local-only by default and
 * only the user can change that (Enterprise TODO §1). This pill says,
 * in one glance, which side of that line the device is on right now —
 * and to where. Warm tint whenever a sharing path is on, so "connected
 * somewhere" is never the visually quiet state. */
async function fetchShareState() {
  const pill = document.getElementById('sharePill');
  if (!pill) return;
  try {
    const r = await cvFetch('/api/mqtt/config', {cache: 'no-store'});
    if (!r.ok) return;
    const j = await r.json();
    if (!j.enabled) {
      pill.textContent = 'Sharing: off — nothing leaves unless you turn it on';
      pill.classList.remove('warm');
    } else if (j.connected) {
      pill.textContent = 'Sharing: on — connected to ' + (j.host || 'your home system');
      pill.classList.add('warm');
    } else {
      pill.textContent = 'Sharing: on — trying to reach ' + (j.host || 'your home system');
      pill.classList.add('warm');
    }
  } catch {}
}

/* ────────────────────────────────────────────────────────────────────────
 *  Sheets
 * ──────────────────────────────────────────────────────────────────────── */
/* Sheet open/close with the focus-management contract a real modal
 * dialog needs (audit: a11y pass).
 *
 *   - openSheet  saves the previously-focused element so closeSheet
 *                can restore it — without this, keyboard users land
 *                back at the top of the page after dismiss.
 *   - openSheet  moves focus to the sheet's first focusable child
 *                (close button, by markup order) so screen readers
 *                announce the dialog and so a Tab keeps the user
 *                inside the modal.
 *   - closeSheet returns focus to the trigger, which is the standard
 *                behavior expected by most assistive tech.
 *
 * The Tab-cycle trap and the global ESC handler live in the
 * keydown listener below — they're shared across all sheets so a
 * future "settings" sheet (PR audit follow-up) gets them for free. */
let s_returnFocusEl = null;
function focusableChildren(root) {
  /* getClientRects().length > 0 instead of offsetParent !== null
   * because the sheets are position:fixed — and Firefox returns null
   * for descendants of fixed-positioned ancestors (PR #401 review
   * r3214242791). getClientRects holds for any element that has
   * actual layout boxes, regardless of its containing block's
   * positioning. summary/select/textarea included for completeness
   * so future markup changes don't silently lose focus targets. */
  return Array.from(root.querySelectorAll(
    'button, [href], input, select, textarea, summary, [tabindex]:not([tabindex="-1"])'
  )).filter(el => !el.hasAttribute('disabled') && el.getClientRects().length > 0);
}
function openSheet(sheet, scrim) {
  s_returnFocusEl = document.activeElement;
  scrim.classList.add('open');
  sheet.classList.add('open');
  document.body.style.overflow = 'hidden';
  /* Defer the focus move until the sheet's open transition has had
   * one frame to apply, so the focus ring isn't visibly thrown
   * before the sheet's slide-in animation. */
  requestAnimationFrame(() => {
    const focusables = focusableChildren(sheet);
    if (focusables.length) focusables[0].focus();
  });
}
function closeSheet(sheet, scrim) {
  scrim.classList.remove('open');
  sheet.classList.remove('open');
  document.body.style.overflow = '';
  if (s_returnFocusEl && typeof s_returnFocusEl.focus === 'function') {
    s_returnFocusEl.focus();
  }
  s_returnFocusEl = null;
  /* Onboarding's "Learn more" path opens the What sheet behind a
   * blurred welcome mask (body.welcome-paused). Clearing the class
   * here means every dismiss path — scrim click, close button, ESC,
   * future swipe-down — restores the welcome card instead of leaving
   * the user stranded with a blurred background and no card to
   * advance through. No-op when the class isn't set, so legitimate
   * dashboard sessions are unaffected (PR #401 reviews r3214241863
   * and r3214242793). */
  if (sheet.id === 'whatSheet') {
    document.body.classList.remove('welcome-paused');
  }
}

const todaySheet = document.getElementById('todaySheet');
const todayScrim = document.getElementById('todayScrim');
document.getElementById('todayBtn').addEventListener('click', () => {
  fetchToday();
  fetchPrivacyBudget();
  fetchShareState();
  openSheet(todaySheet, todayScrim);
});
todayScrim.addEventListener('click', () => closeSheet(todaySheet, todayScrim));

const whatSheet = document.getElementById('whatSheet');
const whatScrim = document.getElementById('whatScrim');
function buildWhatBody() {
  const body = document.getElementById('whatBody');
  const rows = COPY.what.rows.map(r =>
    `<div class="what-row"><div class="scenario">${r.scenario}</div><div class="capability">${r.capability}</div></div>`
  ).join('');
  body.innerHTML = `<div class="what-rows">${rows}</div><p class="what-foot">${COPY.what.foot}</p>`;
}
buildWhatBody();
document.getElementById('helpBtn').addEventListener('click', () => {
  openSheet(whatSheet, whatScrim);
});
whatScrim.addEventListener('click', () => closeSheet(whatSheet, whatScrim));

const fleetSheet = document.getElementById('fleetSheet');
const fleetScrim = document.getElementById('fleetScrim');

/* Peer names are set by other devices on the mesh, so they're untrusted
 * input rendered via innerHTML — escape to prevent stored XSS. */
function escapeHTML(s) {
  const d = document.createElement('div');
  d.textContent = s == null ? '' : String(s);
  return d.innerHTML;
}

function rssiToClass(rssi) {
  if (rssi > -50) return 4;
  if (rssi > -65) return 3;
  if (rssi > -80) return 2;
  return 1;
}

function rssiBars(rssi) {
  const bars = rssiToClass(rssi);
  const heights = [4, 7, 11, 16];
  return `<span class="rssi-bar">${heights.map((h, i) =>
    `<span style="height:${h}px" class="${i < bars ? 'on' : ''}"></span>`
  ).join('')}</span>`;
}

/* One card per Canary on the LAN. `self` renders first without a link;
 * every other device links to its own dashboard by unique hostname (with
 * the raw IP as the visible fallback path). Data comes from
 * /api/fleet/scan — an mDNS browse of the _securacv._tcp service every
 * Canary advertises — NOT the opera-mesh peer list this sheet used to
 * show, which only contained explicitly paired mesh members and left two
 * WiFi-sharing Canaries invisible to each other. */
function fleetPeerCard(p, isSelf) {
  const label = escapeHTML(p.name || p.device_id || 'Canary');
  const id    = escapeHTML(p.device_id || '');
  const host  = escapeHTML(p.mdns_host || '');
  const ip    = escapeHTML(p.ip || '');
  const model = escapeHTML((p.model || '').replace('XIAO ', ''));
  const meta  = [id, host ? host + '.local' : '', ip, model]
    .filter(Boolean).join(' · ');
  if (isSelf) {
    return `<div class="fleet-peer"><div><div class="name">${label} <span style="opacity:.6;font-weight:400">(this one)</span></div><div class="meta">${meta}</div></div></div>`;
  }
  const href = host ? `http://${host}.local/` : (ip ? `http://${ip}/` : '');
  const open = href ? `<a class="fleet-open" href="${href}" target="_blank" rel="noopener">Open &rarr;</a>` : '';
  return `<div class="fleet-peer"><div><div class="name">${label}</div><div class="meta">${meta}</div></div><div>${open}</div></div>`;
}

async function fetchFleetPeers(signal) {
  const el = document.getElementById('fleetPeerList');
  try {
    const r = await cvFetch('/api/fleet/scan', {cache: 'no-store', signal});
    if (!r.ok) {
      el.innerHTML = '<p class="fleet-peer-empty">Discovery not available' +
                     (r.status === 401 ? ' — session expired, reload the page.' : '.') + '</p>';
      return;
    }
    const j = await r.json();
    const self = j.self || null;
    const selfId = self ? self.device_id : null;
    const others = (j.canaries || []).filter(p => p.device_id && p.device_id !== selfId);
    let html = '';
    if (self) html += fleetPeerCard(self, true);
    html += others.map(p => fleetPeerCard(p, false)).join('');
    if (others.length === 0) {
      html += j.scanning
        ? '<p class="fleet-peer-empty">Scanning the network&hellip;</p>'
        : '<p class="fleet-peer-empty">No other Canaries found yet.</p>';
    }
    el.innerHTML = html;
  } catch (e) {
    /* A reopen aborts the prior in-flight request — that's expected, not
     * an error, so leave the existing list in place. */
    if (e && e.name === 'AbortError') return;
    el.innerHTML = '<p class="fleet-peer-empty">Could not reach the device.</p>';
  }
}

async function prefillFleetSsid() {
  try {
    const r = await cvFetch('/api/wifi', {cache: 'no-store'});
    if (!r.ok) return;
    const j = await r.json();
    const inp = document.getElementById('fleetSsid');
    if (inp && j.sta_ssid && !inp.value) inp.value = j.sta_ssid;
  } catch {}
}

/* Refresh the peer list every 5s while the Fleet sheet is open so newly
 * paired Canaries appear without reopening. The loop self-terminates the
 * moment the sheet loses its .open class, which covers every close path
 * (scrim, close button, ESC) without per-path wiring. A generation token
 * invalidates any prior loop on reopen so we never schedule two loops.
 * The AbortController cancels any still-in-flight /api/mesh/peers request
 * on reopen too, so a hung fetch can't accumulate against the device's
 * tiny httpd worker pool across repeated close/reopen. Recursive
 * setTimeout (not setInterval) for the same anti-pile-up reason as
 * pollLoop. */
let fleetPollGen = 0;
let fleetPollAbort = null;
function startFleetPoll() {
  const gen = ++fleetPollGen;
  if (fleetPollAbort) fleetPollAbort.abort();
  fleetPollAbort = new AbortController();
  const signal = fleetPollAbort.signal;
  (async function loop() {
    if (gen !== fleetPollGen || !fleetSheet.classList.contains('open')) return;
    try { await fetchFleetPeers(signal); }
    finally {
      if (gen === fleetPollGen && fleetSheet.classList.contains('open')) {
        setTimeout(loop, 5000);
      }
    }
  })();
}

document.getElementById('fleetBtn').addEventListener('click', () => {
  prefillFleetSsid();
  openSheet(fleetSheet, fleetScrim);
  startFleetPoll();
});
fleetScrim.addEventListener('click', () => closeSheet(fleetSheet, fleetScrim));

document.getElementById('fleetGenBtn').addEventListener('click', async () => {
  const ssid = document.getElementById('fleetSsid').value.trim();
  const pass = document.getElementById('fleetPass').value;
  if (!ssid) { document.getElementById('fleetSsid').focus(); return; }
  const wrap = document.getElementById('fleetQrWrap');
  const img = document.getElementById('fleetQrImg');
  const params = new URLSearchParams({ssid, pass});
  try {
    const r = await cvFetch(`/api/fleet/qr?${params}`, {cache: 'no-store'});
    if (!r.ok) {
      /* Say WHY, not just "Failed" — the old opaque label hid a session
       * expiry (401) behind what looked like a broken generator. */
      const hint = r.status === 401 ? 'Session expired &mdash; reload this page and try again.'
                 : r.status === 429 ? 'Too many attempts &mdash; wait a minute, then retry.'
                 : 'Could not generate (error ' + r.status + '). Re-check the network name.';
      img.innerHTML = `<p style="color:#c00;padding:20px">${hint}</p>`;
      wrap.style.display = '';
      return;
    }
    img.innerHTML = await r.text();
    wrap.style.display = '';
  } catch {
    img.innerHTML = '<p style="color:#c00;padding:20px">Network error &mdash; is the Canary still reachable?</p>';
    wrap.style.display = '';
  }
});

/* Help QR — the Help Desk deep link for the device's current verdict.
 * Plain fetch on purpose: the endpoint is public (selftest-parity
 * boundary), so this works from the wizard on the AP too. Every failure
 * path names the fallback a person can act on — the Help Desk URL works
 * from any browser, QR or no QR. */
document.getElementById('helpQrBtn').addEventListener('click', async () => {
  const wrap = document.getElementById('helpQrWrap');
  const img = document.getElementById('helpQrImg');
  try {
    const r = await fetch('/api/help-qr', {cache: 'no-store'});
    if (!r.ok) {
      img.innerHTML = '<p style="color:#c00;padding:20px">Could not build the code (error ' + r.status + ') &mdash; securacv.com/help works from any browser too.</p>';
      wrap.style.display = '';
      return;
    }
    img.innerHTML = await r.text();
    wrap.style.display = '';
  } catch {
    img.innerHTML = '<p style="color:#c00;padding:20px">Network error &mdash; securacv.com/help works from any browser too.</p>';
    wrap.style.display = '';
  }
});

/* Sheet close-button delegate: every sheet's <button class="sheet-close"
 * data-sheet="..."> routes through here so we don't have to wire a
 * listener per sheet. The data-sheet attribute keys into the sheet/
 * scrim pair; new sheets get keyboard close for free. */
document.addEventListener('click', e => {
  const btn = e.target.closest('.sheet-close');
  if (!btn) return;
  const key = btn.dataset.sheet;
  if (key === 'today')      closeSheet(todaySheet, todayScrim);
  else if (key === 'what')  closeSheet(whatSheet,  whatScrim);
  else if (key === 'fleet') closeSheet(fleetSheet, fleetScrim);
});

/* Global keyboard contract for the modal sheets (audit: a11y pass).
 *
 *   - ESC closes the topmost open sheet — the conventional "get me
 *     out of here" key for any modal dialog.
 *   - Tab / Shift+Tab inside an open sheet cycles focus among the
 *     sheet's own focusables, so keyboard users can't accidentally
 *     tab into the now-hidden background controls. */
document.addEventListener('keydown', e => {
  const openSheetEl = document.querySelector('.sheet.open');
  if (!openSheetEl) return;

  if (e.key === 'Escape') {
    e.preventDefault();
    if (openSheetEl === todaySheet) closeSheet(todaySheet, todayScrim);
    else if (openSheetEl === whatSheet) closeSheet(whatSheet, whatScrim);
    else if (openSheetEl === fleetSheet) closeSheet(fleetSheet, fleetScrim);
    return;
  }

  if (e.key === 'Tab') {
    const focusables = focusableChildren(openSheetEl);
    if (focusables.length === 0) return;
    const first = focusables[0];
    const last  = focusables[focusables.length - 1];
    /* Wrap focus at the boundaries so Tab stays trapped inside the
     * dialog. Standard a11y trap pattern. */
    if (e.shiftKey && document.activeElement === first) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && document.activeElement === last) {
      e.preventDefault();
      first.focus();
    }
  }
});

document.getElementById('settingsBtn').addEventListener('click', () => {
  // The legacy tabbed admin dashboard now lives at /admin. The dashboard
  // is the headline route at /, so /settings deep-links to that deeper
  // power-user surface with the Settings panel already selected.
  window.location.href = '/settings';
});

/* ────────────────────────────────────────────────────────────────────────
 *  Mode + Pet Mode + Calibration
 * ──────────────────────────────────────────────────────────────────────── */
const modeGroup     = document.getElementById('modeGroup');
const modeIndicator = document.getElementById('modeIndicator');
const modeButtons   = modeGroup.querySelectorAll('button[data-mode]');

/* Visual sync only — moves the indicator and updates aria-pressed.
 * persistMode() handles localStorage + the /api/settings round-trip
 * so a window resize doesn't accidentally re-POST every layout shift. */
function setMode(mode) {
  modeButtons.forEach(b => {
    const on = b.dataset.mode === mode;
    b.setAttribute('aria-pressed', on ? 'true' : 'false');
    if (on) {
      const r1 = b.getBoundingClientRect();
      const r2 = modeGroup.getBoundingClientRect();
      modeIndicator.style.transform = `translateX(${r1.left - r2.left - 3}px)`;
      modeIndicator.style.width = r1.width + 'px';
    }
  });
}

async function persistPreset(mode) {
  try {
    await cvFetch('/api/settings', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({preset: mode}),
    });
  } catch {
    /* Device temporarily unreachable; UI + localStorage already
     * updated. Next boot of either side reconciles via /api/settings. */
  }
}

function selectMode(mode) {
  setMode(mode);
  lsSet('csi.mode', mode);
  persistPreset(mode);
}

modeButtons.forEach(b => b.addEventListener('click', () => selectMode(b.dataset.mode)));
setMode(lsGet('csi.mode') || 'balanced');
/* Resize re-runs setMode() (no persist) so the indicator stays
 * aligned with the active button after viewport changes. */
window.addEventListener('resize', () => setMode(lsGet('csi.mode') || 'balanced'));

/* Sensitivity slider: 0..100, 50 = neutral. The server maps the value
 * to a ±20-point offset on top of the preset baseline. We debounce the
 * POST by 400 ms so a continuous drag writes NVS once per pause, not
 * once per slider tick — NVS has a finite write count. */
const sensitivitySlider = document.getElementById('sensitivitySlider');
let g_sensTimer = null;
function persistSensitivity(value) {
  clearTimeout(g_sensTimer);
  g_sensTimer = setTimeout(async () => {
    try {
      await cvFetch('/api/settings', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({sensitivity: value}),
      });
    } catch {}
  }, 400);
}

if (sensitivitySlider) {
  /* Restore from localStorage instantly, then let syncSettingsFromServer
   * reconcile if the device's value differs. */
  const localSens = lsGet('csi.sensitivity');
  if (localSens !== null) sensitivitySlider.value = localSens;
  sensitivitySlider.addEventListener('input', () => {
    /* DON'T use `parseInt(...) || 50` here — `0` is falsy in JS and
     * would silently round a valid minimum-sensitivity drag back up
     * to the neutral midpoint. <input type="range"> always returns a
     * numeric string, so Number() is safe and preserves 0. */
    const v = Number(sensitivitySlider.value);
    lsSet('csi.sensitivity', String(v));
    persistSensitivity(v);
  });
}

function setSwitch(el, on) {
  el.setAttribute('aria-checked', on ? 'true' : 'false');
}
const petSwitch = document.getElementById('petSwitch');
/* Pet Mode lives in two places now: localStorage (instant UI feedback,
 * survives an offline reload) AND the device's NVS via /api/settings
 * (the source of truth that actually changes how core.presence behaves).
 * The localStorage value is the optimistic boot state; we sync from
 * /api/settings as soon as it answers. */
window.PET_MODE = lsGet('csi.pet') === '1';
setSwitch(petSwitch, window.PET_MODE);

/* On load, pull every persistent setting from the device and reconcile
 * against the optimistic localStorage state. The server is the source
 * of truth — if the user changed Pet Mode / preset / sensitivity on
 * another browser, this brings the current tab in line. */
(async function syncSettingsFromServer() {
  try {
    const r = await cvFetch('/api/settings', {cache: 'no-store'});
    if (!r.ok) return;
    const j = await r.json();
    if (typeof j.pet_mode === 'boolean' && j.pet_mode !== window.PET_MODE) {
      window.PET_MODE = j.pet_mode;
      setSwitch(petSwitch, window.PET_MODE);
      lsSet('csi.pet', window.PET_MODE ? '1' : '0');
    }
    if (typeof j.preset === 'string' && j.preset !== lsGet('csi.mode')) {
      lsSet('csi.mode', j.preset);
      setMode(j.preset);
    }
    if (typeof j.sensitivity === 'number' && sensitivitySlider) {
      /* Use String(Number(...)) for consistency with the bytes_today
       * parsing in fetchPrivacyBudget — bitwise `| 0` would coerce
       * to signed 32-bit and is also stylistically inconsistent. */
      const serverSens = String(Number(j.sensitivity));
      if (serverSens !== sensitivitySlider.value) {
        sensitivitySlider.value = serverSens;
        lsSet('csi.sensitivity', serverSens);
      }
    }
    if (j.quiet_hours && typeof j.quiet_hours === 'object') {
      /* Use Number() instead of `| 0` for consistency with the
       * bytes_today / sensitivity parsing established in PRs #370/#371.
       * Minute values are 0..1439 so signed-32-bit coercion would be
       * safe here too — but uniform style keeps the file scannable. */
      const qh = j.quiet_hours;
      if (typeof qh.enabled   === 'boolean') window.QH_ENABLED   = qh.enabled;
      if (typeof qh.start_min === 'number')  window.QH_START_MIN = Number(qh.start_min);
      if (typeof qh.end_min   === 'number')  window.QH_END_MIN   = Number(qh.end_min);
      applyQhUiState();
    }
  } catch {}
})();

async function persistPetMode(value) {
  try {
    await cvFetch('/api/settings', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({pet_mode: value}),
    });
  } catch {
    /* If the device is briefly unreachable we still updated localStorage
     * and the UI; the next boot of either side will reconcile. */
  }
}

function togglePet() {
  window.PET_MODE = !window.PET_MODE;
  setSwitch(petSwitch, window.PET_MODE);
  lsSet('csi.pet', window.PET_MODE ? '1' : '0');
  persistPetMode(window.PET_MODE);
}
petSwitch.addEventListener('click', togglePet);
petSwitch.addEventListener('keydown', e => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); togglePet(); } });

/* ────────────────────────────────────────────────────────────────────────
 *  Quiet Hours
 *
 *  Server stores three NVS keys: enabled (bool), start_min (0..1439),
 *  end_min (0..1439). The dashboard reads them via /api/settings GET and
 *  writes via /api/settings POST under the nested "quiet_hours" object.
 *  Visible effect today is the dimmed activity-ribbon cells; future
 *  notification / anomaly modules will also consult the setting to
 *  suppress alerts.
 *
 *  All wall-clock comparisons happen client-side: the user's browser
 *  has Date(), and the activity ribbon is a 96-cell × 15-min view of
 *  THE USER'S local day. The server doesn't need wall clock for this
 *  feature.
 * ──────────────────────────────────────────────────────────────────────── */
const qhSwitch = document.getElementById('qhSwitch');
const qhTimes  = document.getElementById('qhTimes');
const qhStart  = document.getElementById('qhStart');
const qhEnd    = document.getElementById('qhEnd');

window.QH_ENABLED   = false;
window.QH_START_MIN = 23 * 60;
window.QH_END_MIN   =  7 * 60;

function minutesToTimeStr(m) {
  const h = Math.floor(m / 60), mm = m % 60;
  return String(h).padStart(2,'0') + ':' + String(mm).padStart(2,'0');
}
function timeStrToMinutes(s) {
  const m = /^(\d{1,2}):(\d{2})$/.exec(s || '');
  if (!m) return 0;
  return (parseInt(m[1],10) || 0) * 60 + (parseInt(m[2],10) || 0);
}

function isBucketInQuietHours(bucketIdx) {
  if (!window.QH_ENABLED) return false;
  /* 15-min buckets — convert to minutes-of-day and compare against
   * the [start, end) window. Range may wrap midnight (e.g. start=23:00
   * end=07:00). */
  const bucketMin = bucketIdx * 15;
  const a = window.QH_START_MIN, b = window.QH_END_MIN;
  if (a === b) return false;
  if (a < b)  return bucketMin >= a && bucketMin < b;
  return bucketMin >= a || bucketMin < b;
}

function applyQhUiState() {
  if (qhSwitch) qhSwitch.setAttribute('aria-checked', window.QH_ENABLED ? 'true' : 'false');
  if (qhTimes)  qhTimes.hidden = !window.QH_ENABLED;
  if (qhStart)  qhStart.value = minutesToTimeStr(window.QH_START_MIN);
  if (qhEnd)    qhEnd.value   = minutesToTimeStr(window.QH_END_MIN);
}
applyQhUiState();

let g_qhTimer = null;
async function persistQuietHours() {
  /* Debounce: a user spinning the time picker fires `input` per second.
   * 400 ms matches the sensitivity-slider debounce — one NVS write per
   * pause, never per tick. */
  clearTimeout(g_qhTimer);
  g_qhTimer = setTimeout(async () => {
    try {
      await cvFetch('/api/settings', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          quiet_hours: {
            enabled:   window.QH_ENABLED,
            start_min: window.QH_START_MIN,
            end_min:   window.QH_END_MIN,
          },
        }),
      });
    } catch {}
  }, 400);
}

function toggleQuietHours() {
  window.QH_ENABLED = !window.QH_ENABLED;
  applyQhUiState();
  persistQuietHours();
}
if (qhSwitch) {
  qhSwitch.addEventListener('click', toggleQuietHours);
  qhSwitch.addEventListener('keydown', e => {
    if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); toggleQuietHours(); }
  });
}
if (qhStart) qhStart.addEventListener('input', () => {
  window.QH_START_MIN = timeStrToMinutes(qhStart.value);
  persistQuietHours();
});
if (qhEnd) qhEnd.addEventListener('input', () => {
  window.QH_END_MIN = timeStrToMinutes(qhEnd.value);
  persistQuietHours();
});

const calibrateBtn      = document.getElementById('calibrateBtn');
const calibrateMask     = document.getElementById('calibratingMask');
const calibrateCount    = document.getElementById('calibrateCount');
const calibAcceptBtn    = document.getElementById('calibAcceptBtn');
const calibCancelBtn    = document.getElementById('calibCancelBtn');
const calibCurMotion    = document.getElementById('calibCurMotion');
const calibCurActive    = document.getElementById('calibCurActive');
const calibCurBreath    = document.getElementById('calibCurBreath');
const calibNextMotion   = document.getElementById('calibNextMotion');
const calibNextActive   = document.getElementById('calibNextActive');
const calibNextBreath   = document.getElementById('calibNextBreath');

/* End the overlay regardless of success / cancel / error. Single tear-down
 * spot so the body class, the running flag, and the button label all flip
 * back together. */
function endCalibration(label) {
  document.body.classList.remove('is-calibrating');
  calibrateMask.dataset.state = 'running';
  calibrateBtn.dataset.running = '0';
  if (label) {
    calibrateBtn.textContent = label;
    setTimeout(() => calibrateBtn.textContent = COPY.calibrate.btn, 3000);
  }
}

/* Poll /api/csi/calibrate/status until the device reports ready or
 * timed_out. The recursive setTimeout cadence (rather than setInterval)
 * matches pollStream's pattern — a slow response can't pile up parallel
 * polls. The countdown is derived from samples vs target so the display
 * tracks what the device actually saw, not a JS-side guess that drifts
 * from reality if a window dropped. */
async function pollCalibrationStatus(target) {
  let firstReply = true;
  async function loop() {
    let j;
    try {
      const r = await cvFetch('/api/csi/calibrate/status', {cache: 'no-store'});
      if (!r.ok) throw new Error('status not ok');
      j = await r.json();
    } catch {
      endCalibration(COPY.calibrate.error);
      return;
    }
    if (j.state === 'running') {
      const remain = Math.max(0, target - (j.samples | 0));
      calibrateCount.textContent = remain;
      setTimeout(loop, firstReply ? 250 : 800);
      firstReply = false;
    } else if (j.state === 'ready') {
      calibCurMotion.textContent  = (j.current.motion  | 0);
      calibCurActive.textContent  = (j.current.active  | 0);
      calibCurBreath.textContent  = (j.current.breathing | 0);
      calibNextMotion.textContent = (j.proposed.motion  | 0);
      calibNextActive.textContent = (j.proposed.active  | 0);
      calibNextBreath.textContent = (j.proposed.breathing | 0);
      calibrateMask.dataset.state = 'ready';
    } else {
      /* idle or timed_out — both mean "we have nothing useful to show". */
      endCalibration(COPY.calibrate.error);
    }
  }
  loop();
}

calibrateBtn.addEventListener('click', async () => {
  if (calibrateBtn.dataset.running === '1') return;
  calibrateBtn.dataset.running = '1';
  calibrateMask.dataset.state = 'running';
  document.body.classList.add('is-calibrating');
  /* Default countdown until the first /status returns the real target. */
  calibrateCount.textContent = 10;
  let target = 10;
  try {
    const r = await cvFetch('/api/csi/calibrate/start', {method: 'POST'});
    if (!r.ok) throw new Error('start not ok');
    const j = await r.json();
    if (typeof j.duration_sec === 'number' && j.duration_sec > 0) target = j.duration_sec;
    calibrateCount.textContent = target;
  } catch {
    endCalibration(COPY.calibrate.error);
    return;
  }
  pollCalibrationStatus(target);
});

calibAcceptBtn.addEventListener('click', async () => {
  try {
    const r = await cvFetch('/api/csi/calibrate/apply', {method: 'POST'});
    if (!r.ok) throw new Error('apply not ok');
    endCalibration(COPY.calibrate.done);
  } catch {
    endCalibration(COPY.calibrate.error);
  }
});

calibCancelBtn.addEventListener('click', () => {
  /* No /cancel endpoint — the device just garbage-collects the
   * proposal on the next /start (which resets g_calibration). The
   * dashboard side closes the overlay and the user keeps current
   * thresholds. */
  endCalibration(null);
});

/* ────────────────────────────────────────────────────────────────────────
 *  First-run welcome overlay
 *
 *  Gated by localStorage('csi.onboarding.done') — the user only sees this
 *  once. Pet selection on card 1 auto-enables Pet Mode for cat / small dog
 *  (large-dog Pet Mode is opt-in, see plan). Card 4's primary action runs
 *  the existing calibrate flow so the dashboard arrives baseline-aware.
 * ──────────────────────────────────────────────────────────────────────── */
(function welcomeFlow() {
  if (lsGet('csi.onboarding.done') === '1') return;

  const cards   = COPY.welcome.cards;
  const cardEl  = document.getElementById('welcomeCard');
  if (!cardEl) return;
  let idx       = 0;
  // Multi-select: a household can have a cat AND a large dog. We keep
  // a Set and let the user toggle. "none" is the mutually-exclusive
  // escape hatch — picking it clears the others; picking any specific
  // pet clears "none". Persisted as a comma-separated list in
  // localStorage('csi.pet.kinds').
  const initialKinds = (lsGet('csi.pet.kinds') || '')
    .split(',').map(s => s.trim()).filter(Boolean);
  const chosenPets = new Set(initialKinds);

  /* Focus management state for the modal-dialog upgrade.
   *  - s_returnFocus  : where to send focus when the user skips so
   *                     they don't end up at <body> with no visible
   *                     focus ring.
   *  - s_lastIdx      : the most recent card index render() focused
   *                     for. Comparing against the current `idx` lets
   *                     us focus the card on EVERY step transition
   *                     (so AT announces the new card's title) while
   *                     pet-toggle re-renders within the same step
   *                     keep focus on the just-pressed [data-pet]
   *                     button (PR #402 review r3214577892 /
   *                     r3214577894). Starts at -1 so the very first
   *                     render() (idx=0) trips the focus move. */
  let s_returnFocus = document.activeElement;
  let s_lastIdx     = -1;

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, ch => ({
      '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
    }[ch]));
  }

  function dotsHtml() {
    return '<div class="dots">' +
      cards.map((_, i) => `<span class="dot${i === idx ? ' active' : ''}"></span>`).join('') +
      '</div>';
  }

  function petChoicesHtml() {
    const choices = COPY.welcome.petChoices.map(c =>
      `<button data-pet="${c.id}" role="checkbox" aria-pressed="${chosenPets.has(c.id) ? 'true' : 'false'}">${escapeHtml(c.label)}</button>`
    ).join('');
    return `
      <div class="step">${escapeHtml(COPY.welcome.petQuestion)}</div>
      <p class="pet-hint">${escapeHtml(COPY.welcome.petHint)}</p>
      <div class="pet-choices">${choices}</div>`;
  }

  function render() {
    const c = cards[idx];
    const learn = c.learnMore
      ? `<a href="#" class="learn-more" data-action="what">${escapeHtml(c.learnMore)} →</a>`
      : '';
    cardEl.innerHTML = `
      <div class="step">${escapeHtml(c.step)}</div>
      <h2 id="welcomeTitle">${escapeHtml(c.title)}</h2>
      <p>${escapeHtml(c.body)}</p>
      ${learn}
      ${c.pets ? petChoicesHtml() : ''}
      <div class="actions">
        <button class="skip" data-action="skip">${escapeHtml(c.skip)}</button>
        <button class="primary" data-action="next">${escapeHtml(c.primary)}</button>
      </div>
      ${dotsHtml()}
    `;
    cardEl.classList.remove('show');
    void cardEl.offsetWidth;   // force reflow so the show class re-triggers the spring
    cardEl.classList.add('show');
    /* Move focus to the card on every step transition so screen
     * readers announce the new card's accessible name (the
     * aria-labelledby="welcomeTitle" reference points at the freshly-
     * rendered <h2>) and keyboard users don't lose their place to
     * <body> after innerHTML clobbers the DOM. Deferred to the next
     * animation frame so the focus ring isn't visibly thrown before
     * the spring transition.
     *
     * Pet-toggle re-renders fire within the same step (idx
     * unchanged), so they fall through this block and the click
     * handler's explicit re-focus on the freshly-rendered
     * [data-pet] button takes over — keyboard users keep toggling
     * with Space without focus jumping back to the top. PR #402
     * reviews r3214577892 / r3214577894. */
    if (s_lastIdx !== idx) {
      s_lastIdx = idx;
      requestAnimationFrame(() => cardEl.focus());
    }
  }

  function finish(launchCalibrate) {
    lsSet('csi.onboarding.done', '1');
    // Persist the comma-separated list. Empty Set means the user
    // skipped without answering — leave any previous answer alone.
    if (chosenPets.size > 0) {
      lsSet('csi.pet.kinds', Array.from(chosenPets).join(','));
    }
    document.body.classList.remove('is-onboarding');
    if (launchCalibrate) {
      // Hand off to the existing calibrate path. The calibrate button's
      // click handler manages its own focus (the calibrate overlay's
      // controls take over), so no explicit focus move needed here.
      const btn = document.getElementById('calibrateBtn');
      if (btn) btn.click();
    } else {
      /* Restore focus to wherever the user was before onboarding so
       * keyboard / screen-reader users don't land on <body> with no
       * visible focus ring after dismissing. Falls back to the help
       * button (the natural "what's this?" entry point) if the
       * pre-onboarding active element is gone or was just <body>. */
      if (s_returnFocus && typeof s_returnFocus.focus === 'function'
          && s_returnFocus !== document.body
          && document.contains(s_returnFocus)) {
        s_returnFocus.focus();
      } else {
        const helpBtn = document.getElementById('helpBtn');
        if (helpBtn) helpBtn.focus();
      }
    }
  }

  cardEl.addEventListener('click', e => {
    const pet = e.target.closest('[data-pet]');
    if (pet) {
      const id = pet.dataset.pet;
      // Multi-select toggle with one rule: "none" is mutually exclusive
      // with the specific pets. Picking "none" clears every other choice;
      // picking any specific pet clears "none". This keeps the model
      // both inclusive (a cat AND a large dog is a valid combination)
      // and unambiguous (you can't simultaneously claim "no pets" and
      // "small dog").
      if (id === 'none') {
        if (chosenPets.has('none')) {
          chosenPets.delete('none');
        } else {
          chosenPets.clear();
          chosenPets.add('none');
        }
      } else {
        chosenPets.delete('none');
        if (chosenPets.has(id)) chosenPets.delete(id); else chosenPets.add(id);
      }

      // Pet Mode auto-enable: any of {cat, small dog} → on. Large dog
      // and "no pets" do not change Pet Mode — large dogs occasionally
      // lock briefly on the human Goertzel band (suppressing them with
      // Pet Mode would hide real activity), and a returning user who
      // selects "no pets" while having had Pet Mode on for some other
      // reason shouldn't have it silently flipped off. Users can
      // always toggle Pet Mode from the dashboard switch.
      if (chosenPets.has('cat') || chosenPets.has('small')) {
        window.PET_MODE = true;
        lsSet('csi.pet', '1');
        const sw = document.getElementById('petSwitch');
        if (sw) sw.setAttribute('aria-checked', 'true');
        // Push to the device too so core.presence honors the choice
        // immediately, not just after the user re-toggles the switch.
        persistPetMode(true);
      }
      render();
      /* render() blew away the DOM via innerHTML, so the pet button
       * the user just clicked is now a different element. Find the
       * fresh button by its data-pet id and put focus back on it so
       * a keyboard user can keep toggling with Space without having
       * to Tab back through the card. */
      const restored = cardEl.querySelector('[data-pet="' + CSS.escape(id) + '"]');
      if (restored) restored.focus();
      return;
    }
    const action = e.target.closest('[data-action]');
    if (!action) return;
    e.preventDefault();
    const a = action.dataset.action;
    if (a === 'skip') {
      finish(false);
    } else if (a === 'what') {
      // Open the "What it can / can't see" sheet without dismissing
      // onboarding. The sheet's z-index (101) sits below the welcome
      // mask (110), so we temporarily step the welcome out of the way
      // via body.welcome-paused; the sheet's existing scrim handler
      // restores it when the user dismisses.
      document.body.classList.add('welcome-paused');
      const help = document.getElementById('helpBtn');
      if (help) help.click();
    } else if (a === 'next') {
      idx++;
      if (idx >= cards.length) {
        // Card 4's primary launches calibrate.
        finish(true);
      } else {
        render();
      }
    }
  });

  /* Modal-dialog keyboard contract for the welcome card.
   *
   *   - ESC dismisses the flow as a "skip" (same as the Skip button).
   *     Standard "get me out of here" affordance for a modal dialog.
   *   - Tab / Shift+Tab cycles focus among the card's own focusables
   *     so keyboard users can't accidentally tab into the (still-
   *     rendered but masked) dashboard underneath.
   *
   * Gated on body.is-onboarding so the handler only fires while the
   * welcome flow is active. The nested-sheet exception is two-layered
   * because event ordering vs. side effects matters here:
   *
   *   1. e.defaultPrevented bail catches the ESC race (PR #402
   *      review r3214577082). When the "Learn more" path opens the
   *      What sheet on top of the card and the user presses ESC, the
   *      sheet's keydown handler (registered earlier, in module
   *      scope) runs first, calls preventDefault, and synchronously
   *      removes the sheet's `.open` class. By the time this
   *      handler runs, querySelector('.sheet.open') would return
   *      null — so without the defaultPrevented gate, ESC would
   *      close the sheet AND skip onboarding in one keystroke.
   *
   *   2. The querySelector('.sheet.open') gate covers the Tab path,
   *      where the sheet handler only preventDefaults at boundary
   *      cycling. Mid-sheet Tab presses leave the gate as the only
   *      thing keeping the welcome handler from racing the sheet's
   *      own focus management. */
  document.addEventListener('keydown', e => {
    if (!document.body.classList.contains('is-onboarding')) return;
    if (e.defaultPrevented) return;  /* sheet handler already consumed it */
    if (document.querySelector('.sheet.open')) return;  /* sheet handler wins */

    if (e.key === 'Escape') {
      e.preventDefault();
      finish(false);
      return;
    }
    if (e.key === 'Tab') {
      const focusables = focusableChildren(cardEl);
      if (focusables.length === 0) return;
      const first = focusables[0];
      const last  = focusables[focusables.length - 1];
      if (e.shiftKey && document.activeElement === first) {
        e.preventDefault();
        last.focus();
      } else if (!e.shiftKey && document.activeElement === last) {
        e.preventDefault();
        first.focus();
      }
    }
  });

  document.body.classList.add('is-onboarding');
  render();
})();

/* ────────────────────────────────────────────────────────────────────────
 *  Animation / poll loop
 * ──────────────────────────────────────────────────────────────────────── */
function tick() {
  drawWaveform();
  drawRibbon();
  if (latestRawVector) drawHeatmap();
  // The reveal panel's static rows update on /api/csi/stream poll, but
  // the "Last event · 7s ago" row needs the wall-clock to keep ticking
  // between polls. updateSenseDetail() short-circuits when the
  // disclosure is collapsed so the cost is a single open-flag check
  // per frame in the common case.
  updateSenseDetail();
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);

/* Recursive setTimeout instead of setInterval — async polls can pile up if a
 * fetch hangs (e.g. AP intermittently unreachable), and the device's tiny
 * httpd worker pool is then starved when the next interval fires. The
 * recursive form schedules the next poll only AFTER the current one
 * settles. */
function pollLoop(fn, intervalMs) {
  (async function loop() {
    try { await fn(); }
    finally { setTimeout(loop, intervalMs); }
  })();
}
pollLoop(pollStream, 1000);
pollLoop(pollRawVector, 1000);

/* Mic pill — shows whether the Canary is listening for smoke/CO alarms.
 * Stays hidden on firmware without the mic (the address answers 404/401
 * or ok:false). Tapping it flips the hard mute after a confirm. */
async function pollMicPill() {
  const pill = document.getElementById('micPill');
  if (!pill) return;
  try {
    const r = await cvFetch('/api/audio/status', { cache: 'no-store' });
    if (!r.ok) { pill.style.display = 'none'; return; }
    const j = await r.json();
    if (!j.ok) { pill.style.display = 'none'; return; }
    pill.style.display = '';
    const muted = !!j.muted;
    pill.textContent = muted ? 'Mic muted' : 'Mic on';
    pill.classList.toggle('muted', muted);
    pill.dataset.muted = muted ? '1' : '0';
  } catch (_) { /* keep last rendered state */ }
}
document.getElementById('micPill').addEventListener('click', async () => {
  const pill = document.getElementById('micPill');
  const muted = pill.dataset.muted === '1';
  const ask = muted
    ? 'Turn the microphone back on? The Canary will listen for smoke and CO alarms again.'
    : 'Mute the microphone? The Canary will NOT hear smoke or CO alarms until you turn it back on.';
  if (!window.confirm(ask)) return;
  try {
    await cvFetch('/api/audio/mute', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ muted: !muted }),
    });
  } catch (_) { /* poll below re-syncs the pill either way */ }
  setTimeout(pollMicPill, 600);
});
pollLoop(pollMicPill, 5000);

/* ────────────────────────────────────────────────────────────────────────
 *  Device identity badge
 *
 *  /api/device-info is the public, no-auth endpoint that exposes the
 *  canary's per-device id (e.g. "canary-s3-AB7K"). /api/status carries
 *  the same field but is gated by handle_status_auth, so an
 *  un-authenticated dashboard fetch silently 401s — use /api/device-info
 *  here. Surface the id in the topbar so a user with a fleet always
 *  knows which device they opened, and so a device that just got
 *  renamed or re-flashed shows the right label without a firmware tweak.
 *  Best-effort: a transient network blip leaves the "canary" placeholder
 *  in place, never breaks the dashboard.
 * ──────────────────────────────────────────────────────────────────────── */
(async function fetchDeviceId() {
  try {
    const r = await cvFetch('/api/device-info', {cache: 'no-store'});
    if (!r.ok) return;
    const j = await r.json();
    const el = document.getElementById('device-id');
    if (el && j.device_id) el.textContent = j.device_id;
  } catch {}
})();

/* PWA service worker — caches the dashboard shell so add-to-home-screen
 * works offline. Live API routes deliberately bypass the SW (see /sw.js
 * served by csi_integration.cpp's handle_sense_sw). Registration is
 * best-effort: if the browser doesn't support service workers (very old
 * iOS, embedded WebViews) the dashboard still works from the live
 * network, just without the offline shell. */
if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('/sw.js', { scope: '/' })
      .catch(e => console.warn('SW register failed:', e));
  });
}

/* ────────────────────────────────────────────────────────────────────────
 *  Tuning Lab reveal (Tier 4 #10)
 *
 *  Hidden by design. Two ways in:
 *    1. Long-press the device-id chip in the topbar (~700 ms).
 *    2. Append ?tune=1 to any dashboard URL.
 *
 *  Both paths simply navigate to /tune. The lab itself enforces no auth
 *  beyond the unguessable URL — every coefficient is surfaced as a slider
 *  and any POST writes through the same NVS path the dashboard's settings
 *  panel uses. This is not a security boundary; it's a "you must be
 *  this curious to enter" affordance for tinkerers.
 * ──────────────────────────────────────────────────────────────────────── */
(function tuneLabReveal() {
  if (new URLSearchParams(window.location.search).get('tune') === '1') {
    window.location.replace('/tune');
    return;
  }
  const el = document.getElementById('device-id');
  if (!el) return;
  let pressTimer = 0;
  const start = () => {
    if (pressTimer) clearTimeout(pressTimer);
    pressTimer = setTimeout(() => { window.location.href = '/tune'; }, 700);
  };
  const cancel = () => {
    if (pressTimer) { clearTimeout(pressTimer); pressTimer = 0; }
  };
  el.addEventListener('mousedown',  start);
  el.addEventListener('mouseup',    cancel);
  el.addEventListener('mouseleave', cancel);
  el.addEventListener('touchstart', start, {passive: true});
  el.addEventListener('touchend',   cancel);
  el.addEventListener('touchcancel',cancel);
})();
</script>
</body>
</html>
)DASHBOARD";
#endif  // !CANARY_WEB_ASSETS_GZIPPED

#endif /* SECURACV_CSI_DASHBOARD_HTML_H */
