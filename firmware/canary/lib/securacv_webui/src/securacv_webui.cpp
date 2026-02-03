/*
 * SecuraCV Canary — Web UI
 *
 * Dashboard HTML/CSS/JS as PROGMEM string.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_webui.h"

const char CANARY_UI_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <title>SecuraCV Canary</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #0a0e1a;
      --card: #12182d;
      --card-hover: #1a2340;
      --text: #e8ecf4;
      --muted: #8892a8;
      --accent: #4fd1c5;
      --accent-dim: rgba(79, 209, 197, 0.15);
      --warning: #f6ad55;
      --warning-dim: rgba(246, 173, 85, 0.15);
      --danger: #fc8181;
      --danger-dim: rgba(252, 129, 129, 0.15);
      --success: #68d391;
      --success-dim: rgba(104, 211, 145, 0.15);
      --info: #63b3ed;
      --border: #2d3748;
      --shadow: 0 4px 20px rgba(0,0,0,0.4);
      --mono: "JetBrains Mono", "SF Mono", "Consolas", monospace;
    }
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    html { font-size: 15px; -webkit-tap-highlight-color: transparent; }
    body {
      font-family: "Inter", -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: var(--bg);
      color: var(--text);
      min-height: 100vh;
      line-height: 1.5;
    }
    .container { max-width: 960px; margin: 0 auto; padding: 1rem; }
    
    /* Header */
    header {
      background: linear-gradient(135deg, #1a2340 0%, #12182d 100%);
      border-bottom: 1px solid var(--border);
      padding: 1rem;
      position: sticky;
      top: 0;
      z-index: 100;
    }
    .header-content {
      max-width: 960px;
      margin: 0 auto;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 1rem;
      flex-wrap: wrap;
    }
    .brand {
      display: flex;
      align-items: center;
      gap: 0.75rem;
    }
    .brand-icon {
      width: 36px;
      height: 36px;
      background: var(--accent-dim);
      border-radius: 10px;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 1.2rem;
    }
    .brand h1 {
      font-size: 1.25rem;
      font-weight: 600;
      letter-spacing: -0.02em;
    }
    .brand span {
      font-size: 0.75rem;
      color: var(--muted);
      display: block;
    }
    .status-badges {
      display: flex;
      gap: 0.5rem;
      flex-wrap: wrap;
    }
    .badge {
      display: inline-flex;
      align-items: center;
      gap: 0.35rem;
      padding: 0.3rem 0.6rem;
      border-radius: 6px;
      font-size: 0.72rem;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.03em;
    }
    .badge-dot {
      width: 6px;
      height: 6px;
      border-radius: 50%;
      animation: pulse 2s infinite;
    }
    .badge.success { background: var(--success-dim); color: var(--success); }
    .badge.success .badge-dot { background: var(--success); }
    .badge.warning { background: var(--warning-dim); color: var(--warning); }
    .badge.warning .badge-dot { background: var(--warning); animation: pulse-fast 1s infinite; }
    .badge.danger { background: var(--danger-dim); color: var(--danger); }
    .badge.danger .badge-dot { background: var(--danger); animation: pulse-fast 0.5s infinite; }
    .badge.info { background: var(--accent-dim); color: var(--accent); }
    .badge.info .badge-dot { background: var(--accent); }
    
    @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
    @keyframes pulse-fast { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
    
    /* Navigation */
    nav {
      display: flex;
      gap: 0.25rem;
      background: var(--card);
      padding: 0.25rem;
      border-radius: 10px;
      margin: 1rem 0;
    }
    .nav-btn {
      flex: 1;
      padding: 0.6rem 0.75rem;
      border: none;
      background: transparent;
      color: var(--muted);
      font-size: 0.85rem;
      font-weight: 500;
      border-radius: 8px;
      cursor: pointer;
      transition: all 0.15s ease;
    }
    .nav-btn:hover { color: var(--text); background: rgba(255,255,255,0.05); }
    .nav-btn.active { color: var(--text); background: var(--card-hover); }
    .nav-btn .count {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      min-width: 18px;
      height: 18px;
      padding: 0 5px;
      margin-left: 0.4rem;
      background: var(--danger);
      color: #fff;
      font-size: 0.65rem;
      font-weight: 700;
      border-radius: 9px;
    }
    
    /* Cards */
    .card {
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 1rem;
      margin-bottom: 1rem;
    }
    .card-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 0.75rem;
    }
    .card-title {
      font-size: 0.9rem;
      font-weight: 600;
      color: var(--text);
    }
    .card-subtitle {
      font-size: 0.75rem;
      color: var(--muted);
      margin-top: 0.15rem;
    }
    
    /* Stats Grid */
    .stats-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
      gap: 0.75rem;
    }
    .stat-item {
      background: rgba(0,0,0,0.2);
      border-radius: 8px;
      padding: 0.75rem;
    }
    .stat-label {
      font-size: 0.7rem;
      color: var(--muted);
      text-transform: uppercase;
      letter-spacing: 0.05em;
      margin-bottom: 0.25rem;
    }
    .stat-value {
      font-size: 1.25rem;
      font-weight: 600;
      font-family: var(--mono);
    }
    .stat-unit {
      font-size: 0.75rem;
      color: var(--muted);
      margin-left: 0.25rem;
    }
    
    /* Identity Card */
    .identity-grid {
      display: grid;
      gap: 0.5rem;
    }
    .identity-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 0.5rem 0;
      border-bottom: 1px solid rgba(255,255,255,0.05);
    }
    .identity-row:last-child { border-bottom: none; }
    .identity-label {
      font-size: 0.8rem;
      color: var(--muted);
    }
    .identity-value {
      font-family: var(--mono);
      font-size: 0.8rem;
      color: var(--accent);
      word-break: break-all;
      text-align: right;
      max-width: 60%;
    }
    
    /* Chain visualization */
    .chain-viz {
      display: flex;
      align-items: center;
      gap: 0.5rem;
      padding: 0.75rem;
      background: rgba(0,0,0,0.2);
      border-radius: 8px;
      overflow-x: auto;
    }
    .chain-block {
      display: flex;
      flex-direction: column;
      align-items: center;
      min-width: 80px;
    }
    .chain-hash {
      font-family: var(--mono);
      font-size: 0.65rem;
      color: var(--accent);
      padding: 0.3rem 0.5rem;
      background: var(--accent-dim);
      border-radius: 4px;
    }
    .chain-seq {
      font-size: 0.7rem;
      color: var(--muted);
      margin-top: 0.25rem;
    }
    .chain-arrow {
      color: var(--muted);
      font-size: 1.2rem;
    }
    
    /* Peek/Camera Preview */
    .peek-container {
      display: flex;
      flex-direction: column;
    }
    .peek-frame {
      position: relative;
      width: 100%;
      aspect-ratio: 4 / 3;
      background: #0a0e1a;
      border-radius: 8px;
      overflow: hidden;
      border: 1px solid var(--border);
    }
    .peek-stream {
      width: 100%;
      height: 100%;
      object-fit: contain;
      display: block;
    }
    .peek-offline {
      position: absolute;
      inset: 0;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      gap: 0.75rem;
      color: var(--muted);
    }
    .peek-offline svg {
      opacity: 0.5;
    }
    .peek-offline p {
      font-size: 0.85rem;
    }
    .peek-info {
      padding: 0.5rem;
    }
    
    /* Log list */
    .log-list {
      display: flex;
      flex-direction: column;
      gap: 0.5rem;
    }
    .log-item {
      display: grid;
      grid-template-columns: auto 1fr auto;
      gap: 0.75rem;
      align-items: start;
      padding: 0.75rem;
      background: rgba(0,0,0,0.2);
      border-radius: 8px;
      border-left: 3px solid transparent;
      transition: background 0.15s ease;
    }
    .log-item:hover { background: rgba(0,0,0,0.3); }
    .log-item.unread { border-left-color: var(--warning); }
    .log-item.error { border-left-color: var(--danger); }
    .log-item.critical { border-left-color: var(--danger); background: var(--danger-dim); }
    
    .log-level {
      font-family: var(--mono);
      font-size: 0.7rem;
      font-weight: 600;
      padding: 0.2rem 0.4rem;
      border-radius: 4px;
      text-transform: uppercase;
    }
    .log-level.debug { background: rgba(255,255,255,0.1); color: var(--muted); }
    .log-level.info { background: var(--accent-dim); color: var(--accent); }
    .log-level.warning { background: var(--warning-dim); color: var(--warning); }
    .log-level.error { background: var(--danger-dim); color: var(--danger); }
    .log-level.critical { background: var(--danger); color: #fff; }
    
    .log-content {
      display: flex;
      flex-direction: column;
      gap: 0.2rem;
    }
    .log-message {
      font-size: 0.85rem;
      color: var(--text);
    }
    .log-detail {
      font-size: 0.75rem;
      color: var(--muted);
      font-family: var(--mono);
    }
    .log-meta {
      font-size: 0.7rem;
      color: var(--muted);
    }
    
    .log-actions {
      display: flex;
      gap: 0.25rem;
    }
    
    /* Buttons */
    .btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 0.4rem;
      padding: 0.5rem 0.75rem;
      border: none;
      border-radius: 6px;
      font-size: 0.8rem;
      font-weight: 500;
      cursor: pointer;
      transition: all 0.15s ease;
    }
    .btn-sm { padding: 0.3rem 0.5rem; font-size: 0.7rem; }
    .btn-primary { background: var(--accent); color: var(--bg); }
    .btn-primary:hover { filter: brightness(1.1); }
    .btn-secondary { background: rgba(255,255,255,0.1); color: var(--text); }
    .btn-secondary:hover { background: rgba(255,255,255,0.15); }
    .btn-danger { background: var(--danger-dim); color: var(--danger); }
    .btn-danger:hover { background: var(--danger); color: #fff; }
    .btn-ghost { background: transparent; color: var(--muted); }
    .btn-ghost:hover { color: var(--text); background: rgba(255,255,255,0.05); }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    
    /* GPS Status */
    .gps-grid {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: 0.5rem;
    }
    .gps-item {
      padding: 0.5rem;
      background: rgba(0,0,0,0.2);
      border-radius: 6px;
    }
    .gps-label {
      font-size: 0.65rem;
      color: var(--muted);
      text-transform: uppercase;
    }
    .gps-value {
      font-family: var(--mono);
      font-size: 0.9rem;
    }
    
    /* Witness records */
    .witness-item {
      display: grid;
      grid-template-columns: 60px 1fr auto;
      gap: 0.75rem;
      padding: 0.6rem;
      background: rgba(0,0,0,0.2);
      border-radius: 8px;
    }
    .witness-seq {
      font-family: var(--mono);
      font-size: 0.85rem;
      font-weight: 600;
      color: var(--accent);
    }
    .witness-type {
      font-size: 0.7rem;
      color: var(--muted);
    }
    .witness-hash {
      font-family: var(--mono);
      font-size: 0.7rem;
      color: var(--muted);
    }
    .witness-verified {
      font-size: 0.7rem;
      padding: 0.2rem 0.4rem;
      border-radius: 4px;
      background: var(--success-dim);
      color: var(--success);
    }
    
    /* Panels */
    .panel { display: none; }
    .panel.active { display: block; }
    
    /* Empty state */
    .empty-state {
      text-align: center;
      padding: 2rem;
      color: var(--muted);
    }
    .empty-icon {
      font-size: 2rem;
      margin-bottom: 0.5rem;
      opacity: 0.5;
    }
    
    /* Loading */
    .loading {
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 2rem;
    }
    .spinner {
      width: 24px;
      height: 24px;
      border: 2px solid var(--border);
      border-top-color: var(--accent);
      border-radius: 50%;
      animation: spin 0.8s linear infinite;
    }
    @keyframes spin { to { transform: rotate(360deg); } }
    
    /* Modals */
    .modal-overlay {
      position: fixed;
      inset: 0;
      background: rgba(0,0,0,0.7);
      display: none;
      align-items: center;
      justify-content: center;
      padding: 1rem;
      z-index: 1000;
    }
    .modal-overlay.active { display: flex; }
    .modal {
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: 12px;
      width: 100%;
      max-width: 400px;
      max-height: 90vh;
      overflow-y: auto;
    }
    .modal-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 1rem;
      border-bottom: 1px solid var(--border);
    }
    .modal-title { font-size: 1rem; font-weight: 600; }
    .modal-close {
      background: none;
      border: none;
      color: var(--muted);
      font-size: 1.25rem;
      cursor: pointer;
    }
    .modal-body { padding: 1rem; }
    .modal-footer {
      display: flex;
      gap: 0.5rem;
      justify-content: flex-end;
      padding: 1rem;
      border-top: 1px solid var(--border);
    }
    
    /* Form elements */
    .form-group { margin-bottom: 1rem; }
    .form-label {
      display: block;
      font-size: 0.8rem;
      color: var(--muted);
      margin-bottom: 0.3rem;
    }
    .form-input, .form-select {
      width: 100%;
      padding: 0.6rem;
      background: rgba(0,0,0,0.3);
      border: 1px solid var(--border);
      border-radius: 6px;
      color: var(--text);
      font-size: 0.9rem;
    }
    .form-input:focus, .form-select:focus {
      outline: none;
      border-color: var(--accent);
    }
    
    /* Resolution selector */
    .resolution-selector {
      display: flex;
      gap: 0.5rem;
      flex-wrap: wrap;
      margin-top: 0.5rem;
    }
    .resolution-btn {
      padding: 0.4rem 0.6rem;
      font-size: 0.7rem;
      font-family: var(--mono);
      background: rgba(0,0,0,0.3);
      border: 1px solid var(--border);
      border-radius: 4px;
      color: var(--muted);
      cursor: pointer;
      transition: all 0.15s ease;
    }
    .resolution-btn:hover {
      border-color: var(--accent);
      color: var(--text);
    }
    .resolution-btn.active {
      background: var(--accent-dim);
      border-color: var(--accent);
      color: var(--accent);
    }
    
    /* Responsive */
    @media (max-width: 600px) {
      .stats-grid { grid-template-columns: repeat(2, 1fr); }
      .gps-grid { grid-template-columns: 1fr; }
      .header-content { flex-direction: column; align-items: flex-start; }
    }
  </style>
</head>
<body>
  <header>
    <div class="header-content">
      <div class="brand">
        <div class="brand-icon">🔒</div>
        <div>
          <h1>SecuraCV Canary</h1>
          <span id="deviceId">Loading...</span>
        </div>
      </div>
      <div class="status-badges">
        <div class="badge success" id="chainBadge">
          <span class="badge-dot"></span>
          <span>Chain OK</span>
        </div>
        <div class="badge info" id="gpsBadge">
          <span class="badge-dot"></span>
          <span id="gpsStatus">No Fix</span>
        </div>
        <div class="badge success" id="sdBadge">
          <span class="badge-dot"></span>
          <span>SD OK</span>
        </div>
        <div class="badge info" id="cameraBadge">
          <span class="badge-dot"></span>
          <span id="cameraStatus">CAM</span>
        </div>
        <div class="badge info" id="btBadge">
          <span class="badge-dot"></span>
          <span id="btStatus">BT</span>
        </div>
      </div>
    </div>
  </header>

  <div class="container">
    <nav>
      <button class="nav-btn active" data-panel="status">Status</button>
      <button class="nav-btn" data-panel="peek">Peek</button>
      <button class="nav-btn" data-panel="opera">
        Opera<span class="count" id="operaAlertCount" style="display:none">0</span>
      </button>
      <button class="nav-btn" data-panel="community">
        Community<span class="count" id="chirpCount" style="display:none">0</span>
      </button>
      <button class="nav-btn" data-panel="logs">
        Logs<span class="count" id="logsCount" style="display:none">0</span>
      </button>
      <button class="nav-btn" data-panel="witness">Witness</button>
      <button class="nav-btn" data-panel="settings">Settings</button>
      <button class="nav-btn" data-panel="bluetooth">Bluetooth</button>
    </nav>

    <!-- Status Panel -->
    <div class="panel active" id="panel-status">
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Device Health</div>
            <div class="card-subtitle">Real-time system metrics</div>
          </div>
          <button class="btn btn-ghost btn-sm" onclick="refreshStatus()">↻ Refresh</button>
        </div>
        <div class="stats-grid">
          <div class="stat-item">
            <div class="stat-label">Uptime</div>
            <div class="stat-value" id="uptime">--:--:--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Witness Records</div>
            <div class="stat-value" id="witnessCount">0</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Chain Sequence</div>
            <div class="stat-value" id="chainSeq">0</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Boot Count</div>
            <div class="stat-value" id="bootCount">0</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Free Heap</div>
            <div class="stat-value"><span id="freeHeap">0</span><span class="stat-unit">KB</span></div>
          </div>
          <div class="stat-item">
            <div class="stat-label">SD Free</div>
            <div class="stat-value"><span id="sdFree">0</span><span class="stat-unit">MB</span></div>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Device Identity</div>
            <div class="card-subtitle">Cryptographic fingerprint</div>
          </div>
        </div>
        <div class="identity-grid">
          <div class="identity-row">
            <span class="identity-label">Public Key</span>
            <span class="identity-value" id="pubkey">Loading...</span>
          </div>
          <div class="identity-row">
            <span class="identity-label">Fingerprint (FP8)</span>
            <span class="identity-value" id="fingerprint">Loading...</span>
          </div>
          <div class="identity-row">
            <span class="identity-label">Ruleset</span>
            <span class="identity-value" id="ruleset">securacv:canary:v1.0</span>
          </div>
          <div class="identity-row">
            <span class="identity-label">Firmware</span>
            <span class="identity-value" id="firmware">2.0.1</span>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Hash Chain</div>
            <div class="card-subtitle">Recent chain blocks</div>
          </div>
        </div>
        <div class="chain-viz" id="chainViz">
          <div class="loading"><div class="spinner"></div></div>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">GPS Status</div>
            <div class="card-subtitle" id="gpsSubtitle">Waiting for fix...</div>
          </div>
        </div>
        <div class="gps-grid">
          <div class="gps-item">
            <div class="gps-label">Latitude</div>
            <div class="gps-value" id="gpsLat">--</div>
          </div>
          <div class="gps-item">
            <div class="gps-label">Longitude</div>
            <div class="gps-value" id="gpsLon">--</div>
          </div>
          <div class="gps-item">
            <div class="gps-label">Altitude</div>
            <div class="gps-value" id="gpsAlt">--</div>
          </div>
          <div class="gps-item">
            <div class="gps-label">Speed</div>
            <div class="gps-value" id="gpsSpeed">--</div>
          </div>
          <div class="gps-item">
            <div class="gps-label">Satellites</div>
            <div class="gps-value" id="gpsSats">--</div>
          </div>
          <div class="gps-item">
            <div class="gps-label">HDOP</div>
            <div class="gps-value" id="gpsHdop">--</div>
          </div>
        </div>
      </div>
    </div>

    <!-- Peek Panel (Camera Preview for Setup) -->
    <div class="panel" id="panel-peek">
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Camera Preview</div>
            <div class="card-subtitle" id="peekSubtitle">Live view for positioning (not recorded)</div>
          </div>
          <div style="display:flex;gap:0.5rem;align-items:center;">
            <span id="peekStatus" style="font-size:0.75rem;color:var(--muted);">Ready</span>
            <button class="btn btn-primary btn-sm" id="peekToggle" onclick="togglePeek()">▶ Start</button>
          </div>
        </div>
        <div class="peek-container">
          <div class="peek-frame" id="peekFrame">
            <img id="peekStream" class="peek-stream" style="display:none;" alt="Camera preview">
            <div id="peekOffline" class="peek-offline">
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="48" height="48">
                <path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z"/>
                <circle cx="12" cy="13" r="4"/>
              </svg>
              <p id="peekOfflineText">Click Start to preview</p>
            </div>
          </div>
          <div class="peek-info">
            <p style="font-size:0.8rem;color:var(--muted);margin-top:0.75rem;">
              <strong>Note:</strong> This preview is for camera positioning only. 
              No frames are stored — SecuraCV records semantic events, not video.
            </p>
          </div>
        </div>
      </div>
      
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Camera Controls</div>
            <div class="card-subtitle">Adjust preview settings</div>
          </div>
        </div>
        <div style="display:flex;gap:0.75rem;flex-wrap:wrap;">
          <button class="btn btn-secondary" onclick="takeSnapshot()">📷 Snapshot</button>
          <button class="btn btn-ghost" onclick="refreshPeekStatus()">↻ Refresh Status</button>
        </div>
        
        <!-- Resolution Control -->
        <div style="margin-top:1rem;">
          <div class="form-label">Resolution</div>
          <div class="resolution-selector" id="resolutionSelector">
            <button class="resolution-btn" data-size="4" onclick="setResolution(4)">320×240</button>
            <button class="resolution-btn active" data-size="8" onclick="setResolution(8)">640×480</button>
            <button class="resolution-btn" data-size="9" onclick="setResolution(9)">800×600</button>
            <button class="resolution-btn" data-size="10" onclick="setResolution(10)">1024×768</button>
            <button class="resolution-btn" data-size="11" onclick="setResolution(11)">1280×720</button>
          </div>
          <p id="resolutionStatus" style="font-size:0.7rem;color:var(--muted);margin-top:0.5rem;">Current: 640×480</p>
        </div>
        
        <div id="snapshotPreview" style="margin-top:1rem;display:none;">
          <div class="form-label">Snapshot</div>
          <img id="snapshotImg" style="max-width:100%;border-radius:8px;border:1px solid var(--border);" alt="Snapshot">
        </div>
      </div>
    </div>

    <!-- Opera Panel (Mesh Network) -->
    <div class="panel" id="panel-opera">
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Opera Network</div>
            <div class="card-subtitle" id="operaSubtitle">Mesh network status</div>
          </div>
          <div class="badge info" id="operaBadge">
            <span class="badge-dot"></span>
            <span id="operaState">Loading...</span>
          </div>
        </div>
        <div class="stats-grid">
          <div class="stat-item">
            <div class="stat-label">Status</div>
            <div class="stat-value" id="operaStatus" style="font-size:0.9rem;">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Opera ID</div>
            <div class="stat-value" id="operaId" style="font-size:0.75rem;word-break:break-all;">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Peers Online</div>
            <div class="stat-value"><span id="peersOnline">0</span> / <span id="peersTotal">0</span></div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Alerts</div>
            <div class="stat-value"><span id="alertsReceived">0</span></div>
          </div>
        </div>
      </div>

      <!-- Peers Grid -->
      <div class="card" id="operaPeersCard">
        <div class="card-header">
          <div>
            <div class="card-title">Opera Members</div>
            <div class="card-subtitle" id="peersSubtitle">Devices in your opera</div>
          </div>
          <button class="btn btn-ghost btn-sm" onclick="refreshOpera()">Refresh</button>
        </div>
        <div id="peersList" class="log-list">
          <div class="empty-state">
            <div class="empty-icon">🐦</div>
            <p>No opera configured</p>
          </div>
        </div>
      </div>

      <!-- Opera Alerts -->
      <div class="card" id="operaAlertsCard">
        <div class="card-header">
          <div>
            <div class="card-title">Opera Alerts</div>
            <div class="card-subtitle">Alerts from other canaries</div>
          </div>
          <button class="btn btn-ghost btn-sm" onclick="clearOperaAlerts()">Clear</button>
        </div>
        <div id="operaAlertsList" class="log-list">
          <div class="empty-state">
            <div class="empty-icon">✓</div>
            <p>No alerts</p>
          </div>
        </div>
      </div>

      <!-- Opera Controls -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Opera Management</div>
            <div class="card-subtitle">Create, join, or leave opera</div>
          </div>
        </div>

        <!-- No Opera State -->
        <div id="operaNoOpera">
          <p style="color:var(--muted);margin-bottom:1rem;font-size:0.85rem;">
            Create a new opera or join an existing one. Opera members protect each other
            by broadcasting alerts when tampered with or losing power.
          </p>
          <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
            <button class="btn btn-primary" onclick="startPairing('init')">Create Opera</button>
            <button class="btn btn-secondary" onclick="startPairing('join')">Join Opera</button>
          </div>
        </div>

        <!-- Has Opera State -->
        <div id="operaHasOpera" style="display:none;">
          <div class="form-group">
            <label class="form-label">Opera Name</label>
            <div style="display:flex;gap:0.5rem;">
              <input type="text" class="form-input" id="operaNameInput" placeholder="My Opera" style="flex:1;">
              <button class="btn btn-secondary" onclick="saveOperaName()">Save</button>
            </div>
          </div>
          <div style="display:flex;gap:0.5rem;flex-wrap:wrap;margin-top:1rem;">
            <button class="btn btn-primary" onclick="startPairing('init')">Add Device</button>
            <button class="btn btn-danger" onclick="leaveOpera()">Leave Opera</button>
          </div>
        </div>

        <!-- Pairing State -->
        <div id="operaPairing" style="display:none;">
          <div style="text-align:center;padding:1rem;">
            <div class="spinner" style="margin:0 auto 1rem;"></div>
            <p id="pairingStatus" style="color:var(--muted);margin-bottom:1rem;">Searching for devices...</p>
            <div id="pairingCode" style="display:none;margin-bottom:1rem;">
              <p style="font-size:0.85rem;color:var(--muted);margin-bottom:0.5rem;">Confirm this code matches on both devices:</p>
              <div style="font-family:var(--mono);font-size:2rem;font-weight:bold;color:var(--accent);letter-spacing:0.2em;" id="pairingCodeValue">------</div>
            </div>
            <div style="display:flex;gap:0.5rem;justify-content:center;">
              <button class="btn btn-primary" id="pairingConfirmBtn" onclick="confirmPairing()" style="display:none;">Confirm Match</button>
              <button class="btn btn-secondary" onclick="cancelPairing()">Cancel</button>
            </div>
          </div>
        </div>
      </div>

      <!-- Mesh Enable Toggle -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Mesh Network</div>
            <div class="card-subtitle">Enable or disable mesh networking</div>
          </div>
          <label style="display:flex;align-items:center;gap:0.5rem;cursor:pointer;">
            <input type="checkbox" id="meshEnabled" onchange="toggleMeshEnabled()">
            <span style="font-size:0.85rem;color:var(--muted);">Enabled</span>
          </label>
        </div>
        <p style="font-size:0.8rem;color:var(--muted);margin:0;">
          When enabled, this device will communicate with other canaries in your opera
          using ESP-NOW (direct radio) and WiFi. Disable to operate independently.
        </p>
      </div>
    </div>

    <!-- Community Panel (Chirp Channel) -->
    <div class="panel" id="panel-community">
      <!-- Chirp Status Card -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Community Witness Network</div>
            <div class="card-subtitle" id="chirpSubtitle">Anonymous community alerts</div>
          </div>
          <div class="badge info" id="chirpBadge">
            <span class="badge-dot"></span>
            <span id="chirpState">Disabled</span>
          </div>
        </div>

        <div class="stats-grid">
          <div class="stat-item">
            <div class="stat-label">Your Session</div>
            <div class="stat-value" id="chirpSessionEmoji" style="font-size:1.5rem;">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Nearby Devices</div>
            <div class="stat-value" id="chirpNearbyCount">0</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Recent Chirps</div>
            <div class="stat-value" id="chirpRecentCount">0</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Cooldown</div>
            <div class="stat-value" id="chirpCooldown" style="font-size:0.9rem;">Ready</div>
          </div>
        </div>

        <!-- Enable/Disable Toggle -->
        <div style="margin-top:1rem;padding:0.75rem;background:rgba(0,0,0,0.2);border-radius:8px;display:flex;align-items:center;justify-content:space-between;">
          <div>
            <strong style="font-size:0.85rem;">Enable Chirp Channel</strong>
            <p style="font-size:0.75rem;color:var(--muted);margin:0;">Anonymous community alerts (new identity each session)</p>
          </div>
          <label style="display:flex;align-items:center;gap:0.5rem;cursor:pointer;">
            <input type="checkbox" id="chirpEnabled" onchange="toggleChirpEnabled()">
            <span style="font-size:0.85rem;color:var(--muted);">Enabled</span>
          </label>
        </div>
      </div>

      <!-- Send Chirp Card — Template-based (NO free text) -->
      <div class="card" id="chirpSendCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Share with Community</div>
            <div class="card-subtitle">Structured alerts — witness events, not people</div>
          </div>
        </div>

        <div class="form-group">
          <label class="form-label">What are you witnessing?</label>
          <select class="form-input" id="chirpTemplate" onchange="updateChirpPreview()">
            <optgroup label="Authority Presence">
              <option value="0">🚔 police activity in area</option>
              <option value="1">🚨 heavy law enforcement response</option>
              <option value="2">🚧 road blocked by law enforcement</option>
              <option value="3">🚁 helicopter circling area</option>
              <option value="4">🏛️ federal agents in area</option>
            </optgroup>
            <optgroup label="Infrastructure">
              <option value="16">⚡ power outage</option>
              <option value="17">💧 water service disruption</option>
              <option value="18">🔥 gas smell - evacuate?</option>
              <option value="19">📶 internet outage in area</option>
              <option value="20">🚧 road closed or blocked</option>
            </optgroup>
            <optgroup label="Emergency">
              <option value="32">🔥 fire or smoke visible</option>
              <option value="33">🚑 medical emergency scene</option>
              <option value="34">🚑🚑 multiple ambulances responding</option>
              <option value="35">📢 evacuation in progress</option>
              <option value="36">🏠 shelter in place advisory</option>
            </optgroup>
            <optgroup label="Weather">
              <option value="48">⛈️ severe weather warning</option>
              <option value="49">🌪️ tornado warning</option>
              <option value="50">🌊 flooding reported</option>
              <option value="51">⚡ dangerous lightning nearby</option>
            </optgroup>
            <optgroup label="Mutual Aid">
              <option value="64">🤝 neighbor may need help</option>
              <option value="65">📦 supplies needed in area</option>
              <option value="66">🙋 offering assistance</option>
            </optgroup>
            <optgroup label="All Clear">
              <option value="128">✅ situation resolved</option>
              <option value="129">✅ area appears safe now</option>
              <option value="130">❌ false alarm</option>
            </optgroup>
          </select>
        </div>

        <div class="form-group">
          <label class="form-label">Optional detail</label>
          <select class="form-input" id="chirpDetail">
            <option value="0">(none)</option>
            <option value="1">few vehicles</option>
            <option value="2">many vehicles</option>
            <option value="3">massive response</option>
            <option value="10">ongoing</option>
            <option value="11">contained</option>
            <option value="12">spreading</option>
          </select>
        </div>

        <div class="form-group">
          <label class="form-label">How urgent?</label>
          <div style="display:flex;gap:0.5rem;">
            <label style="flex:1;padding:0.5rem;background:rgba(99,179,237,0.15);border:1px solid var(--border);border-radius:6px;cursor:pointer;text-align:center;">
              <input type="radio" name="chirpUrgency" value="info" checked style="display:none;">
              <span style="color:#63b3ed;font-size:0.85rem;">Info</span>
            </label>
            <label style="flex:1;padding:0.5rem;background:rgba(244,185,66,0.15);border:1px solid var(--border);border-radius:6px;cursor:pointer;text-align:center;">
              <input type="radio" name="chirpUrgency" value="caution" style="display:none;">
              <span style="color:#f4b942;font-size:0.85rem;">Caution</span>
            </label>
            <label style="flex:1;padding:0.5rem;background:rgba(230,126,34,0.15);border:1px solid var(--border);border-radius:6px;cursor:pointer;text-align:center;">
              <input type="radio" name="chirpUrgency" value="urgent" style="display:none;">
              <span style="color:#e67e22;font-size:0.85rem;">Urgent</span>
            </label>
          </div>
        </div>

        <div id="chirpPreview" style="background:var(--surface);padding:0.75rem;border-radius:6px;margin-bottom:1rem;font-size:0.85rem;">
          <strong>Preview:</strong> <span id="chirpPreviewText">🚔 police activity in area</span>
        </div>

        <p style="font-size:0.75rem;color:var(--muted);margin-bottom:0.5rem;" id="chirpNearbyHint">
          This will notify approximately <strong id="chirpNearbyEstimate">0</strong> nearby devices.
        </p>
        <p style="font-size:0.7rem;color:var(--muted);margin-bottom:1rem;">
          ⚠️ Requires 2 neighbor confirmations before spreading. No free text — privacy by design.
        </p>

        <button class="btn btn-primary" id="chirpSendBtn" onclick="sendChirp()" style="width:100%;">
          Send Chirp
        </button>
        <p id="chirpCooldownHint" style="font-size:0.75rem;color:var(--warning);margin-top:0.5rem;display:none;text-align:center;">
          Please wait before sending another chirp
        </p>
        <p id="chirpPresenceHint" style="font-size:0.75rem;color:var(--muted);margin-top:0.5rem;display:none;text-align:center;">
          Must be active for 10 minutes before sending
        </p>
      </div>

      <!-- Recent Chirps Card -->
      <div class="card" id="chirpRecentCard">
        <div class="card-header">
          <div>
            <div class="card-title">Community Activity</div>
            <div class="card-subtitle">Recent alerts from your area</div>
          </div>
          <button class="btn btn-ghost btn-sm" onclick="refreshChirps()">Refresh</button>
        </div>
        <div id="chirpList" class="log-list">
          <div class="empty-state">
            <div class="empty-icon">🐦</div>
            <p>No community alerts</p>
          </div>
        </div>
      </div>

      <!-- Mute Controls Card -->
      <div class="card" id="chirpMuteCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Mute Controls</div>
            <div class="card-subtitle">Temporarily pause community alerts</div>
          </div>
        </div>
        <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
          <button class="btn btn-secondary" onclick="muteChirps(15)">Mute 15m</button>
          <button class="btn btn-secondary" onclick="muteChirps(30)">Mute 30m</button>
          <button class="btn btn-secondary" onclick="muteChirps(60)">Mute 1h</button>
          <button class="btn btn-secondary" onclick="muteChirps(120)">Mute 2h</button>
          <button class="btn btn-ghost" onclick="unmuteChirps()" id="chirpUnmuteBtn" style="display:none;">Unmute</button>
        </div>
        <p id="chirpMuteStatus" style="font-size:0.75rem;color:var(--muted);margin-top:0.5rem;"></p>
      </div>

      <!-- Chirp Settings Card -->
      <div class="card" id="chirpSettingsCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Chirp Settings</div>
            <div class="card-subtitle">Customize your experience</div>
          </div>
        </div>
        <div style="margin-bottom:1rem;">
          <label style="display:flex;align-items:center;gap:0.75rem;cursor:pointer;">
            <input type="checkbox" id="chirpRelayEnabled" checked onchange="updateChirpSettings()">
            <div>
              <strong style="font-size:0.85rem;">Relay others' chirps</strong>
              <p style="font-size:0.75rem;color:var(--muted);margin:0;">Help extend range by forwarding chirps</p>
            </div>
          </label>
        </div>
        <div>
          <label class="form-label">Minimum urgency to show</label>
          <select class="form-input" id="chirpUrgencyFilter" onchange="updateChirpSettings()">
            <option value="info">All (Info and above)</option>
            <option value="caution">Caution and above</option>
            <option value="urgent">Urgent only</option>
          </select>
        </div>
      </div>

      <!-- Philosophy Note -->
      <div class="card" style="background:linear-gradient(135deg,rgba(79,209,197,0.1) 0%,rgba(99,179,237,0.1) 100%);">
        <p style="font-size:0.8rem;color:var(--muted);margin:0;text-align:center;">
          <strong>Safety in numbers, not surveillance.</strong><br>
          No video. No tracking. No permanent records. Just neighbors helping neighbors.
        </p>
      </div>
    </div>

    <!-- Logs Panel -->
    <div class="panel" id="panel-logs">
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">System Logs</div>
            <div class="card-subtitle" id="logsSubtitle">All events and diagnostics</div>
          </div>
          <div style="display:flex;gap:0.5rem;">
            <button class="btn btn-secondary btn-sm" onclick="filterLogs('all')">All</button>
            <button class="btn btn-secondary btn-sm" onclick="filterLogs('unread')">Unread</button>
            <button class="btn btn-danger btn-sm" onclick="ackAllLogs()">Ack All</button>
          </div>
        </div>
        <div class="log-list" id="logList">
          <div class="loading"><div class="spinner"></div></div>
        </div>
      </div>
    </div>

    <!-- Witness Panel -->
    <div class="panel" id="panel-witness">
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Witness Records</div>
            <div class="card-subtitle" id="witnessSubtitle">Cryptographically signed events</div>
          </div>
          <button class="btn btn-primary btn-sm" onclick="exportWitness()">⬇ Export</button>
        </div>
        <div class="log-list" id="witnessList">
          <div class="loading"><div class="spinner"></div></div>
        </div>
      </div>
    </div>

    <!-- Settings Panel -->
    <div class="panel" id="panel-settings">
      <!-- WiFi Configuration Card -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">WiFi Configuration</div>
            <div class="card-subtitle" id="wifiSubtitle">Connect to your home network</div>
          </div>
          <div class="badge info" id="wifiBadge">
            <span class="badge-dot"></span>
            <span id="wifiState">Checking...</span>
          </div>
        </div>

        <!-- WiFi Status -->
        <div id="wifiStatusSection" style="margin-bottom:1rem;">
          <div class="stats-grid">
            <div class="stat-item">
              <div class="stat-label">Device AP</div>
              <div class="stat-value" style="font-size:0.9rem;" id="wifiApSsid">--</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">AP IP</div>
              <div class="stat-value" style="font-size:0.9rem;" id="wifiApIp">--</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Home WiFi</div>
              <div class="stat-value" style="font-size:0.9rem;" id="wifiStaSsid">Not configured</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Home IP</div>
              <div class="stat-value" style="font-size:0.9rem;" id="wifiStaIp">--</div>
            </div>
          </div>
          <div id="wifiRssiBar" style="margin-top:0.75rem;display:none;">
            <div class="stat-label">Signal Strength</div>
            <div style="display:flex;align-items:center;gap:0.5rem;">
              <div style="flex:1;height:8px;background:rgba(0,0,0,0.3);border-radius:4px;overflow:hidden;">
                <div id="wifiRssiLevel" style="height:100%;background:var(--success);width:0%;transition:width 0.3s;"></div>
              </div>
              <span id="wifiRssiValue" style="font-size:0.75rem;color:var(--muted);">-- dBm</span>
            </div>
          </div>
        </div>

        <!-- WiFi Setup Form (hidden when connected) -->
        <div id="wifiSetupSection">
          <div class="form-group">
            <label class="form-label">Home WiFi Network</label>
            <div style="display:flex;gap:0.5rem;">
              <select class="form-input" id="wifiSsidSelect" style="flex:1;">
                <option value="">-- Select network or type below --</option>
              </select>
              <button class="btn btn-secondary" onclick="scanWifi()" id="wifiScanBtn">Scan</button>
            </div>
            <input type="text" class="form-input" id="wifiSsidInput" placeholder="Or enter SSID manually" style="margin-top:0.5rem;">
          </div>
          <div class="form-group">
            <label class="form-label">Password</label>
            <div style="position:relative;">
              <input type="password" class="form-input" id="wifiPassword" placeholder="WiFi password">
              <button type="button" style="position:absolute;right:8px;top:50%;transform:translateY(-50%);background:none;border:none;color:var(--muted);cursor:pointer;font-size:0.9rem;" onclick="togglePasswordVisibility()">Show</button>
            </div>
          </div>
          <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
            <button class="btn btn-primary" onclick="connectWifi()" id="wifiConnectBtn">Connect</button>
            <button class="btn btn-secondary" onclick="disconnectWifi()" id="wifiDisconnectBtn" style="display:none;">Disconnect</button>
            <button class="btn btn-danger" onclick="forgetWifi()" id="wifiForgetBtn" style="display:none;">Forget Network</button>
          </div>
        </div>

        <!-- Connection Progress -->
        <div id="wifiProgress" style="display:none;margin-top:1rem;">
          <div style="display:flex;align-items:center;gap:0.75rem;">
            <div class="spinner"></div>
            <span id="wifiProgressText" style="color:var(--muted);">Connecting...</span>
          </div>
        </div>

        <!-- Help Text -->
        <div style="margin-top:1rem;padding:0.75rem;background:rgba(0,0,0,0.2);border-radius:8px;">
          <p style="font-size:0.8rem;color:var(--muted);margin:0;">
            <strong>Setup Guide:</strong> Connect your phone/computer to the device's AP network first.
            Then select your home WiFi and enter the password. The device will connect to both networks
            simultaneously so you can continue monitoring while connected to your home WiFi.
          </p>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Device Configuration</div>
            <div class="card-subtitle">Modify device settings</div>
          </div>
        </div>
        <div class="form-group">
          <label class="form-label">Record Interval (ms)</label>
          <input type="number" class="form-input" id="configRecordInterval" value="1000" min="100" max="60000">
        </div>
        <div class="form-group">
          <label class="form-label">Time Bucket (ms)</label>
          <input type="number" class="form-input" id="configTimeBucket" value="5000" min="1000" max="60000">
        </div>
        <div class="form-group">
          <label class="form-label">Log Level (min stored)</label>
          <select class="form-input" id="configLogLevel">
            <option value="0">Debug</option>
            <option value="1" selected>Info</option>
            <option value="2">Notice</option>
            <option value="3">Warning</option>
          </select>
        </div>
        <div style="display:flex;gap:0.5rem;margin-top:1rem;">
          <button class="btn btn-primary" onclick="saveConfig()">Save Configuration</button>
          <button class="btn btn-danger" onclick="confirmReboot()">Reboot Device</button>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Storage Management</div>
            <div class="card-subtitle">SD card and log maintenance</div>
          </div>
        </div>
        <div class="stats-grid">
          <div class="stat-item">
            <div class="stat-label">SD Total</div>
            <div class="stat-value"><span id="sdTotal">0</span><span class="stat-unit">MB</span></div>
          </div>
          <div class="stat-item">
            <div class="stat-label">SD Used</div>
            <div class="stat-value"><span id="sdUsed">0</span><span class="stat-unit">MB</span></div>
          </div>
        </div>
        <div style="margin-top:1rem;">
          <button class="btn btn-secondary" onclick="rotateOldLogs()">Rotate Old Logs (30+ days)</button>
        </div>
      </div>
    </div>

    <!-- Bluetooth Panel -->
    <div class="panel" id="panel-bluetooth">
      <!-- Status Card -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Bluetooth Status</div>
            <div class="card-subtitle" id="btSubtitle">BLE connectivity status</div>
          </div>
          <div class="badge info" id="btStateBadge">
            <span class="badge-dot"></span>
            <span id="btStateText">Loading...</span>
          </div>
        </div>
        <div class="stats-grid">
          <div class="stat-item">
            <div class="stat-label">State</div>
            <div class="stat-value" style="font-size:0.9rem;" id="btStateVal">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Device Name</div>
            <div class="stat-value" style="font-size:0.8rem;" id="btDeviceName">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Address</div>
            <div class="stat-value" style="font-size:0.7rem;" id="btLocalAddr">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">TX Power</div>
            <div class="stat-value" id="btTxPower">--<span class="stat-unit">dBm</span></div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Paired Devices</div>
            <div class="stat-value" id="btPairedCount">0</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Connections</div>
            <div class="stat-value" id="btTotalConns">0</div>
          </div>
        </div>
        <div style="display:flex;gap:0.5rem;flex-wrap:wrap;margin-top:1rem;">
          <label class="toggle-label" style="display:flex;align-items:center;gap:0.5rem;">
            <input type="checkbox" id="btEnabled" onchange="toggleBtEnabled()">
            <span>Bluetooth Enabled</span>
          </label>
        </div>
      </div>

      <!-- Connection Card -->
      <div class="card" id="btConnCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Current Connection</div>
            <div class="card-subtitle">Connected device info</div>
          </div>
          <button class="btn btn-danger btn-sm" onclick="btDisconnect()">Disconnect</button>
        </div>
        <div class="stats-grid">
          <div class="stat-item">
            <div class="stat-label">Device</div>
            <div class="stat-value" style="font-size:0.8rem;" id="btConnName">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Address</div>
            <div class="stat-value" style="font-size:0.7rem;" id="btConnAddr">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Security</div>
            <div class="stat-value" style="font-size:0.9rem;" id="btConnSecurity">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Connected</div>
            <div class="stat-value" id="btConnTime">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Sent</div>
            <div class="stat-value" id="btConnSent">0<span class="stat-unit">B</span></div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Received</div>
            <div class="stat-value" id="btConnRecv">0<span class="stat-unit">B</span></div>
          </div>
        </div>
      </div>

      <!-- Pairing Card -->
      <div class="card" id="btPairingCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Pairing In Progress</div>
            <div class="card-subtitle" id="btPairingSubtitle">Waiting for device...</div>
          </div>
        </div>
        <div id="btPairingContent">
          <div class="stat-item" style="text-align:center;padding:2rem;">
            <div class="stat-label">Pairing PIN</div>
            <div class="stat-value" style="font-size:2.5rem;letter-spacing:0.5rem;color:var(--accent);" id="btPairingPin">------</div>
            <p style="font-size:0.8rem;color:var(--muted);margin-top:1rem;">Enter this PIN on the connecting device</p>
          </div>
        </div>
        <div style="display:flex;gap:0.5rem;margin-top:1rem;">
          <button class="btn btn-secondary" onclick="btCancelPairing()">Cancel Pairing</button>
        </div>
      </div>

      <!-- Advertising & Scanning Controls -->
      <div class="card" id="btControlsCard">
        <div class="card-header">
          <div>
            <div class="card-title">Controls</div>
            <div class="card-subtitle">Advertising and scanning</div>
          </div>
        </div>
        <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
          <button class="btn btn-primary" id="btAdvBtn" onclick="toggleBtAdvertising()">Start Advertising</button>
          <button class="btn btn-secondary" id="btPairBtn" onclick="btStartPairing()">Start Pairing</button>
          <button class="btn btn-secondary" id="btScanBtn" onclick="btStartScan()">Scan for Devices</button>
        </div>
        <div style="margin-top:1rem;">
          <label class="toggle-label" style="display:flex;align-items:center;gap:0.5rem;">
            <input type="checkbox" id="btAutoAdv" onchange="saveBtSettings()">
            <span>Auto-advertise on boot</span>
          </label>
          <label class="toggle-label" style="display:flex;align-items:center;gap:0.5rem;margin-top:0.5rem;">
            <input type="checkbox" id="btAllowPairing" onchange="saveBtSettings()">
            <span>Allow new pairings</span>
          </label>
        </div>
      </div>

      <!-- Scan Results Card -->
      <div class="card" id="btScanCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Scan Results</div>
            <div class="card-subtitle" id="btScanSubtitle">Nearby BLE devices</div>
          </div>
          <div style="display:flex;gap:0.5rem;">
            <button class="btn btn-secondary btn-sm" onclick="btRefreshScan()">Refresh</button>
            <button class="btn btn-ghost btn-sm" onclick="btClearScan()">Clear</button>
          </div>
        </div>
        <div class="log-list" id="btScanList" style="max-height:300px;overflow-y:auto;">
          <div class="loading"><div class="spinner"></div></div>
        </div>
      </div>

      <!-- Paired Devices Card -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Paired Devices</div>
            <div class="card-subtitle" id="btPairedSubtitle">Trusted connections</div>
          </div>
          <button class="btn btn-danger btn-sm" onclick="btClearAllPaired()">Clear All</button>
        </div>
        <div class="log-list" id="btPairedList" style="max-height:300px;overflow-y:auto;">
          <p style="color:var(--muted);font-size:0.85rem;text-align:center;padding:1rem;">No paired devices</p>
        </div>
      </div>

      <!-- Settings Card -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Bluetooth Settings</div>
            <div class="card-subtitle">Configure BLE parameters</div>
          </div>
        </div>
        <div class="form-group">
          <label class="form-label">Device Name</label>
          <div style="display:flex;gap:0.5rem;">
            <input type="text" class="form-input" id="btNameInput" placeholder="SecuraCV-Canary" style="flex:1;">
            <button class="btn btn-secondary" onclick="btSetName()">Set</button>
          </div>
        </div>
        <div class="form-group">
          <label class="form-label">TX Power (dBm)</label>
          <div style="display:flex;gap:0.5rem;align-items:center;">
            <input type="range" id="btPowerSlider" min="-12" max="9" step="3" value="3" style="flex:1;" onchange="updatePowerDisplay()">
            <span id="btPowerDisplay" style="min-width:50px;text-align:right;">+3 dBm</span>
          </div>
          <button class="btn btn-secondary btn-sm" onclick="btSetPower()" style="margin-top:0.5rem;">Apply Power</button>
        </div>
        <div class="form-group">
          <label class="form-label">Inactivity Timeout</label>
          <select class="form-input" id="btTimeoutSelect" onchange="saveBtSettings()">
            <option value="0">Never disconnect</option>
            <option value="60">1 minute</option>
            <option value="300" selected>5 minutes</option>
            <option value="600">10 minutes</option>
            <option value="1800">30 minutes</option>
          </select>
        </div>
        <div style="margin-top:1rem;">
          <label class="toggle-label" style="display:flex;align-items:center;gap:0.5rem;">
            <input type="checkbox" id="btRequirePin" checked onchange="saveBtSettings()">
            <span>Require PIN for pairing</span>
          </label>
          <label class="toggle-label" style="display:flex;align-items:center;gap:0.5rem;margin-top:0.5rem;">
            <input type="checkbox" id="btNotifyConnect" checked onchange="saveBtSettings()">
            <span>Log connection events</span>
          </label>
        </div>
      </div>

      <!-- Info Card -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">About Bluetooth</div>
            <div class="card-subtitle">How BLE works on this device</div>
          </div>
        </div>
        <p style="font-size:0.85rem;color:var(--muted);line-height:1.6;">
          <strong>Bluetooth Low Energy (BLE)</strong> allows your phone or tablet to connect
          directly to this device for local monitoring and control. The device broadcasts its
          presence when advertising is enabled, and paired devices can reconnect automatically.
        </p>
        <p style="font-size:0.85rem;color:var(--muted);line-height:1.6;margin-top:0.75rem;">
          <strong>Security:</strong> All connections use encrypted communication. Pairing requires
          PIN confirmation to prevent unauthorized access. Paired devices are stored securely and
          can be managed from this panel.
        </p>
      </div>
    </div>
  </div>

  <!-- Acknowledgment Modal -->
  <div class="modal-overlay" id="ackModal">
    <div class="modal">
      <div class="modal-header">
        <div class="modal-title">Acknowledge Log Entry</div>
        <button class="modal-close" onclick="closeAckModal()">×</button>
      </div>
      <div class="modal-body">
        <p style="margin-bottom:1rem;color:var(--muted);font-size:0.85rem;">
          Acknowledging this entry will mark it as reviewed. The original entry is preserved in the append-only log.
        </p>
        <div class="form-group">
          <label class="form-label">Reason (optional)</label>
          <input type="text" class="form-input" id="ackReason" placeholder="e.g., Expected behavior, Fixed issue">
        </div>
      </div>
      <div class="modal-footer">
        <button class="btn btn-secondary" onclick="closeAckModal()">Cancel</button>
        <button class="btn btn-primary" onclick="submitAck()">Acknowledge</button>
      </div>
    </div>
  </div>

  <script>
    const API_BASE = '';
    let currentPanel = 'status';
    let pendingAckSeq = null;
    let logFilter = 'all';
    
    // Peek state
    let peekActive = false;
    let cameraReady = false;
    let currentResolution = 8; // VGA

    // Navigation
    document.querySelectorAll('.nav-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        const panel = btn.dataset.panel;
        switchPanel(panel);
      });
    });

    function switchPanel(panel) {
      document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
      document.querySelector(`[data-panel="${panel}"]`).classList.add('active');
      document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
      document.getElementById(`panel-${panel}`).classList.add('active');
      currentPanel = panel;

      if (panel === 'logs') loadLogs();
      else if (panel === 'witness') loadWitness();
      else if (panel === 'peek') refreshPeekStatus();
      else if (panel === 'opera') refreshOpera();
      else if (panel === 'community') refreshChirpStatus();
      else if (panel === 'bluetooth') { refreshBtStatus(); loadBtPairedDevices(); }

      // Stop peek stream when leaving peek panel
      if (panel !== 'peek' && peekActive) {
        stopPeek();
      }
    }

    // API calls
    async function api(endpoint, method = 'GET', body = null) {
      const opts = { method, headers: {} };
      if (body) {
        opts.headers['Content-Type'] = 'application/json';
        opts.body = JSON.stringify(body);
      }
      try {
        const res = await fetch(API_BASE + endpoint, opts);
        const text = await res.text();
        try {
          return JSON.parse(text);
        } catch (parseErr) {
          console.error('JSON parse error:', parseErr, 'Response:', text);
          // Truncate error to avoid huge HTML error pages in alerts
          const errMsg = res.ok ? 'Invalid response format' : (text || 'Request failed');
          return { ok: false, success: false, error: errMsg.length > 100 ? errMsg.slice(0, 100) + '...' : errMsg };
        }
      } catch (e) {
        console.error('API error:', e);
        return { ok: false, success: false, error: 'Network error' };
      }
    }

    // ══════════════════════════════════════════════════════════════════
    // PEEK (Camera Preview) — FIXED VERSION
    // ══════════════════════════════════════════════════════════════════
    
    async function refreshPeekStatus() {
      const data = await api('/api/peek/status');
      if (data.ok) {
        cameraReady = data.camera_initialized;
        peekActive = data.peek_active;
        if (typeof data.resolution !== 'undefined') {
          currentResolution = data.resolution;
          updateResolutionUI();
        }
        updatePeekUI();
      }
    }
    
    function updatePeekUI() {
      const btn = document.getElementById('peekToggle');
      const status = document.getElementById('peekStatus');
      const stream = document.getElementById('peekStream');
      const offline = document.getElementById('peekOffline');
      const offlineText = document.getElementById('peekOfflineText');
      
      if (!cameraReady) {
        btn.disabled = true;
        btn.textContent = '⚠ No Camera';
        status.textContent = 'Camera unavailable';
        offlineText.textContent = 'Camera not initialized';
        stream.style.display = 'none';
        offline.style.display = 'flex';
        return;
      }
      
      btn.disabled = false;
      
      if (peekActive) {
        btn.textContent = '⏹ Stop';
        btn.className = 'btn btn-danger btn-sm';
        status.textContent = 'Streaming...';
        stream.style.display = 'block';
        offline.style.display = 'none';
      } else {
        btn.textContent = '▶ Start';
        btn.className = 'btn btn-primary btn-sm';
        status.textContent = 'Ready';
        stream.style.display = 'none';
        offline.style.display = 'flex';
        offlineText.textContent = 'Click Start to preview';
      }
    }
    
    function togglePeek() {
      if (peekActive) {
        stopPeek();
      } else {
        startPeek();
      }
    }
    
    async function startPeek() {
      if (!cameraReady) {
        alert('Camera not available');
        return;
      }
      
      const stream = document.getElementById('peekStream');
      const offline = document.getElementById('peekOffline');
      const status = document.getElementById('peekStatus');
      const btn = document.getElementById('peekToggle');
      
      // Update UI immediately
      status.textContent = 'Connecting...';
      btn.disabled = true;
      
      // The stream endpoint now auto-activates peek_active on the server
      // Just set the src and the server will handle it
      stream.src = API_BASE + '/api/peek/stream?t=' + Date.now();
      
      stream.onloadstart = () => {
        console.log('Stream loading started');
        status.textContent = 'Loading...';
      };
      
      stream.onload = () => {
        console.log('Stream loaded');
        peekActive = true;
        btn.disabled = false;
        updatePeekUI();
      };
      
      stream.onerror = (e) => {
        // Only show error if stream was supposed to be active (not intentionally stopped)
        if (!peekActive) {
          console.log('Stream stopped (intentional)');
          return;
        }
        console.error('Stream error:', e);
        status.textContent = 'Stream error';
        btn.disabled = false;

        // Retry after delay
        setTimeout(() => {
          if (peekActive) {
            console.log('Retrying stream...');
            stream.src = API_BASE + '/api/peek/stream?t=' + Date.now();
          }
        }, 2000);
      };
      
      // Optimistically update UI
      peekActive = true;
      btn.disabled = false;
      updatePeekUI();
    }
    
    async function stopPeek() {
      const stream = document.getElementById('peekStream');

      // Set peekActive false BEFORE clearing src to prevent onerror from showing error
      peekActive = false;

      // Clear the stream source (this triggers onerror, but peekActive is false so it won't show error)
      stream.src = '';

      // Tell server to stop
      await api('/api/peek/stop', 'POST');

      updatePeekUI();
    }
    
    async function takeSnapshot() {
      if (!cameraReady) {
        alert('Camera not available');
        return;
      }
      
      const preview = document.getElementById('snapshotPreview');
      const img = document.getElementById('snapshotImg');
      
      // Fetch snapshot
      img.src = API_BASE + '/api/peek/snapshot?t=' + Date.now();
      img.onload = () => {
        preview.style.display = 'block';
      };
      img.onerror = () => {
        alert('Failed to capture snapshot');
        preview.style.display = 'none';
      };
    }
    
    // ══════════════════════════════════════════════════════════════════
    // RESOLUTION CONTROL — NEW
    // ══════════════════════════════════════════════════════════════════
    
    function updateResolutionUI() {
      const buttons = document.querySelectorAll('.resolution-btn');
      buttons.forEach(btn => {
        const size = parseInt(btn.dataset.size);
        if (size === currentResolution) {
          btn.classList.add('active');
        } else {
          btn.classList.remove('active');
        }
      });
      
      const names = {
        4: '320×240 (QVGA)',
        8: '640×480 (VGA)',
        9: '800×600 (SVGA)',
        10: '1024×768 (XGA)',
        11: '1280×720 (HD)'
      };
      document.getElementById('resolutionStatus').textContent = 
        'Current: ' + (names[currentResolution] || 'Unknown');
    }
    
    async function setResolution(size) {
      const data = await api('/api/peek/resolution', 'POST', { size: size });
      
      if (data.ok) {
        currentResolution = size;
        updateResolutionUI();
        
        // If stream is active, restart it to get new resolution
        if (peekActive) {
          const stream = document.getElementById('peekStream');
          stream.src = API_BASE + '/api/peek/stream?t=' + Date.now();
        }
      } else {
        alert('Failed to set resolution: ' + (data.error || 'Unknown error'));
      }
    }

    // Status updates
    async function refreshStatus() {
      const data = await api('/api/status');
      if (!data.ok) return;
      
      document.getElementById('deviceId').textContent = data.device_id;
      document.getElementById('uptime').textContent = formatUptime(data.uptime_sec);
      document.getElementById('witnessCount').textContent = data.witness_count;
      document.getElementById('chainSeq').textContent = data.chain_seq;
      document.getElementById('bootCount').textContent = data.boot_count;
      document.getElementById('freeHeap').textContent = Math.round(data.free_heap / 1024);
      document.getElementById('fingerprint').textContent = data.fingerprint || '--';
      document.getElementById('pubkey').textContent = truncateHash(data.pubkey, 16);
      document.getElementById('firmware').textContent = data.firmware || '2.0.1';
      
      if (data.gps) updateGps(data.gps);
      updateBadges(data);
      
      // Logs count
      const unacked = data.unacked_count || 0;
      const logsCount = document.getElementById('logsCount');
      if (unacked > 0) {
        logsCount.textContent = unacked;
        logsCount.style.display = 'inline-flex';
      } else {
        logsCount.style.display = 'none';
      }
      
      // SD info
      document.getElementById('sdFree').textContent = Math.round((data.sd_free || 0) / (1024 * 1024));
      document.getElementById('sdTotal').textContent = Math.round((data.sd_total || 0) / (1024 * 1024));
      document.getElementById('sdUsed').textContent = Math.round((data.sd_used || 0) / (1024 * 1024));
      
      // Camera status (for peek panel)
      if (typeof data.camera_ready !== 'undefined') {
        cameraReady = data.camera_ready;
      }
      if (typeof data.peek_active !== 'undefined' && !peekActive) {
        // Only update if we think we're not active (avoid race)
        peekActive = data.peek_active;
      }
      if (typeof data.peek_resolution !== 'undefined') {
        currentResolution = data.peek_resolution;
      }
    }

    function updateGps(gps) {
      const hasFix = gps.valid && gps.quality > 0;
      document.getElementById('gpsStatus').textContent = hasFix ? 
        (gps.satellites + ' Sats') : 'No Fix';
      document.getElementById('gpsSubtitle').textContent = hasFix ?
        `Fix: ${gps.fix_mode || '?'}D, Quality: ${gps.quality || 0}` : 'Waiting for fix...';
      
      const gpsBadge = document.getElementById('gpsBadge');
      gpsBadge.className = 'badge ' + (hasFix ? 'success' : 'warning');
      
      document.getElementById('gpsLat').textContent = gps.lat?.toFixed(6) || '--';
      document.getElementById('gpsLon').textContent = gps.lon?.toFixed(6) || '--';
      document.getElementById('gpsAlt').textContent = gps.alt ? gps.alt.toFixed(1) + ' m' : '--';
      document.getElementById('gpsSpeed').textContent = gps.speed ? gps.speed.toFixed(1) + ' m/s' : '--';
      document.getElementById('gpsSats').textContent = gps.satellites || '--';
      document.getElementById('gpsHdop').textContent = gps.hdop?.toFixed(1) || '--';
    }

    function updateBadges(data) {
      const chainBadge = document.getElementById('chainBadge');
      chainBadge.className = 'badge ' + (data.crypto_healthy ? 'success' : 'danger');
      
      const sdBadge = document.getElementById('sdBadge');
      sdBadge.className = 'badge ' + (data.sd_mounted ? 'success' : 'danger');
      sdBadge.querySelector('span:last-child').textContent = data.sd_mounted ? 'SD OK' : 'SD ERR';
      
      // Camera badge
      const cameraBadge = document.getElementById('cameraBadge');
      const camReady = data.camera_ready;
      const camActive = data.peek_active;
      if (camReady) {
        cameraBadge.className = 'badge ' + (camActive ? 'warning' : 'success');
        document.getElementById('cameraStatus').textContent = camActive ? 'LIVE' : 'CAM';
      } else {
        cameraBadge.className = 'badge danger';
        document.getElementById('cameraStatus').textContent = 'NO CAM';
      }
    }

    // Chain visualization
    async function loadChain() {
      const data = await api('/api/chain');
      const viz = document.getElementById('chainViz');
      
      if (!data.ok || !data.blocks?.length) {
        viz.innerHTML = '<div class="empty-state"><div class="empty-icon">⛓</div><p>No chain data</p></div>';
        return;
      }
      
      viz.innerHTML = data.blocks.slice(-5).map((b, i, arr) => `
        <div class="chain-block">
          <div class="chain-hash">${truncateHash(b.hash, 8)}</div>
          <div class="chain-seq">#${b.seq}</div>
        </div>
        ${i < arr.length - 1 ? '<div class="chain-arrow">→</div>' : ''}
      `).join('');
    }

    // Logs
    async function loadLogs() {
      const filter = logFilter === 'unread' ? '?unacked=true' : '';
      const data = await api('/api/logs' + filter);
      const list = document.getElementById('logList');
      
      if (!data.ok || !data.logs?.length) {
        list.innerHTML = '<div class="empty-state"><div class="empty-icon">📋</div><p>No log entries</p></div>';
        return;
      }
      
      document.getElementById('logsSubtitle').textContent = 
        `${data.logs.length} entries${logFilter === 'unread' ? ' (unread only)' : ''}`;
      
      list.innerHTML = data.logs.map(log => `
        <div class="log-item ${log.ack_status === 'unread' ? 'unread' : ''} ${log.level >= 4 ? 'error' : ''} ${log.level >= 5 ? 'critical' : ''}">
          <div class="log-level ${getLevelClass(log.level)}">${log.level_name}</div>
          <div class="log-content">
            <div class="log-message">${escapeHtml(log.message)}</div>
            ${log.detail ? `<div class="log-detail">${escapeHtml(log.detail)}</div>` : ''}
            <div class="log-meta">${log.category} · ${formatTimestamp(log.timestamp_ms)} · #${log.seq}</div>
          </div>
          <div class="log-actions">
            ${log.ack_status !== 'acknowledged' ? 
              `<button class="btn btn-ghost btn-sm" onclick="openAckModal(${log.seq})">✓ Ack</button>` : 
              '<span style="color:var(--success);font-size:0.7rem;">✓</span>'}
          </div>
        </div>
      `).join('');
    }

    function filterLogs(filter) {
      logFilter = filter;
      loadLogs();
    }

    function getLevelClass(level) {
      if (level <= 1) return 'info';
      if (level <= 2) return 'info';
      if (level === 3) return 'warning';
      if (level === 4) return 'error';
      return 'critical';
    }

    // Witness records
    async function loadWitness() {
      const data = await api('/api/witness');
      const list = document.getElementById('witnessList');
      
      if (!data.ok || !data.records?.length) {
        list.innerHTML = '<div class="empty-state"><div class="empty-icon">🔐</div><p>No witness records</p></div>';
        return;
      }
      
      document.getElementById('witnessSubtitle').textContent = `${data.records.length} records`;
      
      list.innerHTML = data.records.slice(-50).reverse().map(r => `
        <div class="witness-item">
          <div>
            <div class="witness-seq">#${r.seq}</div>
            <div class="witness-type">${r.type_name}</div>
          </div>
          <div>
            <div class="witness-hash">Chain: ${truncateHash(r.chain_hash, 16)}</div>
            <div class="log-meta">TB: ${r.time_bucket} · ${r.payload_len} bytes</div>
          </div>
          <div class="witness-verified">${r.verified ? '✓ Verified' : '⚠ Unverified'}</div>
        </div>
      `).join('');
    }

    async function exportWitness() {
      const data = await api('/api/export', 'POST');
      if (data.ok && data.download_url) {
        window.location.href = data.download_url;
      } else {
        alert('Export failed: ' + (data.error || 'Unknown error'));
      }
    }

    // Acknowledgment
    function openAckModal(seq) {
      pendingAckSeq = seq;
      document.getElementById('ackReason').value = '';
      document.getElementById('ackModal').classList.add('active');
    }

    function closeAckModal() {
      pendingAckSeq = null;
      document.getElementById('ackModal').classList.remove('active');
    }

    async function submitAck() {
      if (pendingAckSeq === null) return;
      const reason = document.getElementById('ackReason').value;
      const data = await api(`/api/logs/${pendingAckSeq}/ack`, 'POST', { reason });
      closeAckModal();
      if (data.ok) {
        loadLogs();
        refreshStatus();
      } else {
        alert('Acknowledgment failed: ' + (data.error || 'Unknown error'));
      }
    }

    async function ackAllLogs() {
      if (!confirm('Acknowledge all unread log entries?')) return;
      const data = await api('/api/logs/ack-all', 'POST', { level: 3 });
      if (data.ok) {
        loadLogs();
        refreshStatus();
      }
    }

    // Settings
    async function saveConfig() {
      const config = {
        record_interval_ms: parseInt(document.getElementById('configRecordInterval').value),
        time_bucket_ms: parseInt(document.getElementById('configTimeBucket').value),
        log_level: parseInt(document.getElementById('configLogLevel').value)
      };
      const data = await api('/api/config', 'POST', config);
      alert(data.ok ? 'Configuration saved!' : 'Save failed: ' + (data.error || 'Unknown'));
    }

    function confirmReboot() {
      if (confirm('Reboot the device? All unsaved data will be persisted first.')) {
        api('/api/reboot', 'POST');
        alert('Device is rebooting. Please wait 10 seconds and refresh.');
      }
    }

    async function rotateOldLogs() {
      if (!confirm('Delete logs older than 30 days?')) return;
      const data = await api('/api/logs/rotate', 'POST', { max_age_days: 30 });
      alert(data.ok ? `Rotated ${data.deleted_count || 0} entries` : 'Rotation failed');
    }

    // Utilities
    function formatUptime(sec) {
      const h = Math.floor(sec / 3600);
      const m = Math.floor((sec % 3600) / 60);
      const s = sec % 60;
      return `${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}:${s.toString().padStart(2,'0')}`;
    }

    function formatTimestamp(ms) {
      if (!ms) return '--';
      const d = new Date(ms);
      return d.toLocaleTimeString();
    }

    function truncateHash(hash, len) {
      if (!hash || hash.length <= len) return hash || '--';
      return hash.substring(0, len) + '...';
    }

    function escapeHtml(str) {
      const div = document.createElement('div');
      div.textContent = str || '';
      return div.innerHTML;
    }

    // ══════════════════════════════════════════════════════════════════
    // FLOCK (MESH NETWORK)
    // ══════════════════════════════════════════════════════════════════

    let operaState = null;
    let pairingPollingInterval = null;

    async function refreshOpera() {
      const data = await api('/api/mesh');
      if (!data.ok) return;

      operaState = data;

      // Update stats
      document.getElementById('operaStatus').textContent = data.state || 'DISABLED';
      document.getElementById('operaId').textContent = data.opera_id ? data.opera_id.substring(0, 16) + '...' : '--';
      document.getElementById('peersOnline').textContent = data.peers_online || 0;
      document.getElementById('peersTotal').textContent = data.peers_total || 0;
      document.getElementById('alertsReceived').textContent = data.alerts_received || 0;

      // Update badge
      const badge = document.getElementById('operaBadge');
      const stateText = document.getElementById('operaState');

      if (data.state === 'ACTIVE') {
        badge.className = 'badge success';
        stateText.textContent = 'Active';
        document.getElementById('operaSubtitle').textContent = data.peers_online + ' peer(s) online';
      } else if (data.state === 'CONNECTING') {
        badge.className = 'badge warning';
        stateText.textContent = 'Connecting';
        document.getElementById('operaSubtitle').textContent = 'Searching for opera members...';
      } else if (data.state === 'NO_FLOCK') {
        badge.className = 'badge info';
        stateText.textContent = 'No Opera';
        document.getElementById('operaSubtitle').textContent = 'Create or join an opera to get started';
      } else if (data.state === 'DISABLED') {
        badge.className = 'badge info';
        stateText.textContent = 'Disabled';
        document.getElementById('operaSubtitle').textContent = 'Mesh networking is disabled';
      } else if (data.state && data.state.startsWith('PAIRING')) {
        badge.className = 'badge warning';
        stateText.textContent = 'Pairing';
        document.getElementById('operaSubtitle').textContent = 'Pairing in progress...';
      } else {
        badge.className = 'badge info';
        stateText.textContent = data.state || 'Unknown';
      }

      // Update enabled checkbox
      document.getElementById('meshEnabled').checked = data.enabled !== false;

      // Show/hide opera states
      const hasOpera = data.has_opera || (data.peers_total > 0);
      const isPairing = data.state && data.state.startsWith('PAIRING');

      document.getElementById('operaNoOpera').style.display = (!hasOpera && !isPairing) ? 'block' : 'none';
      document.getElementById('operaHasOpera').style.display = (hasOpera && !isPairing) ? 'block' : 'none';
      document.getElementById('operaPairing').style.display = isPairing ? 'block' : 'none';

      if (hasOpera && data.opera_name) {
        document.getElementById('operaNameInput').value = data.opera_name;
      }

      // Load peers
      if (hasOpera) {
        loadPeers();
      }

      // Load alerts
      loadOperaAlerts();

      // Alert count badge in nav
      const alertCount = document.getElementById('operaAlertCount');
      const unreadAlerts = data.alerts_received || 0;
      if (unreadAlerts > 0) {
        alertCount.textContent = unreadAlerts;
        alertCount.style.display = 'inline-flex';
      } else {
        alertCount.style.display = 'none';
      }
    }

    async function loadPeers() {
      const data = await api('/api/mesh/peers');
      const list = document.getElementById('peersList');

      if (!data.ok || !data.peers || data.peers.length === 0) {
        list.innerHTML = '<div class="empty-state"><div class="empty-icon">🐦</div><p>No peers in opera</p></div>';
        return;
      }

      document.getElementById('peersSubtitle').textContent = data.peers.length + ' device(s) in opera';

      list.innerHTML = data.peers.map(peer => {
        const stateClass = peer.state === 'CONNECTED' ? 'success' :
                          peer.state === 'STALE' ? 'warning' :
                          peer.state === 'ALERT' ? 'danger' : 'info';
        const stateIcon = peer.state === 'CONNECTED' ? '🟢' :
                         peer.state === 'STALE' ? '🟡' :
                         peer.state === 'ALERT' ? '🔴' :
                         peer.state === 'OFFLINE' ? '⚫' : '⚪';

        return `
          <div class="log-item ${peer.state === 'ALERT' ? 'critical' : ''}">
            <div style="font-size:1.5rem;">${stateIcon}</div>
            <div class="log-content">
              <div class="log-message">${escapeHtml(peer.name || 'Unknown Device')}</div>
              <div class="log-detail">FP: ${peer.fingerprint || '--'}</div>
              <div class="log-meta">${peer.state} · ${peer.rssi ? peer.rssi + ' dBm' : '--'} · ${peer.last_seen_sec ? peer.last_seen_sec + 's ago' : 'never'}</div>
            </div>
            <div class="log-actions">
              <button class="btn btn-ghost btn-sm" onclick="removePeer('${peer.fingerprint}')" title="Remove from opera">✕</button>
            </div>
          </div>
        `;
      }).join('');
    }

    async function loadOperaAlerts() {
      const data = await api('/api/mesh/alerts');
      const list = document.getElementById('operaAlertsList');

      if (!data.ok || !data.alerts || data.alerts.length === 0) {
        list.innerHTML = '<div class="empty-state"><div class="empty-icon">✓</div><p>No alerts from opera</p></div>';
        return;
      }

      list.innerHTML = data.alerts.map(alert => {
        const levelClass = alert.severity >= 6 ? 'critical' : alert.severity >= 4 ? 'error' : 'warning';
        return `
          <div class="log-item ${levelClass}">
            <div class="log-level ${levelClass}">${alert.type || 'ALERT'}</div>
            <div class="log-content">
              <div class="log-message">From: ${escapeHtml(alert.sender_name || 'Unknown')}</div>
              <div class="log-detail">${escapeHtml(alert.detail || '')}</div>
              <div class="log-meta">${formatTimestamp(alert.timestamp_ms)}</div>
            </div>
          </div>
        `;
      }).join('');
    }

    async function startPairing(mode) {
      const endpoint = mode === 'init' ? '/api/mesh/pair/start' : '/api/mesh/pair/join';
      const data = await api(endpoint, 'POST');

      if (!data.ok) {
        alert('Failed to start pairing: ' + (data.error || 'Unknown error'));
        return;
      }

      document.getElementById('operaNoOpera').style.display = 'none';
      document.getElementById('operaHasOpera').style.display = 'none';
      document.getElementById('operaPairing').style.display = 'block';
      document.getElementById('pairingStatus').textContent = mode === 'init' ?
        'Waiting for another device to join...' : 'Searching for opera to join...';
      document.getElementById('pairingCode').style.display = 'none';
      document.getElementById('pairingConfirmBtn').style.display = 'none';

      // Start polling for pairing status
      startPairingPolling();
    }

    function startPairingPolling() {
      if (pairingPollingInterval) clearInterval(pairingPollingInterval);

      pairingPollingInterval = setInterval(async () => {
        const data = await api('/api/mesh');
        if (!data.ok) return;

        if (data.state === 'PAIRING_CONFIRM' && data.pairing_code) {
          // Show confirmation code
          document.getElementById('pairingStatus').textContent = 'Verify the code matches on both devices:';
          document.getElementById('pairingCodeValue').textContent = String(data.pairing_code).padStart(6, '0');
          document.getElementById('pairingCode').style.display = 'block';
          document.getElementById('pairingConfirmBtn').style.display = 'inline-flex';
        } else if (data.state === 'ACTIVE' || data.state === 'CONNECTING') {
          // Pairing complete
          stopPairingPolling();
          refreshOpera();
          if (data.state === 'ACTIVE') {
            alert('Successfully joined opera!');
          }
        } else if (data.state === 'NO_FLOCK' || data.state === 'DISABLED') {
          // Pairing cancelled or failed
          stopPairingPolling();
          refreshOpera();
        }
      }, 1000);
    }

    function stopPairingPolling() {
      if (pairingPollingInterval) {
        clearInterval(pairingPollingInterval);
        pairingPollingInterval = null;
      }
    }

    async function confirmPairing() {
      const data = await api('/api/mesh/pair/confirm', 'POST');
      if (!data.ok) {
        alert('Pairing confirmation failed: ' + (data.error || 'Unknown error'));
      }
      document.getElementById('pairingStatus').textContent = 'Completing pairing...';
      document.getElementById('pairingConfirmBtn').style.display = 'none';
    }

    async function cancelPairing() {
      stopPairingPolling();
      await api('/api/mesh/pair/cancel', 'POST');
      refreshOpera();
    }

    async function saveOperaName() {
      const name = document.getElementById('operaNameInput').value.trim();
      if (!name) {
        alert('Please enter an opera name');
        return;
      }
      const data = await api('/api/mesh/name', 'POST', { name });
      if (data.ok) {
        refreshOpera();
      } else {
        alert('Failed to save name: ' + (data.error || 'Unknown error'));
      }
    }

    async function leaveOpera() {
      if (!confirm('Leave this opera? You will need to re-pair to rejoin.')) return;

      const data = await api('/api/mesh/leave', 'POST');
      if (data.ok) {
        refreshOpera();
      } else {
        alert('Failed to leave opera: ' + (data.error || 'Unknown error'));
      }
    }

    async function removePeer(fingerprint) {
      if (!confirm('Remove this device from the opera?')) return;

      const data = await api('/api/mesh/remove', 'POST', { fingerprint });
      if (data.ok) {
        loadPeers();
      } else {
        alert('Failed to remove peer: ' + (data.error || 'Unknown error'));
      }
    }

    async function toggleMeshEnabled() {
      const enabled = document.getElementById('meshEnabled').checked;
      const data = await api('/api/mesh/enable', 'POST', { enabled });
      if (!data.ok) {
        alert('Failed to toggle mesh: ' + (data.error || 'Unknown error'));
        document.getElementById('meshEnabled').checked = !enabled;
      }
      refreshOpera();
    }

    async function clearOperaAlerts() {
      await api('/api/mesh/alerts', 'DELETE');
      loadOperaAlerts();
    }

    // ══════════════════════════════════════════════════════════════════
    // WIFI PROVISIONING
    // ══════════════════════════════════════════════════════════════════

    let wifiState = null;
    let wifiPollingInterval = null;

    async function loadWifiStatus() {
      const data = await api('/api/wifi');
      if (!data.ok) return;

      wifiState = data;

      // Update UI elements
      document.getElementById('wifiApSsid').textContent = data.ap_ssid || '--';
      document.getElementById('wifiApIp').textContent = data.ap_ip || '--';
      document.getElementById('wifiStaSsid').textContent = data.configured ? data.sta_ssid : 'Not configured';
      document.getElementById('wifiStaIp').textContent = data.sta_connected ? data.sta_ip : '--';

      // Update badge
      const badge = document.getElementById('wifiBadge');
      const state = document.getElementById('wifiState');

      if (data.sta_connected) {
        badge.className = 'badge success';
        state.textContent = 'Connected';
        document.getElementById('wifiSubtitle').textContent = 'Connected to home network';
      } else if (data.state === 'connecting') {
        badge.className = 'badge warning';
        state.textContent = 'Connecting...';
        document.getElementById('wifiSubtitle').textContent = 'Attempting to connect...';
      } else if (data.state === 'failed') {
        badge.className = 'badge danger';
        state.textContent = 'Failed';
        document.getElementById('wifiSubtitle').textContent = 'Connection failed - check credentials';
      } else if (data.configured) {
        badge.className = 'badge info';
        state.textContent = 'Disconnected';
        document.getElementById('wifiSubtitle').textContent = 'Home WiFi configured but not connected';
      } else {
        badge.className = 'badge info';
        state.textContent = 'AP Only';
        document.getElementById('wifiSubtitle').textContent = 'Connect to your home network';
      }

      // RSSI bar
      const rssiBar = document.getElementById('wifiRssiBar');
      if (data.sta_connected && data.rssi) {
        rssiBar.style.display = 'block';
        // RSSI typically ranges from -30 (excellent) to -90 (poor)
        const rssiPercent = Math.max(0, Math.min(100, (data.rssi + 90) * 1.67));
        document.getElementById('wifiRssiLevel').style.width = rssiPercent + '%';
        document.getElementById('wifiRssiLevel').style.background =
          rssiPercent > 60 ? 'var(--success)' : rssiPercent > 30 ? 'var(--warning)' : 'var(--danger)';
        document.getElementById('wifiRssiValue').textContent = data.rssi + ' dBm';
      } else {
        rssiBar.style.display = 'none';
      }

      // Show/hide buttons
      document.getElementById('wifiConnectBtn').style.display = data.sta_connected ? 'none' : 'inline-flex';
      document.getElementById('wifiDisconnectBtn').style.display = data.sta_connected ? 'inline-flex' : 'none';
      document.getElementById('wifiForgetBtn').style.display = data.configured ? 'inline-flex' : 'none';

      // Show progress if connecting
      document.getElementById('wifiProgress').style.display = data.state === 'connecting' ? 'block' : 'none';
    }

    async function scanWifi() {
      const btn = document.getElementById('wifiScanBtn');
      const select = document.getElementById('wifiSsidSelect');

      btn.disabled = true;
      btn.textContent = 'Scanning...';
      select.innerHTML = '<option value="">Scanning...</option>';

      // Poll for async scan completion (non-blocking on device)
      let data;
      let attempts = 0;
      const maxAttempts = 20;  // Max 10 seconds (500ms * 20)

      while (attempts < maxAttempts) {
        data = await api('/api/wifi/scan');
        if (!data.ok || !data.scanning) break;
        await new Promise(r => setTimeout(r, 500));
        attempts++;
      }

      btn.disabled = false;
      btn.textContent = 'Scan';

      if (!data.ok) {
        select.innerHTML = '<option value="">Scan failed - try again</option>';
        return;
      }

      if (data.scanning) {
        select.innerHTML = '<option value="">Scan timed out - try again</option>';
        return;
      }

      select.innerHTML = '<option value="">-- Select network --</option>';

      if (data.networks && data.networks.length > 0) {
        // Sort by signal strength
        data.networks.sort((a, b) => b.rssi - a.rssi);

        for (const net of data.networks) {
          if (!net.ssid) continue;
          const signal = net.rssi > -50 ? '████' : net.rssi > -60 ? '███░' : net.rssi > -70 ? '██░░' : '█░░░';
          const opt = document.createElement('option');
          opt.value = net.ssid;
          opt.textContent = `${net.ssid} (${signal} ${net.security})`;
          select.appendChild(opt);
        }
      } else {
        select.innerHTML = '<option value="">No networks found</option>';
      }
    }

    function togglePasswordVisibility() {
      const input = document.getElementById('wifiPassword');
      const btn = event.target;
      if (input.type === 'password') {
        input.type = 'text';
        btn.textContent = 'Hide';
      } else {
        input.type = 'password';
        btn.textContent = 'Show';
      }
    }

    async function connectWifi() {
      const selectSsid = document.getElementById('wifiSsidSelect').value;
      const inputSsid = document.getElementById('wifiSsidInput').value.trim();
      const ssid = inputSsid || selectSsid;
      const password = document.getElementById('wifiPassword').value;

      if (!ssid) {
        alert('Please select or enter a WiFi network name');
        return;
      }

      document.getElementById('wifiProgress').style.display = 'block';
      document.getElementById('wifiProgressText').textContent = 'Saving credentials and connecting...';
      document.getElementById('wifiConnectBtn').disabled = true;

      const data = await api('/api/wifi/connect', 'POST', { ssid, password });

      document.getElementById('wifiConnectBtn').disabled = false;

      if (!data.ok) {
        document.getElementById('wifiProgress').style.display = 'none';
        alert('Failed to save credentials: ' + (data.error || 'Unknown error'));
        return;
      }

      // Start polling for connection status
      document.getElementById('wifiProgressText').textContent = 'Connecting to ' + ssid + '...';
      startWifiPolling();
    }

    async function disconnectWifi() {
      if (!confirm('Disconnect from home WiFi? The AP will remain active.')) return;

      const data = await api('/api/wifi/disconnect', 'POST');
      if (data.ok) {
        loadWifiStatus();
      } else {
        alert('Failed to disconnect: ' + (data.error || 'Unknown error'));
      }
    }

    async function forgetWifi() {
      if (!confirm('Forget saved WiFi credentials? You will need to re-enter them to reconnect.')) return;

      const data = await api('/api/wifi/forget', 'POST');
      if (data.ok) {
        document.getElementById('wifiSsidInput').value = '';
        document.getElementById('wifiPassword').value = '';
        loadWifiStatus();
      } else {
        alert('Failed to forget credentials: ' + (data.error || 'Unknown error'));
      }
    }

    function startWifiPolling() {
      if (wifiPollingInterval) clearInterval(wifiPollingInterval);

      let pollCount = 0;
      wifiPollingInterval = setInterval(async () => {
        await loadWifiStatus();
        pollCount++;

        // Check if connected or failed
        if (wifiState) {
          if (wifiState.sta_connected) {
            stopWifiPolling();
            document.getElementById('wifiProgress').style.display = 'none';
            alert('Successfully connected to ' + wifiState.sta_ssid + '!\n\nIP: ' + wifiState.sta_ip);
          } else if (wifiState.state === 'failed' || pollCount > 20) {
            stopWifiPolling();
            document.getElementById('wifiProgress').style.display = 'none';
            if (pollCount > 20) {
              alert('Connection timeout. Please check your credentials and try again.');
            }
          }
        }
      }, 1000);
    }

    function stopWifiPolling() {
      if (wifiPollingInterval) {
        clearInterval(wifiPollingInterval);
        wifiPollingInterval = null;
      }
    }

    // SSID select -> input sync
    document.getElementById('wifiSsidSelect').addEventListener('change', function() {
      if (this.value) {
        document.getElementById('wifiSsidInput').value = this.value;
      }
    });

    // ══════════════════════════════════════════════════════════════════
    // CHIRP CHANNEL (Community Witness Network)
    // ══════════════════════════════════════════════════════════════════

    let chirpState = null;

    async function refreshChirpStatus() {
      const data = await api('/api/chirp');
      if (!data.state) return;

      chirpState = data;

      // Update stats
      document.getElementById('chirpSessionEmoji').textContent = data.session_emoji || '--';
      document.getElementById('chirpNearbyCount').textContent = data.nearby_count || 0;
      document.getElementById('chirpRecentCount').textContent = data.recent_chirps || 0;
      document.getElementById('chirpNearbyEstimate').textContent = data.nearby_count || 0;

      // Cooldown display with tier info
      const cooldownEl = document.getElementById('chirpCooldown');
      const presenceHint = document.getElementById('chirpPresenceHint');
      const cooldownHint = document.getElementById('chirpCooldownHint');

      // Check if presence requirement is met
      if (!data.presence_met) {
        cooldownEl.textContent = 'Warming up...';
        document.getElementById('chirpSendBtn').disabled = true;
        presenceHint.style.display = 'block';
        cooldownHint.style.display = 'none';
      } else if (data.cooldown_remaining_sec > 0) {
        const mins = Math.floor(data.cooldown_remaining_sec / 60);
        const secs = data.cooldown_remaining_sec % 60;
        cooldownEl.textContent = `${mins}:${secs.toString().padStart(2, '0')} (tier ${data.cooldown_tier || 1})`;
        document.getElementById('chirpSendBtn').disabled = true;
        cooldownHint.style.display = 'block';
        presenceHint.style.display = 'none';
      } else {
        cooldownEl.textContent = data.cooldown_tier > 0 ? `Ready (tier ${data.cooldown_tier})` : 'Ready';
        document.getElementById('chirpSendBtn').disabled = false;
        cooldownHint.style.display = 'none';
        presenceHint.style.display = 'none';
      }

      // Update badge
      const badge = document.getElementById('chirpBadge');
      const stateText = document.getElementById('chirpState');
      const enabled = data.state !== 'disabled';

      document.getElementById('chirpEnabled').checked = enabled;

      if (data.state === 'active') {
        badge.className = 'badge success';
        stateText.textContent = 'Active';
        document.getElementById('chirpSubtitle').textContent =
          `${data.nearby_count} device(s) nearby`;
      } else if (data.state === 'muted') {
        badge.className = 'badge warning';
        stateText.textContent = 'Muted';
        document.getElementById('chirpSubtitle').textContent =
          `Muted for ${Math.ceil(data.mute_remaining_sec / 60)} min`;
      } else if (data.state === 'cooldown') {
        badge.className = 'badge info';
        stateText.textContent = 'Cooldown';
        document.getElementById('chirpSubtitle').textContent =
          'Please wait before sending again';
      } else if (data.state === 'listening') {
        badge.className = 'badge info';
        stateText.textContent = 'Listening';
        document.getElementById('chirpSubtitle').textContent =
          'Receiving community alerts';
      } else {
        badge.className = 'badge info';
        stateText.textContent = 'Disabled';
        document.getElementById('chirpSubtitle').textContent =
          'Enable to join community network';
      }

      // Show/hide cards based on state
      document.getElementById('chirpSendCard').style.display = enabled ? 'block' : 'none';
      document.getElementById('chirpMuteCard').style.display = enabled ? 'block' : 'none';
      document.getElementById('chirpSettingsCard').style.display = enabled ? 'block' : 'none';

      // Mute button state
      if (data.muted) {
        document.getElementById('chirpUnmuteBtn').style.display = 'inline-flex';
        document.getElementById('chirpMuteStatus').textContent =
          `Muted for ${Math.ceil(data.mute_remaining_sec / 60)} more minutes`;
      } else {
        document.getElementById('chirpUnmuteBtn').style.display = 'none';
        document.getElementById('chirpMuteStatus').textContent = '';
      }

      // Settings
      document.getElementById('chirpRelayEnabled').checked = data.relay_enabled !== false;

      // Chirp count badge in nav
      const chirpCountBadge = document.getElementById('chirpCount');
      if (data.recent_chirps > 0) {
        chirpCountBadge.textContent = data.recent_chirps;
        chirpCountBadge.style.display = 'inline-flex';
      } else {
        chirpCountBadge.style.display = 'none';
      }

      // Load recent chirps
      loadChirps();
    }

    async function loadChirps() {
      const data = await api('/api/chirp/recent');
      const list = document.getElementById('chirpList');

      if (!data.chirps || data.chirps.length === 0) {
        list.innerHTML = '<div class="empty-state"><div class="empty-icon">🐦</div><p>No community alerts</p></div>';
        return;
      }

      list.innerHTML = data.chirps.map(chirp => {
        const urgencyColor = chirp.urgency === 'urgent' ? '#e67e22' :
                            chirp.urgency === 'caution' ? '#f4b942' : '#63b3ed';
        const urgencyBg = chirp.urgency === 'urgent' ? 'rgba(230,126,34,0.15)' :
                         chirp.urgency === 'caution' ? 'rgba(244,185,66,0.15)' : 'rgba(99,179,237,0.15)';
        const categoryIcon = chirp.category === 'authority' ? '🚔' :
                            chirp.category === 'infrastructure' ? '⚡' :
                            chirp.category === 'emergency' ? '🚨' :
                            chirp.category === 'weather' ? '⛈️' :
                            chirp.category === 'mutual_aid' ? '🤝' :
                            chirp.category === 'all_clear' ? '✅' : '👁️';
        const validationBadge = chirp.validated ? '' :
                               `<span style="background:rgba(244,185,66,0.2);color:#f4b942;padding:0.1rem 0.3rem;border-radius:3px;font-size:0.65rem;margin-left:0.3rem;">awaiting confirmation (${chirp.confirm_count || 0}/2)</span>`;

        return `
          <div class="log-item" style="border-left-color:${urgencyColor};${chirp.suppressed ? 'opacity:0.5;' : ''}">
            <div style="font-size:1.5rem;min-width:2rem;text-align:center;">${categoryIcon}</div>
            <div class="log-content">
              <div class="log-message">
                <span style="opacity:0.8;">${chirp.emoji}</span> witnessed:
                <span style="background:${urgencyBg};color:${urgencyColor};padding:0.1rem 0.3rem;border-radius:3px;font-size:0.75rem;">${chirp.urgency}</span>
                ${validationBadge}
              </div>
              <div class="log-detail" style="font-weight:500;">${escapeHtml(chirp.template_text || 'unknown alert')}${chirp.detail ? ' — ' + escapeHtml(chirp.detail) : ''}</div>
              <div class="log-meta">
                ${chirp.category} · ${formatChirpAge(chirp.age_sec)} ago · ${chirp.hop_count} hop(s) · ${chirp.confirm_count || 0} confirm(s)
                ${chirp.relayed ? ' · relayed' : ''}${chirp.suppressed ? ' · suppressed' : ''}
              </div>
            </div>
            <div class="log-actions">
              ${!chirp.validated ? `<button class="btn btn-ghost btn-sm" onclick="confirmChirp('${chirp.nonce}')" title="I see this too">👁️ Confirm</button>` : ''}
              <button class="btn btn-ghost btn-sm" onclick="dismissChirp('${chirp.nonce}')" title="Dismiss">✕</button>
            </div>
          </div>
        `;
      }).join('');
    }

    function formatChirpAge(sec) {
      if (sec < 60) return sec + 's';
      if (sec < 3600) return Math.floor(sec / 60) + 'm';
      return Math.floor(sec / 3600) + 'h';
    }

    function refreshChirps() {
      loadChirps();
    }

    async function toggleChirpEnabled() {
      const enabled = document.getElementById('chirpEnabled').checked;
      const endpoint = enabled ? '/api/chirp/enable' : '/api/chirp/disable';
      const data = await api(endpoint, 'POST');

      if (!data.success) {
        alert('Failed to toggle chirp channel: ' + (data.error || 'Unknown error'));
        document.getElementById('chirpEnabled').checked = !enabled;
      }

      refreshChirpStatus();
    }

    function updateChirpPreview() {
      const select = document.getElementById('chirpTemplate');
      const text = select.options[select.selectedIndex].text;
      document.getElementById('chirpPreviewText').textContent = text;
    }

    async function sendChirp() {
      const template_id = parseInt(document.getElementById('chirpTemplate').value);
      const detail = parseInt(document.getElementById('chirpDetail').value);
      const urgency = document.querySelector('input[name="chirpUrgency"]:checked').value;

      const templateText = document.getElementById('chirpTemplate').options[
        document.getElementById('chirpTemplate').selectedIndex
      ].text;

      const confirmMsg = urgency === 'urgent' ?
        `Send URGENT alert: "${templateText}"?\n\nThis requires 2 neighbor confirmations before spreading.` :
        `Send alert: "${templateText}"?\n\nThis requires 2 neighbor confirmations before spreading.`;

      if (!confirm(confirmMsg)) return;

      const data = await api('/api/chirp/send', 'POST', {
        template_id,
        detail,
        urgency,
        ttl_minutes: 15
      });

      if (data.success) {
        alert('Alert sent! Waiting for neighbor confirmations before it spreads.');
      } else {
        alert('Failed to send: ' + (data.message || data.error || 'Unknown error'));
      }

      refreshChirpStatus();
    }

    async function confirmChirp(nonce) {
      // Human witness confirmation - "I see this too"
      const data = await api('/api/chirp/confirm', 'POST', { nonce });
      if (data.success) {
        // Reload chirps to show updated confirmation count
        loadChirps();
      }
    }

    async function dismissChirp(nonce) {
      await api('/api/chirp/dismiss', 'POST', { nonce });
      loadChirps();
    }

    async function muteChirps(minutes) {
      const data = await api('/api/chirp/mute', 'POST', { duration_minutes: minutes });
      if (!data.success) {
        alert('Failed to mute: ' + (data.error || 'Unknown error'));
      }
      refreshChirpStatus();
    }

    async function unmuteChirps() {
      await api('/api/chirp/unmute', 'POST');
      refreshChirpStatus();
    }

    async function updateChirpSettings() {
      const relay_enabled = document.getElementById('chirpRelayEnabled').checked;
      const urgency_filter = document.getElementById('chirpUrgencyFilter').value;

      await api('/api/chirp/settings', 'POST', { relay_enabled, urgency_filter });
    }

    // Urgency radio button styling
    document.querySelectorAll('input[name="chirpUrgency"]').forEach(radio => {
      radio.addEventListener('change', function() {
        document.querySelectorAll('input[name="chirpUrgency"]').forEach(r => {
          r.parentElement.style.borderColor = 'var(--border)';
        });
        this.parentElement.style.borderColor = 'var(--accent)';
      });
    });

    // ══════════════════════════════════════════════════════════════════
    // BLUETOOTH
    // ══════════════════════════════════════════════════════════════════

    let btState = null;
    let btScanning = false;

    async function refreshBtStatus() {
      const data = await api('/api/bluetooth');
      if (!data.state) return;

      btState = data;

      // Update status display
      document.getElementById('btStateVal').textContent = data.state;
      document.getElementById('btDeviceName').textContent = data.device_name || '--';
      document.getElementById('btLocalAddr').textContent = data.local_address || '--';
      document.getElementById('btTxPower').innerHTML = (data.tx_power >= 0 ? '+' : '') + data.tx_power + '<span class="stat-unit">dBm</span>';
      document.getElementById('btPairedCount').textContent = data.paired_count || 0;
      document.getElementById('btTotalConns').textContent = data.stats?.total_connections || 0;

      // Update enabled checkbox
      document.getElementById('btEnabled').checked = data.enabled;

      // Update header badge
      const btBadge = document.getElementById('btBadge');
      const btStatus = document.getElementById('btStatus');
      if (data.connected) {
        btBadge.className = 'badge success';
        btStatus.textContent = 'Connected';
      } else if (data.advertising) {
        btBadge.className = 'badge info';
        btStatus.textContent = 'Advertising';
      } else if (data.enabled) {
        btBadge.className = 'badge info';
        btStatus.textContent = 'BT On';
      } else {
        btBadge.className = 'badge info';
        btStatus.textContent = 'BT Off';
      }

      // Update state badge
      const stateBadge = document.getElementById('btStateBadge');
      const stateText = document.getElementById('btStateText');
      stateText.textContent = data.state.charAt(0).toUpperCase() + data.state.slice(1);

      if (data.state === 'connected') {
        stateBadge.className = 'badge success';
        document.getElementById('btSubtitle').textContent = 'Device connected';
      } else if (data.state === 'advertising') {
        stateBadge.className = 'badge info';
        document.getElementById('btSubtitle').textContent = 'Waiting for connections';
      } else if (data.state === 'scanning') {
        stateBadge.className = 'badge info';
        document.getElementById('btSubtitle').textContent = 'Scanning for devices';
      } else if (data.state === 'pairing') {
        stateBadge.className = 'badge warning';
        document.getElementById('btSubtitle').textContent = 'Pairing in progress';
      } else if (data.state === 'disabled') {
        stateBadge.className = 'badge info';
        document.getElementById('btSubtitle').textContent = 'Bluetooth is disabled';
      } else {
        stateBadge.className = 'badge info';
        document.getElementById('btSubtitle').textContent = 'BLE connectivity status';
      }

      // Update advertising button
      const advBtn = document.getElementById('btAdvBtn');
      if (data.advertising) {
        advBtn.textContent = 'Stop Advertising';
        advBtn.className = 'btn btn-danger';
      } else {
        advBtn.textContent = 'Start Advertising';
        advBtn.className = 'btn btn-primary';
      }

      // Update connection card
      const connCard = document.getElementById('btConnCard');
      if (data.connected && data.connection) {
        connCard.style.display = 'block';
        document.getElementById('btConnName').textContent = data.connection.name || '--';
        document.getElementById('btConnAddr').textContent = data.connection.address || '--';
        document.getElementById('btConnSecurity').textContent = data.connection.security || '--';
        document.getElementById('btConnTime').textContent = formatDuration(data.connection.connected_sec || 0);
        document.getElementById('btConnSent').innerHTML = formatBytes(data.connection.bytes_sent || 0);
        document.getElementById('btConnRecv').innerHTML = formatBytes(data.connection.bytes_received || 0);
      } else {
        connCard.style.display = 'none';
      }

      // Update pairing card
      const pairingCard = document.getElementById('btPairingCard');
      if (data.pairing && data.pairing.state !== 'none') {
        pairingCard.style.display = 'block';
        if (data.pairing.pin) {
          document.getElementById('btPairingPin').textContent = String(data.pairing.pin).padStart(6, '0');
          document.getElementById('btPairingSubtitle').textContent = 'Enter PIN on connecting device';
        } else {
          document.getElementById('btPairingPin').textContent = '------';
          document.getElementById('btPairingSubtitle').textContent = 'Waiting for device...';
        }
      } else {
        pairingCard.style.display = 'none';
      }

      // Load settings into form
      document.getElementById('btNameInput').placeholder = data.device_name || 'SecuraCV-Canary';
      document.getElementById('btPowerSlider').value = data.tx_power || 3;
      updatePowerDisplay();
    }

    async function loadBtSettings() {
      const data = await api('/api/bluetooth/settings');
      if (!data.enabled === undefined) return;

      document.getElementById('btAutoAdv').checked = data.auto_advertise;
      document.getElementById('btAllowPairing').checked = data.allow_pairing;
      document.getElementById('btRequirePin').checked = data.require_pin;
      document.getElementById('btNotifyConnect').checked = data.notify_on_connect;
      document.getElementById('btTimeoutSelect').value = data.inactivity_timeout_sec || 300;
    }

    async function saveBtSettings() {
      const settings = {
        auto_advertise: document.getElementById('btAutoAdv').checked,
        allow_pairing: document.getElementById('btAllowPairing').checked,
        require_pin: document.getElementById('btRequirePin').checked,
        notify_on_connect: document.getElementById('btNotifyConnect').checked,
        inactivity_timeout_sec: parseInt(document.getElementById('btTimeoutSelect').value)
      };

      await api('/api/bluetooth/settings', 'POST', settings);
    }

    async function toggleBtEnabled() {
      const enabled = document.getElementById('btEnabled').checked;
      const endpoint = enabled ? '/api/bluetooth/enable' : '/api/bluetooth/disable';
      const data = await api(endpoint, 'POST');
      if (!data.success) {
        alert('Failed: ' + (data.error || 'Unknown error'));
        document.getElementById('btEnabled').checked = !enabled;
      }
      refreshBtStatus();
    }

    async function toggleBtAdvertising() {
      const isAdv = btState && btState.advertising;
      const endpoint = isAdv ? '/api/bluetooth/advertise/stop' : '/api/bluetooth/advertise/start';
      const data = await api(endpoint, 'POST');
      if (!data.success) {
        alert('Failed: ' + (data.error || 'Unknown error'));
      }
      refreshBtStatus();
    }

    async function btStartPairing() {
      const data = await api('/api/bluetooth/pair/start', 'POST');
      if (!data.success) {
        alert('Failed to start pairing: ' + (data.error || 'Unknown error'));
      }
      refreshBtStatus();
    }

    async function btCancelPairing() {
      await api('/api/bluetooth/pair/cancel', 'POST');
      refreshBtStatus();
    }

    async function btDisconnect() {
      const data = await api('/api/bluetooth/disconnect', 'POST');
      if (!data.success) {
        alert('Failed to disconnect: ' + (data.error || 'Unknown error'));
      }
      refreshBtStatus();
    }

    async function btStartScan() {
      const btn = document.getElementById('btScanBtn');
      btn.disabled = true;
      btn.textContent = 'Scanning...';

      document.getElementById('btScanCard').style.display = 'block';
      document.getElementById('btScanList').innerHTML = '<div class="loading"><div class="spinner"></div></div>';

      const data = await api('/api/bluetooth/scan/start', 'POST', { duration_sec: 10 });
      if (!data.success) {
        alert('Failed to start scan: ' + (data.error || 'Unknown error'));
        btn.disabled = false;
        btn.textContent = 'Scan for Devices';
        return;
      }

      btScanning = true;

      // Poll for results
      const pollInterval = setInterval(async () => {
        const results = await api('/api/bluetooth/scan/results');
        if (results.devices) {
          renderScanResults(results.devices);
        }
        if (!results.scanning) {
          clearInterval(pollInterval);
          btScanning = false;
          btn.disabled = false;
          btn.textContent = 'Scan for Devices';
        }
      }, 1000);

      // Timeout fallback
      setTimeout(() => {
        if (btScanning) {
          clearInterval(pollInterval);
          btScanning = false;
          btn.disabled = false;
          btn.textContent = 'Scan for Devices';
        }
      }, 15000);
    }

    function renderScanResults(devices) {
      const list = document.getElementById('btScanList');
      document.getElementById('btScanSubtitle').textContent = devices.length + ' device(s) found';

      if (devices.length === 0) {
        list.innerHTML = '<p style="color:var(--muted);font-size:0.85rem;text-align:center;padding:1rem;">No devices found</p>';
        return;
      }

      let html = '';
      devices.forEach(dev => {
        const typeIcon = dev.type === 'phone' ? '📱' :
                        dev.type === 'tablet' ? '📱' :
                        dev.type === 'computer' ? '💻' :
                        dev.type === 'wearable' ? '⌚' :
                        dev.is_securacv ? '🔒' : '📶';
        const rssiColor = dev.rssi >= -60 ? 'var(--success)' :
                         dev.rssi >= -80 ? 'var(--warning)' : 'var(--danger)';

        html += '<div class="log-item" style="padding:0.75rem;">';
        html += '<div style="display:flex;justify-content:space-between;align-items:center;">';
        html += '<div>';
        html += '<span style="font-size:1.2rem;margin-right:0.5rem;">' + typeIcon + '</span>';
        html += '<strong>' + (dev.name || 'Unknown Device') + '</strong>';
        if (dev.is_securacv) {
          html += ' <span class="badge success" style="font-size:0.65rem;">SecuraCV</span>';
        }
        html += '<div style="font-size:0.75rem;color:var(--muted);">' + dev.address + '</div>';
        html += '</div>';
        html += '<div style="text-align:right;">';
        html += '<div style="color:' + rssiColor + ';font-weight:600;">' + dev.rssi + ' dBm</div>';
        html += '<div style="font-size:0.7rem;color:var(--muted);">' + dev.type + '</div>';
        html += '</div>';
        html += '</div>';
        html += '</div>';
      });

      list.innerHTML = html;
    }

    async function btRefreshScan() {
      const results = await api('/api/bluetooth/scan/results');
      if (results.devices) {
        renderScanResults(results.devices);
      }
    }

    async function btClearScan() {
      await api('/api/bluetooth/scan/results', 'DELETE');
      document.getElementById('btScanCard').style.display = 'none';
    }

    async function loadBtPairedDevices() {
      const data = await api('/api/bluetooth/paired');
      if (!data.devices) return;

      const list = document.getElementById('btPairedList');
      document.getElementById('btPairedSubtitle').textContent = data.count + ' paired device(s)';

      if (data.devices.length === 0) {
        list.innerHTML = '<p style="color:var(--muted);font-size:0.85rem;text-align:center;padding:1rem;">No paired devices</p>';
        return;
      }

      let html = '';
      data.devices.forEach(dev => {
        const trustBadge = dev.trusted ? '<span class="badge success" style="font-size:0.6rem;">Trusted</span>' : '';
        const blockBadge = dev.blocked ? '<span class="badge danger" style="font-size:0.6rem;">Blocked</span>' : '';

        html += '<div class="log-item" style="padding:0.75rem;">';
        html += '<div style="display:flex;justify-content:space-between;align-items:start;">';
        html += '<div>';
        html += '<strong>' + (dev.name || 'Unknown') + '</strong> ' + trustBadge + blockBadge;
        html += '<div style="font-size:0.75rem;color:var(--muted);">' + dev.address + '</div>';
        html += '<div style="font-size:0.7rem;color:var(--muted);">Security: ' + dev.security + ' | Connections: ' + dev.connection_count + '</div>';
        html += '</div>';
        html += '<div style="display:flex;gap:0.25rem;">';
        html += '<button class="btn btn-ghost btn-sm" onclick="btToggleTrust(\'' + dev.address + '\', ' + !dev.trusted + ')">' + (dev.trusted ? 'Untrust' : 'Trust') + '</button>';
        html += '<button class="btn btn-danger btn-sm" onclick="btRemovePaired(\'' + dev.address + '\')">Remove</button>';
        html += '</div>';
        html += '</div>';
        html += '</div>';
      });

      list.innerHTML = html;
    }

    async function btRemovePaired(address) {
      if (!confirm('Remove this paired device?')) return;
      const data = await api('/api/bluetooth/paired', 'DELETE', { address });
      if (!data.success) {
        alert('Failed: ' + (data.error || 'Unknown error'));
      }
      loadBtPairedDevices();
    }

    async function btClearAllPaired() {
      if (!confirm('Remove ALL paired devices? This cannot be undone.')) return;
      const data = await api('/api/bluetooth/paired/all', 'DELETE');
      if (!data.success) {
        alert('Failed: ' + (data.error || 'Unknown error'));
      }
      loadBtPairedDevices();
    }

    async function btToggleTrust(address, trusted) {
      await api('/api/bluetooth/paired/trust', 'POST', { address, trusted });
      loadBtPairedDevices();
    }

    async function btSetName() {
      const name = document.getElementById('btNameInput').value.trim();
      if (!name) {
        alert('Please enter a device name');
        return;
      }
      const data = await api('/api/bluetooth/name', 'POST', { name });
      if (data.success) {
        alert('Device name updated. Restart Bluetooth to apply.');
        refreshBtStatus();
      } else {
        alert('Failed: ' + (data.error || 'Unknown error'));
      }
    }

    function updatePowerDisplay() {
      const power = parseInt(document.getElementById('btPowerSlider').value);
      const display = document.getElementById('btPowerDisplay');
      display.textContent = (power >= 0 ? '+' : '') + power + ' dBm';
    }

    async function btSetPower() {
      const power = parseInt(document.getElementById('btPowerSlider').value);
      const data = await api('/api/bluetooth/power', 'POST', { power });
      if (data.success) {
        refreshBtStatus();
      } else {
        alert('Failed: ' + (data.error || 'Unknown error'));
      }
    }

    function formatDuration(seconds) {
      if (seconds < 60) return seconds + 's';
      if (seconds < 3600) return Math.floor(seconds / 60) + 'm ' + (seconds % 60) + 's';
      const hours = Math.floor(seconds / 3600);
      const mins = Math.floor((seconds % 3600) / 60);
      return hours + 'h ' + mins + 'm';
    }

    function formatBytes(bytes) {
      if (bytes < 1024) return bytes + '<span class="stat-unit">B</span>';
      if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + '<span class="stat-unit">KB</span>';
      return (bytes / (1024 * 1024)).toFixed(1) + '<span class="stat-unit">MB</span>';
    }

    // ══════════════════════════════════════════════════════════════════
    // Initialize
    // ══════════════════════════════════════════════════════════════════

    refreshStatus();
    loadChain();
    loadWifiStatus();
    refreshOpera();
    refreshChirpStatus();
    refreshBtStatus();
    loadBtSettings();
    loadBtPairedDevices();
    updateResolutionUI();
    setInterval(refreshStatus, 2000);
    setInterval(loadWifiStatus, 5000);
    setInterval(() => {
      if (currentPanel === 'logs') loadLogs();
      else if (currentPanel === 'witness') loadWitness();
      else if (currentPanel === 'opera') refreshOpera();
      else if (currentPanel === 'community') refreshChirpStatus();
      else if (currentPanel === 'bluetooth') refreshBtStatus();
    }, 5000);
  </script>
</body>
</html>
)rawliteral";
