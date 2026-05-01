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
.tab-nav{display:flex;gap:.3rem;background:var(--surface);padding:.25rem;border-radius:12px;margin-bottom:.75rem;border:1px solid var(--border)}
.tab-btn{flex:1;padding:.55rem;background:transparent;border:none;color:var(--muted);font-size:.85rem;font-weight:600;border-radius:9px;cursor:pointer;-webkit-tap-highlight-color:transparent;font-family:inherit}
.tab-btn.active{background:var(--surface-2);color:var(--text)}
.tab-content{display:block}
.tab-content.hidden{display:none!important}
.ap-row{display:flex;align-items:center;gap:.6rem;padding:.65rem .5rem;border-radius:10px;cursor:pointer;-webkit-tap-highlight-color:transparent;border:1px solid transparent}
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

<!-- Tab nav appears after connect; switches between Status / WiFi panels. -->
<div class="tab-nav hidden" id="tab-nav">
  <button class="tab-btn active" data-tab="status">Status</button>
  <button class="tab-btn" data-tab="wifi">WiFi</button>
  <button class="tab-btn" data-tab="logs">Logs</button>
</div>

<div class="tab-content" id="tab-status">
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

<div class="tab-content hidden" id="tab-logs">
  <div class="card hidden" id="logs-card">
    <div class="card-title">
      <span>Recent events</span>
      <span class="badge badge-disconnected" id="logs-count-badge">0 / 0</span>
    </div>
    <p class="intro">Newest first. Auto-refreshes when new events land. Drawn from the device's in-RAM ring buffer (capped at <span id="logs-ring-size">100</span>).</p>
    <div style="display:flex;gap:.5rem;margin-bottom:.5rem">
      <button class="btn btn-secondary" id="logs-refresh-btn" style="flex:1">Refresh</button>
    </div>
    <div class="log-list" id="logs-list">
      <p class="intro" style="text-align:center;padding:1rem 0">Tap Refresh to fetch the most recent entries.</p>
    </div>
  </div>
</div>

<div class="tab-content hidden" id="tab-wifi">
  <div class="card hidden" id="wifi-card">
    <div class="card-title">
      <span>WiFi setup</span>
      <span class="badge badge-disconnected" id="wifi-state-badge">idle</span>
    </div>
    <p class="intro">Pick a network, type the password, send. The Canary will join your home WiFi and you can close this page.</p>
    <button class="btn btn-secondary" id="scan-btn">Scan for networks</button>
    <div id="ap-list" style="margin-top:.5rem"></div>
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
  if (!payload || !Array.isArray(payload.aps) || payload.aps.length === 0) {
    list.innerHTML = '<p class="intro" style="text-align:center">No networks heard. Try again.</p>';
    return;
  }
  // Backend sorts by RSSI descending already; just render.
  list.innerHTML = payload.aps.map((ap, i) => {
    const bars = rssiBars(ap.rssi || -100);
    const sec = ap.sec || 'open';
    const safe = (s) => String(s).replace(/[<>&"']/g, c => ({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;',"'":'&#39;'}[c]));
    return '<div class="ap-row" data-i="' + i + '" data-ssid="' + safe(ap.ssid) + '" data-sec="' + safe(sec) + '">' +
             '<div class="ap-meta">' +
               '<div class="ap-ssid">' + (safe(ap.ssid) || '<em style="color:var(--muted)">(hidden)</em>') + '</div>' +
               '<div class="ap-sub">' + safe(sec) + ' · ' + (ap.rssi || '?') + ' dBm</div>' +
             '</div>' +
             '<div class="ap-bars s' + bars + '"><span></span><span></span><span></span><span></span></div>' +
           '</div>';
  }).join('');
  // Wire row taps. Skip rows with empty SSID (hidden networks aren't
  // selectable in this v1 — let the user enter manually in a future
  // PR if there's demand).
  list.querySelectorAll('.ap-row').forEach(row => {
    if (!row.dataset.ssid) return;
    row.addEventListener('click', () => {
      list.querySelectorAll('.ap-row').forEach(r => r.classList.remove('sel'));
      row.classList.add('sel');
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
  } catch (e) { showErr('Bad scan payload: ' + e.message); }
}
function onProvState(event){
  try {
    const text = new TextDecoder().decode(event.target.value);
    const data = JSON.parse(text);
    setProvStateBadge(data.state, data.error);
    // Always re-derive the header from the current state so a previous
    // "WiFi joined" doesn't stick after a subsequent failed / disconnect /
    // rate_limited. Caught by Gemini.
    const base = (device && device.name) ? device.name : 'Connected';
    $('conn-state').textContent = (data.state === 'connected')
      ? (base + ' · WiFi joined')
      : base;
  } catch (e) {}
}
async function wifiScan(){
  if (!provScanTrigger) return;
  setProvStateBadge('scanning');
  $('ap-list').innerHTML = '<p class="intro" style="text-align:center">Scanning…</p>';
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
  if (!entries.length) {
    list.innerHTML = '<p class="intro" style="text-align:center;padding:1rem 0">No log entries yet.</p>';
    return;
  }
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
    $('logs-list').innerHTML = '<p class="intro" style="text-align:center;padding:1rem 0">Fetching ' + want + ' entries…</p>';
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

// ── tab switching ──────────────────────────────────────────────────────────
function showTab(name){
  document.querySelectorAll('.tab-btn').forEach(b => {
    b.classList.toggle('active', b.dataset.tab === name);
  });
  $('tab-status').classList.toggle('hidden', name !== 'status');
  $('tab-wifi').classList.toggle('hidden', name !== 'wifi');
}

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
      optionalServices: [SVC_CONSOLE, SVC_PROVISION, SVC_LOG,
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
    } catch (e) {
      console.warn('Log-export service unavailable:', e.message);
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
  $('wifi-card').classList.add('hidden');
  $('logs-card').classList.add('hidden');
  $('creds-box').classList.add('hidden');
  $('pw-input').value = '';
  $('logs-list').innerHTML = '<p class="intro" style="text-align:center;padding:1rem 0">Tap Refresh to fetch the most recent entries.</p>';
  snapshotChar = provScanTrigger = provScanResults = provCreds = provState = null;
  logHead = logRequest = logRecord = null;
  logHeadData = { count: 0, ring_size: 0 };
  logFetchPending = null;
  logFetchInProgress = false;
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
