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
<!-- Optional tab buttons start hidden; each is revealed only when the
     corresponding GATT service is successfully discovered on the device.
     Older firmware that ships only the Console service shows just Status. -->
<div class="tab-nav hidden" id="tab-nav">
  <button class="tab-btn active" data-tab="status">Status</button>
  <button class="tab-btn hidden" data-tab="wifi" id="tab-btn-wifi">WiFi</button>
  <button class="tab-btn hidden" data-tab="logs" id="tab-btn-logs">Logs</button>
  <button class="tab-btn hidden" data-tab="witness" id="tab-btn-witness">Witness</button>
  <button class="tab-btn hidden" data-tab="ota"  id="tab-btn-ota">OTA</button>
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

<div class="tab-content hidden" id="tab-witness">
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
      <div class="verify-icon" id="verify-icon">&mdash;</div>
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

<div class="tab-content hidden" id="tab-ota">
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
    <div id="ota-error" class="err" style="margin-top:.5rem"></div>
    <p class="intro" style="font-size:.7rem;margin-top:.6rem">Don't disconnect or close this page during transfer. The device reboots into the new image automatically when the bytes verify.</p>
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
  document.querySelectorAll('.tab-btn').forEach(b => {
    b.classList.toggle('active', b.dataset.tab === name);
  });
  $('tab-status').classList.toggle('hidden', name !== 'status');
  $('tab-wifi').classList.toggle('hidden', name !== 'wifi');
  $('tab-logs').classList.toggle('hidden', name !== 'logs');
  $('tab-witness').classList.toggle('hidden', name !== 'witness');
  $('tab-ota').classList.toggle('hidden', name !== 'ota');
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
