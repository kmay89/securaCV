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

#include <Arduino.h>

static const char CSI_DASHBOARD_HTML[] PROGMEM = R"DASHBOARD(<!doctype html>
<html lang="en" data-state="sensing">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0c0a18" media="(prefers-color-scheme: dark)">
<meta name="theme-color" content="#f5f4ff" media="(prefers-color-scheme: light)">
<title>Canary · Sensing</title>
<style>
  /* ── Design tokens ──────────────────────────────────────────────────── */
  :root {
    --font-sans: -apple-system, BlinkMacSystemFont, "SF Pro Display", "SF Pro Text",
                 "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;

    --bg-base:        #f5f4ff;
    --bg-veil:        rgba(255,255,255,0.55);
    --fg:             #1a1730;
    --fg-soft:        #5b5478;
    --fg-mute:        #8a82a3;
    --hairline:       rgba(26,23,48,0.10);
    --shadow-card:    0 4px 24px rgba(60,30,160,0.10), 0 1px 3px rgba(60,30,160,0.06);

    /* Hero gradients keyed off room state via [data-state] on <html> */
    --orb-1:          #cfd6ff;
    --orb-2:          #b9c9ff;
    --orb-3:          #8e9eff;
    --orb-glow:       rgba(140,158,255,0.6);
    --bg-1:           #f5f4ff;
    --bg-2:           #ecf2ff;
    --bg-3:           #f9f7ff;
    --accent:         #8e9eff;

    --orb-size:       min(72vmin, 420px);
    --pulse-dur:      4.3s; /* default inhale-exhale; replaced by JS at confirmed */

    --ease-soft:      cubic-bezier(.22,.94,.34,1.0);
    --ease-spring:    cubic-bezier(.34,1.56,.64,1.0);
  }

  @media (prefers-color-scheme: dark) {
    :root {
      --bg-base:    #0c0a18;
      --bg-veil:    rgba(20,17,40,0.55);
      --fg:         #efeaff;
      --fg-soft:    #b3aad6;
      --fg-mute:    #6a6285;
      --hairline:   rgba(255,255,255,0.10);
      --shadow-card:0 8px 28px rgba(0,0,0,0.5), 0 1px 3px rgba(0,0,0,0.4);

      --orb-1:      #5466b8;
      --orb-2:      #364080;
      --orb-3:      #1f2546;
      --orb-glow:   rgba(120,140,255,0.45);
      --bg-1:       #0c0a18;
      --bg-2:       #131027;
      --bg-3:       #0d0a1c;
    }
  }

  /* State-driven theme. The JS sets <html data-state="..."> on each /api/csi/stream tick. */
  html[data-state="empty"]    { --orb-1:#cfd6ff;--orb-2:#b9c9ff;--orb-3:#8e9eff;--orb-glow:rgba(140,158,255,.55); }
  html[data-state="sensing"]  { --orb-1:#dad6ff;--orb-2:#c0bce5;--orb-3:#9c97c2;--orb-glow:rgba(180,170,220,.45); }
  html[data-state="subtle"]   { --orb-1:#ffe0c2;--orb-2:#ffc99a;--orb-3:#f9a86b;--orb-glow:rgba(249,168,107,.55); }
  html[data-state="quiet"]    { --orb-1:#c2f0e4;--orb-2:#7ed8c0;--orb-3:#3eb89e;--orb-glow:rgba(62,184,158,.55); }
  html[data-state="active"]   { --orb-1:#a4f4eb;--orb-2:#62e5d4;--orb-3:#1ec5b1;--orb-glow:rgba(30,197,177,.65); }
  html[data-state="together"] { --orb-1:#cdb8ff;--orb-2:#9c87f3;--orb-3:#5e4cc5;--orb-glow:rgba(94,76,197,.55); }

  @media (prefers-color-scheme: dark) {
    html[data-state="empty"]    { --orb-1:#3b4380;--orb-2:#262d5b;--orb-3:#171a36;--orb-glow:rgba(110,130,220,.45); }
    html[data-state="sensing"]  { --orb-1:#48426a;--orb-2:#312c52;--orb-3:#1d1936;--orb-glow:rgba(150,140,200,.40); }
    html[data-state="subtle"]   { --orb-1:#7a4c2a;--orb-2:#522e15;--orb-3:#2c170b;--orb-glow:rgba(220,140,90,.50); }
    html[data-state="quiet"]    { --orb-1:#1f5d4f;--orb-2:#103a30;--orb-3:#082019;--orb-glow:rgba(50,200,160,.50); }
    html[data-state="active"]   { --orb-1:#1d7d6e;--orb-2:#0e4f44;--orb-3:#062a25;--orb-glow:rgba(30,225,190,.65); }
    html[data-state="together"] { --orb-1:#4d3da3;--orb-2:#332884;--orb-3:#1d164a;--orb-glow:rgba(140,110,240,.55); }
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

  .pet-row {
    display: inline-flex; gap: 10px; align-items: center;
    font-size: 14px; color: var(--fg-soft);
  }
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
  .privacy-pill {
    font-size: 11px; color: var(--fg-mute);
    background: var(--bg-veil);
    border: 1px solid var(--hairline);
    padding: 5px 10px; border-radius: 999px;
    letter-spacing: 0.02em;
  }
  .privacy-pill.warm { color: #b87800; }
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
  @media (prefers-color-scheme: dark) { .calibrating-mask { background: rgba(0,0,0,0.55); } }
</style>
</head>
<body>

<header class="topbar">
  <div class="brand">SecuraCV<span class="device" id="device-id">canary</span></div>
  <div class="topbar-actions">
    <button class="iconbtn" id="todayBtn" data-tip="todayBtn">Today</button>
    <button class="iconbtn" id="settingsBtn" data-tip="settingsBtn">Settings</button>
  </div>
</header>

<main>
  <section class="hero">
    <div class="plate">
      <h1 class="state" id="state">Sensing…</h1>
      <p class="sub"   id="sub">Just getting a feel for the room.</p>
      <p class="meta"  id="meta">tentative</p>
      <button class="help" id="helpBtn" aria-label="What can the sensor see?" data-tip="helpBtn">?</button>
    </div>
    <div class="orb-wrap" id="orbWrap" data-tip="orb">
      <div class="orb"        id="orb"></div>
      <div class="orb-shell"  id="orbShell"></div>
      <div class="ripple"     id="ripple"></div>
    </div>
  </section>

  <section class="ribbon-card">
    <div class="ribbon-head">
      <span>Last 24 hours</span>
      <span id="ribbonReadout">—</span>
    </div>
    <canvas id="ribbon" width="800" height="56"></canvas>
  </section>

  <section class="dock">
    <canvas id="waveform" width="800" height="96"></canvas>
    <div class="waveform-legend">
      <span><span class="legend-dot motion"></span>motion</span>
      <span><span class="legend-dot breathing"></span>breathing</span>
      <span style="margin-left:auto" id="frameCount">—</span>
    </div>

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

      <details class="tinker">
        <summary>Details</summary>
        <div class="tinker-row">
          <label>
            <span>Sensitivity <span data-tip="sensitivity">ⓘ</span></span>
            <input type="range" id="sensitivitySlider" min="0" max="100" value="50">
          </label>
          <label>
            <span>Breath sound <span data-tip="breathAudio">ⓘ</span></span>
            <span class="switch" id="audioSwitch" role="switch" aria-checked="false" tabindex="0" data-tip="breathAudio"></span>
          </label>
          <label style="grid-column:1/-1">
            <span>Live numbers <span data-tip="rawVector">ⓘ</span></span>
            <canvas id="rawHeatmap" width="800" height="32" style="width:100%;height:32px;border-radius:6px;display:block;background:rgba(0,0,0,0.04)"></canvas>
          </label>
        </div>
      </details>
    </div>
  </section>
</main>

<!-- Today receipts sheet -->
<div class="sheet-scrim" id="todayScrim"></div>
<aside class="sheet" id="todaySheet" aria-labelledby="todayTitle">
  <div class="sheet-grab"></div>
  <div class="sheet-head">
    <h2 id="todayTitle">Today</h2>
    <span class="privacy-pill" id="privacyPill">Today: 0 bytes left the device</span>
  </div>
  <div class="sheet-body" id="todayBody">
    <p style="color:var(--fg-mute)">Loading…</p>
  </div>
</aside>

<!-- "What it can / can't see" sheet -->
<div class="sheet-scrim" id="whatScrim"></div>
<aside class="sheet" id="whatSheet" aria-labelledby="whatTitle">
  <div class="sheet-grab"></div>
  <div class="sheet-head">
    <h2 id="whatTitle">What the sensor can and can't see</h2>
  </div>
  <div class="sheet-body" id="whatBody"></div>
</aside>

<!-- First-run welcome overlay -->
<div class="welcome-mask" id="welcomeMask">
  <div class="welcome-card" id="welcomeCard" role="dialog" aria-labelledby="welcomeTitle"></div>
</div>

<!-- Calibration overlay -->
<div class="calibrating-mask" id="calibratingMask">
  <div>
    <div class="label">Learning your empty room.</div>
    <div class="count" id="calibrateCount">60</div>
    <div class="label" style="margin-top:8px">Step out for a minute.</div>
  </div>
</div>

<script>
"use strict";

/* ────────────────────────────────────────────────────────────────────────
 *  Microcopy bank — all user-facing strings live here.
 *  Reading-grade target ≤ 6th. Localization is a one-file edit.
 * ──────────────────────────────────────────────────────────────────────── */
const COPY = {
  states: {
    sensing:   { name: 'Sensing…',       sub: "Just getting a feel for the room." },
    empty:     { name: 'Empty',          sub: "Nobody's home right now." },
    subtle:    { name: 'Subtle motion',  sub: "Something small is moving." },
    quiet:     { name: 'Quiet',          sub: "Someone's here. Sitting still." },
    quiet_bpm: { name: 'Quiet',          sub: "Someone's here. Breathing about {bpm} a minute." },
    active:    { name: 'Active',         sub: "Lots of movement right now." },
    together:  { name: 'Together',       sub: "More than one person." },
    pet:       { name: 'Empty',          sub: "A small movement, probably your pet." },
  },
  tooltips: {
    orb:         "What the room feels like right now.",
    helpBtn:     "See exactly what the sensor can and can't notice.",
    todayBtn:    "See everything that happened today.",
    settingsBtn: "Tweak how the sensor behaves.",
    calibrate:   "Step out for one minute so the sensor learns your empty room.",
    sensitive:   "Picks up small movements. Best for one quiet room.",
    balanced:    "Catches normal movement. Good for most homes.",
    quiet:       "Only big movements. Best with kids, pets, or open spaces.",
    petMode:     "Cats and small dogs breathe faster than people. Turn this on so they don't trigger 'someone's here'.",
    sensitivity: "Slide right to notice more. Slide left to ignore tiny movements.",
    rawVector:   "For tinkerers. Shows the live numbers behind the scenes.",
    breathAudio: "Play a soft breath sound that follows the rhythm in the room. Off by default.",
    ribbonCell:  "Tap a moment to see what was happening then.",
  },
  errors: {
    disconnect:  "Can't reach the sensor. Move closer to your router and try again.",
    calibrating: "Learning your empty room. {seconds} seconds left.",
    calibrated:  "Got it. Your empty room is set.",
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
        body: "This little device watches the WiFi waves bouncing around your room. When something moves, the waves change. That's it.",
        primary: "Got it",
        skip: "Skip",
        // Card 1 also asks the optional Pets question — handled in JS.
        pets: true,
      },
      {
        step: "Step 2 of 4",
        title: "Watches WiFi waves, not video.",
        body: "No camera. No microphone. No MAC addresses stored. Nothing leaves the device.",
        primary: "Next",
        skip: "Skip",
      },
      {
        step: "Step 3 of 4",
        title: "Best in the same room.",
        body: "Through one wall: motion only, no breathing claims. Through a floor: depends on your home. We're honest about what the sensor can and can't see.",
        learnMore: "What it can and can't see",
        primary: "Next",
        skip: "Skip",
      },
      {
        step: "Step 4 of 4",
        title: "One last thing — let's learn your empty room.",
        body: "Step out of this room for a minute. The sensor will use that minute to figure out what 'empty' looks like, so it knows when somebody's actually here.",
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
 *  Live waveform
 * ──────────────────────────────────────────────────────────────────────── */
const HISTORY_LEN = 60;
const motionHist = new Array(HISTORY_LEN).fill(0);
const breathHist = new Array(HISTORY_LEN).fill(0);

function pushHistory(motion, breathing) {
  motionHist.shift(); motionHist.push(motion);
  breathHist.shift(); breathHist.push(breathing);
}

function drawWaveform() {
  const canvas = document.getElementById('waveform');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const w = canvas.width = canvas.clientWidth * (window.devicePixelRatio||1);
  const h = canvas.height = canvas.clientHeight * (window.devicePixelRatio||1);
  ctx.clearRect(0,0,w,h);

  function trace(arr, hue, alpha, width) {
    ctx.beginPath();
    for (let i = 0; i < arr.length; i++) {
      const x = (i / (HISTORY_LEN-1)) * w;
      const y = h - (arr[i]/100) * (h * 0.92) - h * 0.04;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = hue;
    ctx.globalAlpha = alpha;
    ctx.lineWidth = width;
    ctx.lineCap = 'round'; ctx.lineJoin = 'round';
    ctx.stroke();
    ctx.globalAlpha = 1;
  }
  // Motion: glow + crisp
  const motionColor   = getComputedStyle(document.documentElement).getPropertyValue('--orb-3').trim() || '#1ec5b1';
  const breathColor   = getComputedStyle(document.documentElement).getPropertyValue('--accent').trim() || '#8e9eff';
  trace(motionHist, motionColor, 0.18, 8);
  trace(motionHist, motionColor, 1.0, 2 * (window.devicePixelRatio||1));
  trace(breathHist, breathColor, 0.18, 8);
  trace(breathHist, breathColor, 1.0, 2 * (window.devicePixelRatio||1));
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
    const a = 0.25 + intensity * 0.65;
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

async function pollStream() {
  try {
    const r = await fetch('/api/csi/stream', {cache: 'no-store'});
    if (!r.ok) throw new Error('stream not ok');
    const j = await r.json();

    const motion    = (j.motion    | 0);
    const breathing = (j.breathing | 0);
    pushHistory(motion, breathing);

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
      }
      // Push current bucket intensity
      const idx = currentBucketIdx();
      const combined = Math.min(255, Math.max(motion*2.55, ribbonData[idx]));
      ribbonData[idx] = combined;
    } else {
      setState('sensing', {confidence: j.confidence || 'tentative'});
    }

    document.getElementById('frameCount').textContent =
      `motion ${motion} · breathing ${breathing}`;
    document.body.classList.remove('disconnected');
  } catch (err) {
    document.body.classList.add('disconnected');
    document.getElementById('frameCount').textContent = COPY.errors.disconnect;
  }
}

async function fetchToday() {
  const body = document.getElementById('todayBody');
  body.innerHTML = '<p style="color:var(--fg-mute)">Loading…</p>';
  try {
    const r = await fetch('/api/events/today', {cache: 'no-store'});
    if (!r.ok) throw new Error('today fetch returned ' + r.status);
    const j = await r.json();
    const events = j.events || [];
    if (events.length === 0) {
      body.innerHTML = '<p style="color:var(--fg-mute);padding:20px 0">Nothing happened yet today. The sensor is watching.</p>';
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
        await fetch('/api/events/dismiss', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({event_id: id}),
        });
        btn.closest('.event-row').classList.add('dismissed');
        btn.disabled = true;
      });
    });
  } catch (err) {
    body.innerHTML = '<p style="color:var(--fg-mute)">' + COPY.errors.disconnect + '</p>';
  }
}

async function pollRawVector() {
  if (!document.querySelector('details.tinker[open]')) return;
  try {
    const r = await fetch('/api/csi/window', {cache: 'no-store'});
    if (!r.ok) return;  // 403 means privacy ceiling not raised; silently skip.
    const j = await r.json();
    if (j.v) latestRawVector = j.v;
    drawHeatmap();
  } catch {}
}

/* Privacy Budget pill — literal byte counter for outbound traffic.
 * /api/privacy-budget returns {bytes_today, ceiling, since_ms}. We
 * format human-friendly ("0 bytes" / "47 bytes" / "1.2 KB") and
 * warm-tint the pill if either bytes > 0 or the privacy ceiling is
 * above P0. Best-effort — a transient fetch failure leaves the
 * placeholder in place. */
function formatBytes(n) {
  if (n < 1024)             return n + ' bytes';
  if (n < 1024 * 1024)      return (n / 1024).toFixed(1) + ' KB';
  return (n / (1024 * 1024)).toFixed(1) + ' MB';
}

async function fetchPrivacyBudget() {
  const pill = document.getElementById('privacyPill');
  if (!pill) return;
  try {
    const r = await fetch('/api/privacy-budget', {cache: 'no-store'});
    if (!r.ok) return;
    const j = await r.json();
    const bytes = (j.bytes_today | 0);
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

/* ────────────────────────────────────────────────────────────────────────
 *  Sheets
 * ──────────────────────────────────────────────────────────────────────── */
function openSheet(sheet, scrim) {
  scrim.classList.add('open');
  sheet.classList.add('open');
  document.body.style.overflow = 'hidden';
}
function closeSheet(sheet, scrim) {
  scrim.classList.remove('open');
  sheet.classList.remove('open');
  document.body.style.overflow = '';
}

const todaySheet = document.getElementById('todaySheet');
const todayScrim = document.getElementById('todayScrim');
document.getElementById('todayBtn').addEventListener('click', () => {
  fetchToday();
  fetchPrivacyBudget();
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
whatScrim.addEventListener('click', () => {
  closeSheet(whatSheet, whatScrim);
  // If we got here from the welcome flow's "Learn more" link, restore
  // the welcome mask so the user lands back on the same card. No-op
  // when the class isn't set, so legitimate dashboard visits are
  // unaffected.
  document.body.classList.remove('welcome-paused');
});

document.getElementById('settingsBtn').addEventListener('click', () => {
  // The legacy tabbed admin dashboard now lives at /admin. The dashboard
  // is the headline route at /, so the Settings button points users
  // toward the deeper power-user surface (camera peek, witness export,
  // device-level config, etc.).
  window.location.href = '/admin';
});

/* ────────────────────────────────────────────────────────────────────────
 *  Mode + Pet Mode + Calibration
 * ──────────────────────────────────────────────────────────────────────── */
const modeGroup     = document.getElementById('modeGroup');
const modeIndicator = document.getElementById('modeIndicator');
const modeButtons   = modeGroup.querySelectorAll('button[data-mode]');

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
  localStorage.setItem('csi.mode', mode);
}
modeButtons.forEach(b => b.addEventListener('click', () => setMode(b.dataset.mode)));
setMode(localStorage.getItem('csi.mode') || 'balanced');
window.addEventListener('resize', () => setMode(localStorage.getItem('csi.mode') || 'balanced'));

function setSwitch(el, on) {
  el.setAttribute('aria-checked', on ? 'true' : 'false');
}
const petSwitch = document.getElementById('petSwitch');
/* Pet Mode lives in two places now: localStorage (instant UI feedback,
 * survives an offline reload) AND the device's NVS via /api/settings
 * (the source of truth that actually changes how core.presence behaves).
 * The localStorage value is the optimistic boot state; we sync from
 * /api/settings as soon as it answers. */
window.PET_MODE = localStorage.getItem('csi.pet') === '1';
setSwitch(petSwitch, window.PET_MODE);

(async function syncPetModeFromServer() {
  try {
    const r = await fetch('/api/settings', {cache: 'no-store'});
    if (!r.ok) return;
    const j = await r.json();
    if (typeof j.pet_mode === 'boolean' && j.pet_mode !== window.PET_MODE) {
      window.PET_MODE = j.pet_mode;
      setSwitch(petSwitch, window.PET_MODE);
      localStorage.setItem('csi.pet', window.PET_MODE ? '1' : '0');
    }
  } catch {}
})();

async function persistPetMode(value) {
  try {
    await fetch('/api/settings', {
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
  localStorage.setItem('csi.pet', window.PET_MODE ? '1' : '0');
  persistPetMode(window.PET_MODE);
}
petSwitch.addEventListener('click', togglePet);
petSwitch.addEventListener('keydown', e => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); togglePet(); } });

const audioSwitch = document.getElementById('audioSwitch');
window.AUDIO_ON = false;
audioSwitch.addEventListener('click', () => {
  window.AUDIO_ON = !window.AUDIO_ON;
  setSwitch(audioSwitch, window.AUDIO_ON);
});

const calibrateBtn   = document.getElementById('calibrateBtn');
const calibrateMask  = document.getElementById('calibratingMask');
const calibrateCount = document.getElementById('calibrateCount');
calibrateBtn.addEventListener('click', () => {
  if (calibrateBtn.dataset.running === '1') return;
  calibrateBtn.dataset.running = '1';
  document.body.classList.add('is-calibrating');
  let secs = 60;
  calibrateCount.textContent = secs;
  const tid = setInterval(() => {
    secs--;
    calibrateCount.textContent = secs;
    if (secs <= 0) {
      clearInterval(tid);
      document.body.classList.remove('is-calibrating');
      calibrateBtn.dataset.running = '0';
      calibrateBtn.textContent = 'Got it. Your empty room is set.';
      setTimeout(() => calibrateBtn.textContent = 'Calibrate empty room', 3000);
    }
  }, 1000);
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
  if (localStorage.getItem('csi.onboarding.done') === '1') return;

  const cards   = COPY.welcome.cards;
  const cardEl  = document.getElementById('welcomeCard');
  if (!cardEl) return;
  let idx       = 0;
  // Multi-select: a household can have a cat AND a large dog. We keep
  // a Set and let the user toggle. "none" is the mutually-exclusive
  // escape hatch — picking it clears the others; picking any specific
  // pet clears "none". Persisted as a comma-separated list in
  // localStorage('csi.pet.kinds').
  const initialKinds = (localStorage.getItem('csi.pet.kinds') || '')
    .split(',').map(s => s.trim()).filter(Boolean);
  const chosenPets = new Set(initialKinds);

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
  }

  function finish(launchCalibrate) {
    localStorage.setItem('csi.onboarding.done', '1');
    // Persist the comma-separated list. Empty Set means the user
    // skipped without answering — leave any previous answer alone.
    if (chosenPets.size > 0) {
      localStorage.setItem('csi.pet.kinds', Array.from(chosenPets).join(','));
    }
    document.body.classList.remove('is-onboarding');
    if (launchCalibrate) {
      // Hand off to the existing calibrate path.
      const btn = document.getElementById('calibrateBtn');
      if (btn) btn.click();
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
        localStorage.setItem('csi.pet', '1');
        const sw = document.getElementById('petSwitch');
        if (sw) sw.setAttribute('aria-checked', 'true');
        // Push to the device too so core.presence honors the choice
        // immediately, not just after the user re-toggles the switch.
        persistPetMode(true);
      }
      render();
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

/* ────────────────────────────────────────────────────────────────────────
 *  Device identity badge
 *
 *  /api/device-info is the public, no-auth endpoint that exposes the
 *  canary's per-device id (e.g. "canary-s3-AB12"). /api/status carries
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
    const r = await fetch('/api/device-info', {cache: 'no-store'});
    if (!r.ok) return;
    const j = await r.json();
    const el = document.getElementById('device-id');
    if (el && j.device_id) el.textContent = j.device_id;
  } catch {}
})();

// Service worker registration is left to companion_pwa.h's existing scope.
</script>
</body>
</html>
)DASHBOARD";

#endif /* SECURACV_CSI_DASHBOARD_HTML_H */
