/**
 * @file setup_page_html.h
 * @brief PROGMEM head + tail of the captive-portal setup page (Tier 5 #11).
 *
 * The page is rendered in three pieces by handle_captive_portal:
 *   1. SETUP_PAGE_HTML_HEAD  — everything up to where the QR SVG goes.
 *   2. <runtime SVG>         — generated from the one-shot pairing URL
 *                              via Nayuki's qrcodegen, drawn as a single
 *                              <path> for compactness.
 *   3. SETUP_PAGE_HTML_TAIL  — manual fallback link + footer + closing tags.
 *                              Contains a `data-pair-url="..."` attribute so
 *                              the server can splice in the same URL the QR
 *                              encodes, no second HTTP round-trip.
 *
 * Why split? Each piece is static (independent of the per-request token)
 * and lives in PROGMEM. Only the QR + the manual link href are dynamic;
 * those are streamed in between the two PROGMEM halves. This keeps the
 * render handler small and the page payload predictable.
 *
 * Microcopy: this is the FIRST thing a brand-new user sees. Same plain-words
 * doctrine as the dashboard — but the page never appears in the lint
 * scope (the lint trio scans csi_dashboard_html.h only). Manual review is
 * the gate. Strings are short, sentence-case, no jargon.
 */

#ifndef SECURACV_SETUP_PAGE_HTML_H
#define SECURACV_SETUP_PAGE_HTML_H

#include <pgmspace.h>

const char SETUP_PAGE_HTML_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Set up your Canary</title>
<meta name="theme-color" content="#0b0d10">
<style>
:root{
  --bg:#0b0d10; --fg:#f0f2f5; --muted:#a0a8b0; --accent:#7cdcff;
  --card:rgba(255,255,255,.05); --line:rgba(255,255,255,.10);
}
*{box-sizing:border-box}
html,body{margin:0;background:radial-gradient(1200px 800px at 50% -100px,#1a2030 0%,var(--bg) 60%) fixed;color:var(--fg);font:16px/1.5 -apple-system,"SF Pro Text",BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
body{min-height:100dvh;display:flex;align-items:center;justify-content:center;padding:24px}
main{
  max-width:520px;width:100%;
  background:var(--card);border:1px solid var(--line);
  border-radius:24px;padding:28px 24px 22px;
  backdrop-filter:blur(18px) saturate(160%);
  box-shadow:0 30px 80px -30px rgba(0,0,0,.6),inset 0 1px 0 rgba(255,255,255,.05);
}
h1{margin:0 0 8px 0;font-size:22px;font-weight:600;letter-spacing:-0.02em}
.lead{margin:0 0 22px 0;color:var(--muted);font-size:14px;line-height:1.5}
.qr-card{
  background:#fff;border-radius:16px;padding:18px;margin:6px auto 18px;
  display:flex;align-items:center;justify-content:center;
  width:fit-content;
  box-shadow:0 8px 30px -10px rgba(0,0,0,.45);
}
.qr-card svg{display:block;width:240px;height:240px;image-rendering:pixelated}
.steps{margin:0 0 18px 0;padding:0;list-style:none;color:var(--fg);font-size:14px}
.steps li{display:flex;gap:10px;align-items:flex-start;padding:7px 0}
.steps li b{display:inline-block;width:22px;height:22px;line-height:22px;text-align:center;border-radius:50%;background:rgba(124,220,255,.18);color:var(--accent);font-size:12px;flex:none}
.fallback{
  border-top:1px dashed var(--line);padding-top:14px;
  font-size:13px;color:var(--muted);text-align:center
}
.fallback a{color:var(--accent);text-decoration:none;font-weight:500}
.fallback a:hover{text-decoration:underline}
.foot{margin-top:18px;padding-top:14px;border-top:1px solid var(--line);color:var(--muted);font-size:11px;text-align:center;line-height:1.5}
.foot strong{color:var(--fg)}
@media (prefers-color-scheme: light){
  :root{--bg:#f4f5f7;--fg:#0b0d10;--muted:#5b6470;--card:rgba(255,255,255,.92);--line:rgba(0,0,0,.08)}
  html,body{background:radial-gradient(1200px 800px at 50% -100px,#dfe5ee 0%,var(--bg) 60%) fixed}
  .foot strong{color:#0b0d10}
}
</style>
</head>
<body>
<main>
<h1>Set up your Canary</h1>
<p class="lead">Scan the code with your phone's camera to connect your Canary to your home WiFi. No app to install. Nothing leaves your home.</p>
<div class="qr-card" role="img" aria-label="QR code — scan with your phone camera to start Canary setup. If you can't scan, use the manual link below.">
)HTML";

const char SETUP_PAGE_HTML_TAIL_FMT[] PROGMEM = R"HTML(</div>
<ol class="steps" role="list">
  <li><b aria-hidden="true">1</b><span>Point your phone's camera at the code above.</span></li>
  <li><b aria-hidden="true">2</b><span>Tap the link your phone shows. The setup page opens.</span></li>
  <li><b aria-hidden="true">3</b><span>Pick your home WiFi and type the password.</span></li>
</ol>
<p class="fallback">Camera not working? <a href="%s">Tap here to set up by hand.</a></p>
<p class="foot"><strong>SecuraCV Canary.</strong> Privacy-first sensing. Local-only by default.</p>
</main>
</body>
</html>
)HTML";

#endif /* SECURACV_SETUP_PAGE_HTML_H */
