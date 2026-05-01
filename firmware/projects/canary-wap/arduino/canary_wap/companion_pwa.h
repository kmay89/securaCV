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

#include <Arduino.h>

// ────────────────────────────────────────────────────────────────────────────
// HTML
// ────────────────────────────────────────────────────────────────────────────

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
</style>
</head>
<body>
<header>
  <h1>SecuraCV Canary
    <span class="sub" id="conn-state">Not connected</span>
  </h1>
</header>

<div class="card" id="connect-card">
  <p class="intro">Pair your Canary in <strong>Settings &rsaquo; Bluetooth</strong> first, then tap below to open the live console.</p>
  <button class="btn btn-primary" id="connect-btn">Connect</button>
  <div class="err" id="err-msg"></div>
</div>

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

<div class="card hidden" id="actions-card">
  <button class="btn btn-danger" id="disconnect-btn">Disconnect</button>
</div>

<footer>
  iOS: install <a href="https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055" target="_blank" rel="noopener">Bluefy</a> for Web Bluetooth support.<br>
  Android &amp; desktop Chrome work natively.
</footer>

<script>
'use strict';
const SVC_CONSOLE  = '8fc1cee0-b162-4401-9607-c8ac21383e90';
const CHR_SNAPSHOT = '8fc1cee1-b162-4401-9607-c8ac21383e90';

let device = null;
let snapshotChar = null;

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

async function connect(){
  clearErr();
  if(!navigator.bluetooth){
    showErr('Web Bluetooth not supported by this browser. On iOS, install Bluefy.');
    return;
  }
  try {
    $('conn-state').textContent = 'Selecting device…';
    device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: 'SecuraCV' }],
      optionalServices: [SVC_CONSOLE,
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
  snapshotChar = null;
}

async function disconnect(){
  if (device && device.gatt && device.gatt.connected) device.gatt.disconnect();
  onDisconnect();
}

$('connect-btn').addEventListener('click', connect);
$('disconnect-btn').addEventListener('click', disconnect);

if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('/companion-sw.js', { scope: '/companion' })
    .catch(e => console.warn('SW register failed:', e));
}
</script>
</body>
</html>
)COMPANIONHTML";

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
