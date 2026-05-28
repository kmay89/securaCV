/**
 * @file setup_page_html.h
 * @brief Static captive-portal landing page.
 *
 * Captive-portal mini-browsers (iOS Captive Network Assistant, Android's
 * sign-in sheet) are stripped-down webviews that can't reliably run the
 * companion setup wizard's SPA — trying to render it there is what produced
 * the blank white screen. So the captive page is intentionally plain static
 * HTML with a single job: tell the user to open `canary.local` in their real
 * browser, where the wizard runs. No JavaScript, no redirect, no QR.
 *
 * canary.local is shown as instruction text (type it in your browser). A
 * clickable 192.168.4.1 fallback is offered too: the numeric IP always
 * resolves on the AP even when .local doesn't (e.g. some Android browsers),
 * so a user who can't type or whose mDNS fails still has a one-tap route.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_SETUP_PAGE_HTML_H
#define SECURACV_SETUP_PAGE_HTML_H

#include <pgmspace.h>

const char CAPTIVE_PORTAL_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Set up your Canary</title>
<meta name="theme-color" content="#0b0d10">
<style>
:root{--bg:#0b0d10;--fg:#f0f2f5;--muted:#a0a8b0;--accent:#7cdcff;--card:rgba(255,255,255,.05);--line:rgba(255,255,255,.10)}
*{box-sizing:border-box}
html,body{margin:0;min-height:100vh;min-height:100dvh;background:radial-gradient(1200px 800px at 50% -100px,#1a2030 0%,var(--bg) 60%) fixed;color:var(--fg);font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
body{display:flex;align-items:center;justify-content:center;padding:24px}
main{max-width:480px;width:100%;background:var(--card);border:1px solid var(--line);border-radius:24px;padding:28px 24px;text-align:center;backdrop-filter:blur(18px) saturate(160%);box-shadow:0 30px 80px -30px rgba(0,0,0,.6)}
h1{margin:0 0 10px;font-size:22px;font-weight:600;letter-spacing:-.02em}
.lead{margin:0;color:var(--muted);font-size:14px}
.url{display:block;margin:18px 0;padding:16px;border-radius:14px;background:rgba(124,220,255,.12);color:var(--accent);font-size:26px;font-weight:700;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;user-select:all}
.steps{text-align:left;margin:0;padding:0;list-style:none;font-size:14px}
.steps li{display:flex;gap:10px;align-items:flex-start;padding:7px 0}
.steps li b{flex:none;width:22px;height:22px;line-height:22px;text-align:center;border-radius:50%;background:rgba(124,220,255,.18);color:var(--accent);font-size:12px}
.fallback{margin:16px 0 0;font-size:13px;color:var(--muted)}
.fallback a{color:var(--accent);font-weight:600;text-decoration:none;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.fallback a:active{text-decoration:underline}
.foot{margin-top:20px;padding-top:14px;border-top:1px solid var(--line);color:var(--muted);font-size:11px}
.foot strong{color:var(--fg)}
@media (prefers-color-scheme:light){:root{--bg:#f4f5f7;--fg:#0b0d10;--muted:#5b6470;--card:rgba(255,255,255,.92);--line:rgba(0,0,0,.08)}html,body{background:radial-gradient(1200px 800px at 50% -100px,#dfe5ee 0%,var(--bg) 60%) fixed}.foot strong{color:#0b0d10}}
</style>
</head>
<body>
<main>
<h1>Set up your Canary</h1>
<p class="lead">Finish setup in your phone's web browser.</p>
<span class="url">canary.local</span>
<ol class="steps">
<li><b>1</b><span>Stay connected to this SecuraCV WiFi network.</span></li>
<li><b>2</b><span>Open your browser (Safari or Chrome).</span></li>
<li><b>3</b><span>Type <strong>canary.local</strong> in the address bar and go.</span></li>
</ol>
<p class="fallback">canary.local not working? Tap <a href="http://192.168.4.1/">192.168.4.1</a></p>
<p class="foot"><strong>SecuraCV Canary.</strong> Local-only by default. Nothing leaves your home.</p>
</main>
</body>
</html>
)HTML";

#endif /* SECURACV_SETUP_PAGE_HTML_H */
