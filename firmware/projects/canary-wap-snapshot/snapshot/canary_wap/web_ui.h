/*
 * SecuraCV Canary — Web UI
 * Version 2.0.1 — Fixed camera streaming + resolution controls
 * 
 * Professional dashboard for device monitoring, log review, and management.
 * Embedded as PROGMEM string for flash storage efficiency.
 */

#ifndef SECURACV_WEB_UI_H
#define SECURACV_WEB_UI_H

#include <Arduino.h>

static const char CANARY_UI_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
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
      </div>
    </div>
  </header>

  <div class="container">
    <nav>
      <button class="nav-btn active" data-panel="status">Status</button>
      <button class="nav-btn" data-panel="peek">Peek</button>
      <button class="nav-btn" data-panel="logs">
        Logs<span class="count" id="logsCount" style="display:none">0</span>
      </button>
      <button class="nav-btn" data-panel="witness">Witness</button>
      <button class="nav-btn" data-panel="settings">Settings</button>
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
        return await res.json();
      } catch (e) {
        console.error('API error:', e);
        return { ok: false, error: e.message };
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
        console.error('Stream error:', e);
        status.textContent = 'Stream error';
        btn.disabled = false;
        
        // Check if we should retry
        if (peekActive) {
          setTimeout(() => {
            if (peekActive) {
              console.log('Retrying stream...');
              stream.src = API_BASE + '/api/peek/stream?t=' + Date.now();
            }
          }, 2000);
        }
      };
      
      // Optimistically update UI
      peekActive = true;
      btn.disabled = false;
      updatePeekUI();
    }
    
    async function stopPeek() {
      const stream = document.getElementById('peekStream');
      
      // Clear the stream source first
      stream.src = '';
      
      // Tell server to stop
      await api('/api/peek/stop', 'POST');
      
      peekActive = false;
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
    // Initialize
    // ══════════════════════════════════════════════════════════════════

    refreshStatus();
    loadChain();
    loadWifiStatus();
    updateResolutionUI();
    setInterval(refreshStatus, 2000);
    setInterval(loadWifiStatus, 5000);
    setInterval(() => {
      if (currentPanel === 'logs') loadLogs();
      else if (currentPanel === 'witness') loadWitness();
    }, 5000);
  </script>
</body>
</html>
)rawliteral";

#endif // SECURACV_WEB_UI_H
