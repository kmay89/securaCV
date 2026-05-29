/*
 * SecuraCV Canary — Companion PWA assets (Web Bluetooth)
 *
 * A small static PWA served from the device's web server. Installs to a
 * phone's home screen, caches itself via service worker so the page is
 * available offline, and connects to the Canary's BLE Console service
 * (8fc1cee0-…) over Web Bluetooth to render the live snapshot.
 *
 * BROWSER SUPPORT
 * ───────────────
 * - Android Chrome / Edge: native Web Bluetooth.
 * - Desktop Chrome / Edge: native Web Bluetooth.
 * - iOS: requires the Bluefy browser (App Store) — Safari and iOS Chrome
 *   deliberately omit Web Bluetooth from WebKit. Bluefy wraps WKWebView
 *   and exposes the same JS API; the user pairs the Canary in iOS
 *   Settings > Bluetooth first, then opens this page in Bluefy.
 *
 * DEPLOYMENT PATHS (all serve the same characteristic surface)
 * ─────────────────────────────────────────────────────────────
 *  1. Served by THIS device at /companion when WiFi is reachable.
 *  2. Cached locally by the service worker after the first visit, so
 *     subsequent loads work offline / over BLE-only.
 *  3. Mirror at a public URL (e.g. GitHub Pages) so the page can be
 *     loaded even when canary.local is unreachable AND the user
 *     hasn't installed the PWA yet. (Future, not part of this PR.)
 *
 * ROUTES
 * ──────
 *  GET /companion                         — main HTML
 *  GET /companion-sw.js                   — service worker (scope /companion)
 *  GET /companion-manifest.webmanifest    — PWA manifest
 */

#ifndef SECURACV_COMPANION_PWA_H
#define SECURACV_COMPANION_PWA_H

#include "build_config.h"  // CANARY_WEB_ASSETS_GZIPPED
#include <Arduino.h>

// ────────────────────────────────────────────────────────────────────────────
// HTML
// ────────────────────────────────────────────────────────────────────────────

// Source of truth for the companion PWA page. Compiled out in normal builds:
// the binary ships the gzip copy from web_assets_gz.h (CANARY_WEB_ASSETS_GZIPPED).
// The small service-worker + manifest assets below stay uncompressed.
// Regenerate with gen_web_assets_gz.py after editing the HTML below.
#if !defined(CANARY_WEB_ASSETS_GZIPPED)
static const char COMPANION_HTML[] PROGMEM = R"COMPANIONHTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="Canary">
<meta name="theme-color" content="#0a0e1a">
<title>SecuraCV Canary</title>
<link rel="manifest" href="/companion-manifest.webmanifest">
<style>
:root{--bg:#0a0e1a;--surface:#131826;--surface-2:#1a2030;--text:#e8eaed;--muted:#8b95a8;--accent:#66b3ff;--success:#48bb78;--warning:#f6ad55;--danger:#f56565;--border:#232a3d}
*{box-sizing:border-box;margin:0;padding:0}
html,body{background:var(--bg);color:var(--text);font:15px/1.4 -apple-system,BlinkMacSystemFont,'SF Pro Text',system-ui,sans-serif;min-height:100vh}
body{padding:env(safe-area-inset-top) 1rem env(safe-area-inset-bottom);max-width:480px;margin:0 auto}
header{padding:1.25rem 0 .75rem}
h1{font-size:1.4rem;font-weight:700;letter-spacing:-.02em}
h1 .sub{display:block;font-size:.75rem;color:var(--muted);font-weight:400;margin-top:.2rem}
.card{background:var(--surface);border-radius:14px;padding:1rem;margin-bottom:.75rem;border:1px solid var(--border)}
.stat{display:flex;justify-content:space-between;align-items:baseline;padding:.4rem 0;border-top:1px solid var(--border)}
.stat:first-of-type{border-top:none}
.stat-l{color:var(--muted);font-size:.8rem}
.stat-v{font-size:.95rem;font-weight:600;font-variant-numeric:tabular-nums;text-align:right;max-width:60%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.btn{display:block;width:100%;padding:.85rem 1rem;border:none;border-radius:12px;font-size:1rem;font-weight:600;cursor:pointer;-webkit-tap-highlight-color:transparent;transition:opacity .15s,transform .1s;font-family:inherit}
.btn:active{transform:scale(.98)}
.btn-primary{background:var(--accent);color:var(--bg)}
.btn-secondary{background:var(--surface-2);color:var(--text)}
.btn-danger{background:rgba(245,101,101,.15);color:var(--danger)}
.badge{display:inline-flex;align-items:center;gap:.3rem;padding:.2rem .55rem;border-radius:999px;font-size:.7rem;font-weight:600;letter-spacing:.05em}
.badge-home{background:rgba(102,179,255,.15);color:var(--accent)}
.badge-away{background:rgba(72,187,120,.15);color:var(--success)}
.badge-warn{background:rgba(246,173,85,.15);color:var(--warning)}
.badge-disconnected{background:rgba(139,149,168,.15);color:var(--muted)}
.err{color:var(--danger);font-size:.8rem;margin-top:.5rem;padding:.5rem;background:rgba(245,101,101,.05);border:1px solid rgba(245,101,101,.2);border-radius:8px;display:none}
.err.show{display:block}
.hidden{display:none!important}
.card-title{font-size:.75rem;font-weight:700;color:var(--muted);letter-spacing:.08em;text-transform:uppercase;margin-bottom:.5rem;display:flex;justify-content:space-between;align-items:center}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block;background:var(--muted);margin-right:.4rem;vertical-align:middle}
.dot.live{background:var(--success);box-shadow:0 0 6px var(--success)}
footer{color:var(--muted);font-size:.7rem;text-align:center;padding:1rem 0 1.5rem;line-height:1.5}
footer a{color:var(--accent);text-decoration:none}
.intro{color:var(--muted);font-size:.85rem;margin-bottom:.75rem;line-height:1.45}
.tab-nav{display:flex;gap:.3rem;background:var(--surface);padding:.25rem;border-radius:12px;margin-bottom:.75rem;border:1px solid var(--border)}
.tab-btn{flex:1;padding:.55rem;background:transparent;border:none;color:var(--muted);font-size:.85rem;font-weight:600;border-radius:9px;cursor:pointer;-webkit-tap-highlight-color:transparent;font-family:inherit}
.tab-btn.active{background:var(--surface-2);color:var(--text)}
.tab-content{display:block}
.tab-content.hidden{display:none!important}
/* .ap-row is a native <button type="button"> so Tab focuses it and
 * Enter/Space activate it without a keydown handler. Reset strips
 * UA button defaults (background, font, text alignment, width) that
 * would otherwise fight the row layout — the rounded transparent
 * border stays as the click target outline. */
.ap-row{display:flex;align-items:center;gap:.6rem;padding:.65rem .5rem;border-radius:10px;cursor:pointer;-webkit-tap-highlight-color:transparent;border:1px solid transparent;background:transparent;color:inherit;font:inherit;text-align:left;width:100%}
.ap-row[disabled]{cursor:default;opacity:.6}
.ap-row:active{background:var(--surface-2)}
.ap-row.sel{background:var(--surface-2);border-color:var(--accent)}
.ap-meta{flex:1;min-width:0}
.ap-ssid{font-weight:600;font-size:.92rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.ap-sub{font-size:.7rem;color:var(--muted);margin-top:.1rem}
.ap-bars{display:inline-flex;gap:2px;align-items:flex-end;height:14px}
.ap-bars span{width:3px;background:var(--border);border-radius:1px}
.ap-bars span:nth-child(1){height:30%}
.ap-bars span:nth-child(2){height:55%}
.ap-bars span:nth-child(3){height:80%}
.ap-bars span:nth-child(4){height:100%}
.ap-bars.s4 span{background:var(--success)}
.ap-bars.s3 span:nth-child(-n+3){background:var(--success)}
.ap-bars.s2 span:nth-child(-n+2){background:var(--warning)}
.ap-bars.s1 span:nth-child(-n+1){background:var(--danger)}
.input{width:100%;padding:.7rem .85rem;background:var(--surface-2);border:1px solid var(--border);border-radius:10px;color:var(--text);font-size:.95rem;font-family:inherit}
.input:focus{outline:none;border-color:var(--accent)}
.row-flex{display:flex;gap:.5rem;align-items:center}
.log-list{max-height:60vh;overflow-y:auto;-webkit-overflow-scrolling:touch}
.log-row{padding:.55rem .5rem;border-top:1px solid var(--border);font-size:.78rem;line-height:1.35}
.log-row:first-child{border-top:none}
.log-head{display:flex;align-items:center;gap:.4rem;margin-bottom:.15rem;flex-wrap:wrap}
.log-lvl{display:inline-flex;align-items:center;padding:.05rem .35rem;border-radius:5px;font-size:.62rem;font-weight:700;letter-spacing:.04em;text-transform:uppercase}
.log-lvl.debug   {background:rgba(139,149,168,.15);color:var(--muted)}
.log-lvl.info    {background:rgba(102,179,255,.15);color:var(--accent)}
.log-lvl.notice  {background:rgba(102,179,255,.15);color:var(--accent)}
.log-lvl.warn    {background:rgba(246,173,85,.15);color:var(--warning)}
.log-lvl.error   {background:rgba(245,101,101,.15);color:var(--danger)}
.log-lvl.crit    {background:var(--danger);color:#fff}
.log-meta{color:var(--muted);font-size:.65rem;font-family:ui-monospace,'SF Mono',Menlo,monospace}
.log-msg{color:var(--text)}
.log-detail{color:var(--muted);font-size:.7rem;margin-top:.15rem;font-family:ui-monospace,'SF Mono',Menlo,monospace;overflow-wrap:anywhere}
.file-row{display:flex;flex-direction:column;gap:.3rem;margin-bottom:.65rem}
.file-row label{font-size:.75rem;color:var(--muted);font-weight:600}
.file-row input[type=file]{font-size:.8rem;color:var(--text);font-family:inherit}
.file-row input[type=file]::-webkit-file-upload-button{padding:.45rem .7rem;background:var(--surface-2);border:1px solid var(--border);border-radius:8px;color:var(--text);font-family:inherit;font-size:.8rem;cursor:pointer;-webkit-tap-highlight-color:transparent}
.bar-track{height:8px;background:var(--surface-2);border-radius:4px;overflow:hidden;margin-top:.25rem}
.bar-fill{height:100%;background:linear-gradient(90deg,var(--accent),var(--success));width:0;transition:width .25s ease-out}
.bar-fill.fail{background:var(--danger)}
.bar-meta{display:flex;justify-content:space-between;font-size:.7rem;color:var(--muted);font-variant-numeric:tabular-nums;margin-top:.25rem}
.hash{font-family:ui-monospace,'SF Mono',Menlo,monospace;font-size:.65rem;word-break:break-all;color:var(--muted);background:var(--surface-2);padding:.4rem .55rem;border-radius:6px;line-height:1.45}
.verify-row{display:flex;align-items:center;gap:.5rem;padding:.6rem .65rem;background:var(--surface-2);border-radius:8px;margin-top:.5rem}
.verify-icon{font-size:1.1rem}
.verify-icon.ok{color:var(--success)}
.verify-icon.bad{color:var(--danger)}
.verify-icon.warn{color:var(--warning)}
.verify-text{flex:1;font-size:.8rem}
.verify-sub{color:var(--muted);font-size:.7rem;margin-top:.15rem}

/* ── Onboarding wizard (token mode, Tier 5 #12) ─────────────────────────── */
.wiz-step{display:none}
.wiz-step.active{display:block}
.wiz-progress{display:flex;gap:6px;margin:0 0 14px 0}
.wiz-progress span{flex:1;height:4px;border-radius:2px;background:var(--surface-2)}
.wiz-progress span.done{background:var(--accent)}
.wiz-progress span.now{background:var(--accent);box-shadow:0 0 10px var(--accent)}
.wiz-h{font-size:1.1rem;font-weight:600;margin:0 0 .35rem;letter-spacing:-.01em}
.wiz-sub{color:var(--muted);font-size:.85rem;margin:0 0 1rem;line-height:1.45}
.wiz-input{width:100%;padding:.75rem .85rem;border-radius:10px;background:var(--surface-2);border:1px solid var(--border);color:var(--text);font-size:1rem;font-family:inherit;-webkit-appearance:none}
.wiz-input:focus{outline:none;border-color:var(--accent)}
.wiz-btnrow{display:flex;gap:.5rem;margin-top:1rem}
.wiz-btnrow .btn{flex:1}
.wiz-net-list{max-height:280px;overflow-y:auto;margin:.5rem 0 .25rem;border:1px solid var(--border);border-radius:10px;background:var(--surface-2)}
/* .wiz-net-row is a native <button type="button"> so Tab focuses it
 * and Enter/Space activate it without a keydown handler. The reset
 * here strips UA button defaults (background, border, font, text
 * alignment, width) that would otherwise fight the row layout. */
.wiz-net-row{display:flex;align-items:center;gap:.6rem;padding:.7rem .8rem;cursor:pointer;-webkit-tap-highlight-color:transparent;border-top:1px solid var(--border);background:transparent;border-left:none;border-right:none;border-bottom:none;color:inherit;font:inherit;text-align:left;width:100%}
.wiz-net-row:first-child{border-top:none}
.wiz-net-row:active{background:rgba(102,179,255,.1)}
.wiz-net-name{flex:1;font-size:.95rem;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.wiz-net-rssi{color:var(--muted);font-size:.7rem;font-variant-numeric:tabular-nums}
.wiz-net-lock{color:var(--muted);font-size:.85rem}
.wiz-net-empty{padding:1rem;text-align:center;color:var(--muted);font-size:.85rem}
.wiz-spin{display:flex;align-items:center;gap:.6rem;padding:.75rem 0;color:var(--muted);font-size:.85rem}
.wiz-spin::before{content:'';width:14px;height:14px;border:2px solid var(--border);border-top-color:var(--accent);border-radius:50%;animation:wiz-spin .8s linear infinite}
@keyframes wiz-spin{to{transform:rotate(360deg)}}
.wiz-tick{display:inline-block;width:42px;height:42px;border-radius:50%;background:rgba(72,187,120,.15);color:var(--success);text-align:center;line-height:42px;font-size:1.4rem;margin-bottom:.5rem}
.wiz-cross{display:inline-block;width:42px;height:42px;border-radius:50%;background:rgba(245,101,101,.15);color:var(--danger);text-align:center;line-height:42px;font-size:1.4rem;margin-bottom:.5rem}
.wiz-link-row{display:flex;flex-direction:column;gap:.5rem;margin-top:.75rem}
.wiz-link-row a{display:block;padding:.7rem .85rem;background:var(--surface-2);border:1px solid var(--border);border-radius:10px;color:var(--accent);text-decoration:none;font-size:.9rem;text-align:center;font-weight:500}
.wiz-link-row a:active{background:rgba(102,179,255,.1)}
/* Pre-flight self-test (step 5). One row per probe: name on the
 * left, plain-language detail in the middle, status icon on the
 * right. The whole row is a <details> so a power user can expand
 * any line to see structured metrics (rssi_dbm, sensor_pid, free
 * bytes, …). The summary acts as the row, so the disclosure
 * affordance lives on the row itself. */
.wiz-check-list{display:flex;flex-direction:column;gap:.4rem;margin:.5rem 0 .25rem}
.wiz-check-row{border:1px solid var(--border);border-radius:10px;background:var(--surface-2);overflow:hidden}
.wiz-check-row > summary{display:flex;align-items:center;gap:.6rem;padding:.65rem .8rem;cursor:pointer;list-style:none;-webkit-tap-highlight-color:transparent}
.wiz-check-row > summary::-webkit-details-marker{display:none}
.wiz-check-row > summary::marker{display:none}
.wiz-check-row[open] > summary{border-bottom:1px solid var(--border)}
.wiz-check-name{flex:0 0 auto;font-size:.9rem;font-weight:500;min-width:5.5rem}
.wiz-check-detail{flex:1;color:var(--muted);font-size:.8rem;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.wiz-check-icon{flex:0 0 auto;width:18px;height:18px;border-radius:50%;display:inline-flex;align-items:center;justify-content:center;font-size:.7rem;line-height:1}
.wiz-check-icon.pass{background:rgba(72,187,120,.18);color:var(--success)}
.wiz-check-icon.fail{background:rgba(245,101,101,.18);color:var(--danger)}
.wiz-check-icon.skip,.wiz-check-icon.absent{background:var(--surface);color:var(--muted)}
.wiz-check-icon.unknown{background:var(--surface);color:var(--muted)}
.wiz-check-icon.spin{background:transparent;border:2px solid var(--border);border-top-color:var(--accent);animation:wiz-spin .8s linear infinite}
.wiz-check-meta{padding:.55rem .8rem .7rem;font-family:ui-monospace,'SF Mono',Menlo,monospace;font-size:.7rem;color:var(--text);background:var(--surface);white-space:pre-wrap;word-break:break-all}
.wiz-check-summary{margin-top:.6rem;padding:.55rem .7rem;border-radius:8px;font-size:.8rem;text-align:center}
.wiz-check-summary.pass{background:rgba(72,187,120,.12);color:var(--success)}
.wiz-check-summary.fail{background:rgba(245,101,101,.12);color:var(--danger)}
/* "What to do" remediation block. Rendered inside a failing/needs-action
 * row ABOVE the raw metric JSON so the user sees plain-language guidance
 * first. Failing rows are auto-expanded (see renderProbes) so this is
 * visible without the user having to discover the disclosure. */
.wiz-check-hint{padding:.55rem .8rem .65rem;font-size:.8rem;line-height:1.45;color:var(--text);background:var(--surface);border-top:1px solid var(--border);white-space:normal}
.wiz-check-hint strong{color:var(--danger);font-weight:600}
@media (forced-colors: active){
  .wiz-check-row{border:1px solid CanvasText;background:Canvas}
  .wiz-check-icon.pass,.wiz-check-icon.fail,.wiz-check-icon.skip,.wiz-check-icon.absent,.wiz-check-icon.unknown{background:Canvas;color:CanvasText;border:1px solid CanvasText}
  .wiz-check-icon.fail{outline:2px solid CanvasText;outline-offset:1px}
  .wiz-check-meta{background:Canvas;color:CanvasText;border-top:1px solid CanvasText}
  .wiz-check-hint{background:Canvas;color:CanvasText;border-top:1px solid CanvasText}
  .wiz-check-hint strong{color:CanvasText}
  .wiz-check-summary.pass,.wiz-check-summary.fail{background:Canvas;color:CanvasText;border:1px solid CanvasText}
}

/* Screen-reader-only utility for the off-screen #a11y-announcer.
   Uses the conventional clip-path/zero-size pattern that takes the
   element out of layout while still letting AT read its contents. */
.sr-only{
  position:absolute;width:1px;height:1px;padding:0;margin:-1px;
  overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0;
}

/* Focus-visible: project's existing :focus rules use `outline:none +
   border-color: accent`. That works for the input chrome but leaves
   buttons + tab buttons without a focus ring at all. Add a generic
   accent outline so keyboard users always see where they are. */
.btn:focus-visible, .tab-btn:focus-visible, .wiz-net-row:focus-visible, .ap-row:focus-visible{
  outline:2px solid var(--accent);
  outline-offset:2px;
}

/* Forced-colors mode (Windows High Contrast / accessibility tools).
   Same playbook as PR #406 (headline dashboard) and PR #407 (admin):
   custom colors flatten to the user's chosen system palette, so we
   need explicit borders + system colors to keep controls
   identifiable. The companion is the most consequential surface for
   this since it's the primary mobile install path — operators on
   accessibility tooling reach for it before they ever see /admin.
   Reference: WCAG 2.1 SC 1.4.11 + W3C CSS Color Adjust §3. */
@media (forced-colors: active){
  .btn{border:1px solid ButtonText}
  .btn-danger{border-width:2px}                /* non-color hazard cue */
  .input,.wiz-input{border:1px solid CanvasText;background:Canvas;color:CanvasText}
  .input:focus,.wiz-input:focus{outline:2px solid CanvasText;outline-offset:1px;border-color:CanvasText}
  .tab-btn{border:1px solid ButtonText}
  .tab-btn[aria-selected="true"]{background:Highlight;color:HighlightText;border-color:Highlight}
  .card{border:1px solid CanvasText;background:Canvas}
  .wiz-net-list{border:1px solid CanvasText;background:Canvas}
  .wiz-net-row{border-top-color:CanvasText}
  /* BLE-flow scan rows added by PRs #411 / #413: under forced-colors,
   * the var(--surface-2)/var(--accent) selection fill is neutralized,
   * so mirror the tablist "selected" pattern with Highlight pair so
   * the chosen network is still visually distinct. .ap-row[disabled]
   * (hidden-SSID rows) gets GrayText so the dimmed state survives —
   * opacity:.6 is also dropped under forced-colors.
   *
   * .ap-bars span gets background-color:currentColor so the signal
   * gauge bars track the surrounding text color (CanvasText / GrayText
   * / HighlightText for default / disabled / selected). Without this,
   * the per-bar var(--success/warning/danger) backgrounds collapse to
   * the engine default and disabled rows would show normally-colored
   * bars next to grayed text. .ap-row also gets a border-top to match
   * .wiz-net-row's separator language. Caught by Gemini in PR #420
   * review. */
  .ap-row{border-top-color:CanvasText}
  .ap-row.sel{background:Highlight;color:HighlightText;border-color:Highlight}
  .ap-row[disabled]{color:GrayText}
  .ap-bars span{background-color:currentColor}
  .badge{border:1px solid CanvasText;background:Canvas;color:CanvasText}
  :focus-visible{outline:2px solid CanvasText;outline-offset:2px}
}
</style>
</head>
<body>
<!-- Single off-screen aria-live region the JS pipes status changes
     into so screen-reader users hear connection / OTA / wizard state
     transitions without hunting for them visually. polite (not
     assertive) so the canary doesn't interrupt the user mid-task.
     The announcer is positioned with the sr-only utility (defined in
     <style>) so it occupies no layout. (Audit: companion a11y pass.) -->
<div id="a11y-announcer" class="sr-only" role="status" aria-live="polite" aria-atomic="true"></div>
<header>
  <h1>SecuraCV Canary
    <!-- conn-state mirrors the announcer for sighted users. Marked
         live so a screen-reader user who happens to focus the heading
         hears the same updates the announcer pushes elsewhere. -->
    <span class="sub" id="conn-state" aria-live="polite">Not connected</span>
  </h1>
</header>

<!-- ─────────────────────────────────────────────────────────────────────────
     Onboarding wizard (Tier 5 #12). Active only when the URL contains a
     ?token=<hex> parameter handed off by the captive-portal QR. The wizard
     is HTTP-only (no BLE pairing required) because the user is on the
     device's AP at 192.168.4.1, where the WiFi-provisioning HTTP routes
     are reachable without authentication. Token validation is best-effort:
     the AP itself is the security boundary, the QR token is a UX gate.
     ──────────────────────────────────────────────────────────────────────── -->
<div class="card hidden" id="onboard-card">
  <div class="wiz-progress">
    <span id="wiz-prog-1" class="now"></span>
    <span id="wiz-prog-2"></span>
    <span id="wiz-prog-3"></span>
    <span id="wiz-prog-4"></span>
    <span id="wiz-prog-5"></span>
  </div>

  <div class="wiz-step active" id="wiz-step-1">
    <div id="wiz-welcome-view">
      <h2 class="wiz-h" tabindex="-1">Connect your Canary to WiFi</h2>
      <p class="wiz-sub">Have your home WiFi password ready. Stay on this network until setup finishes.</p>
      <div class="wiz-btnrow">
        <button class="btn btn-primary" id="wiz-go-2">Let's go</button>
      </div>
      <div id="wiz-qr-offer" class="hidden" style="text-align:center;margin-top:.75rem">
        <p style="color:var(--muted);font-size:.8rem;margin:0 0 .4rem">or</p>
        <button class="btn btn-secondary" id="wiz-qr-start" style="font-size:.85rem">I have a WiFi QR code</button>
      </div>
    </div>
    <div id="wiz-qr-mode" class="hidden">
      <h2 class="wiz-h" tabindex="-1">Show the QR code</h2>
      <p class="wiz-sub">Hold a WiFi QR code up to your Canary's camera lens &mdash; about 15&nbsp;cm away.</p>
      <p style="color:var(--muted);font-size:.8rem;margin:0 0 .75rem;line-height:1.45">You can find one in your phone's WiFi settings (Android: Settings &rsaquo; WiFi &rsaquo; tap your network &rsaquo; Share) or on your router's label.</p>
      <div class="wiz-spin" id="wiz-qr-spin">Looking for a QR code&hellip;</div>
      <div class="err" id="wiz-qr-err" role="alert"></div>
      <div class="wiz-btnrow">
        <button class="btn btn-secondary" id="wiz-qr-back">Type password instead</button>
      </div>
    </div>
  </div>

  <div class="wiz-step" id="wiz-step-2">
    <h2 class="wiz-h" tabindex="-1">Pick your home WiFi</h2>
    <p class="wiz-sub">These are the networks your Canary can see. Pick the one it should join.</p>
    <div class="wiz-net-list" id="wiz-nets" role="group" aria-label="Available networks" aria-busy="true">
      <div class="wiz-spin">Looking for networks…</div>
    </div>
    <div class="err" id="wiz-step-2-err" role="alert"></div>
    <div class="wiz-btnrow">
      <button class="btn btn-secondary" id="wiz-rescan">Scan again</button>
    </div>
  </div>

  <div class="wiz-step" id="wiz-step-3">
    <h2 class="wiz-h" tabindex="-1">Type the password</h2>
    <p class="wiz-sub">Joining <strong id="wiz-picked-ssid">…</strong>. Your password is sent only to the Canary.</p>
    <input type="password" class="wiz-input" id="wiz-pw" placeholder="Network password" autocomplete="new-password" spellcheck="false" aria-describedby="wiz-step-3-err" aria-invalid="false">
    <label style="display:flex;gap:.5rem;align-items:center;margin-top:.5rem;color:var(--muted);font-size:.8rem">
      <input type="checkbox" id="wiz-show-pw"> Show password
    </label>
    <div class="err" id="wiz-step-3-err" role="alert"></div>
    <div class="wiz-btnrow">
      <button class="btn btn-secondary" id="wiz-back-2">Back</button>
      <button class="btn btn-primary" id="wiz-go-4">Connect</button>
    </div>
  </div>

  <div class="wiz-step" id="wiz-step-4">
    <div id="wiz-step-4-progress">
      <h2 class="wiz-h" tabindex="-1">Connecting…</h2>
      <p class="wiz-sub" id="wiz-progress-text">Talking to your Canary.</p>
      <div class="wiz-spin" id="wiz-spin">Waiting for your home WiFi.</div>
    </div>
    <div id="wiz-step-4-success" class="hidden">
      <div class="wiz-tick">✓</div>
      <h2 class="wiz-h" tabindex="-1">Your Canary is online.</h2>
      <p class="wiz-sub">Joined <strong id="wiz-success-ssid">your home WiFi</strong>. Running one quick check that the sensors are awake.</p>
    </div>
    <div id="wiz-step-4-failure" class="hidden">
      <div class="wiz-cross">!</div>
      <h2 class="wiz-h" tabindex="-1">Couldn't connect</h2>
      <p class="wiz-sub" id="wiz-fail-reason">Check the password and try again.</p>
      <div class="wiz-btnrow">
        <button class="btn btn-secondary" id="wiz-fail-back">Try again</button>
      </div>
    </div>
  </div>

  <!-- Step 5 — Pre-flight checks. We poll /api/selftest once the
       device is on the home network and render one row per
       subsystem (Wi-Fi / Camera / Bluetooth / SD / Microphone /
       GPIO). Each row is a <details> the user can expand to see
       the structured metric blob a power user wants. The whole
       step is wrapped in role="status" + aria-live so AT users
       hear the running → result transition without us manually
       managing announcements per row. -->
  <div class="wiz-step" id="wiz-step-5">
    <div id="wiz-step-5-running">
      <h2 class="wiz-h" tabindex="-1">Pre-flight checks</h2>
      <p class="wiz-sub">Confirming the sensors are awake. About a second.</p>
      <div class="wiz-spin" id="wiz-st-spin">Running checks…</div>
    </div>
    <div id="wiz-step-5-result" class="hidden">
      <h2 class="wiz-h" tabindex="-1" id="wiz-st-heading">Pre-flight result</h2>
      <p class="wiz-sub" id="wiz-st-sub">…</p>
      <div class="wiz-check-list" id="wiz-st-list" role="group" aria-label="Pre-flight checks" aria-busy="false"></div>
      <div class="wiz-check-summary" id="wiz-st-summary" role="status" aria-live="polite"></div>

      <!-- Shown only when a check fails. Explains that greyed rows are
           normal and that the user is not stuck: they can fix-and-rerun
           or continue to the dashboard anyway. -->
      <p class="wiz-sub" id="wiz-st-failnote" style="display:none;margin-top:.6rem">
        Greyed rows (like Camera or Microphone) just mean that feature isn't on this device &mdash; that's normal.
        Fix anything marked in red and tap <strong>Run again</strong>, or <strong>Continue anyway</strong> to open your Canary now.
        You can re-run these checks any time from the dashboard.
      </p>

      <!-- Multi-Canary branch (shown only on all_passed). Two paths:
           "Set up another" reveals a quick how-to (#wiz-another-block);
           "I'm done" reveals the original link-row (#wiz-st-links) and
           finish button (#wiz-st-finish) so the single-device close-out
           path is preserved exactly.
           Microcopy and 5-bullet placement card sourced from
           docs/audit/wap_multi_device_ux_audit.md §5 + §6. -->
      <div id="wiz-multi-block" style="display:none;margin-top:.85rem">
        <h3 class="wiz-h" style="font-size:1rem;margin:0 0 .25rem" tabindex="-1">Most homes use 3 or 4.</h3>
        <p class="wiz-sub" style="margin:0 0 .6rem">Want to add the next room now? Each one takes about a minute. You can stop anytime &mdash; up to 8 in one home.</p>
        <details class="wiz-check-row" style="margin:0 0 .65rem">
          <summary style="font-size:.9rem;font-weight:500;display:flex;align-items:center">
            <span style="flex:1">Where to put the next one</span>
            <span aria-hidden="true" style="color:var(--muted);font-size:.75rem;margin-left:.5rem">▾</span>
          </summary>
          <div class="wiz-check-meta" style="font-family:inherit;font-size:.8rem;line-height:1.5;background:var(--surface-2);color:var(--text);white-space:normal">
            <ul style="margin:0;padding-left:1.1rem">
              <li>About head height. Crossing motion reads better than overhead.</li>
              <li>Three meters from the last Canary. Closer and they hear each other.</li>
              <li>One good wall away from your WiFi router.</li>
              <li>Not behind a TV or large screen.</li>
              <li>One Canary per room. L-shaped rooms count as two.</li>
            </ul>
          </div>
        </details>
        <div class="wiz-btnrow">
          <button class="btn btn-secondary" id="wiz-done-here">I'm done for now</button>
          <button class="btn btn-primary"   id="wiz-add-another">Set up another</button>
        </div>
      </div>

      <!-- Re-entry guide for the second-and-onward Canary. Reveal only
           when the user taps "Set up another". Pure copy + a back-out
           link; no network calls. -->
      <div id="wiz-another-block" style="display:none;margin-top:.85rem">
        <h3 class="wiz-h" style="font-size:1rem;margin:0 0 .25rem" tabindex="-1">Set up the next Canary</h3>
        <ol class="intro" style="margin:0 0 .6rem;padding-left:1.2rem;line-height:1.65">
          <li>Power on the next Canary.</li>
          <li>Open WiFi settings on your phone. Wait for a fresh <strong>SecuraCV-XXXX</strong> network to appear.</li>
          <li>Tap it. The setup page opens by itself.</li>
        </ol>
        <p class="wiz-sub" style="margin:.25rem 0 .6rem">After it finishes, all your Canaries see each other on the home network. You can name and arrange them from the dashboard.</p>
        <div class="wiz-btnrow">
          <button class="btn btn-secondary" id="wiz-another-back">Back</button>
          <button class="btn btn-primary"   id="wiz-another-open">Open this one's dashboard</button>
        </div>
      </div>

      <div class="wiz-link-row" id="wiz-st-links" style="display:none">
        <p class="wiz-sub" style="margin:0">Setup's done. Rejoin your home WiFi on your phone, then tap a link below to open your Canary. Tip: name it by its room from the dashboard's Settings sheet &mdash; the name shows up in alerts.</p>
        <a id="wiz-link-mdns" href="http://canary.local/">Open canary.local</a>
        <a id="wiz-link-ip" href="#" style="display:none"></a>
      </div>
      <div class="wiz-btnrow">
        <button class="btn btn-secondary" id="wiz-st-rerun">Run again</button>
        <button class="btn btn-secondary" id="wiz-st-continue" style="display:none">Continue anyway</button>
        <button class="btn btn-primary"   id="wiz-st-finish" style="display:none">Finish</button>
      </div>
    </div>
  </div>
</div>

<div class="card" id="connect-card">
  <p class="intro">Pair your Canary in <strong>Settings &rsaquo; Bluetooth</strong> first, then tap below to open the live console.</p>
  <button class="btn btn-primary" id="connect-btn">Connect</button>
</div>

<!-- #err-msg is the BLE flow's top-level error sink, written by
     showErr(). It must live OUTSIDE #connect-card because the connect
     card flips to .hidden after a successful pair (see onConnect →
     $('connect-card').classList.add('hidden')), but post-connect
     showErr() calls (snapshot bad payload, scan bad payload, scan
     failed, send failed) keep firing — and were previously invisible
     to BOTH sighted and SR users (and role="alert" wouldn't fire
     either, since AT skip live regions inside hidden ancestors).
     Moving it to a top-level sibling of #connect-card keeps it
     reachable for the entire post-pair lifecycle. Caught by Gemini
     in PR #416 review. -->
<div class="err" id="err-msg" role="alert"></div>

<!-- Tab nav appears after connect; switches between Status / WiFi panels.
     Optional tab buttons start hidden; each is revealed only when the
     corresponding GATT service is successfully discovered on the device.
     Older firmware that ships only the Console service shows just Status.

     ARIA tablist semantics let assistive tech recognize the row as a
     tablist (vs. a sequence of unrelated buttons), announce each tab's
     selection state, and surface keyboard arrow-key navigation that
     screen-reader users expect from this pattern. showTab() in the JS
     keeps aria-selected + tabindex in sync with the visible .active
     class. (Audit: companion a11y pass.) -->
<div class="tab-nav hidden" id="tab-nav" role="tablist" aria-label="Companion sections">
  <button class="tab-btn active" data-tab="status" id="tab-btn-status" role="tab" aria-selected="true" aria-controls="tab-status" tabindex="0">Status</button>
  <button class="tab-btn hidden" data-tab="wifi"    id="tab-btn-wifi"    role="tab" aria-selected="false" aria-controls="tab-wifi"    tabindex="-1">WiFi</button>
  <button class="tab-btn hidden" data-tab="logs"    id="tab-btn-logs"    role="tab" aria-selected="false" aria-controls="tab-logs"    tabindex="-1">Logs</button>
  <button class="tab-btn hidden" data-tab="witness" id="tab-btn-witness" role="tab" aria-selected="false" aria-controls="tab-witness" tabindex="-1">Witness</button>
  <button class="tab-btn hidden" data-tab="ota"     id="tab-btn-ota"     role="tab" aria-selected="false" aria-controls="tab-ota"     tabindex="-1">OTA</button>
</div>

<div class="tab-content" id="tab-status" role="tabpanel" aria-labelledby="tab-btn-status" tabindex="0">
  <div class="card hidden" id="status-card">
    <div class="card-title">
      <span><span class="dot live" id="live-dot"></span>Live status</span>
      <span class="badge badge-disconnected" id="ctx-badge">&mdash;</span>
    </div>
    <div class="stat"><span class="stat-l">Device</span><span class="stat-v" id="s-id">&mdash;</span></div>
    <div class="stat"><span class="stat-l">Firmware</span><span class="stat-v" id="s-fw">&mdash;</span></div>
    <div class="stat"><span class="stat-l">Uptime</span><span class="stat-v" id="s-up">&mdash;</span></div>
    <div class="stat"><span class="stat-l">Free heap</span><span class="stat-v" id="s-heap">&mdash;</span></div>
    <div class="stat"><span class="stat-l">WiFi</span><span class="stat-v" id="s-wifi">&mdash;</span></div>
    <div class="stat"><span class="stat-l">Owner seen</span><span class="stat-v" id="s-owner">&mdash;</span></div>
    <div class="stat"><span class="stat-l">Household devices</span><span class="stat-v" id="s-hh">&mdash;</span></div>
    <div class="stat"><span class="stat-l">BLE adverts</span><span class="stat-v" id="s-ble">&mdash;</span></div>
    <div class="stat"><span class="stat-l">RF motion</span><span class="stat-v" id="s-motion">&mdash;</span></div>
  </div>
</div>

<div class="tab-content hidden" id="tab-witness" role="tabpanel" aria-labelledby="tab-btn-witness" tabindex="0">
  <div class="card hidden" id="witness-card">
    <div class="card-title">
      <span>Witness chain</span>
      <span class="badge badge-disconnected" id="witness-state-badge">no data</span>
    </div>
    <p class="intro">The device's signed evidence chain. Public-key cryptography lets you verify the device hasn't lied about what it recorded — without trusting any server in between.</p>
    <div class="stat"><span class="stat-l">Chain seq</span><span class="stat-v" id="w-seq">&mdash;</span></div>
    <div class="stat"><span class="stat-l">Total records</span><span class="stat-v" id="w-total">&mdash;</span></div>
    <div class="stat"><span class="stat-l">Last seq</span><span class="stat-v" id="w-rseq">&mdash;</span></div>
    <div class="stat"><span class="stat-l">Last type</span><span class="stat-v" id="w-rtype">&mdash;</span></div>
    <div class="stat"><span class="stat-l">Time bucket</span><span class="stat-v" id="w-tb">&mdash;</span></div>
    <div class="stat"><span class="stat-l">Payload bytes</span><span class="stat-v" id="w-plen">&mdash;</span></div>
    <div style="margin-top:.75rem;font-size:.65rem;color:var(--muted);font-weight:700;letter-spacing:.05em;text-transform:uppercase">Chain head</div>
    <div class="hash" id="w-head">&mdash;</div>
    <div style="margin-top:.5rem;font-size:.65rem;color:var(--muted);font-weight:700;letter-spacing:.05em;text-transform:uppercase">Device pubkey (Ed25519)</div>
    <div class="hash" id="w-pk">&mdash;</div>
    <div class="verify-row" id="verify-row" style="display:none">
      <div class="verify-icon" id="verify-icon" aria-hidden="true">&mdash;</div>
      <div>
        <div class="verify-text" id="verify-text">&mdash;</div>
        <div class="verify-sub" id="verify-sub">&mdash;</div>
      </div>
    </div>
    <div style="display:flex;gap:.5rem;margin-top:.6rem">
      <button class="btn btn-secondary" id="witness-refresh-btn" style="flex:1">Refresh + verify</button>
    </div>
  </div>
</div>

<div class="tab-content hidden" id="tab-ota" role="tabpanel" aria-labelledby="tab-btn-ota" tabindex="0">
  <div class="card hidden" id="ota-card">
    <div class="card-title">
      <span>Firmware update</span>
      <span class="badge badge-disconnected" id="ota-state-badge">idle</span>
    </div>
    <p class="intro">Pick a signed firmware image (<code>.bin</code>) and its signature manifest (<code>.json</code> with <code>sha256</code>, <code>signature</code>, and <code>version</code>). The image is verified against the device's release public key before any flash partition is touched.</p>
    <div class="file-row">
      <label for="ota-bin">Firmware image (.bin)</label>
      <input type="file" id="ota-bin" accept=".bin,application/octet-stream">
    </div>
    <div class="file-row">
      <label for="ota-sig">Signature manifest (.json)</label>
      <input type="file" id="ota-sig" accept=".json,application/json">
    </div>
    <div id="ota-meta" class="intro hidden" style="font-family:ui-monospace,'SF Mono',Menlo,monospace;font-size:.7rem;background:var(--surface-2);padding:.5rem .65rem;border-radius:8px;white-space:pre-wrap"></div>
    <button class="btn btn-primary" id="ota-start-btn" disabled style="margin-top:.5rem;opacity:.5">Pick both files first</button>
    <div id="ota-progress" class="hidden" style="margin-top:.6rem">
      <div class="bar-track"><div class="bar-fill" id="ota-bar"></div></div>
      <div class="bar-meta">
        <span id="ota-pct">0%</span>
        <span id="ota-bytes">0 / 0 B</span>
      </div>
    </div>
    <div id="ota-error" class="err" role="alert" style="margin-top:.5rem"></div>
    <p class="intro" style="font-size:.7rem;margin-top:.6rem">Don't disconnect or close this page during transfer. The device reboots into the new image automatically when the bytes verify.</p>
  </div>
</div>

<div class="tab-content hidden" id="tab-logs" role="tabpanel" aria-labelledby="tab-btn-logs" tabindex="0">
  <div class="card hidden" id="logs-card">
    <div class="card-title">
      <span>Recent events</span>
      <span class="badge badge-disconnected" id="logs-count-badge">0 / 0</span>
    </div>
    <p class="intro">Newest first. Auto-refreshes when new events land. Drawn from the device's in-RAM ring buffer (capped at <span id="logs-ring-size">100</span>).</p>
    <div style="display:flex;gap:.5rem;margin-bottom:.5rem">
      <button class="btn btn-secondary" id="logs-refresh-btn" style="flex:1">Refresh</button>
    </div>
    <div class="log-list" id="logs-list" role="group" aria-label="Recent events log" aria-busy="false">
      <p class="intro" style="text-align:center;padding:1rem 0">Tap Refresh to fetch the most recent entries.</p>
    </div>
  </div>
</div>

<div class="tab-content hidden" id="tab-wifi" role="tabpanel" aria-labelledby="tab-btn-wifi" tabindex="0">
  <div class="card hidden" id="wifi-card">
    <div class="card-title">
      <span>WiFi setup</span>
      <span class="badge badge-disconnected" id="wifi-state-badge">idle</span>
    </div>
    <p class="intro">Pick a network, type the password, send. The Canary will join your home WiFi and you can close this page.</p>
    <button class="btn btn-secondary" id="scan-btn">Scan for networks</button>
    <div id="ap-list" role="group" aria-label="Heard networks" aria-busy="false" style="margin-top:.5rem"></div>
    <div id="creds-box" class="hidden" style="margin-top:.75rem">
      <input type="password" class="input" id="pw-input" placeholder="WiFi password" autocomplete="new-password" spellcheck="false">
      <div class="row-flex" style="margin-top:.5rem">
        <button class="btn btn-primary" id="creds-send" style="flex:1">Send credentials</button>
      </div>
      <p class="intro" style="margin-top:.5rem;font-size:.7rem">Bonded link is encrypted; the password is write-only on the BLE characteristic and never readable back.</p>
    </div>
  </div>
</div>

<div class="card hidden" id="actions-card">
  <button class="btn btn-danger" id="disconnect-btn">Disconnect</button>
</div>

<footer>
  iOS: install <a href="https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055" target="_blank" rel="noopener">Bluefy</a> for Web Bluetooth support.<br>
  Android &amp; desktop Chrome work natively.
</footer>

<script>
'use strict';

/* ──────────────────────────────────────────────────────────────────────────
 *  Onboarding wizard (Tier 5 #12). HTTP-only flow that runs against
 *  /api/wifi/scan + /api/wifi/connect + /api/wifi while the phone is on
 *  the device's AP. Activated when the URL contains ?token=<hex>.
 * ────────────────────────────────────────────────────────────────────────── */
(function onboardWizard() {
  const params = new URLSearchParams(window.location.search);
  const token = params.get('token');
  if (!token || !/^[0-9a-fA-F]{64}$/.test(token)) return;  // BLE flow stays default

  const $w = id => document.getElementById(id);
  const card = $w('onboard-card');
  const oldCard = $w('connect-card');
  if (!card) return;

  // Reveal wizard, hide BLE flow.
  card.classList.remove('hidden');
  if (oldCard) oldCard.classList.add('hidden');
  // The BLE flow's tab nav and action card are also irrelevant here.
  ['tab-nav','actions-card'].forEach(k => {
    const el = $w(k); if (el) el.classList.add('hidden');
  });

  let pickedSsid = '';
  let pickedSecure = true;
  let scanTimer = 0;

  function setStep(n) {
    for (let i = 1; i <= 5; i++) {
      $w('wiz-step-' + i).classList.toggle('active', i === n);
      const dot = $w('wiz-prog-' + i);
      dot.classList.remove('done', 'now');
      if (i < n) dot.classList.add('done');
      else if (i === n) dot.classList.add('now');
    }
    /* Move focus to the active step's heading on the next frame so
     * AT announces the new card's title and keyboard users land on
     * the new content. The .wiz-h headings each have tabindex="-1"
     * so .focus() works on them; AT reads the heading's text + H2
     * level via semantics. announce() pushes the same text through
     * the off-screen live region as a belt-and-suspenders for AT
     * setups that don't auto-announce focus moves. */
    requestAnimationFrame(() => focusActiveStepHeading());
  }

  /* Find and focus the first visible .wiz-h inside the active step.
   * The visibility check matters for step 4, which contains three
   * sub-views (progress / success / failure) that each carry their
   * own heading — we want the user to land on the one currently
   * shown, not the first one in DOM order. getClientRects().length
   * > 0 is the visibility test (consistent with PR #401's pattern). */
  function focusActiveStepHeading() {
    const active = card.querySelector('.wiz-step.active');
    if (!active) return;
    for (const h of active.querySelectorAll('.wiz-h')) {
      if (h.getClientRects().length > 0) {
        h.focus();
        announce(h.textContent);
        return;
      }
    }
  }

  function setErr(stepN, msg) {
    const e = $w('wiz-step-' + stepN + '-err');
    if (!e) return;
    if (msg) { e.textContent = msg; e.classList.add('show'); }
    else     { e.textContent = '';  e.classList.remove('show'); }
    /* Step 3 has the password input — toggle aria-invalid on it so
     * AT users hear "invalid entry" when they re-focus the field
     * after a validation failure. The aria-describedby on the input
     * already points to this err div so SR users get the message
     * text alongside. Step 2's only "input" is the network list
     * (rendered by renderNets), and the error there is about the
     * scan, not user input — no aria-invalid target. */
    if (stepN === 3) {
      const pw = $w('wiz-pw');
      if (pw) pw.setAttribute('aria-invalid', msg ? 'true' : 'false');
    }
  }

  // ── Card 1 ─────────────────────────────────────────────────────────────
  $w('wiz-go-2').addEventListener('click', () => {
    setStep(2);
    startScan();
  });

  // ── Card 1b: QR scan mode ─────────────────────────────────────────────
  let qrPollTimer = 0;

  fetch('/api/wifi').then(r => r.json()).then(j => {
    if (j.qr_provision) {
      const offer = $w('wiz-qr-offer');
      if (offer) offer.classList.remove('hidden');
    }
  }).catch(() => {});

  function showQrMode() {
    const welcome = $w('wiz-welcome-view');
    const qrMode = $w('wiz-qr-mode');
    if (welcome) welcome.classList.add('hidden');
    if (qrMode) qrMode.classList.remove('hidden');
    const qrErr = $w('wiz-qr-err');
    if (qrErr) { qrErr.textContent = ''; qrErr.classList.remove('show'); }
    const spin = $w('wiz-qr-spin');
    if (spin) spin.style.display = '';
    announce('Looking for a QR code.');
  }

  function hideQrMode() {
    const welcome = $w('wiz-welcome-view');
    const qrMode = $w('wiz-qr-mode');
    if (welcome) welcome.classList.remove('hidden');
    if (qrMode) qrMode.classList.add('hidden');
    if (qrPollTimer) { clearInterval(qrPollTimer); qrPollTimer = 0; }
  }

  function showQrError(msg) {
    const spin = $w('wiz-qr-spin');
    if (spin) spin.style.display = 'none';
    const err = $w('wiz-qr-err');
    if (err) { err.textContent = msg; err.classList.add('show'); }
    announce(msg);
  }

  function pollQrScan() {
    qrPollTimer = setInterval(async () => {
      try {
        const r = await fetch('/api/wifi/qr-scan');
        const j = await r.json();
        if (j.success) {
          clearInterval(qrPollTimer); qrPollTimer = 0;
          announce('QR code found. Connecting to ' + (j.ssid || 'your network') + '.');
          setStep(4);
          showProgress('Connecting to ' + (j.ssid || 'your WiFi') + '.');
          pollWifiUntilConnected();
        } else if (!j.scanning) {
          clearInterval(qrPollTimer); qrPollTimer = 0;
          showQrError(j.error === 'timeout'
            ? 'Couldn’t find a QR code. Make sure the code fills most of the camera’s view.'
            : (j.error || 'Scan stopped.'));
        }
      } catch (e) {
        clearInterval(qrPollTimer); qrPollTimer = 0;
        showQrError('Lost connection to your Canary.');
      }
    }, 800);
  }

  if ($w('wiz-qr-start')) {
    $w('wiz-qr-start').addEventListener('click', async () => {
      showQrMode();
      try {
        const r = await fetch('/api/wifi/qr-scan', {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify({ token: token }),
        });
        const j = await r.json();
        if (!j.ok) {
          showQrError(j.error || 'Could not start scanner.');
          return;
        }
        pollQrScan();
      } catch (e) {
        showQrError('Could not reach your Canary.');
      }
    });
  }

  if ($w('wiz-qr-back')) {
    $w('wiz-qr-back').addEventListener('click', async () => {
      if (qrPollTimer) { clearInterval(qrPollTimer); qrPollTimer = 0; }
      try { await fetch('/api/wifi/qr-scan', { method: 'DELETE' }); } catch (e) {}
      hideQrMode();
      setStep(2);
      startScan();
    });
  }

  // ── Card 2: scan + pick ────────────────────────────────────────────────
  function renderNets(nets) {
    const list = $w('wiz-nets');
    list.setAttribute('aria-busy', 'false');
    if (!nets || nets.length === 0) {
      list.innerHTML = '<div class="wiz-net-empty">No networks found. Move closer to your router and try again.</div>';
      announce('No networks found.');
      return;
    }
    /* Keep raw SSIDs in JS-side arrays and reference them from the DOM
     * by integer index. Embedding the raw string in a data-* attribute
     * works on most browsers but mixes encoding domains: an SSID
     * containing &, <, >, " or ' gets HTML-escaped for safe markup,
     * and round-tripping that back through dataset depends on browser-
     * implementation details. The index pattern is unambiguous: the DOM
     * never sees the SSID, and the JS click handler always sees the
     * exact bytes the scan API returned, so SSIDs like "AT&T" or
     * "Mom's WiFi" reach /api/wifi/connect verbatim. */
    const rawSsids  = nets.map(n => n.ssid || '');
    /* The scan API returns a `security` string ("open" / "wpa" / "wpa2"
     * / "wpa/wpa2" / "wpa3" / "wpa2/wpa3" / "other"), not a `secure`
     * boolean. Treat anything other than "open" as secure so the lock
     * icon and the empty-password guard fire correctly. */
    const isSecure  = nets.map(n => {
      const s = (n.security || '').toLowerCase();
      return s !== '' && s !== 'open' && s !== 'none';
    });
    /* Each row is a native <button type="button"> so it's tab-focusable
     * and Enter/Space activate it without a keydown handler. The
     * container is role="group" (set in static markup) so SR users
     * hear "Available networks, group, with N items" before tabbing
     * through. aria-label on each button gives SR users the full row
     * content as a single phrase ("HomeWiFi, secured, signal -55 dBm")
     * rather than the visual SSID/lock/RSSI fragments stitched
     * together; the inner spans are aria-hidden so they don't
     * double-announce. */
    list.innerHTML = nets.map((n, i) => {
      const ssid    = escHtml(rawSsids[i]);
      const lock    = isSecure[i] ? '🔒' : '';
      const rssi    = (n.rssi != null) ? escHtml(n.rssi + ' dBm') : '';
      const aLabel  = (rawSsids[i] || '(no name)')
                    + (isSecure[i] ? ', secured' : ', open')
                    + (n.rssi != null ? ', signal ' + n.rssi + ' dBm' : '');
      return '<button type="button" class="wiz-net-row" data-idx="' + i + '" aria-label="' + escHtml(aLabel) + '">'
           + '<span class="wiz-net-name" aria-hidden="true">' + (ssid || '(no name)') + '</span>'
           + '<span class="wiz-net-lock" aria-hidden="true">' + lock + '</span>'
           + '<span class="wiz-net-rssi" aria-hidden="true">' + rssi + '</span>'
           + '</button>';
    }).join('');
    announce(nets.length === 1
      ? 'Found 1 network.'
      : 'Found ' + nets.length + ' networks.');
    list.querySelectorAll('.wiz-net-row').forEach(row => {
      row.addEventListener('click', () => {
        const i = Number(row.dataset.idx);
        if (!Number.isInteger(i) || i < 0 || i >= rawSsids.length) return;
        pickedSsid   = rawSsids[i];
        pickedSecure = isSecure[i];
        $w('wiz-picked-ssid').textContent = pickedSsid;
        if (!pickedSecure) {
          $w('wiz-pw').value = '';
          $w('wiz-pw').setAttribute('placeholder', 'No password (open network)');
        } else {
          $w('wiz-pw').setAttribute('placeholder', 'Network password');
        }
        /* User has chosen a network — stop polling the radio. The
         * pollScan loop would otherwise keep re-scanning in the
         * background while the user is typing the password. */
        if (scanTimer) { clearTimeout(scanTimer); scanTimer = 0; }
        setStep(3);
        setTimeout(() => $w('wiz-pw').focus(), 100);
      });
    });
  }

  async function pollScan() {
    try {
      const r = await fetch('/api/wifi/scan', { cache: 'no-store' });
      if (!r.ok) throw new Error('scan HTTP ' + r.status);
      const j = await r.json();
      if (j.scanning) {
        scanTimer = setTimeout(pollScan, 800);
        return;
      }
      // Server returns either {ok,scanning:false,networks:[...]} or just
      // {ok,networks:[...]}. Normalize.
      const nets = (j.networks || []).slice().sort((a, b) => (b.rssi || -999) - (a.rssi || -999));
      // Deduplicate by SSID, keeping the strongest.
      const seen = new Set();
      const dedup = [];
      for (const n of nets) {
        if (!n.ssid || seen.has(n.ssid)) continue;
        seen.add(n.ssid);
        dedup.push(n);
      }
      renderNets(dedup);
    } catch (e) {
      setErr(2, 'Scan failed: ' + e.message);
      const list = $w('wiz-nets');
      list.setAttribute('aria-busy', 'false');
      list.innerHTML = '<div class="wiz-net-empty">Try again in a moment.</div>';
      announce('Scan failed. Try again in a moment.');
    }
  }

  function startScan() {
    setErr(2, '');
    const list = $w('wiz-nets');
    list.setAttribute('aria-busy', 'true');
    list.innerHTML = '<div class="wiz-spin">Looking for networks…</div>';
    if (scanTimer) clearTimeout(scanTimer);
    pollScan();
  }

  $w('wiz-rescan').addEventListener('click', startScan);

  // ── Card 3: password + connect ─────────────────────────────────────────
  $w('wiz-back-2').addEventListener('click', () => setStep(2));
  $w('wiz-show-pw').addEventListener('change', e => {
    $w('wiz-pw').type = e.target.checked ? 'text' : 'password';
  });
  $w('wiz-pw').addEventListener('keydown', e => {
    if (e.key === 'Enter') $w('wiz-go-4').click();
  });
  $w('wiz-go-4').addEventListener('click', async () => {
    setErr(3, '');
    const pw = $w('wiz-pw').value || '';
    if (pickedSecure && pw.length === 0) {
      setErr(3, 'This network needs a password.');
      return;
    }
    setStep(4);
    showProgress('Sending credentials to your Canary.');
    try {
      const r = await fetch('/api/wifi/connect', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ ssid: pickedSsid, password: pw, token: token }),
      });
      const j = await r.json();
      if (!r.ok || j.ok === false) {
        // Token-rejection errors are pre-credential, not a connection
        // failure — render them raw so the user reads them as
        // "your setup link is broken" rather than as the awkward
        // "We couldn't connect: <…link broken sentence…>".
        const isTokenErr = j && j.code === 'invalid_token';
        showFailure((j && j.error) ? j.error : ('HTTP ' + r.status),
                    isTokenErr ? { raw: true } : undefined);
        return;
      }
      pollWifiUntilConnected();
    } catch (e) {
      showFailure(e.message);
    }
  });

  // ── Card 4: progress + success / failure ───────────────────────────────
  // Each sub-view reveal calls focusActiveStepHeading() after toggling
  // the .hidden classes so AT lands on the now-visible heading
  // ("Connecting…" → "Your Canary is online." or "Couldn't connect"),
  // not the stale one that was previously focused.
  //
  // showProgress() is also called on every poll tick (~1.5s) by
  // pollWifiUntilConnected() to refresh the status text ("Talking
  // to your Canary." → "Joining your home WiFi." → "Almost there…")
  // — refocusing on every tick would steal focus and re-announce
  // the heading repeatedly. So gate the focus call on a real
  // hidden→visible transition of the progress sub-view. This still
  // covers the retry path after a failure (showFailure hides the
  // progress sub-view; re-entering via the join click handler
  // re-shows it) without firing during steady-state polling.
  function showProgress(msg) {
    const prog = $w('wiz-step-4-progress');
    const wasHidden = prog.classList.contains('hidden');
    prog.classList.remove('hidden');
    $w('wiz-step-4-success').classList.add('hidden');
    $w('wiz-step-4-failure').classList.add('hidden');
    if (msg) $w('wiz-progress-text').textContent = msg;
    if (wasHidden) requestAnimationFrame(() => focusActiveStepHeading());
  }
  // staIp captured here is reused on step 5 for the IP fallback link
  // — the device may not be reachable as canary.local on networks
  // without mDNS, so we surface the raw IP next to the mDNS hostname.
  let connectedStaIp = '';
  function showSuccess(staIp) {
    connectedStaIp = staIp || '';
    $w('wiz-step-4-progress').classList.add('hidden');
    $w('wiz-step-4-failure').classList.add('hidden');
    $w('wiz-step-4-success').classList.remove('hidden');
    $w('wiz-success-ssid').textContent = pickedSsid || 'your home WiFi';
    [4].forEach(i => {
      const dot = $w('wiz-prog-' + i);
      dot.classList.remove('now');
      dot.classList.add('done');
    });
    requestAnimationFrame(() => focusActiveStepHeading());
    /* Brief breath on the success card so the user reads "Your Canary
     * is online" before we slide to pre-flight. 700 ms is the same
     * cadence the OTA flow uses between status transitions. */
    setTimeout(() => {
      setStep(5);
      runSelfTest();
    }, 700);
  }
  // showFailure() defaults to wrapping the reason as "We couldn't
  // connect: {reason}" because the HTTP / network call-sites pass tight
  // fragments ("HTTP 500", "Failed to fetch") that read fine with that
  // prefix. Callers that already provide a complete sentence — like
  // the WiFi fail-reason mapper below — opt out with { raw: true } to
  // avoid awkward double-statements.
  function showFailure(reason, opts) {
    $w('wiz-step-4-progress').classList.add('hidden');
    $w('wiz-step-4-success').classList.add('hidden');
    $w('wiz-step-4-failure').classList.remove('hidden');
    const raw = opts && opts.raw === true;
    let text;
    if (!reason) {
      text = 'Check the password and try again.';
    } else if (raw) {
      text = reason;
    } else {
      text = 'We couldn’t connect: ' + reason;
    }
    $w('wiz-fail-reason').textContent = text;
    requestAnimationFrame(() => focusActiveStepHeading());
  }
  $w('wiz-fail-back').addEventListener('click', () => setStep(3));

  // Map the firmware's last_fail_reason string to a complete, actionable
  // sentence. Loose regexes so future firmware wording tweaks don't
  // silently drop into the generic branch. The matched buckets line up
  // with the three distinct fail modes the firmware emits
  // (canary_wap.ino:5217-5232): auth reject, SSID not present, and
  // post-connect timeout/loss.
  function wifiFailMessage(raw) {
    if (/auth|password|wrong/i.test(raw)) {
      return 'Wrong password. Check for caps lock and try again.';
    }
    if (/not\s*found|no\s*ssid|no\s*network/i.test(raw)) {
      return 'Couldn’t find that network. Check the name, or move closer to the router.';
    }
    if (/timeout|lost/i.test(raw)) {
      return 'Connected, then lost the link. Move closer to the router and try again.';
    }
    // The failure view's h2 already says "Couldn't connect", so the
    // sub-line just quotes the raw firmware reason and asks for a
    // retry — no need to repeat the heading.
    return raw + '. Try again.';
  }

  async function pollWifiUntilConnected() {
    const t0 = Date.now();
    // 90 s gives the wizard headroom over the 30 s firmware connect
    // timeout — enough budget for one retry without the wizard giving
    // up before the device does.
    const TIMEOUT_MS = 90_000;
    const STEP_MS    = 1500;
    let attempt = 0;
    while (Date.now() - t0 < TIMEOUT_MS) {
      attempt++;
      showProgress(attempt === 1 ? 'Talking to your Canary.'
                  : attempt < 5  ? 'Joining your home WiFi.'
                                 : 'Almost there…');
      try {
        const r = await fetch('/api/wifi', { cache: 'no-store' });
        if (r.ok) {
          const j = await r.json();
          if (j && j.sta_connected === true) {
            showSuccess(j.sta_ip || '');
            return;
          }
          // Any non-empty fail_reason is the firmware's last word on
          // the attempt — surface it through the mapper rather than
          // letting the wizard time out with a generic message.
          if (j && j.fail_reason) {
            showFailure(wifiFailMessage(j.fail_reason), { raw: true });
            return;
          }
        }
      } catch (_) { /* the AP may briefly drop while STA bring-up runs */ }
      await new Promise(res => setTimeout(res, STEP_MS));
    }
    // 90 s elapsed without a definitive answer — most likely the
    // Canary fell off the AP we were polling on. Tell the user how to
    // recover rather than blaming WiFi range.
    showFailure('The Canary stopped answering. Connect to its setup network again and start over.', { raw: true });
  }

  // ── Card 5: pre-flight self-test ───────────────────────────────────────
  // Calls /api/selftest once on entry and renders one row per probe.
  // Each row is a <details> the user can expand to see the structured
  // metric blob (rssi_dbm, sensor_pid, free_bytes, …). PASS/FAIL/SKIP/
  // ABSENT all map to a small status icon so the row is scannable
  // without reading text. We never block the wizard on a non-fail
  // (ABSENT/SKIP); only a real FAIL prevents Finish from enabling.
  const ICON = { pass: '✓', fail: '!', skip: '–', absent: '–', unknown: '·' };
  const ICON_LABEL = {
    pass:    'Pass',
    fail:    'Needs attention',
    skip:    'Not active',
    absent:  'Not present',
    unknown: 'Unknown',
  };

  // Plain-language "what to do" guidance, keyed by probe name. Shown only
  // for rows the user can act on (FAIL always; a couple of actionable
  // SKIPs). Each entry returns a string given the probe so we can tailor
  // the wording to the specific status/detail when it helps. Returning ''
  // (or no entry) means "no guidance needed" and the row renders without a
  // hint block. Kept terse and non-technical — the raw metric JSON stays
  // available under the same row for anyone who wants it.
  const HINT = {
    wifi: (p) => p.status === 'fail'
      ? 'The Canary could not reach Wi-Fi. Go back a step and re-check your network name and password, then run the checks again.'
      : '',
    camera: (p) => p.status === 'fail'
      ? 'The camera stopped responding. Unplug the Canary for five seconds, power it back on, then tap Run again.'
      : (p.status === 'absent'
        ? 'No camera was found. If your board has one, reseat the camera ribbon and re-run. If it has no camera, this is expected — you can continue.'
        : ''),
    bluetooth: (p) => p.status === 'fail'
      ? 'Bluetooth didn\'t start. Unplug the Canary for five seconds and power it back on, then tap Run again. If it keeps failing, this unit\'s Bluetooth may be faulty — you can still continue; Wi-Fi features work without it.'
      : '',
    sd: (p) => p.status === 'fail'
      ? 'The SD card couldn\'t be read. Reseat it, or try another microSD card formatted as FAT32, then tap Run again.'
      : '',
    microphone: () => '',
    gpio: (p) => (p.status === 'fail' || p.status === 'skip')
      ? 'A pin looked stuck. Make sure nothing is pressing the BOOT button on the Canary, then tap Run again.'
      : '',
    fetch: () => 'Reconnect your phone to the Canary\'s setup Wi-Fi network (it starts with SecuraCV-), then tap Run again.',
  };

  function hintFor(p) {
    const fn = HINT[(p.name || '').toLowerCase()];
    if (!fn) return '';
    try { return fn(p) || ''; } catch (_) { return ''; }
  }

  function escText(s) {
    // The detail/metric strings come from the device, but the device
    // is on our LAN and we control the firmware; defense-in-depth
    // says we still don't drop anything into innerHTML directly.
    return String(s == null ? '' : s);
  }

  function renderProbes(probes) {
    const list = $w('wiz-st-list');
    list.setAttribute('aria-busy', 'false');
    list.innerHTML = '';
    for (const p of probes || []) {
      const status = (p.status || 'unknown').toLowerCase();
      const det = document.createElement('details');
      det.className = 'wiz-check-row';
      const sum = document.createElement('summary');
      // ARIA: announce the row as "Camera, Sensor online, Pass" so SR
      // users get the full state in one phrase. The visible icon span
      // is aria-hidden so we don't double-announce a glyph.
      sum.setAttribute('aria-label',
        (p.label || p.name || 'Check') + ', ' +
        (p.detail || '') + ', ' +
        (ICON_LABEL[status] || 'Unknown'));
      const nameEl = document.createElement('span');
      nameEl.className = 'wiz-check-name';
      nameEl.textContent = escText(p.label || p.name || 'Check');
      const detailEl = document.createElement('span');
      detailEl.className = 'wiz-check-detail';
      detailEl.textContent = escText(p.detail || '');
      const iconEl = document.createElement('span');
      iconEl.className = 'wiz-check-icon ' + status;
      iconEl.setAttribute('aria-hidden', 'true');
      iconEl.textContent = ICON[status] || ICON.unknown;
      sum.appendChild(nameEl);
      sum.appendChild(detailEl);
      sum.appendChild(iconEl);
      det.appendChild(sum);
      // "What to do" — plain-language remediation. Rendered above the raw
      // metric JSON so guidance comes first. A failing row is auto-opened
      // so the user sees the guidance without having to find the
      // disclosure; passing rows stay collapsed to keep the list scannable.
      const hint = hintFor(p);
      if (hint) {
        const hintEl = document.createElement('div');
        hintEl.className = 'wiz-check-hint';
        const lead = document.createElement('strong');
        lead.textContent = (status === 'fail') ? 'What to do: ' : 'Note: ';
        hintEl.appendChild(lead);
        hintEl.appendChild(document.createTextNode(escText(hint)));
        det.appendChild(hintEl);
        if (status === 'fail') det.open = true;
      }
      // Metric reveal — JSON.stringify with 2-space indent gives the
      // power user a copy-pasteable block without us having to design
      // a per-probe table.
      const meta = document.createElement('div');
      meta.className = 'wiz-check-meta';
      const obj = Object.assign({ code: p.code }, p.metric || {});
      try {
        meta.textContent = JSON.stringify(obj, null, 2);
      } catch (_) {
        meta.textContent = '(metric unavailable)';
      }
      det.appendChild(meta);
      list.appendChild(det);
    }
  }

  function showRunning() {
    $w('wiz-step-5-running').classList.remove('hidden');
    $w('wiz-step-5-result').classList.add('hidden');
    requestAnimationFrame(() => focusActiveStepHeading());
  }

  function showResult(j) {
    $w('wiz-step-5-running').classList.add('hidden');
    $w('wiz-step-5-result').classList.remove('hidden');
    $w('wiz-st-heading').textContent = j.all_passed
      ? 'Your Canary is ready.'
      : 'A check needs your attention';
    $w('wiz-st-sub').textContent = j.all_passed
      ? 'Tap any row to see the technical detail. Then finish.'
      : 'Open the row below for the detail. Re-run after fixing it.';
    renderProbes(j.probes);

    const summary = $w('wiz-st-summary');
    summary.classList.remove('pass', 'fail');
    summary.classList.add(j.all_passed ? 'pass' : 'fail');
    summary.textContent = j.summary || (j.all_passed ? 'All checks passed.' : 'Checks failed.');

    // CTA wiring: mDNS link is always shown (default works on most
    // home routers), IP fallback is only shown when we captured one
    // during step 4. The multi-Canary branch (#wiz-multi-block) is
    // shown FIRST on all_passed; the open-link row + Finish stay hidden
    // until the user either taps "I'm done for now" or finishes the
    // "Set up another" sub-pane. On failure the user is NOT stuck: the
    // per-row "What to do" hints tell them how to fix it, and a
    // "Continue anyway" button lets them open the dashboard regardless
    // (a failed pre-flight is a heads-up, not a hard gate — the device
    // still boots and its working subsystems are usable).
    const links     = $w('wiz-st-links');
    const multi     = $w('wiz-multi-block');
    const another   = $w('wiz-another-block');
    const failnote  = $w('wiz-st-failnote');
    const cont      = $w('wiz-st-continue');
    if (j.all_passed) {
      prepareFinishLinks();
      // Default reveal: the "another room?" pane. The user picks the
      // close-out path from there.
      multi.style.display    = 'block';
      another.style.display  = 'none';
      links.style.display    = 'none';
      failnote.style.display = 'none';
      cont.style.display     = 'none';
      $w('wiz-st-finish').style.display = 'none';
      [5].forEach(i => {
        const dot = $w('wiz-prog-' + i);
        dot.classList.remove('now');
        dot.classList.add('done');
      });
    } else {
      // Keep the close-out panes hidden until the user explicitly opts to
      // continue, but DO offer the escape hatch + guidance up front.
      multi.style.display    = 'none';
      another.style.display  = 'none';
      links.style.display    = 'none';
      failnote.style.display = 'block';
      cont.style.display     = 'inline-flex';
      $w('wiz-st-finish').style.display = 'none';
    }
    requestAnimationFrame(() => focusActiveStepHeading());
  }

  // Mirrors canary_wap.ino's sanitize loop (lowercase [a-z0-9-], any
  // other byte becomes '-', trim leading/trailing hyphens, fall back to
  // "canary" if empty). Kept here so the JS can construct the same
  // hostname the device just registered with mDNS.
  function sanitizeMdnsHostname(raw) {
    let out = '';
    for (const ch of String(raw || '').toLowerCase()) {
      out += /[a-z0-9-]/.test(ch) ? ch : '-';
    }
    out = out.replace(/^-+|-+$/g, '');
    return out || 'canary';
  }

  async function updateMdnsLinkFromDevice() {
    const link = $w('wiz-link-mdns');
    if (!link) return;
    try {
      const r = await fetch('/api/status', { cache: 'no-store' });
      if (!r.ok) return;
      const j = await r.json();
      const id = j && j.device_id ? j.device_id : '';
      if (!id) return;
      const host = sanitizeMdnsHostname(id);
      link.href = 'http://' + host + '.local/';
      link.textContent = 'Open ' + host + '.local';
    } catch (_) {
      // Network blip: fall back to the static canary.local link.
    }
  }

  // Point the close-out links at the right place. The IP fallback is only
  // shown when we captured one during step 4; the mDNS link is rewritten
  // from the device's actual per-device hostname (canary-s3-XXXX.local) so
  // two Canaries on one LAN don't race for `canary.local`. Best-effort: on
  // fetch error the static canary.local link stays in place. Used by both
  // the all-passed close-out and the "Continue anyway" failure escape hatch.
  function prepareFinishLinks() {
    const ipLink = $w('wiz-link-ip');
    if (connectedStaIp) {
      ipLink.href = 'http://' + connectedStaIp + '/';
      ipLink.textContent = 'Open ' + connectedStaIp;
      ipLink.style.display = 'block';
    } else {
      ipLink.style.display = 'none';
    }
    updateMdnsLinkFromDevice();
  }

  // ── Multi-Canary close-out wiring (Step 5 result branches) ────────────
  // Three buttons, two reveal targets, no network calls. The "I'm done"
  // and "Open this one's dashboard" paths both end at the same place
  // (the mDNS/IP link-row + Finish), so factor that out.
  function showFinishLinks() {
    $w('wiz-multi-block').style.display   = 'none';
    $w('wiz-another-block').style.display = 'none';
    $w('wiz-st-failnote').style.display   = 'none';
    $w('wiz-st-continue').style.display   = 'none';
    $w('wiz-st-links').style.display      = 'flex';
    $w('wiz-st-finish').style.display     = 'inline-flex';
    requestAnimationFrame(() => focusActiveStepHeading());
  }
  $w('wiz-done-here').addEventListener('click', showFinishLinks);
  $w('wiz-another-open').addEventListener('click', showFinishLinks);
  $w('wiz-add-another').addEventListener('click', () => {
    $w('wiz-multi-block').style.display   = 'none';
    $w('wiz-another-block').style.display = 'block';
    requestAnimationFrame(() => focusActiveStepHeading());
  });
  $w('wiz-another-back').addEventListener('click', () => {
    $w('wiz-another-block').style.display = 'none';
    $w('wiz-multi-block').style.display   = 'block';
    requestAnimationFrame(() => focusActiveStepHeading());
  });

  async function runSelfTest() {
    showRunning();
    try {
      const r = await fetch('/api/selftest', { cache: 'no-store' });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const j = await r.json();
      showResult(j);
    } catch (e) {
      // Render a synthetic single-row failure so the UI is honest
      // about what happened without crashing the wizard.
      showResult({
        all_passed: false,
        summary: 'Could not reach the Canary: ' + (e && e.message ? e.message : 'unknown'),
        probes: [{
          name: 'fetch', label: 'Connection',
          status: 'fail', code: -1,
          detail: 'Self-test endpoint unreachable',
          metric: { error: String(e && e.message || e) },
        }],
      });
    }
  }

  $w('wiz-st-rerun').addEventListener('click', runSelfTest);
  // Escape hatch on failure: a failed pre-flight is a heads-up, not a hard
  // gate. "Continue anyway" prepares the close-out links and drops the user
  // into the same Finish path the all-passed flow uses, so they're never
  // stuck behind a check they've chosen to live with (or can only fix from
  // the dashboard). We also relax the heading so it reads as resolved.
  $w('wiz-st-continue').addEventListener('click', () => {
    prepareFinishLinks();
    $w('wiz-st-heading').textContent = 'Continuing setup';
    $w('wiz-st-sub').textContent =
      'You can re-run these checks any time from the dashboard. Open your Canary below.';
    showFinishLinks();
  });
  $w('wiz-st-finish').addEventListener('click', () => {
    // Finish hands the user off to canary.local (or the IP fallback).
    // We don't navigate programmatically — the link inside the
    // wiz-link-row is the source of truth so the user gets the
    // browser's normal "open in new tab" / long-press affordances.
    const mdns = $w('wiz-link-mdns');
    if (mdns) mdns.click();
  });
})();

const SVC_CONSOLE   = '8fc1cee0-b162-4401-9607-c8ac21383e90';
const CHR_SNAPSHOT  = '8fc1cee1-b162-4401-9607-c8ac21383e90';
// ble_provision (PR #330): write SCAN_TRIGGER, read+notify SCAN_RESULTS,
// write-only CREDS, read+notify STATE. All bonded.
const SVC_PROVISION = '8fc1cef0-b162-4401-9607-c8ac21383e90';
const CHR_PROV_SCAN_TRIGGER = '8fc1cef1-b162-4401-9607-c8ac21383e90';
const CHR_PROV_SCAN_RESULTS = '8fc1cef2-b162-4401-9607-c8ac21383e90';
const CHR_PROV_CREDS        = '8fc1cef3-b162-4401-9607-c8ac21383e90';
const CHR_PROV_STATE        = '8fc1cef4-b162-4401-9607-c8ac21383e90';
// ble_log_export (PR #332): read+notify HEAD ({count, oldest_seq,
// newest_seq, ring_size}), write-only REQUEST ({"index": N}), read+notify
// RECORD (one log entry as compact JSON). All bonded.
const SVC_LOG          = '8fc1cef5-b162-4401-9607-c8ac21383e90';
const CHR_LOG_HEAD     = '8fc1cef6-b162-4401-9607-c8ac21383e90';
const CHR_LOG_REQUEST  = '8fc1cef7-b162-4401-9607-c8ac21383e90';
const CHR_LOG_RECORD   = '8fc1cef8-b162-4401-9607-c8ac21383e90';
// ble_witness_export (PR #340): read+notify HEAD ({s,t,h,pk}), read RECORD
// (full last-record JSON, up to 512 B). All bonded.
const SVC_WITNESS    = '8fc1cefa-b162-4401-9607-c8ac21383e90';
const CHR_WIT_HEAD   = '8fc1cefb-b162-4401-9607-c8ac21383e90';
const CHR_WIT_RECORD = '8fc1cefc-b162-4401-9607-c8ac21383e90';

// ble_ota (PR #327): write+notify CONTROL (BEGIN+OtaHeader / ABORT),
// write+write_nr DATA (firmware bytes streamed), read+notify STATUS
// (8-byte tuple: {state, pct, bytes_left:u32}). All bonded; OTA hard-
// disabled until SECURACV_OTA_RELEASE_PUBKEY is provisioned (default zero).
const SVC_OTA          = '8fc1ced0-b162-4401-9607-c8ac21383e90';
const CHR_OTA_CONTROL  = '8fc1ced1-b162-4401-9607-c8ac21383e90';
const CHR_OTA_DATA     = '8fc1ced2-b162-4401-9607-c8ac21383e90';
const CHR_OTA_STATUS   = '8fc1ced3-b162-4401-9607-c8ac21383e90';

// State enum mirrors ble_ota.h::OtaState. Notification packet layout:
//   byte 0     state (0..4)
//   byte 1     progress %
//   bytes 2-5  bytes_left (uint32 LE)
//   bytes 6-7  reserved
const OTA_STATES = ['idle', 'receiving', 'verifying', 'rebooting', 'failed'];

const LOGS_PAGE = 30;  // max entries auto-loaded per Refresh

let device = null;
let snapshotChar = null;
let provScanTrigger = null;
let provScanResults = null;
let provCreds = null;
let provState = null;
let selectedAp = null;  // {ssid, sec} of currently-tapped row
let logHead = null, logRequest = null, logRecord = null;
let logHeadData = { count: 0, ring_size: 0 };
let logFetchPending = null;   // resolver for the current in-flight RECORD
let logFetchInProgress = false;
let witHead = null, witRecord = null;
let witHeadData = null, witRecordData = null;
let otaControl = null, otaData = null, otaStatus = null;
let otaBinFile = null;        // File object for the .bin
let otaManifest = null;       // parsed JSON {sha256, signature, version}
let otaInProgress = false;
/* Track the last announced OTA state so we only push to the live region
 * on actual state transitions, not on every status notification (which
 * fires repeatedly during streaming with the same `state` but updated
 * pct/bytes_left). Initialized to 'idle' to match the static markup. */
let lastOtaState = 'idle';

const $ = (id) => document.getElementById(id);
function showErr(msg){const e=$('err-msg');e.textContent=msg;e.classList.add('show');}
function clearErr(){$('err-msg').classList.remove('show');}

function fmtUptime(sec){
  if(!sec)return '—';
  const d=Math.floor(sec/86400),h=Math.floor((sec%86400)/3600),m=Math.floor((sec%3600)/60);
  if(d) return d+'d '+h+'h';
  if(h) return h+'h '+m+'m';
  return m+'m '+(sec%60)+'s';
}
function fmtBytes(n){
  if(!n) return '—';
  if(n<1024) return n+' B';
  if(n<1048576) return (n/1024).toFixed(1)+' KB';
  return (n/1048576).toFixed(2)+' MB';
}

function renderSnapshot(data){
  $('s-id').textContent     = data.id || '—';
  $('s-fw').textContent     = data.fw || '—';
  $('s-up').textContent     = fmtUptime(data.up);
  $('s-heap').textContent   = fmtBytes(data.heap);
  $('s-wifi').textContent   = (data.wifi||'—') + (data.wrssi!=null?(' ('+data.wrssi+' dBm)'):'');
  $('s-owner').textContent  = data.owner_min!=null?(data.owner_min+' min ago'):'never';
  $('s-hh').textContent     = data.hh!=null?String(data.hh):'—';
  $('s-ble').textContent    = data.ble!=null?(data.ble+' ('+(data.ble_hh||0)+' household)'):'—';
  $('s-motion').textContent = data.motion!=null?String(data.motion):'—';
  const ctx = data.ctx || '—';
  const b = $('ctx-badge');
  b.textContent = ctx.toUpperCase();
  b.className = 'badge ' + (ctx==='home'?'badge-home':ctx==='away'?'badge-away':'badge-warn');
}

function onSnapshot(event){
  try {
    const text = new TextDecoder().decode(event.target.value);
    renderSnapshot(JSON.parse(text));
    clearErr();
  } catch (e) {
    showErr('Bad snapshot payload: ' + e.message);
  }
}

// ── ble_provision ──────────────────────────────────────────────────────────
function rssiBars(rssi){
  if (rssi >= -55) return 4;
  if (rssi >= -70) return 3;
  if (rssi >= -85) return 2;
  return 1;
}
function setProvStateBadge(state, error){
  const b = $('wifi-state-badge');
  if (!b) return;
  b.textContent = error ? (state + ' · ' + error) : state;
  b.className = 'badge ' + (
    state === 'connected'    ? 'badge-away' :
    state === 'connecting' ||
    state === 'scanning'     ? 'badge-warn' :
    state === 'failed' ||
    state === 'rate_limited' ? 'badge-warn' :
                               'badge-disconnected'
  );
}
function renderApList(payload){
  const list = $('ap-list');
  if (!list) return;
  list.setAttribute('aria-busy', 'false');
  if (!payload || !Array.isArray(payload.aps) || payload.aps.length === 0) {
    list.innerHTML = '<p class="intro" style="text-align:center">No networks heard. Try again.</p>';
    announce('No networks heard. Try again.');
    return;
  }
  // Backend sorts by RSSI descending already; just render.
  /* Each row is a native <button type="button"> so Tab focuses it and
   * Enter/Space activate it. Hidden-SSID rows (ap.ssid === '') stay
   * non-selectable in this v1 — they get the `disabled` attribute,
   * which removes them from the Tab order automatically and stops
   * Enter/Space from firing. Visible rows carry an aria-label that
   * reads as one phrase ("HomeWiFi, secured WPA2, signal -55 dBm")
   * so SR users get the row contents in one breath; the SSID / sec /
   * RSSI sub-elements are aria-hidden so they don't double-announce. */
  list.innerHTML = payload.aps.map((ap, i) => {
    const bars = rssiBars(ap.rssi || -100);
    const sec = ap.sec || 'open';
    const safe = (s) => String(s).replace(/[<>&"']/g, c => ({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;',"'":'&#39;'}[c]));
    const isHidden = !ap.ssid;
    const aLabel  = (ap.ssid || 'Hidden network')
                  + (sec === 'open' ? ', open' : ', secured ' + sec)
                  + (ap.rssi != null ? ', signal ' + ap.rssi + ' dBm' : '');
    return '<button type="button" class="ap-row" data-i="' + i + '" data-ssid="' + safe(ap.ssid) + '" data-sec="' + safe(sec) + '"' +
             ' aria-label="' + safe(aLabel) + '"' +
             (isHidden ? ' disabled' : '') + '>' +
             '<div class="ap-meta" aria-hidden="true">' +
               '<div class="ap-ssid">' + (safe(ap.ssid) || '<em style="color:var(--muted)">(hidden)</em>') + '</div>' +
               '<div class="ap-sub">' + safe(sec) + ' · ' + (ap.rssi || '?') + ' dBm</div>' +
             '</div>' +
             '<div class="ap-bars s' + bars + '" aria-hidden="true"><span></span><span></span><span></span><span></span></div>' +
           '</button>';
  }).join('');
  announce(payload.aps.length === 1
    ? 'Heard 1 network.'
    : 'Heard ' + payload.aps.length + ' networks.');
  // Wire row taps. The disabled attribute already blocks hidden-SSID
  // rows from firing click; the dataset.ssid guard below is a
  // belt-and-suspenders for that.
  list.querySelectorAll('.ap-row').forEach(row => {
    if (!row.dataset.ssid) return;
    row.addEventListener('click', () => {
      /* aria-current='true' mirrors the visual `.sel` class for SR
       * users so they hear "current" on the chosen row. Removed from
       * other rows on every selection so only one row carries it. */
      list.querySelectorAll('.ap-row').forEach(r => {
        r.classList.remove('sel');
        r.removeAttribute('aria-current');
      });
      row.classList.add('sel');
      row.setAttribute('aria-current', 'true');
      selectedAp = { ssid: row.dataset.ssid, sec: row.dataset.sec };
      $('creds-box').classList.remove('hidden');
      $('pw-input').focus();
    });
  });
}
function onScanResults(event){
  try {
    const text = new TextDecoder().decode(event.target.value);
    renderApList(JSON.parse(text));
  } catch (e) {
    /* Drop aria-busy on parse failure too so the spinner phrase
     * doesn't keep being read while the error is on screen. */
    const list = $('ap-list');
    if (list) list.setAttribute('aria-busy', 'false');
    showErr('Bad scan payload: ' + e.message);
    announce('Scan failed. Try again.');
  }
}
function onProvState(event){
  try {
    const text = new TextDecoder().decode(event.target.value);
    const data = JSON.parse(text);
    setProvStateBadge(data.state, data.error);
    // Always re-derive the header from the current state so a previous
    // "WiFi joined" doesn't stick after a subsequent failed / disconnect /
    // rate_limited. Caught by Gemini.
    //
    // The suffix is the only WiFi-state cue SR users get from the
    // header — #conn-state has aria-live="polite" so changes auto-
    // announce, but #wifi-state-badge does not. Without a per-state
    // suffix, "failed" and "rate_limited" collapse to just the device
    // name and SR users get no hint about why their WiFi attempt
    // didn't take. Add a short phrase for each meaningful state.
    const base = (device && device.name) ? device.name : 'Connected';
    let suffix = '';
    if      (data.state === 'connected')    suffix = ' · WiFi joined';
    else if (data.state === 'failed')       suffix = ' · WiFi join failed' + (data.error ? ': ' + data.error : '');
    else if (data.state === 'rate_limited') suffix = ' · too many tries — wait a minute';
    else if (data.state === 'connecting')   suffix = ' · connecting to WiFi…';
    else if (data.state === 'scanning')     suffix = ' · scanning for networks…';
    $('conn-state').textContent = base + suffix;
  } catch (e) {}
}
async function wifiScan(){
  if (!provScanTrigger) return;
  setProvStateBadge('scanning');
  const list = $('ap-list');
  list.setAttribute('aria-busy', 'true');
  list.innerHTML = '<p class="intro" style="text-align:center">Scanning…</p>';
  // Reset prior selection so the password field doesn't hang around with a
  // stale SSID while new results render. Caught by Gemini.
  selectedAp = null;
  $('creds-box').classList.add('hidden');
  $('pw-input').value = '';
  try {
    // SCAN_TRIGGER is provisioned as NIMBLE_PROPERTY::WRITE
    // (write-with-response) only — NOT WRITE_NR. Web Bluetooth throws
    // NotSupportedError if we use writeValueWithoutResponse on a
    // characteristic that doesn't advertise that property. Caught by
    // Codex P1 + Gemini high.
    await provScanTrigger.writeValue(new Uint8Array([1]));
  } catch (e) {
    showErr('Scan failed: ' + e.message);
  }
}
async function wifiSendCreds(){
  if (!provCreds || !selectedAp) return;
  const pw = $('pw-input').value || '';
  if (selectedAp.ssid.length > 32) { showErr('SSID too long'); return; }
  if (pw.length > 64)              { showErr('Password too long'); return; }
  const payload = JSON.stringify({ ssid: selectedAp.ssid, password: pw });
  try {
    setProvStateBadge('connecting');
    await provCreds.writeValue(new TextEncoder().encode(payload));
    // Clear the password field immediately so it doesn't sit in the DOM.
    $('pw-input').value = '';
  } catch (e) {
    showErr('Send failed: ' + e.message);
  }
}

// ── ble_log_export ─────────────────────────────────────────────────────────
function escHtml(s){return String(s).replace(/[<>&"']/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;',"'":'&#39;'}[c]));}
function fmtUptimeMs(ms){
  if (ms == null) return '—';
  const sec = Math.floor(ms / 1000);
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
  if (h) return h + 'h' + (m < 10 ? '0' : '') + m + 'm';
  if (m) return m + 'm' + (s < 10 ? '0' : '') + s + 's';
  return s + 's';
}
function levelClass(lvl){
  switch (lvl) {
    case 'debug':  return 'debug';
    case 'info':
    case 'notice': return 'info';
    case 'warn':   return 'warn';
    case 'error':  return 'error';
    case 'crit':   return 'crit';
    default:       return 'info';
  }
}
function renderLogsHead(h){
  $('logs-count-badge').textContent = (h.count || 0) + ' / ' + (h.ring_size || 0);
  if (h.ring_size) $('logs-ring-size').textContent = h.ring_size;
}
function onLogHead(event){
  try {
    const text = new TextDecoder().decode(event.target.value);
    const data = JSON.parse(text);
    logHeadData = data;
    renderLogsHead(data);
    // Auto-refresh the visible log list when new entries land — but only
    // if the Logs tab is the one in front. Otherwise just update the
    // count and let the user pull when they switch over.
    if (!$('tab-logs').classList.contains('hidden') && !logFetchInProgress) {
      fetchLogs();
    }
  } catch (e) {}
}
function onLogRecord(event){
  // Resolve the pending fetch promise so the for-loop in fetchLogs
  // can advance to the next index. RECORD is set+notify-once-per-REQUEST
  // by the firmware so this is a clean 1:1 pairing.
  if (!logFetchPending) return;
  try {
    const text = new TextDecoder().decode(event.target.value);
    logFetchPending(JSON.parse(text));
  } catch (e) {
    logFetchPending(null);
  }
  logFetchPending = null;
}
async function requestOneLog(index){
  // Issue a REQUEST write and wait for the matching RECORD notification.
  // Resolves with the parsed JSON or null on timeout.
  return new Promise(async (resolve) => {
    logFetchPending = resolve;
    const timeout = setTimeout(() => {
      if (logFetchPending === resolve) {
        logFetchPending = null;
        resolve(null);
      }
    }, 3000);
    try {
      await logRequest.writeValue(new TextEncoder().encode(
        JSON.stringify({ index: index })));
    } catch (e) {
      clearTimeout(timeout);
      if (logFetchPending === resolve) {
        logFetchPending = null;
        resolve(null);
      }
    }
  });
}
function renderLogList(entries){
  const list = $('logs-list');
  list.setAttribute('aria-busy', 'false');
  if (!entries.length) {
    list.innerHTML = '<p class="intro" style="text-align:center;padding:1rem 0">No log entries yet.</p>';
    announce('No log entries yet.');
    return;
  }
  announce(entries.length === 1
    ? 'Showing 1 log entry.'
    : 'Showing ' + entries.length + ' log entries.');
  list.innerHTML = entries.map(e => {
    if (!e) return '';
    const lvl = e.lvl || 'info';
    const cat = (e.cat != null) ? ('cat ' + e.cat) : '';
    const ts  = fmtUptimeMs(e.ts);
    const seq = (e.seq != null) ? ('#' + e.seq) : '';
    return '<div class="log-row">' +
             '<div class="log-head">' +
               '<span class="log-lvl ' + levelClass(lvl) + '">' + escHtml(lvl) + '</span>' +
               '<span class="log-meta">' + escHtml(seq) + ' · ' + escHtml(ts) + (cat ? ' · ' + escHtml(cat) : '') + '</span>' +
             '</div>' +
             '<div class="log-msg">' + escHtml(e.msg || '') + '</div>' +
             (e.det ? '<div class="log-detail">' + escHtml(e.det) + '</div>' : '') +
           '</div>';
  }).join('');
}
async function fetchLogs(){
  if (!logRequest || !logRecord) return;
  if (logFetchInProgress) return;
  logFetchInProgress = true;
  try {
    const want = Math.min(LOGS_PAGE, logHeadData.count || 0);
    if (want === 0) {
      renderLogList([]);
      return;
    }
    const list = $('logs-list');
    list.setAttribute('aria-busy', 'true');
    list.innerHTML = '<p class="intro" style="text-align:center;padding:1rem 0">Fetching ' + want + ' entries…</p>';
    const out = [];
    for (let i = 0; i < want; i++) {
      const entry = await requestOneLog(i);
      if (!entry) break;  // timeout / error — render what we have so far
      out.push(entry);
    }
    renderLogList(out);
  } finally {
    logFetchInProgress = false;
  }
}

// ── ble_witness_export ─────────────────────────────────────────────────────
function setWitnessVerify(state, text, sub) {
  // state: 'ok' | 'bad' | 'warn'
  const row = $('verify-row');
  const icon = $('verify-icon');
  if (!row) return;
  row.style.display = '';
  icon.className = 'verify-icon ' + state;
  icon.textContent = state === 'ok' ? '✓' : state === 'bad' ? '✗' : '!';
  $('verify-text').textContent = text;
  $('verify-sub').textContent = sub || '';
  /* Push the verdict through the off-screen aria-live region so SR
   * users hear it without hunting for the verify-row visually. The
   * icon glyph (✓/✗/!) is aria-hidden because it's decorative — the
   * `text` strings are already meaningful phrases ("Signature
   * verified", "Signature INVALID", "No record to verify", etc.).
   * Prefix with a state word so the verdict is unambiguous when the
   * text is itself ambiguous (e.g. "Verification skipped" — without
   * "Warning:" prefix, SR users can't tell if that's good or bad). */
  const prefix = state === 'ok'  ? 'Verified: '
              : state === 'bad' ? 'Failed: '
                                : 'Warning: ';
  announce(prefix + text + (sub ? '. ' + sub : '') + '.');
}
function shortenHash(h) {
  if (!h) return '—';
  return h.length <= 24 ? h : (h.substr(0, 12) + '…' + h.substr(-12));
}
function renderWitness() {
  const head = witHeadData;
  const rec  = witRecordData;
  if (!head) {
    $('witness-state-badge').textContent = 'no data';
    $('witness-state-badge').className = 'badge badge-disconnected';
    return;
  }
  $('w-seq').textContent   = head.s != null ? String(head.s) : '—';
  $('w-total').textContent = head.t != null ? String(head.t) : '—';
  $('w-head').textContent  = head.h || '—';
  $('w-pk').textContent    = head.pk || '—';

  if (!rec || rec.empty) {
    $('w-rseq').textContent  = '—';
    $('w-rtype').textContent = 'no records yet';
    $('w-tb').textContent    = '—';
    $('w-plen').textContent  = '—';
    $('witness-state-badge').textContent = 'empty';
    $('witness-state-badge').className = 'badge badge-disconnected';
    $('verify-row').style.display = 'none';
    return;
  }
  $('w-rseq').textContent  = String(rec.seq != null ? rec.seq : '—');
  $('w-rtype').textContent = rec.t || '—';
  $('w-tb').textContent    = rec.tb != null ? String(rec.tb) : '—';
  $('w-plen').textContent  = rec.plen != null ? (rec.plen + ' B') : '—';
  $('witness-state-badge').textContent = 'live';
  $('witness-state-badge').className = 'badge badge-away';
}
async function verifyWitnessSignature() {
  const head = witHeadData;
  const rec  = witRecordData;
  if (!head || !head.pk || !head.h) return;
  if (!rec || rec.empty || !rec.sig || !rec.ch) {
    setWitnessVerify('warn', 'No record to verify',
      'Chain is empty — no signed records have been generated yet.');
    return;
  }
  // Sanity check 1: the record's chain_hash should equal HEAD.h (the
  // most recent record IS the current head). If not, the device is
  // already inconsistent — no need to even check the signature.
  if (rec.ch !== head.h) {
    setWitnessVerify('bad', 'Chain head mismatch',
      'RECORD.ch ≠ HEAD.h. Device is reporting inconsistent state.');
    return;
  }
  // Web Crypto Ed25519 support is uneven: Chrome/Edge/Firefox have it
  // recently; Safari/iOS-WKWebView (incl. Bluefy) may not. Probe before
  // attempting and fall back to an honest "couldn't verify" message
  // rather than silently passing.
  if (!crypto || !crypto.subtle || typeof crypto.subtle.importKey !== 'function') {
    setWitnessVerify('warn', 'Verification unavailable',
      'This browser doesn\'t expose Web Crypto. Use Chrome/Edge to verify.');
    return;
  }
  try {
    const pubBytes = hexToBytes(head.pk);
    const sigBytes = hexToBytes(rec.sig);
    const msgBytes = hexToBytes(rec.ch);
    if (!pubBytes || pubBytes.length !== 32 ||
        !sigBytes || sigBytes.length !== 64 ||
        !msgBytes || msgBytes.length !== 32) {
      setWitnessVerify('bad', 'Malformed cryptographic field',
        'pubkey/signature/chain_hash had unexpected length.');
      return;
    }
    // Ed25519 in SubtleCrypto became standard track relatively recently.
    // Older Chrome may need raw-format key import.
    const key = await crypto.subtle.importKey(
      'raw', pubBytes, { name: 'Ed25519' }, false, ['verify']);
    const ok = await crypto.subtle.verify(
      'Ed25519', key, sigBytes, msgBytes);
    if (ok) {
      setWitnessVerify('ok', 'Signature verified',
        "The device's pubkey signed this chain head. Evidence is authentic.");
    } else {
      setWitnessVerify('bad', 'Signature INVALID',
        'The signature does not match the chain head. Do not trust this record.');
    }
  } catch (e) {
    // Most likely path here is the algorithm name not being recognized
    // (older browsers). Surface honestly rather than bury.
    const msg = (e && e.message) ? e.message : String(e);
    setWitnessVerify('warn', 'Verification skipped',
      'Web Crypto refused Ed25519: ' + msg);
  }
}
function onWitnessHead(event) {
  try {
    const text = new TextDecoder().decode(event.target.value);
    witHeadData = JSON.parse(text);
    renderWitness();
    // If the Witness tab is in front, auto-refresh RECORD too — the
    // chain advanced.
    if (!$('tab-witness').classList.contains('hidden') && witRecord) {
      witRecord.readValue().then(v => {
        try {
          witRecordData = JSON.parse(new TextDecoder().decode(v));
        } catch (_) { witRecordData = null; }
        renderWitness();
        verifyWitnessSignature();
      }).catch(()=>{});
    }
  } catch (_) {}
}
async function refreshWitness() {
  if (!witHead || !witRecord) return;
  try {
    const hv = await witHead.readValue();
    witHeadData = JSON.parse(new TextDecoder().decode(hv));
  } catch (e) { witHeadData = null; }
  try {
    const rv = await witRecord.readValue();
    witRecordData = JSON.parse(new TextDecoder().decode(rv));
  } catch (e) { witRecordData = null; }
  renderWitness();
  verifyWitnessSignature();
}

// ── ble_ota ────────────────────────────────────────────────────────────────
function hexToBytes(hex){
  if (typeof hex !== 'string') return null;
  hex = hex.trim().replace(/[^0-9a-fA-F]/g, '');
  if (hex.length % 2) return null;
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i*2, 2), 16);
  return out;
}
function bytesToHex(buf){
  return Array.from(new Uint8Array(buf)).map(b => b.toString(16).padStart(2, '0')).join('');
}
async function sha256Hex(buf){
  const h = await crypto.subtle.digest('SHA-256', buf);
  return bytesToHex(h);
}
function setOtaState(state, pct, bytesLeft, errorText){
  const b = $('ota-state-badge');
  b.textContent = (errorText && state === 'failed') ? (state + ' · ' + errorText) : state;
  b.className = 'badge ' + (
    state === 'rebooting' ? 'badge-away' :
    state === 'receiving' ||
    state === 'verifying' ? 'badge-warn' :
    state === 'failed'    ? 'badge-warn' :
                            'badge-disconnected'
  );
  if (state === 'failed' && errorText) {
    const e = $('ota-error');
    e.textContent = 'Update failed: ' + errorText;
    e.classList.add('show');
  }
  if (typeof pct === 'number') {
    const bar = $('ota-bar');
    bar.style.width = Math.max(0, Math.min(100, pct)) + '%';
    if (state === 'failed') bar.classList.add('fail'); else bar.classList.remove('fail');
    $('ota-pct').textContent = Math.round(pct) + '%';
  }
  if (typeof bytesLeft === 'number' && otaBinFile) {
    const sent = otaBinFile.size - bytesLeft;
    $('ota-bytes').textContent = (sent > 0 ? sent : 0) + ' / ' + otaBinFile.size + ' B';
  }
  /* Announce only on actual state transitions — onOtaStatus calls this
   * every notification (~per chunk) with the same `state` but evolving
   * pct/bytes, and re-announcing "Sending firmware…" each time would
   * spam the live region. Each phrase below is a full sentence so SR
   * users get the meaning, not just the state word.
   *
   * Failure refinement: onOtaStatus fires setOtaState('failed', …)
   * twice per BLE-driven failure — first with no errorText (just the
   * raw status packet's state byte), then again with the synthetic
   * 'see device serial log' explanation. Without the early-return
   * below, the FIRST call would announce "Update failed: unknown
   * error." (because errorText is undefined) and lock lastOtaState
   * to 'failed', suppressing the SECOND call's correct phrasing.
   * Skip the empty-errorText 'failed' transition while otaInProgress
   * is true so the refinement call gets to fire. Once otaInProgress
   * flips false (after the second call), any subsequent 'failed'
   * with no errorText (e.g., a leftover stray packet) does announce
   * the generic phrase — which is correct fallback behavior.
   *
   * Caught by chatgpt-codex-connector and gemini-code-assist on
   * PR #412 review. */
  if (state !== lastOtaState) {
    if (state === 'failed' && !errorText && otaInProgress) return;
    lastOtaState = state;
    if      (state === 'receiving') announce('Sending firmware to your Canary.');
    else if (state === 'verifying') announce('Verifying firmware signature.');
    else if (state === 'rebooting') announce('Update applied. Canary is rebooting.');
    else if (state === 'failed')    announce('Update failed: ' + (errorText || 'unknown error') + '.');
  }
}
function onOtaStatus(event){
  // Packet: {state:u8, pct:u8, bytes_left:u32 LE, reserved:u16}.
  // event.target.value is ALREADY a DataView per the Web Bluetooth spec —
  // recreating one from .buffer is wrong because it ignores byteOffset
  // and may include adjacent bytes if the underlying ArrayBuffer is
  // larger than the actual packet. Use it directly.
  const dv = event.target.value;
  if (!dv || dv.byteLength < 6) return;
  const state = OTA_STATES[dv.getUint8(0)] || 'unknown';
  const pct   = dv.getUint8(1);
  const left  = dv.getUint32(2, /*littleEndian=*/true);
  setOtaState(state, pct, left);
  if (state === 'failed' && otaInProgress) {
    // Firmware sets last_error which we don't get over BLE; surface
    // generic + suggest re-pick.
    setOtaState('failed', pct, left, 'see device serial log');
    otaInProgress = false;
    refreshOtaStartButton();
  }
  if (state === 'rebooting') {
    // Firmware will reboot in ~500 ms; the BLE link drops on reboot
    // and onDisconnect will tidy up. Just freeze the UI.
    otaInProgress = false;
  }
}
async function readFileAsArrayBuffer(file){
  return new Promise((resolve, reject) => {
    const r = new FileReader();
    r.onload  = () => resolve(r.result);
    r.onerror = () => reject(r.error || new Error('read failed'));
    r.readAsArrayBuffer(file);
  });
}
async function readFileAsText(file){
  return new Promise((resolve, reject) => {
    const r = new FileReader();
    r.onload  = () => resolve(r.result);
    r.onerror = () => reject(r.error || new Error('read failed'));
    r.readAsText(file);
  });
}
async function loadOtaInputs(){
  // Load both files when each input changes; ungate the Start button
  // when both validate.
  const binEl = $('ota-bin'), sigEl = $('ota-sig');
  if (binEl.files && binEl.files[0]) otaBinFile = binEl.files[0]; else otaBinFile = null;
  if (sigEl.files && sigEl.files[0]) {
    try {
      const txt = await readFileAsText(sigEl.files[0]);
      const m = JSON.parse(txt);
      if (typeof m.sha256 !== 'string' || typeof m.signature !== 'string' || typeof m.version !== 'string') {
        throw new Error('manifest must have sha256, signature, and version');
      }
      const sha = hexToBytes(m.sha256);
      const sig = hexToBytes(m.signature);
      if (!sha || sha.length !== 32) throw new Error('sha256 must be 64 hex chars');
      if (!sig || sig.length !== 64) throw new Error('signature must be 128 hex chars');
      // The firmware OtaHeader.version slot is 32 bytes including the NUL
      // terminator (see ble_ota.h). Validate the UTF-8 BYTE length — not
      // the JS char count — so a 31-char string with multi-byte glyphs
      // (emoji, CJK) doesn't sneak past and get silently truncated by
      // memcpy on the device.
      if (new TextEncoder().encode(m.version).length > 31) {
        throw new Error('version must be ≤ 31 UTF-8 bytes');
      }
      otaManifest = { sha256: sha, signature: sig, version: m.version };
    } catch (e) {
      otaManifest = null;
      const me = $('ota-error');
      me.textContent = 'Manifest invalid: ' + e.message;
      me.classList.add('show');
      // No explicit announce() — #ota-error has role="alert" (added
      // alongside this commit), so AT auto-announces the textContent
      // change. The PR #412 announce() call here would now double up
      // with the live-region path. Caught by Gemini in PR #416 review.
      refreshOtaStartButton();
      return;
    }
  } else {
    otaManifest = null;
  }
  $('ota-error').classList.remove('show');
  if (otaBinFile && otaManifest) {
    $('ota-meta').classList.remove('hidden');
    $('ota-meta').textContent =
      'Image: ' + otaBinFile.name + ' (' + otaBinFile.size + ' B)\n' +
      'Version: ' + otaManifest.version + '\n' +
      'SHA-256: ' + bytesToHex(otaManifest.sha256);
  } else {
    $('ota-meta').classList.add('hidden');
  }
  refreshOtaStartButton();
}
function refreshOtaStartButton(){
  const btn = $('ota-start-btn');
  if (otaInProgress) {
    btn.disabled = true; btn.style.opacity = '.5';
    btn.textContent = 'Updating…';
  } else if (otaBinFile && otaManifest && otaControl && otaData) {
    btn.disabled = false; btn.style.opacity = '1';
    btn.textContent = 'Start update';
  } else {
    btn.disabled = true; btn.style.opacity = '.5';
    btn.textContent = otaControl ? 'Pick both files first' : 'Service not available';
  }
}
async function startOta(){
  if (otaInProgress) return;
  if (!otaControl || !otaData) return;
  if (!otaBinFile || !otaManifest) return;
  $('ota-error').classList.remove('show');
  $('ota-progress').classList.remove('hidden');
  setOtaState('idle', 0, otaBinFile.size);
  otaInProgress = true;
  refreshOtaStartButton();

  try {
    const imageBuf = await readFileAsArrayBuffer(otaBinFile);
    // Client-side defense: hash the bin and compare to the manifest BEFORE
    // streaming. Catches "user picked the wrong files" cases without
    // burning radio time / wear-leveling on the inactive flash partition.
    const actualSha = await sha256Hex(imageBuf);
    const expectedSha = bytesToHex(otaManifest.sha256);
    if (actualSha !== expectedSha) {
      throw new Error('local SHA-256 of image does not match manifest');
    }

    // Build the 132-byte OtaHeader (see ble_ota.h).
    //   [0..3]    image_size (u32 LE)
    //   [4..35]   sha256
    //   [36..99]  signature (Ed25519, 64 B)
    //   [100..131] version (null-terminated, max 31 chars + NUL)
    const header = new Uint8Array(132);
    const dv = new DataView(header.buffer);
    dv.setUint32(0, imageBuf.byteLength, /*littleEndian=*/true);
    header.set(otaManifest.sha256, 4);
    header.set(otaManifest.signature, 36);
    const verBytes = new TextEncoder().encode(otaManifest.version);
    header.set(verBytes.subarray(0, Math.min(31, verBytes.length)), 100);
    // bytes 100+verLen..131 stay zero (NUL terminator + padding)

    // BEGIN: command byte 0x01 followed by the header (133 bytes total).
    const beginPkt = new Uint8Array(1 + header.length);
    beginPkt[0] = 0x01;
    beginPkt.set(header, 1);
    setOtaState('receiving', 0, imageBuf.byteLength);
    await otaControl.writeValue(beginPkt);

    // Stream firmware bytes. ATT MTU is at least 23, typically 247 after
    // PR #327's negotiation; payload size is MTU - 3 (ATT WriteWithoutResp
    // overhead). 240 is a safe target inside the negotiated MTU.
    const CHUNK = 240;
    const total = imageBuf.byteLength;
    const view = new Uint8Array(imageBuf);
    let offset = 0;
    while (offset < total && otaInProgress) {
      const end = Math.min(offset + CHUNK, total);
      const slice = view.subarray(offset, end);
      // writeValueWithoutResponse uses ATT Write Command (no PDU back),
      // 10-100x faster than write-with-response over the same link.
      // The DATA characteristic advertises WRITE_NR per ble_ota.cpp.
      try {
        await otaData.writeValueWithoutResponse(slice);
      } catch (e) {
        // Some Web Bluetooth stacks throttle without-response writes by
        // returning a NetworkError when the queue is full. Backoff and
        // retry once.
        await new Promise(r => setTimeout(r, 30));
        await otaData.writeValueWithoutResponse(slice);
      }
      offset = end;
      // STATUS notifications drive the visible progress bar; no need to
      // update from here. But if notifications haven't fired yet, set a
      // local optimistic value so the bar moves smoothly.
      const pct = Math.round((offset / total) * 100);
      $('ota-bar').style.width = pct + '%';
      $('ota-pct').textContent = pct + '%';
      $('ota-bytes').textContent = offset + ' / ' + total + ' B';
    }
    // After the last chunk, the firmware verifies + reboots. Final state
    // arrives via STATUS notifications.
  } catch (err) {
    setOtaState('failed', null, null, err.message || String(err));
    otaInProgress = false;
    refreshOtaStartButton();
    // Best-effort ABORT so the firmware drops back to OTA_IDLE rather
    // than waiting on more DATA writes that aren't coming.
    try { if (otaControl) await otaControl.writeValue(new Uint8Array([0x02])); } catch (_) {}
  }
}

// ── tab switching ──────────────────────────────────────────────────────────
function showTab(name){
  /* Keep aria-selected + roving tabindex in sync with the visible
   * .active class so AT consistently sees what's foregrounded. The
   * tabindex roving (only the active tab is tab-stoppable, inactive
   * are tabindex=-1 but reachable via arrow keys) is the standard
   * tablist keyboard pattern AT / keyboard users expect. */
  document.querySelectorAll('.tab-btn').forEach(b => {
    const active = b.dataset.tab === name;
    b.classList.toggle('active', active);
    b.setAttribute('aria-selected', active ? 'true' : 'false');
    b.setAttribute('tabindex', active ? '0' : '-1');
  });
  $('tab-status').classList.toggle('hidden', name !== 'status');
  $('tab-wifi').classList.toggle('hidden', name !== 'wifi');
  $('tab-logs').classList.toggle('hidden', name !== 'logs');
  $('tab-witness').classList.toggle('hidden', name !== 'witness');
  $('tab-ota').classList.toggle('hidden', name !== 'ota');
}

/* Push a string into the off-screen aria-live region so screen-reader
 * users hear status changes (connect / disconnect / OTA milestones /
 * wizard transitions) without hunting for them visually. We toggle
 * the text via clear-then-set on the next frame because AT
 * implementations sometimes coalesce identical-content updates and
 * skip the announcement. */
function announce(msg){
  const el = document.getElementById('a11y-announcer');
  if (!el) return;
  el.textContent = '';
  requestAnimationFrame(() => { el.textContent = msg; });
}

/* Arrow-key navigation across the tablist — the keyboard shortcut
 * that AT users expect from role="tablist". Left/Right move to the
 * previous/next visible tab, Home/End jump to first/last. We only
 * cycle among VISIBLE tabs (.hidden tabs are skipped) since
 * unrelated services have hidden their entry buttons. */
document.getElementById('tab-nav').addEventListener('keydown', e => {
  if (!['ArrowLeft','ArrowRight','Home','End'].includes(e.key)) return;
  const visible = Array.from(document.querySelectorAll('.tab-btn'))
    .filter(b => !b.classList.contains('hidden'));
  if (visible.length === 0) return;
  const cur = visible.indexOf(document.activeElement);
  let next = cur;
  if (e.key === 'ArrowLeft')  next = (cur <= 0) ? visible.length - 1 : cur - 1;
  if (e.key === 'ArrowRight') next = (cur >= visible.length - 1) ? 0 : cur + 1;
  if (e.key === 'Home')       next = 0;
  if (e.key === 'End')        next = visible.length - 1;
  if (next === cur) return;
  e.preventDefault();
  visible[next].focus();
  visible[next].click();   /* activates the tab via existing handler */
});

async function connect(){
  clearErr();
  // Web Bluetooth requires a secure context (HTTPS or localhost). On
  // plain HTTP the API is undefined regardless of browser support, so
  // detect that explicitly — otherwise users on a perfectly capable
  // Chrome/Edge see a misleading "browser not supported" error and
  // never reach this flow. canary.local ships HTTP by default; HTTPS
  // is opt-in via the firmware's TLS toggle.
  if (!window.isSecureContext) {
    showErr('Web Bluetooth requires HTTPS. Open this page over https:// (enable TLS on the device) or use localhost. Cached PWAs launched from the home screen inherit the origin they were installed from.');
    return;
  }
  if(!navigator.bluetooth){
    showErr('Web Bluetooth not supported by this browser. On iOS, install Bluefy.');
    return;
  }
  try {
    $('conn-state').textContent = 'Selecting device…';
    device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: 'SecuraCV' }],
      optionalServices: [SVC_CONSOLE, SVC_PROVISION, SVC_LOG, SVC_WITNESS, SVC_OTA,
        '0000180a-0000-1000-8000-00805f9b34fb',
        '0000180f-0000-1000-8000-00805f9b34fb']
    });
    device.addEventListener('gattserverdisconnected', onDisconnect);
    $('conn-state').textContent = 'Connecting…';
    const server  = await device.gatt.connect();
    const service = await server.getPrimaryService(SVC_CONSOLE);
    snapshotChar  = await service.getCharacteristic(CHR_SNAPSHOT);
    $('conn-state').textContent = device.name || 'Connected';
    $('connect-card').classList.add('hidden');
    $('status-card').classList.remove('hidden');
    $('actions-card').classList.remove('hidden');
    const value = await snapshotChar.readValue();
    onSnapshot({ target: { value } });
    await snapshotChar.startNotifications();
    snapshotChar.addEventListener('characteristicvaluechanged', onSnapshot);

    // Provisioning service — best-effort. Older firmware (pre-#330)
    // doesn't ship it; if getPrimaryService throws, skip the WiFi tab
    // entirely rather than show a tab that does nothing. Reveal the
    // tab nav + WiFi card only AFTER discovery succeeds. Caught by
    // Gemini.
    try {
      const provSvc = await server.getPrimaryService(SVC_PROVISION);
      provScanTrigger = await provSvc.getCharacteristic(CHR_PROV_SCAN_TRIGGER);
      provScanResults = await provSvc.getCharacteristic(CHR_PROV_SCAN_RESULTS);
      provCreds       = await provSvc.getCharacteristic(CHR_PROV_CREDS);
      provState       = await provSvc.getCharacteristic(CHR_PROV_STATE);
      await provScanResults.startNotifications();
      provScanResults.addEventListener('characteristicvaluechanged', onScanResults);
      await provState.startNotifications();
      provState.addEventListener('characteristicvaluechanged', onProvState);
      // Prime the state badge with the current value.
      try {
        const sv = await provState.readValue();
        onProvState({ target: { value: sv } });
      } catch (_) {}
      // Only NOW reveal the WiFi UI — discovery worked.
      $('tab-nav').classList.remove('hidden');
      $('wifi-card').classList.remove('hidden');
      $('tab-btn-wifi').classList.remove('hidden');
    } catch (e) {
      console.warn('Provisioning service unavailable:', e.message);
    }

    // Log-export service (PR #332) — best-effort like provisioning.
    // Older firmware without it just leaves the Logs tab hidden.
    try {
      const logSvc = await server.getPrimaryService(SVC_LOG);
      logHead    = await logSvc.getCharacteristic(CHR_LOG_HEAD);
      logRequest = await logSvc.getCharacteristic(CHR_LOG_REQUEST);
      logRecord  = await logSvc.getCharacteristic(CHR_LOG_RECORD);
      await logHead.startNotifications();
      logHead.addEventListener('characteristicvaluechanged', onLogHead);
      await logRecord.startNotifications();
      logRecord.addEventListener('characteristicvaluechanged', onLogRecord);
      // Prime the head badge with the current count.
      try {
        const hv = await logHead.readValue();
        onLogHead({ target: { value: hv } });
      } catch (_) {}
      // The tab nav was already revealed by the provisioning block; if
      // that path skipped, reveal nav here so Logs is reachable on its
      // own.
      $('tab-nav').classList.remove('hidden');
      $('logs-card').classList.remove('hidden');
      $('tab-btn-logs').classList.remove('hidden');
    } catch (e) {
      console.warn('Log-export service unavailable:', e.message);
    }

    // Witness export service (PR #340) — bonded read-only access to the
    // chain head + most recent signed record. Best-effort discovery; the
    // tab stays hidden if the firmware doesn't ship it.
    try {
      const witSvc = await server.getPrimaryService(SVC_WITNESS);
      witHead   = await witSvc.getCharacteristic(CHR_WIT_HEAD);
      witRecord = await witSvc.getCharacteristic(CHR_WIT_RECORD);
      await witHead.startNotifications();
      witHead.addEventListener('characteristicvaluechanged', onWitnessHead);
      try {
        const hv = await witHead.readValue();
        witHeadData = JSON.parse(new TextDecoder().decode(hv));
      } catch (_) {}
      try {
        const rv = await witRecord.readValue();
        witRecordData = JSON.parse(new TextDecoder().decode(rv));
      } catch (_) {}
      renderWitness();
      $('tab-nav').classList.remove('hidden');
      $('tab-btn-witness').classList.remove('hidden');
      $('witness-card').classList.remove('hidden');
    } catch (e) {
      console.warn('Witness-export service unavailable:', e.message);
    }

    // OTA service (PR #327) — best-effort like the others. The release
    // pubkey may be unprovisioned (zero), in which case the firmware
    // refuses BEGIN; the tab still renders so the user can see what's
    // wrong. Older firmware without ble_ota at all just skips the tab.
    try {
      const otaSvc = await server.getPrimaryService(SVC_OTA);
      otaControl = await otaSvc.getCharacteristic(CHR_OTA_CONTROL);
      otaData    = await otaSvc.getCharacteristic(CHR_OTA_DATA);
      otaStatus  = await otaSvc.getCharacteristic(CHR_OTA_STATUS);
      await otaStatus.startNotifications();
      otaStatus.addEventListener('characteristicvaluechanged', onOtaStatus);
      try {
        const sv = await otaStatus.readValue();
        onOtaStatus({ target: { value: sv } });
      } catch (_) {}
      $('tab-nav').classList.remove('hidden');
      $('ota-card').classList.remove('hidden');
      $('tab-btn-ota').classList.remove('hidden');
      refreshOtaStartButton();
    } catch (e) {
      console.warn('OTA service unavailable:', e.message);
    }
  } catch (err) {
    showErr(err.message || String(err));
    $('conn-state').textContent = 'Not connected';
  }
}

function onDisconnect(){
  $('conn-state').textContent = 'Disconnected';
  $('connect-card').classList.remove('hidden');
  $('status-card').classList.add('hidden');
  $('actions-card').classList.add('hidden');
  $('tab-nav').classList.add('hidden');
  // Re-hide optional tab buttons so a future connect to a leaner-firmware
  // device starts from a clean state — each is re-revealed by its own
  // discovery block in connect().
  $('tab-btn-wifi').classList.add('hidden');
  $('tab-btn-logs').classList.add('hidden');
  $('tab-btn-witness').classList.add('hidden');
  $('tab-btn-ota').classList.add('hidden');
  $('wifi-card').classList.add('hidden');
  $('logs-card').classList.add('hidden');
  $('witness-card').classList.add('hidden');
  $('ota-card').classList.add('hidden');
  $('creds-box').classList.add('hidden');
  $('pw-input').value = '';
  $('logs-list').innerHTML = '<p class="intro" style="text-align:center;padding:1rem 0">Tap Refresh to fetch the most recent entries.</p>';
  $('ota-progress').classList.add('hidden');
  $('ota-error').classList.remove('show');
  $('ota-bar').style.width = '0';
  $('ota-pct').textContent = '0%';
  $('ota-bytes').textContent = '0 / 0 B';
  $('ota-meta').classList.add('hidden');
  $('ota-bin').value = '';
  $('ota-sig').value = '';
  snapshotChar = provScanTrigger = provScanResults = provCreds = provState = null;
  logHead = logRequest = logRecord = null;
  logHeadData = { count: 0, ring_size: 0 };
  logFetchPending = null;
  logFetchInProgress = false;
  witHead = witRecord = null;
  witHeadData = witRecordData = null;
  $('verify-row').style.display = 'none';
  otaControl = otaData = otaStatus = null;
  otaBinFile = null;
  otaManifest = null;
  otaInProgress = false;
  selectedAp = null;
  showTab('status');
}

async function disconnect(){
  if (device && device.gatt && device.gatt.connected) device.gatt.disconnect();
  onDisconnect();
}

$('connect-btn').addEventListener('click', connect);
$('disconnect-btn').addEventListener('click', disconnect);
$('scan-btn').addEventListener('click', wifiScan);
$('creds-send').addEventListener('click', wifiSendCreds);
$('logs-refresh-btn').addEventListener('click', fetchLogs);
$('witness-refresh-btn').addEventListener('click', refreshWitness);
$('ota-bin').addEventListener('change', loadOtaInputs);
$('ota-sig').addEventListener('change', loadOtaInputs);
$('ota-start-btn').addEventListener('click', startOta);
document.querySelectorAll('.tab-btn').forEach(b => {
  b.addEventListener('click', () => {
    showTab(b.dataset.tab);
    // Auto-fetch on first switch to Logs so the user doesn't have to
    // tap Refresh just to see something land.
    if (b.dataset.tab === 'logs' && logRequest && !logFetchInProgress) {
      const list = $('logs-list');
      // Only auto-fetch if we haven't loaded anything yet.
      if (list && list.querySelector('.log-row') === null) fetchLogs();
    }
    // Run signature verification on first switch to Witness so the user
    // sees the green check (or honest red) without an extra tap.
    if (b.dataset.tab === 'witness' && witHead) {
      const row = $('verify-row');
      if (row && row.style.display === 'none') verifyWitnessSignature();
    }
  });
});

// Up-front capability check so the user sees the real blocker before
// they tap Connect. Two distinct failure modes get distinct messages:
// (1) insecure context → tell them to use HTTPS; (2) no Web Bluetooth
// API → tell iOS users to install Bluefy.
(function checkCapabilities(){
  const btn = $('connect-btn');
  const disable = (msg) => {
    btn.disabled = true;
    btn.style.opacity = '0.5';
    showErr(msg);
  };
  if (!window.isSecureContext) {
    disable('This page is loaded over an insecure origin. Web Bluetooth requires HTTPS or localhost — open over https:// to connect.');
  } else if (!navigator.bluetooth) {
    disable('Web Bluetooth is not available in this browser. On iOS, install Bluefy from the App Store and reopen this page in it.');
  }
})();

if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('/companion-sw.js', { scope: '/companion' })
    .catch(e => console.warn('SW register failed:', e));
}
</script>
</body>
</html>
)COMPANIONHTML";
#endif  // !CANARY_WEB_ASSETS_GZIPPED

// ────────────────────────────────────────────────────────────────────────────
// SERVICE WORKER
// ────────────────────────────────────────────────────────────────────────────
//
// Network-first for the cached URLs so updates land when WiFi is reachable;
// cache fallback when offline / canary.local is unreachable. Caches only the
// shell (HTML + manifest) — BLE characteristic reads always go to the live
// device, never to cache.

static const char COMPANION_SW_JS[] PROGMEM = R"COMPANIONSW(const CACHE='securacv-companion-v1';
const URLS=['/companion','/companion-manifest.webmanifest'];
self.addEventListener('install',e=>{e.waitUntil(caches.open(CACHE).then(c=>c.addAll(URLS)));self.skipWaiting();});
self.addEventListener('activate',e=>{e.waitUntil(caches.keys().then(keys=>Promise.all(keys.filter(k=>k!==CACHE).map(k=>caches.delete(k)))).then(()=>self.clients.claim()));});
self.addEventListener('fetch',e=>{
  if(e.request.method!=='GET')return;
  const u=new URL(e.request.url);
  const wantsCache=URLS.some(p=>u.pathname===p);
  if(!wantsCache)return; // pass through everything else
  e.respondWith(
    fetch(e.request).then(r=>{
      if(r&&r.ok){const copy=r.clone();caches.open(CACHE).then(c=>c.put(e.request,copy));}
      return r;
    }).catch(()=>caches.match(e.request))
  );
});
)COMPANIONSW";

// ────────────────────────────────────────────────────────────────────────────
// MANIFEST
// ────────────────────────────────────────────────────────────────────────────
//
// scope == start_url so iOS / Android home-screen launches drop the user
// straight into the connect flow. Icon is an inline SVG data URI so we
// don't have to serve a separate binary asset from flash.

static const char COMPANION_MANIFEST[] PROGMEM = R"COMPANIONMAN({
"name":"SecuraCV Canary",
"short_name":"Canary",
"description":"Out-of-band BLE console for the SecuraCV Canary.",
"start_url":"/companion",
"scope":"/companion",
"display":"standalone",
"orientation":"portrait-primary",
"background_color":"#0a0e1a",
"theme_color":"#0a0e1a",
"icons":[{"src":"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 192 192'><rect width='192' height='192' rx='32' fill='%230a0e1a'/><circle cx='96' cy='100' r='52' fill='%23f6ad55'/><circle cx='80' cy='90' r='6' fill='%230a0e1a'/><circle cx='112' cy='90' r='6' fill='%230a0e1a'/><path d='M86 112 q10 8 20 0' stroke='%230a0e1a' stroke-width='4' fill='none' stroke-linecap='round'/></svg>","sizes":"192x192","type":"image/svg+xml"}]
}
)COMPANIONMAN";

#endif  // SECURACV_COMPANION_PWA_H
