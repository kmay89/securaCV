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

    /* Live-indicator row on the Status tab */
    .live-indicator-row {
      display: grid;
      grid-template-columns: repeat(5, 1fr);
      gap: 8px;
      padding: 4px 4px 6px;
    }
    @media (max-width: 520px) {
      .live-indicator-row { grid-template-columns: repeat(3, 1fr); }
    }
    .live-indicator {
      text-align: center;
      padding: 12px 4px;
      background: rgba(255,255,255,0.02);
      border-radius: 10px;
      border: 1px solid rgba(255,255,255,0.05);
      transition: all 200ms ease-out;
    }
    .live-indicator__icon {
      font-size: 1.4rem;
      margin-bottom: 4px;
      filter: grayscale(100%);
      opacity: 0.4;
      transition: filter 200ms, opacity 200ms;
    }
    .live-indicator__label {
      font-size: 0.7rem;
      color: var(--muted);
      letter-spacing: 0.4px;
      text-transform: uppercase;
    }
    .live-indicator[data-state="on"] {
      background: rgba(255,69,58,0.18);
      border-color: rgba(255,69,58,0.45);
    }
    .live-indicator[data-state="on"] .live-indicator__icon {
      filter: none;
      opacity: 1;
    }
    .live-indicator[data-state="on"] .live-indicator__label {
      color: #ff453a;
    }
    .live-indicator[data-state="info"] {
      background: rgba(90,200,250,0.10);
      border-color: rgba(90,200,250,0.35);
    }
    .live-indicator[data-state="info"] .live-indicator__icon {
      filter: none;
      opacity: 0.85;
    }

    /* ── Sensing panel — gauges, pill, bar graphs, arrows ─────────── */
    .sensing-pill {
      display: inline-block;
      padding: 8px 22px;
      border-radius: 999px;
      font-weight: 600;
      font-size: 1rem;
      letter-spacing: 0.5px;
      text-transform: capitalize;
    }
    .sensing-pill--offline  { background: rgba(120,120,128,0.18); color: #98989d; }
    .sensing-pill--quiet    { background: rgba(120,120,128,0.18); color: #98989d; }
    .sensing-pill--presence { background: rgba(52,199,89,0.18);  color: #34c759; }
    .sensing-pill--motion   { background: rgba(90,200,250,0.18); color: #5ac8fa; }
    .sensing-pill--active   { background: rgba(255,159,10,0.20); color: #ff9f0a; }
    .sensing-pill--muted    { background: rgba(120,120,128,0.28); color: #c7c7cc; }
    /* "Supplement, not a replacement" banner on the Acoustic card. */
    .sensing-caution {
      margin: 10px 0 0 0;
      padding: 10px 12px;
      border-radius: 10px;
      background: rgba(255,159,10,0.10);
      border: 1px solid rgba(255,159,10,0.35);
      color: #ffb454;
      font-size: 0.88rem;
      line-height: 1.35;
    }
    .sensing-caution strong { color: #ff9f0a; }
    /* Live RMS level meter — the SAME 20 ms scalar the hysteresis uses.
       The two notches are the OFF (clear) and ON (alarm) thresholds. */
    .audio-meter {
      position: relative;
      height: 22px;
      background: rgba(255,255,255,0.05);
      border-radius: 6px;
      overflow: hidden;
      margin: 8px 0 4px;
    }
    .audio-meter__fill {
      height: 100%;
      width: 0%;
      background: linear-gradient(90deg, #34c759 0%, #ffd60a 60%, #ff9f0a 90%, #ff453a 100%);
      transition: width 120ms linear;
    }
    .audio-meter__notch {
      position: absolute;
      top: 0;
      bottom: 0;
      width: 2px;
      background: rgba(255,255,255,0.5);
      pointer-events: none;
    }
    .audio-meter__legend {
      display: flex;
      justify-content: space-between;
      font-size: 0.72rem;
      color: var(--muted);
    }
    .audio-trace {
      display: flex;
      gap: 2px;
      height: 28px;
      margin-top: 8px;
      align-items: stretch;
    }
    .audio-trace__seg {
      flex: 1 1 0;
      min-width: 6px;
      border-radius: 3px;
      background: rgba(120,120,128,0.25);
    }
    .audio-trace__seg--on { background: #ff9f0a; }
    .audio-mic-row {
      display: flex;
      align-items: center;
      gap: 10px;
      margin-top: 8px;
      flex-wrap: wrap;
    }
    .audio-mic-row__dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      flex-shrink: 0;
    }
    .audio-mic-row__dot--live   { background: #34c759; box-shadow: 0 0 6px #34c759; }
    .audio-mic-row__dot--muted  { background: #98989d; }
    .audio-mic-row__dot--offline{ background: #ff453a; }
    .sensing-explain {
      margin-top: 10px;
      color: var(--muted);
      font-size: 0.92rem;
      max-width: 520px;
      margin-left: auto;
      margin-right: auto;
    }
    .vi-tune { margin-bottom: 0.75rem; }
    .vi-tune label { display:block; font-size:0.85rem; color:var(--muted); margin-bottom:4px; }
    .vi-tune label span { color:var(--fg); font-weight:600; }
    .vi-tune input[type=range] { width:100%; accent-color:var(--accent); }
    .sensing-gauges {
      display: grid;
      grid-template-columns: repeat(3, minmax(140px, 1fr));
      gap: 16px;
      padding: 10px 4px 4px;
    }
    @media (max-width: 520px) {
      .sensing-gauges { grid-template-columns: 1fr; }
    }
    .sensing-gauge {
      text-align: center;
      padding: 12px 8px;
      background: rgba(255,255,255,0.02);
      border-radius: 12px;
    }
    .sensing-gauge__title {
      font-size: 0.78rem;
      color: var(--muted);
      letter-spacing: 0.6px;
      text-transform: uppercase;
      margin-bottom: 6px;
    }
    .sensing-gauge__svg { width: 100%; height: 64px; }
    .sensing-gauge__num { font-size: 1.4rem; font-weight: 600; margin-top: -8px; }
    .sensing-gauge__unit { font-size: 0.72rem; color: var(--muted); margin-left: 3px; }
    .sensing-bars {
      display: flex;
      align-items: flex-end;
      gap: 6px;
      height: 110px;
      padding: 6px 4px 4px;
    }
    .sensing-bars--narrow { height: 80px; }
    .sensing-bar {
      flex: 1 1 0;
      background: linear-gradient(180deg, #5ac8fa 0%, #0a84ff 100%);
      border-radius: 4px 4px 0 0;
      min-height: 3px;
      transition: height 200ms ease-out;
    }
    .sensing-bars--narrow .sensing-bar {
      background: linear-gradient(180deg, #34c759 0%, #30b350 100%);
    }
    .sensing-arrows {
      display: flex;
      justify-content: space-around;
      gap: 6px;
      padding: 8px 0 4px;
      font-size: 1.6rem;
      color: var(--muted);
    }
    .sensing-arrow {
      flex: 1 1 0;
      text-align: center;
      transition: color 200ms, transform 200ms;
    }
    .sensing-arrow--pos { color: #5ac8fa; transform: translateY(-2px); }
    .sensing-arrow--neg { color: #ff9f0a; transform: translateY(-2px) rotate(180deg); }
    
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
      <button class="nav-btn" data-panel="sensing">Sensing</button>
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

      <!-- Live Sensing summary — surfaces the room-state pill + critical
           alarm indicators on the landing page so the user sees "what
           the radio feels right now" without navigating to the Sensing
           tab. Hidden cleanly when the build has no sensing features. -->
      <div class="card" id="liveSensingCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Live sensing</div>
            <div class="card-subtitle">
              What this canary is feeling right now — full detail under the Sensing tab.
            </div>
          </div>
          <button class="btn btn-ghost btn-sm" onclick="switchPanel('sensing')">Open Sensing →</button>
        </div>
        <div style="text-align:center; padding:16px 8px 8px;">
          <div id="liveActivityPill" class="sensing-pill sensing-pill--offline">Offline</div>
        </div>
        <!-- Five quick-glance indicators. Each one lights up red while
             its underlying event is fresh; otherwise stays grey. -->
        <div class="live-indicator-row">
          <div class="live-indicator" id="liveIndSmoke" data-state="off">
            <div class="live-indicator__icon">🔥</div>
            <div class="live-indicator__label">Smoke alarm</div>
          </div>
          <div class="live-indicator" id="liveIndCO" data-state="off">
            <div class="live-indicator__icon">⚠</div>
            <div class="live-indicator__label">CO alarm</div>
          </div>
          <div class="live-indicator" id="liveIndPanic" data-state="off">
            <div class="live-indicator__icon">🚨</div>
            <div class="live-indicator__label">Silent panic</div>
          </div>
          <div class="live-indicator" id="liveIndTamper" data-state="off">
            <div class="live-indicator__icon">🔧</div>
            <div class="live-indicator__label">Tamper</div>
          </div>
          <div class="live-indicator" id="liveIndIR" data-state="off">
            <div class="live-indicator__icon">📺</div>
            <div class="live-indicator__label">Appliance</div>
          </div>
        </div>
        <!-- Three small score readouts so the eye gets motion/breathing/RSSI at a glance. -->
        <div class="stats-grid" style="margin-top:8px;">
          <div class="stat-item">
            <div class="stat-label">Motion</div>
            <div class="stat-value"><span id="liveMotion">0</span><span class="stat-unit">%</span></div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Breathing</div>
            <div class="stat-value"><span id="liveBreath">0</span><span class="stat-unit">%</span></div>
          </div>
          <div class="stat-item">
            <div class="stat-label">RSSI</div>
            <div class="stat-value"><span id="liveRssi">--</span><span class="stat-unit">dBm</span></div>
          </div>
        </div>
      </div>

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

    <!-- Sensing Panel — live CSI motion / breathing / activity visualization -->
    <div class="panel" id="panel-sensing">
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">What this room feels like</div>
            <div class="card-subtitle">
              Privacy-safe RF sensing — no cameras, no microphones, no identifiers.
              The radio sees the shape of motion in the room.
            </div>
          </div>
          <button class="btn btn-ghost btn-sm" onclick="refreshSensing()">↻ Refresh</button>
        </div>

        <!-- Hero status pill -->
        <div id="sensingHero" style="text-align:center; padding:24px 16px;">
          <div id="sensingPill" class="sensing-pill sensing-pill--offline">Offline</div>
          <div id="sensingExplain" class="sensing-explain">
            Waiting for the WiFi radio to come up…
          </div>
        </div>

        <!-- Three radial gauges -->
        <div class="sensing-gauges">
          <div class="sensing-gauge">
            <div class="sensing-gauge__title">Motion</div>
            <svg viewBox="0 0 100 60" class="sensing-gauge__svg" aria-hidden="true">
              <path d="M10,55 A40,40 0 0 1 90,55" stroke="rgba(255,255,255,0.08)" stroke-width="8" fill="none" stroke-linecap="round"/>
              <path id="motionArc" d="M10,55 A40,40 0 0 1 90,55" stroke="#5ac8fa" stroke-width="8" fill="none" stroke-linecap="round" stroke-dasharray="0 200"/>
            </svg>
            <div class="sensing-gauge__num"><span id="motionVal">0</span><span class="sensing-gauge__unit">%</span></div>
          </div>
          <div class="sensing-gauge">
            <div class="sensing-gauge__title">Breathing-band</div>
            <svg viewBox="0 0 100 60" class="sensing-gauge__svg" aria-hidden="true">
              <path d="M10,55 A40,40 0 0 1 90,55" stroke="rgba(255,255,255,0.08)" stroke-width="8" fill="none" stroke-linecap="round"/>
              <path id="breathArc" d="M10,55 A40,40 0 0 1 90,55" stroke="#34c759" stroke-width="8" fill="none" stroke-linecap="round" stroke-dasharray="0 200"/>
            </svg>
            <div class="sensing-gauge__num"><span id="breathVal">0</span><span class="sensing-gauge__unit">%</span></div>
          </div>
          <div class="sensing-gauge">
            <div class="sensing-gauge__title">Signal</div>
            <svg viewBox="0 0 100 60" class="sensing-gauge__svg" aria-hidden="true">
              <path d="M10,55 A40,40 0 0 1 90,55" stroke="rgba(255,255,255,0.08)" stroke-width="8" fill="none" stroke-linecap="round"/>
              <path id="rssiArc" d="M10,55 A40,40 0 0 1 90,55" stroke="#ff9f0a" stroke-width="8" fill="none" stroke-linecap="round" stroke-dasharray="0 200"/>
            </svg>
            <div class="sensing-gauge__num"><span id="rssiVal">--</span><span class="sensing-gauge__unit">dBm</span></div>
          </div>
        </div>
      </div>

      <!-- Amplitude band bars + Doppler arrows -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Subcarrier activity</div>
            <div class="card-subtitle">
              Eight frequency bands. Tall bars = lots of variance = something
              moving. Direction arrows show whether the radio sees objects
              approaching or receding.
            </div>
          </div>
        </div>
        <div class="sensing-bars" id="ampBars" role="img" aria-label="Eight amplitude variance bands"></div>
        <div class="sensing-arrows" id="dopArrows" role="img" aria-label="Four directional Doppler bands"></div>
      </div>

      <!-- Breathing-band spectrum -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">0.1–0.5 Hz spectrum</div>
            <div class="card-subtitle">
              The frequency range a calmly breathing person sits in. A clear
              peak in one of these bins means someone is in the room and at
              rest — not necessarily who.
            </div>
          </div>
        </div>
        <div class="sensing-bars sensing-bars--narrow" id="breathBars" role="img" aria-label="Eight breathing-band Goertzel bins"></div>
      </div>

      <!-- Acoustic events (T3 smoke / T4 CO cadence detection) -->
      <div class="card" id="acousticCard" style="display:none;">
        <div class="card-header">
          <div style="flex:1;">
            <div class="card-title">Acoustic alarms</div>
            <div class="card-subtitle">
              The radio is silent here — this card listens for the standard
              cadences every code-compliant smoke and CO alarm emits. No
              audio ever leaves the device, only the pattern match.
            </div>
            <div class="sensing-caution">
              ⚠ <strong>Supplement, not a substitute.</strong> This is not a
              UL-listed life-safety device. Keep your existing smoke and CO
              alarms. The Canary helps a UL-listed alarm get noticed — it
              does not replace one, and it cannot detect smoke, fire, or CO
              directly. Reliable detection range is roughly 3 m at typical
              alarm volume; through a closed door or in a noisy room,
              detection becomes unreliable.
            </div>
          </div>
        </div>
        <div id="acousticHero" style="text-align:center; padding:18px 16px;">
          <div id="acousticPill" class="sensing-pill sensing-pill--quiet">No alarms</div>
          <div id="acousticExplain" class="sensing-explain">
            The PDM microphone is listening for NFPA 72 (smoke) and UL 2034
            (CO) cadences. Nothing detected.
          </div>
        </div>
        <div class="audio-mic-row">
          <span id="acMicDot" class="audio-mic-row__dot audio-mic-row__dot--live" aria-hidden="true"></span>
          <span id="acMicLabel" style="font-weight:600;">Mic live</span>
          <span style="color:var(--muted); font-size:0.85rem; flex:1;">
            Mute releases GPIO 41/42 at the I2S driver — verifiable, persisted across reboots.
          </span>
          <button class="btn btn-secondary btn-sm" id="acMicMuteBtn" onclick="toggleMicMute()">Mute microphone</button>
        </div>
        <!-- Last-toggle audit line. Hidden until the API returns a real
             source; populated by refreshLiveSensing(). Lets the user see
             at a glance whether the dashboard, Home Assistant, or boot
             set the current mic state. -->
        <div id="acMicSourceRow" style="display:none; font-size:0.8rem; color:var(--muted); margin:-4px 0 8px 22px;">
          <span id="acMicSourceText"></span>
        </div>
        <div class="stats-grid" style="margin-top:14px;">
          <div class="stat-item"><div class="stat-label">T3 cycles seen</div><div class="stat-value" id="acT3">0</div></div>
          <div class="stat-item"><div class="stat-label">T4 cycles seen</div><div class="stat-value" id="acT4">0</div></div>
          <div class="stat-item"><div class="stat-label">Envelope frames</div><div class="stat-value" id="acFrames">0</div></div>
          <div class="stat-item"><div class="stat-label">On-transitions</div><div class="stat-value" id="acOn">0</div></div>
          <div class="stat-item"><div class="stat-label">Off-transitions</div><div class="stat-value" id="acOff">0</div></div>
          <div class="stat-item"><div class="stat-label">I2S errors</div><div class="stat-value" id="acErr">0</div></div>
        </div>
        <!-- Test panel: live level meter + alarm pattern self-test.
             Hidden by default to avoid leaving a visible "loudness number"
             on screen in normal use; user opens it explicitly. -->
        <details style="margin-top:14px;">
          <summary style="cursor:pointer; color:var(--muted); font-size:0.92rem;">
            Test the microphone
          </summary>
          <div style="margin-top:10px; padding:10px; background:rgba(255,255,255,0.02); border-radius:10px;">
            <div style="font-size:0.88rem; color:var(--muted); margin-bottom:6px;">
              <strong>Step 1 — Is the mic alive?</strong> Clap or speak near
              the device. The bar below moves with the current 20 ms loudness
              number — the only thing this module ever sees. The two notches
              are the OFF (clear) and ON (alarm) thresholds.
            </div>
            <div class="audio-meter" id="acMeter">
              <div class="audio-meter__fill" id="acMeterFill"></div>
              <div class="audio-meter__notch" id="acMeterOff" style="left:5%;"></div>
              <div class="audio-meter__notch" id="acMeterOn" style="left:12%;"></div>
            </div>
            <div class="audio-meter__legend">
              <span id="acMeterRms">RMS 0</span>
              <span id="acMeterFlags">·</span>
            </div>
            <div class="audio-trace" id="acTrace" title="Last 16 on/off transitions, newest on the right"></div>
            <div style="font-size:0.88rem; color:var(--muted); margin:12px 0 6px;">
              <strong>Step 2 — Does the pattern detector work?</strong>
              Press the physical TEST button on your smoke or CO alarm with
              the device within ~3 m, then click below. A match here does
              <em>not</em> fire any Home Assistant automation — it's a
              detection-only test.
            </div>
            <div style="display:flex; gap:10px; align-items:center; flex-wrap:wrap;">
              <button class="btn btn-primary btn-sm" id="acTestBtn" onclick="startAudioSelftest()">
                Listen for 30 s
              </button>
              <span id="acTestStatus" style="font-size:0.9rem; color:var(--muted);">
                Idle.
              </span>
            </div>

            <div style="font-size:0.88rem; color:var(--muted); margin:14px 0 6px;">
              <strong>No alarm handy?</strong> This browser can synthesize
              a T3 or T4 cadence so you can verify the detector works
              without setting off a real alarm. Hold your phone or
              laptop within ~30 cm of the Canary, click <em>Listen for
              30 s</em> above, then start a tone below. This is a
              <em>synthetic</em> test pattern — real alarms vary in
              frequency and reverberation. Always also test against an
              actual alarm before trusting the detector.
            </div>
            <div style="display:flex; gap:8px; align-items:center; flex-wrap:wrap;">
              <button class="btn btn-secondary btn-sm" id="acToneT3Btn" onclick="toggleToneT3()">
                Play T3 (smoke)
              </button>
              <button class="btn btn-secondary btn-sm" id="acToneT4Btn" onclick="toggleToneT4()">
                Play T4 (CO)
              </button>
              <span id="acToneStatus" style="font-size:0.85rem; color:var(--muted);">
                Tone off.
              </span>
            </div>
          </div>
        </details>
      </div>

      <!-- Touch (silent panic / enclosure tamper / approach) -->
      <div class="card" id="touchCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Touch</div>
            <div class="card-subtitle">
              A capacitive pad wired to a side GPIO. Three behaviours, all
              local: a silent panic long-press, an enclosure-tamper
              detector, and an optional proximity hint. No audio, no
              video.
            </div>
          </div>
        </div>
        <div id="touchHero" style="text-align:center; padding:18px 16px;">
          <div id="touchPill" class="sensing-pill sensing-pill--quiet">Idle</div>
          <div id="touchExplain" class="sensing-explain">
            The pad is calibrating its baseline — give it a couple of seconds.
          </div>
        </div>
        <div class="stats-grid">
          <div class="stat-item"><div class="stat-label">Pad channel</div><div class="stat-value" id="tcChan">--</div></div>
          <div class="stat-item"><div class="stat-label">Baseline</div><div class="stat-value" id="tcBase">--</div></div>
          <div class="stat-item"><div class="stat-label">Reading</div><div class="stat-value" id="tcVal">--</div></div>
          <div class="stat-item"><div class="stat-label">Panic events</div><div class="stat-value" id="tcPanic">0</div></div>
          <div class="stat-item"><div class="stat-label">Tamper events</div><div class="stat-value" id="tcTamper">0</div></div>
          <div class="stat-item"><div class="stat-label">Total reads</div><div class="stat-value" id="tcReads">0</div></div>
        </div>
      </div>

      <!-- IR appliance activity (NEC / RC5 / Sony remote presses) -->
      <div class="card" id="irCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Appliance activity</div>
            <div class="card-subtitle">
              An IR receiver listens for the standard remote-control
              cadences. The device records that something <i>was</i>
              pressed — never which button, channel, or temperature
              setting. The hash bucket below differs by remote within
              one session and resets on every reboot.
            </div>
          </div>
        </div>
        <div id="irHero" style="text-align:center; padding:18px 16px;">
          <div id="irPill" class="sensing-pill sensing-pill--quiet">No activity</div>
          <div id="irExplain" class="sensing-explain">
            The household's IR remotes are silent right now.
          </div>
        </div>
        <div class="stats-grid">
          <div class="stat-item"><div class="stat-label">Last protocol</div><div class="stat-value" id="irProto">--</div></div>
          <div class="stat-item"><div class="stat-label">Hash bucket</div><div class="stat-value" id="irBucket">--</div></div>
          <div class="stat-item"><div class="stat-label">Frames received</div><div class="stat-value" id="irRx">0</div></div>
          <div class="stat-item"><div class="stat-label">Frames decoded</div><div class="stat-value" id="irDec">0</div></div>
          <div class="stat-item"><div class="stat-label">Frames unknown</div><div class="stat-value" id="irUnk">0</div></div>
          <div class="stat-item"><div class="stat-label">Events emitted</div><div class="stat-value" id="irEvt">0</div></div>
        </div>
      </div>

      <!-- Vision detection -->
      <div class="card" id="visionCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Vision Detection</div>
            <div class="card-subtitle">3-layer cascaded pipeline. Click grid cells to mask zones.</div>
          </div>
          <span id="visionBadge" class="badge" style="display:none;"><span class="badge-dot"></span><span id="visionBadgeText">Idle</span></span>
        </div>
        <div id="visionHero" style="text-align:center; padding:14px 16px;">
          <div id="visionPill" class="sensing-pill sensing-pill--quiet">Idle</div>
          <div id="visionExplain" class="sensing-explain">Waiting for motion events.</div>
        </div>
        <div style="display:grid;grid-template-columns:repeat(10, 1fr);gap:2px;margin:0.75rem 0;padding:0 0.5rem;" id="visionGrid"></div>
        <div class="stats-grid">
          <div class="stat-item"><div class="stat-label">Frames</div><div class="stat-value" id="viFrames">0</div></div>
          <div class="stat-item"><div class="stat-label">L1 pass</div><div class="stat-value" id="viL1">0</div></div>
          <div class="stat-item"><div class="stat-label">L2 pass</div><div class="stat-value" id="viL2">0</div></div>
          <div class="stat-item"><div class="stat-label">L3 pass</div><div class="stat-value" id="viL3">0</div></div>
          <div class="stat-item"><div class="stat-label">Motion events</div><div class="stat-value" id="viMotion">0</div></div>
          <div class="stat-item"><div class="stat-label">Person events</div><div class="stat-value" id="viPerson">0</div></div>
        </div>
      </div>

      <!-- Vision settings (tuning card) -->
      <div class="card" id="visionSettingsCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Vision Tuning</div>
            <div class="card-subtitle">
              Adjust detection thresholds and timing live. Changes take
              effect immediately. Click Save to persist across reboots.
            </div>
          </div>
          <div style="display:flex;gap:6px;align-items:center;">
            <span id="viSaveStatus" style="font-size:0.7rem;color:var(--muted);"></span>
            <button id="viSaveBtn" class="badge" style="cursor:pointer;border:none;font-size:0.7rem;" onclick="viSaveConfig()">Save</button>
            <button id="viResetBtn" class="badge" style="cursor:pointer;border:none;font-size:0.7rem;" onclick="viResetDefaults()">Reset defaults</button>
          </div>
        </div>
        <div style="padding:0 1rem 1rem;">
          <div class="vi-tune">
            <label>L1 — JPEG delta threshold <span id="viTJpeg">15</span>%</label>
            <input type="range" id="viJpeg" min="1" max="100" step="1" value="15"
                   oninput="viSlider('jpeg_delta_pct', this.value, 'viTJpeg', '%')">
          </div>
          <div class="vi-tune">
            <label>L2 — Block change threshold <span id="viTBlock">20</span>%</label>
            <input type="range" id="viBlock" min="1" max="100" step="1" value="20"
                   oninput="viSlider('block_change_pct', this.value, 'viTBlock', '%')">
          </div>
          <div class="vi-tune">
            <label>L2 — Luminance delta <span id="viTLum">20</span></label>
            <input type="range" id="viLum" min="1" max="255" step="1" value="20"
                   oninput="viSlider('luminance_threshold', this.value, 'viTLum', '')">
          </div>
          <div class="vi-tune">
            <label>L3 — Person confidence <span id="viTPerson">60</span>%</label>
            <input type="range" id="viPerson2" min="1" max="100" step="1" value="60"
                   oninput="viSlider('person_confidence_min', this.value, 'viTPerson', '%')">
          </div>
          <div class="vi-tune">
            <label>Sample interval <span id="viTInterval">200</span> ms</label>
            <input type="range" id="viInterval" min="50" max="5000" step="50" value="200"
                   oninput="viSlider('process_interval_ms', this.value, 'viTInterval', ' ms')">
          </div>
          <div class="vi-tune">
            <label>Motion hold <span id="viTHold">3000</span> ms</label>
            <input type="range" id="viHold" min="500" max="30000" step="500" value="3000"
                   oninput="viSlider('motion_hold_ms', this.value, 'viTHold', ' ms')">
          </div>
          <div class="vi-tune">
            <label>Duty cycle <span id="viTDuty">50</span>% active</label>
            <input type="range" id="viDuty" min="10" max="100" step="5" value="50"
                   oninput="viSlider('duty_active_pct', this.value, 'viTDuty', '% active')">
          </div>
        </div>
      </div>

      <!-- Internal temperature drift (tamper) -->
      <div class="card" id="tempCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Thermal drift</div>
            <div class="card-subtitle">
              The chip's own die temperature is sampled once a minute and
              compared against a learned baseline. A sudden ±5 °C step
              while the device is otherwise idle is consistent with the
              case being opened or the device being moved between rooms.
            </div>
          </div>
        </div>
        <div id="tempHero" style="text-align:center; padding:18px 16px;">
          <div id="tempPill" class="sensing-pill sensing-pill--quiet">Stable</div>
          <div id="tempExplain" class="sensing-explain">
            The internal temperature is steady — no tamper indicators.
          </div>
        </div>
        <div class="stats-grid">
          <div class="stat-item"><div class="stat-label">Baseline (°C)</div><div class="stat-value" id="thBase">--</div></div>
          <div class="stat-item"><div class="stat-label">Current (°C)</div><div class="stat-value" id="thNow">--</div></div>
          <div class="stat-item"><div class="stat-label">Samples</div><div class="stat-value" id="thSamples">0</div></div>
          <div class="stat-item"><div class="stat-label">Drift events</div><div class="stat-value" id="thDrift">0</div></div>
        </div>
      </div>

      <!-- Power (boot / wake reason; sleep capability) -->
      <div class="card" id="powerCard" style="display:none;">
        <div class="card-header">
          <div>
            <div class="card-title">Power & wake</div>
            <div class="card-subtitle">
              ESP32-S3 native deep-sleep abstraction. The chip can be woken
              by a timer, the touch peripheral, an RTC GPIO, or the ULP
              coprocessor.
            </div>
          </div>
        </div>
        <div class="stats-grid">
          <div class="stat-item"><div class="stat-label">Last wake</div><div class="stat-value" id="lpWake">--</div></div>
          <div class="stat-item"><div class="stat-label">Wake pad</div><div class="stat-value" id="lpWakePad">--</div></div>
          <div class="stat-item"><div class="stat-label">Caps</div><div class="stat-value" id="lpCaps" style="font-size:0.8rem;">--</div></div>
        </div>
      </div>

      <!-- Diagnostics -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Driver health</div>
            <div class="card-subtitle">For your installer / Home Assistant</div>
          </div>
        </div>
        <div class="stats-grid">
          <div class="stat-item"><div class="stat-label">Frames in window</div><div class="stat-value" id="snFramesWin">0</div></div>
          <div class="stat-item"><div class="stat-label">Dropped est.</div><div class="stat-value" id="snDropped">0</div></div>
          <div class="stat-item"><div class="stat-label">Channel</div><div class="stat-value" id="snChan">--</div></div>
          <div class="stat-item"><div class="stat-label">Bandwidth</div><div class="stat-value" id="snBw">--</div></div>
          <div class="stat-item"><div class="stat-label">Windows seen</div><div class="stat-value" id="snWindows">0</div></div>
          <div class="stat-item"><div class="stat-label">Last window age</div><div class="stat-value" id="snAge">--</div></div>
          <div class="stat-item"><div class="stat-label">Frames received</div><div class="stat-value" id="snRx">0</div></div>
          <div class="stat-item"><div class="stat-label">Drop: rate-limit</div><div class="stat-value" id="snDropRate">0</div></div>
          <div class="stat-item"><div class="stat-label">Drop: RSSI floor</div><div class="stat-value" id="snDropRssi">0</div></div>
          <div class="stat-item"><div class="stat-label">Drop: ring full</div><div class="stat-value" id="snDropFull">0</div></div>
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
          <button class="btn btn-ghost btn-sm" onclick="resetSensorDefaults()" title="Reset sensor to surveillance defaults">Reset</button>
        </div>
        <div style="display:flex;gap:0.75rem;flex-wrap:wrap;">
          <button class="btn btn-secondary" onclick="takeSnapshot()">Snapshot</button>
          <button class="btn btn-ghost" onclick="refreshPeekStatus()">Refresh Status</button>
        </div>

        <!-- Presets -->
        <div style="margin-top:1rem;">
          <div class="form-label">Scene Presets</div>
          <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
            <button class="btn btn-sm btn-primary" onclick="applyPreset('dynamic')">Dynamic Auto</button>
            <button class="btn btn-sm" onclick="applyPreset('indoor')">Indoor</button>
            <button class="btn btn-sm" onclick="applyPreset('outdoor')">Outdoor</button>
            <button class="btn btn-sm" onclick="applyPreset('low_light')">Low Light</button>
            <button class="btn btn-sm" onclick="applyPreset('night')">Night</button>
            <button class="btn btn-sm" onclick="applyPreset('high_contrast')">High Contrast</button>
          </div>
        </div>

        <!-- Resolution Control -->
        <div style="margin-top:1rem;">
          <div class="form-label">Resolution</div>
          <div class="resolution-selector" id="resolutionSelector">
            <button class="resolution-btn" data-size="4" onclick="setResolution(4)">320x240</button>
            <button class="resolution-btn active" data-size="8" onclick="setResolution(8)">640x480</button>
            <button class="resolution-btn" data-size="9" onclick="setResolution(9)">800x600</button>
            <button class="resolution-btn" data-size="10" onclick="setResolution(10)">1024x768</button>
            <button class="resolution-btn" data-size="11" onclick="setResolution(11)">1280x720</button>
          </div>
          <p id="resolutionStatus" style="font-size:0.7rem;color:var(--muted);margin-top:0.5rem;">Current: 640x480</p>
        </div>

        <!-- Sensor tuning — writes straight through to OV2640/OV3660 via /api/peek/sensor -->
        <div style="margin-top:1.25rem;">
          <div class="form-label">Picture Quality</div>
          <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:0.6rem 1.25rem;font-size:0.82rem;">
            <label>JPEG Quality (lower = sharper): <span id="sensorQualityVal">--</span>
              <input type="range" id="sensorQuality" min="4" max="40" step="1" oninput="onSensorSlider('quality', this.value, 'sensorQualityVal')">
            </label>
            <label>Brightness: <span id="sensorBrightnessVal">--</span>
              <input type="range" id="sensorBrightness" min="-2" max="2" step="1" oninput="onSensorSlider('brightness', this.value, 'sensorBrightnessVal')">
            </label>
            <label>Contrast: <span id="sensorContrastVal">--</span>
              <input type="range" id="sensorContrast" min="-2" max="2" step="1" oninput="onSensorSlider('contrast', this.value, 'sensorContrastVal')">
            </label>
            <label>Saturation: <span id="sensorSaturationVal">--</span>
              <input type="range" id="sensorSaturation" min="-2" max="2" step="1" oninput="onSensorSlider('saturation', this.value, 'sensorSaturationVal')">
            </label>
            <label>AE Level (exposure bias): <span id="sensorAeLevelVal">--</span>
              <input type="range" id="sensorAeLevel" min="-2" max="2" step="1" oninput="onSensorSlider('ae_level', this.value, 'sensorAeLevelVal')">
            </label>
            <label>Frame delay (ms): <span id="sensorFrameDelayVal">--</span>
              <input type="range" id="sensorFrameDelay" min="20" max="200" step="5" oninput="onSensorSlider('frame_delay_ms', this.value, 'sensorFrameDelayVal')">
            </label>
          </div>
        </div>

        <div style="margin-top:1rem;">
          <div class="form-label">Exposure / Gain</div>
          <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:0.4rem 1rem;font-size:0.82rem;">
            <label><input type="checkbox" id="sensorAec" onchange="onSensorToggle('aec', this.checked)"> Auto Exposure (AEC)</label>
            <label><input type="checkbox" id="sensorAec2" onchange="onSensorToggle('aec2', this.checked)"> AEC DSP (AEC2)</label>
            <label><input type="checkbox" id="sensorAgc" onchange="onSensorToggle('agc', this.checked)"> Auto Gain (AGC)</label>
            <label>Gain ceiling
              <select id="sensorGainCeiling" onchange="onSensorSelect('gainceiling', this.value)">
                <option value="0">2x</option><option value="1">4x</option>
                <option value="2">8x</option><option value="3">16x</option>
                <option value="4">32x</option><option value="5">64x</option>
                <option value="6">128x</option>
              </select>
            </label>
          </div>
        </div>

        <div style="margin-top:1rem;">
          <div class="form-label">White Balance</div>
          <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:0.4rem 1rem;font-size:0.82rem;">
            <label><input type="checkbox" id="sensorAwb" onchange="onSensorToggle('awb', this.checked)"> Auto WB</label>
            <label><input type="checkbox" id="sensorAwbGain" onchange="onSensorToggle('awb_gain', this.checked)"> AWB Gain</label>
            <label>WB Mode
              <select id="sensorWbMode" onchange="onSensorSelect('wb_mode', this.value)">
                <option value="0">Auto</option><option value="1">Sunny</option>
                <option value="2">Cloudy</option><option value="3">Office</option>
                <option value="4">Home</option>
              </select>
            </label>
            <label>Special Effect
              <select id="sensorSpecialEffect" onchange="onSensorSelect('special_effect', this.value)">
                <option value="0">None</option><option value="1">Negative</option>
                <option value="2">Grayscale</option><option value="3">Red Tint</option>
                <option value="4">Green Tint</option><option value="5">Blue Tint</option>
                <option value="6">Sepia</option>
              </select>
            </label>
          </div>
        </div>

        <div style="margin-top:1rem;">
          <div class="form-label">Image Cleanup &amp; Orientation</div>
          <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:0.4rem 1rem;font-size:0.82rem;">
            <label><input type="checkbox" id="sensorBpc" onchange="onSensorToggle('bpc', this.checked)"> Black-pixel correct</label>
            <label><input type="checkbox" id="sensorWpc" onchange="onSensorToggle('wpc', this.checked)"> White-pixel correct</label>
            <label><input type="checkbox" id="sensorRawGma" onchange="onSensorToggle('raw_gma', this.checked)"> Gamma</label>
            <label><input type="checkbox" id="sensorLenc" onchange="onSensorToggle('lenc', this.checked)"> Lens correction</label>
            <label><input type="checkbox" id="sensorDcw" onchange="onSensorToggle('dcw', this.checked)"> DCW (downsize)</label>
            <label><input type="checkbox" id="sensorHmirror" onchange="onSensorToggle('hmirror', this.checked)"> H-mirror</label>
            <label><input type="checkbox" id="sensorVflip" onchange="onSensorToggle('vflip', this.checked)"> V-flip</label>
            <label><input type="checkbox" id="sensorColorbar" onchange="onSensorToggle('colorbar', this.checked)"> Test pattern</label>
          </div>
        </div>

        <div id="snapshotPreview" style="margin-top:1rem;display:none;">
          <div class="form-label">Snapshot</div>
          <img id="snapshotImg" style="max-width:100%;border-radius:8px;border:1px solid var(--border);" alt="Snapshot">
        </div>
      </div>

      <!-- Camera Info — live stream metrics from /api/peek/status -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Stream Metrics</div>
            <div class="card-subtitle">Updates every second while streaming</div>
          </div>
          <span id="camInfoLive" class="badge info" style="display:none;"><span class="badge-dot"></span>LIVE</span>
        </div>
        <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:0.6rem 1rem;font-size:0.82rem;">
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Sensor</div><div id="camSensor">--</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Resolution</div><div id="camRes">--</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Frame Rate</div><div id="camFps">idle</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Last Frame</div><div id="camLastFrame">idle</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Throughput</div><div id="camKbps">idle</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Frames Sent</div><div id="camFrameCount">idle</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Stream Uptime</div><div id="camUptime">idle</div></div>
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
    // Bearer credential injected by handle_ui() at render time (Phase 2.5).
    // In the unprovisioned/dev-preview path the placeholder stays in place
    // and the api() helper skips the Authorization header — requests then
    // fail closed on the server side.
    const CV_TOKEN = '__CV_TOKEN__';
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
      else if (panel === 'peek') { refreshPeekStatus(); refreshSensorState(); }
      else if (panel === 'opera') refreshOpera();
      else if (panel === 'community') refreshChirpStatus();
      else if (panel === 'bluetooth') { refreshBtStatus(); loadBtPairedDevices(); }
      else if (panel === 'sensing') refreshSensing();
      else if (panel === 'status') refreshLiveSensing();

      // Stop peek stream and metrics polling when leaving peek panel
      if (panel !== 'peek') {
        stopCamInfoPolling();
        if (peekActive) stopPeek();
      }
    }

    // API calls
    async function api(endpoint, method = 'GET', body = null) {
      const opts = { method, headers: {} };
      if (CV_TOKEN && CV_TOKEN.charAt(0) !== '_') {
        opts.headers['Authorization'] = 'Bearer ' + CV_TOKEN;
      }
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
      if (data && data.ok) {
        const wasReady = cameraReady;
        cameraReady = data.camera_initialized;
        peekActive = data.peek_active;
        if (typeof data.resolution !== 'undefined') {
          currentResolution = data.resolution;
          updateResolutionUI();
        }
        applyCameraInfo(data);
        updatePeekUI();
        if (peekActive) startCamInfoPolling(); else stopCamInfoPolling();
        if (cameraReady && !wasReady) refreshSensorState();
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
      setTimeout(refreshPeekStatus, 300);
    }

    async function stopPeek() {
      const stream = document.getElementById('peekStream');

      // Set peekActive false BEFORE clearing src to prevent onerror from showing error
      peekActive = false;
      stopCamInfoPolling();

      // Clear the stream source (this triggers onerror, but peekActive is false so it won't show error)
      stream.src = '';

      // Tell server to stop
      await api('/api/peek/stop', 'POST');

      updatePeekUI();
      refreshPeekStatus();
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

    // ══════════════════════════════════════════════════════════════════
    // STREAM METRICS — 1s polling while streaming
    // ══════════════════════════════════════════════════════════════════
    let camInfoTimer = null;

    function fmtBytes(n) {
      if (n == null || isNaN(n)) return '--';
      if (n < 1024) return n + ' B';
      if (n < 1024*1024) return (n/1024).toFixed(1) + ' KB';
      return (n/1048576).toFixed(2) + ' MB';
    }
    function fmtDuration(ms) {
      if (!ms || ms < 0) return '--';
      const s = Math.floor(ms / 1000);
      if (s < 60) return s + 's';
      const m = Math.floor(s / 60);
      return m + 'm ' + (s % 60) + 's';
    }

    function applyCameraInfo(data) {
      if (!data || !data.ok) return;
      setText('camSensor', data.sensor_model || '--');
      setText('camRes', data.resolution_name || '--');

      const hasMetrics = data.frame_count != null && data.frame_count > 0;
      const live = document.getElementById('camInfoLive');

      if (data.peek_active) {
        setText('camFps', data.fps != null ? data.fps + ' fps' : '--');
        setText('camLastFrame', fmtBytes(data.last_frame_bytes));
        setText('camKbps', data.avg_kbps != null ? data.avg_kbps + ' kbps' : '--');
        setText('camFrameCount', String(data.frame_count));
        setText('camUptime', fmtDuration(data.stream_uptime_ms));
        if (live) { live.textContent = 'LIVE'; live.className = 'badge info'; live.style.display = 'inline-flex'; }
      } else if (hasMetrics) {
        setText('camFps', data.fps != null ? data.fps + ' fps' : '--');
        setText('camLastFrame', fmtBytes(data.last_frame_bytes));
        setText('camKbps', data.avg_kbps != null ? data.avg_kbps + ' kbps' : '--');
        setText('camFrameCount', String(data.frame_count));
        setText('camUptime', fmtDuration(data.stream_uptime_ms));
        if (live) { live.textContent = 'LAST STREAM'; live.className = 'badge'; live.style.display = 'inline-flex'; }
      } else {
        ['camFps','camLastFrame','camKbps','camFrameCount','camUptime'].forEach(id => setText(id, 'idle'));
        if (live) live.style.display = 'none';
      }
    }

    function startCamInfoPolling() {
      if (camInfoTimer) return;
      camInfoTimer = setInterval(refreshPeekStatus, 1000);
    }
    function stopCamInfoPolling() {
      if (camInfoTimer) { clearInterval(camInfoTimer); camInfoTimer = null; }
    }

    // ══════════════════════════════════════════════════════════════════
    // SENSOR TUNING
    // Real-time read/write of OV2640/OV3660 sensor parameters via
    // /api/peek/sensor. Slider events are coalesced into one POST per
    // ~200ms so dragging doesn't spam the device.
    // ══════════════════════════════════════════════════════════════════
    let sensorPostTimer = null;
    let sensorPendingPatch = {};

    function flushSensorPatch() {
      sensorPostTimer = null;
      const patch = sensorPendingPatch;
      sensorPendingPatch = {};
      if (Object.keys(patch).length === 0) return;
      api('/api/peek/sensor', 'POST', patch).then(data => {
        if (data && data.ok) applySensorState(data);
      });
    }
    function queueSensorPatch(patch) {
      Object.assign(sensorPendingPatch, patch);
      if (!sensorPostTimer) sensorPostTimer = setTimeout(flushSensorPatch, 200);
    }
    function onSensorSlider(field, value, valId) {
      const v = parseInt(value, 10);
      const el = document.getElementById(valId);
      if (el) el.textContent = String(v);
      queueSensorPatch({ [field]: v });
    }
    function onSensorToggle(field, checked) {
      queueSensorPatch({ [field]: checked ? 1 : 0 });
    }
    function onSensorSelect(field, value) {
      queueSensorPatch({ [field]: parseInt(value, 10) });
    }

    function setText(id, v) { const el = document.getElementById(id); if (el) el.textContent = (v != null) ? String(v) : '--'; }
    function setVal(id, v) { const el = document.getElementById(id); if (el != null && v != null) el.value = v; }
    function setChecked(id, v) { const el = document.getElementById(id); if (el != null) el.checked = !!v; }

    function applySensorState(data) {
      if (!data || !data.ok) return;
      setVal('sensorQuality',     data.quality);     setText('sensorQualityVal',    data.quality);
      setVal('sensorBrightness',  data.brightness);  setText('sensorBrightnessVal', data.brightness);
      setVal('sensorContrast',    data.contrast);    setText('sensorContrastVal',   data.contrast);
      setVal('sensorSaturation',  data.saturation);  setText('sensorSaturationVal', data.saturation);
      setVal('sensorAeLevel',     data.ae_level);    setText('sensorAeLevelVal',    data.ae_level);
      setVal('sensorFrameDelay',  data.frame_delay_ms); setText('sensorFrameDelayVal', data.frame_delay_ms);
      setChecked('sensorAec',       data.aec);
      setChecked('sensorAec2',      data.aec2);
      setChecked('sensorAgc',       data.agc);
      setChecked('sensorAwb',       data.awb);
      setChecked('sensorAwbGain',   data.awb_gain);
      setChecked('sensorBpc',       data.bpc);
      setChecked('sensorWpc',       data.wpc);
      setChecked('sensorRawGma',    data.raw_gma);
      setChecked('sensorLenc',      data.lenc);
      setChecked('sensorDcw',       data.dcw);
      setChecked('sensorHmirror',   data.hmirror);
      setChecked('sensorVflip',     data.vflip);
      setChecked('sensorColorbar',  data.colorbar);
      setVal('sensorGainCeiling',   data.gainceiling);
      setVal('sensorWbMode',        data.wb_mode);
      setVal('sensorSpecialEffect', data.special_effect);
    }

    async function refreshSensorState() {
      const data = await api('/api/peek/sensor');
      if (data && data.ok) applySensorState(data);
    }

    async function resetSensorDefaults() {
      const data = await api('/api/peek/sensor', 'POST', { reset_defaults: true });
      if (data && data.ok) applySensorState(data);
    }

    async function applyPreset(name) {
      const data = await api('/api/peek/sensor', 'POST', { preset: name });
      if (data && data.ok) applySensorState(data);
    }

    // ════════════════════════════════════════════════════════════════
    // Sensing — live CSI motion / breathing / activity visualization
    // ════════════════════════════════════════════════════════════════
    function buildSensingBars() {
      const amp = document.getElementById('ampBars');
      if (amp && !amp.dataset.built) {
        for (let i = 0; i < 8; i++) {
          const d = document.createElement('div');
          d.className = 'sensing-bar';
          d.style.height = '3px';
          amp.appendChild(d);
        }
        amp.dataset.built = '1';
      }
      const br = document.getElementById('breathBars');
      if (br && !br.dataset.built) {
        for (let i = 0; i < 8; i++) {
          const d = document.createElement('div');
          d.className = 'sensing-bar';
          d.style.height = '3px';
          br.appendChild(d);
        }
        br.dataset.built = '1';
      }
      const arr = document.getElementById('dopArrows');
      if (arr && !arr.dataset.built) {
        for (let i = 0; i < 4; i++) {
          const d = document.createElement('div');
          d.className = 'sensing-arrow';
          d.textContent = '·';
          arr.appendChild(d);
        }
        arr.dataset.built = '1';
      }
    }

    function setArc(elId, pct) {
      // The SVG arc path is 125.66 units long (40-radius hemisphere); we
      // animate stroke-dasharray to fill 0..pct of it.
      const e = document.getElementById(elId);
      if (!e) return;
      const len = 125.66;
      const fill = Math.max(0, Math.min(100, pct)) / 100 * len;
      e.setAttribute('stroke-dasharray', fill + ' 200');
    }

    const SENSING_EXPLAIN = {
      offline:  'Sensing is starting up. The radio needs WiFi to be running before CSI frames arrive.',
      quiet:    'The room looks empty or perfectly still. The radio sees no movement larger than a still chair.',
      presence: 'Steady micro-motion — the kind of signal a person sitting and breathing makes. No camera, no microphone.',
      motion:   'Clear room-scale movement. Someone is walking or moving objects.',
      active:   'Sustained, vigorous activity. Multiple people, or one person moving fast.'
    };

    async function refreshSensing() {
      buildSensingBars();
      const data = await api('/api/sensing');
      if (!data || !data.ok) {
        // Endpoint disabled (build without FEATURE_CSI) or offline.
        document.getElementById('sensingPill').className = 'sensing-pill sensing-pill--offline';
        document.getElementById('sensingPill').textContent = 'Unavailable';
        document.getElementById('sensingExplain').textContent =
          'This build was compiled without WiFi-CSI sensing.';
        return;
      }

      // Hero pill
      const label = data.label || 'offline';
      const pill = document.getElementById('sensingPill');
      pill.className = 'sensing-pill sensing-pill--' + label;
      pill.textContent = label;
      document.getElementById('sensingExplain').textContent =
        SENSING_EXPLAIN[label] || SENSING_EXPLAIN.offline;

      // Gauges
      const motion = data.motion | 0;
      const breath = data.breathing | 0;
      document.getElementById('motionVal').textContent = motion;
      document.getElementById('breathVal').textContent = breath;
      setArc('motionArc', motion);
      setArc('breathArc', breath);

      const rssi = data.rssi_dbm;
      document.getElementById('rssiVal').textContent =
        (rssi === undefined || rssi === 0) ? '--' : rssi;
      // RSSI gauge: -90 dBm → 0%, -30 dBm → 100%
      const rssiPct = (rssi === undefined) ? 0 : Math.max(0, Math.min(100, (rssi + 90) * 100 / 60));
      setArc('rssiArc', rssiPct);

      // Bar graphs
      const ampMax = 100;  // amp_bands are int8 0..127 in our scaling; clamp 100
      const ampEls = document.querySelectorAll('#ampBars .sensing-bar');
      (data.amp_bands || []).forEach((v, i) => {
        if (!ampEls[i]) return;
        const h = Math.max(3, Math.min(100, Math.abs(v) * 100 / ampMax));
        ampEls[i].style.height = h + '%';
      });
      const brEls = document.querySelectorAll('#breathBars .sensing-bar');
      (data.breathing_bins || []).forEach((v, i) => {
        if (!brEls[i]) return;
        const h = Math.max(3, Math.min(100, Math.abs(v) * 100 / 100));
        brEls[i].style.height = h + '%';
      });

      // Doppler arrows — sign-aware; ↑ for positive, ↓ for negative, · for ~0
      const arrowEls = document.querySelectorAll('#dopArrows .sensing-arrow');
      (data.doppler || []).forEach((v, i) => {
        if (!arrowEls[i]) return;
        if (v > 8)        { arrowEls[i].textContent = '↑'; arrowEls[i].className = 'sensing-arrow sensing-arrow--pos'; }
        else if (v < -8)  { arrowEls[i].textContent = '↑'; arrowEls[i].className = 'sensing-arrow sensing-arrow--neg'; }
        else              { arrowEls[i].textContent = '·'; arrowEls[i].className = 'sensing-arrow'; }
      });

      // Diagnostics
      const set = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; };
      set('snFramesWin', data.frames_in_window | 0);
      set('snDropped',   data.dropped_estimate | 0);
      set('snChan',      data.channel || '--');
      set('snBw',        (data.bandwidth_code === 1) ? 'HT40' : 'HT20');
      set('snWindows',   data.windows_seen | 0);
      const age = data.last_window_age_ms;
      set('snAge', (age === undefined || age < 0) ? '--' : (age + ' ms'));
      const st = data.stats || {};
      set('snRx',       st.frames_received || 0);
      set('snDropRate', st.frames_dropped_rate || 0);
      set('snDropRssi', st.frames_dropped_rssi || 0);
      set('snDropFull', st.frames_dropped_full || 0);

      // ─── Acoustic alarms (T3 smoke / T4 CO) ──────────────────────────
      // Card stays hidden if the firmware build doesn't include
      // FEATURE_ACOUSTIC_EVENTS (the acoustic key won't be in the JSON).
      // CRITICAL: do NOT early-return here — the touch/ir/temp/power cards
      // render after this section and any return would hide them all.
      const acCard = document.getElementById('acousticCard');
      const ac = data.acoustic;
      if (ac) {
        if (acCard) acCard.style.display = '';

        const acPill = document.getElementById('acousticPill');
        const acExp  = document.getElementById('acousticExplain');
        const evtName = ac.last_event || 'none';
        // NB: variable name MUST NOT be `age` — that's already taken by
        // the CSI block above. JS would throw `Identifier 'age' has
        // already been declared` at parse time, breaking refreshSensing()
        // entirely.
        const acAge = ac.last_event_age_ms;
        const acMuted = ac.muted === true;

        if (acMuted) {
          acPill.className = 'sensing-pill sensing-pill--muted';
          acPill.textContent = '🚫 Mic muted';
          acExp.textContent = 'You have muted the microphone. The I2S driver is uninstalled and GPIO 41/42 are released. Unmute below to listen for smoke / CO alarm cadences again.';
        } else if (!ac.enabled) {
          acPill.className = 'sensing-pill sensing-pill--offline';
          acPill.textContent = 'Mic offline';
          acExp.textContent = 'The PDM microphone failed to start. Check the device serial log.';
        } else if (evtName === 'smoke_alarm_t3' && acAge >= 0 && acAge < 30000) {
          acPill.className = 'sensing-pill sensing-pill--active';
          acPill.textContent = '🔥 Smoke alarm pattern';
          acExp.textContent = 'NFPA 72 / ISO 8201 cadence detected — your smoke alarm is sounding.';
        } else if (evtName === 'co_alarm_t4' && acAge >= 0 && acAge < 30000) {
          acPill.className = 'sensing-pill sensing-pill--active';
          acPill.textContent = '⚠ CO alarm pattern';
          acExp.textContent = 'UL 2034 cadence detected — your carbon monoxide alarm is sounding.';
        } else if (evtName === 'glass_break' && acAge >= 0 && acAge < 5000) {
          acPill.className = 'sensing-pill sensing-pill--active';
          acPill.textContent = '💥 Glass break pattern';
          acExp.textContent = 'Sustained high-band transient — heuristic match, NOT a UL-listed glass-break sensor. Verify before acting on it.';
        } else if (evtName === 'doorbell' && acAge >= 0 && acAge < 5000) {
          acPill.className = 'sensing-pill sensing-pill--active';
          acPill.textContent = '🔔 Doorbell pattern';
          acExp.textContent = 'Two-tone chime detected — heuristic match. Modern wireless / melodic doorbells may not register.';
        } else if (evtName === 'knock' && acAge >= 0 && acAge < 5000) {
          acPill.className = 'sensing-pill sensing-pill--active';
          acPill.textContent = '🥁 Knock pattern';
          acExp.textContent = 'Three even impulses with low-band character — heuristic match. May also fire on drum hits or hand claps.';
        } else {
          acPill.className = 'sensing-pill sensing-pill--quiet';
          acPill.textContent = 'No alarms';
          acExp.textContent = 'Listening for the standard NFPA 72 (smoke) and UL 2034 (CO) cadences. Nothing detected.';
        }

        // Mic status row (dot + label + mute button).
        const micDot = document.getElementById('acMicDot');
        const micLabel = document.getElementById('acMicLabel');
        const micBtn = document.getElementById('acMicMuteBtn');
        if (acMuted) {
          micDot.className = 'audio-mic-row__dot audio-mic-row__dot--muted';
          micLabel.textContent = 'Mic muted';
          micBtn.textContent = 'Unmute microphone';
        } else if (!ac.enabled) {
          micDot.className = 'audio-mic-row__dot audio-mic-row__dot--offline';
          micLabel.textContent = 'Mic offline';
          micBtn.textContent = 'Try unmute';
        } else {
          micDot.className = 'audio-mic-row__dot audio-mic-row__dot--live';
          micLabel.textContent = 'Mic live';
          micBtn.textContent = 'Mute microphone';
        }

        // Last-toggle audit line. Backed by ac.last_mute_source (0=boot,
        // 1=http/dashboard, 2=mqtt/HA) and ac.last_mute_age_ms. -1 means
        // no toggle has happened this boot — hide the row entirely so we
        // don't confuse fresh users.
        const srcRow = document.getElementById('acMicSourceRow');
        const srcTxt = document.getElementById('acMicSourceText');
        const lms = ac.last_mute_source;
        const lage = ac.last_mute_age_ms;
        if (srcRow && srcTxt && typeof lms === 'number' && lms >= 0 && lage >= 0) {
          const who = lms === 0 ? 'at boot'
                    : lms === 1 ? 'from this dashboard'
                    : lms === 2 ? 'by Home Assistant'
                    : 'by an unknown source';
          let when;
          if (lage < 60000)             when = Math.round(lage / 1000)  + ' s ago';
          else if (lage < 3600000)      when = Math.round(lage / 60000) + ' min ago';
          else                          when = Math.round(lage / 3600000) + ' h ago';
          const verb = acMuted ? 'Muted' : 'Unmuted';
          srcTxt.textContent = verb + ' ' + who + ' · ' + when;
          srcRow.style.display = '';
        } else if (srcRow) {
          srcRow.style.display = 'none';
        }

        const ast = ac.stats || {};
        set('acT3',     ast.t3_detected || 0);
        set('acT4',     ast.t4_detected || 0);
        set('acFrames', ast.frames_processed || 0);
        set('acOn',     ast.on_transitions || 0);
        set('acOff',    ast.off_transitions || 0);
        set('acErr',    ast.i2s_read_errors || 0);
      } else if (acCard) {
        acCard.style.display = 'none';
      }

      // ─── Touch (silent panic / enclosure tamper) ────────────────────
      const tcCard = document.getElementById('touchCard');
      const tc = data.touch;
      if (tc) {
        if (tcCard) tcCard.style.display = '';
        const tcPill = document.getElementById('touchPill');
        const tcExp  = document.getElementById('touchExplain');
        const evt    = tc.last_event || 'none';
        const tAge   = tc.last_event_age_ms;

        if (!tc.enabled) {
          tcPill.className = 'sensing-pill sensing-pill--offline';
          tcPill.textContent = 'Pad offline';
          tcExp.textContent = 'The touch peripheral failed to start. Check the device serial log.';
        } else if (!tc.baseline_locked) {
          tcPill.className = 'sensing-pill sensing-pill--quiet';
          tcPill.textContent = 'Calibrating';
          tcExp.textContent = 'The pad is sampling its idle baseline so accidental brushes can\'t trigger panic. This takes ~2 s.';
        } else if (evt === 'silent_panic' && tAge >= 0 && tAge < 60000) {
          tcPill.className = 'sensing-pill sensing-pill--active';
          tcPill.textContent = '🚨 Silent panic';
          tcExp.textContent = 'A long-press on the panic pad triggered. The device did not flash, beep, or otherwise indicate the press to anyone in the room.';
        } else if (evt === 'enclosure_tamper' && tAge >= 0 && tAge < 60000) {
          tcPill.className = 'sensing-pill sensing-pill--active';
          tcPill.textContent = '⚠ Enclosure tamper';
          tcExp.textContent = 'The pad reads above its calibrated baseline — typically the case has been opened or the device removed from its mount.';
        } else if (evt === 'approach' && tAge >= 0 && tAge < 60000) {
          tcPill.className = 'sensing-pill sensing-pill--motion';
          tcPill.textContent = 'Approach';
          tcExp.textContent = 'A hand or body passed within a few centimetres of the pad without contact.';
        } else {
          tcPill.className = 'sensing-pill sensing-pill--quiet';
          tcPill.textContent = 'Idle';
          tcExp.textContent = 'The pad is armed and watching for a long-press (panic) or a baseline shift (tamper).';
        }

        set('tcChan',   tc.pad_channel || '--');
        set('tcBase',   tc.baseline_value || '--');
        set('tcVal',    tc.last_value || '--');
        const tst = tc.stats || {};
        set('tcPanic',  tst.panic_events || 0);
        set('tcTamper', tst.tamper_events || 0);
        set('tcReads',  tst.reads_total || 0);
      } else if (tcCard) {
        tcCard.style.display = 'none';
      }

      // ─── IR (NEC / RC5 / Sony appliance activity) ──────────────────
      const irCard = document.getElementById('irCard');
      const irObj = data.ir;
      if (irObj) {
        if (irCard) irCard.style.display = '';
        const irPill = document.getElementById('irPill');
        const irExp  = document.getElementById('irExplain');
        const proto  = irObj.last_protocol || 'unknown';
        const irAge  = irObj.last_event_age_ms;

        if (!irObj.enabled) {
          irPill.className = 'sensing-pill sensing-pill--offline';
          irPill.textContent = 'IR offline';
          irExp.textContent = 'The RMT receiver failed to start. Check the device serial log.';
        } else if (irAge >= 0 && irAge < 10000) {
          irPill.className = 'sensing-pill sensing-pill--motion';
          irPill.textContent = 'Active';
          irExp.textContent = 'A ' + proto.toUpperCase() + ' remote was used. The device knows ' +
                              'something was pressed (bucket #' + (irObj.hash_bucket | 0) +
                              '), not which button.';
        } else {
          irPill.className = 'sensing-pill sensing-pill--quiet';
          irPill.textContent = 'No activity';
          irExp.textContent = 'The household\'s IR remotes are silent right now.';
        }

        set('irProto', (proto === 'unknown' || proto === '?') ? '--' : proto.toUpperCase());
        set('irBucket', (irObj.hash_bucket === undefined) ? '--' : ('#' + irObj.hash_bucket));
        const ist = irObj.stats || {};
        set('irRx',  ist.frames_received || 0);
        set('irDec', ist.frames_decoded || 0);
        set('irUnk', ist.frames_unknown || 0);
        set('irEvt', ist.events_emitted || 0);
      } else if (irCard) {
        irCard.style.display = 'none';
      }

      // ─── Thermal drift (internal die temp) ─────────────────────────
      // ─── Vision detection ────────────────────────────────────────────
      const viCard = document.getElementById('visionCard');
      const viObj = data.vision;
      if (viObj) {
        if (viCard) viCard.style.display = '';
        const viPill = document.getElementById('visionPill');
        const viExp  = document.getElementById('visionExplain');
        const viBadge = document.getElementById('visionBadge');
        const viBadgeText = document.getElementById('visionBadgeText');
        const viGrid = document.getElementById('visionGrid');

        if (!viObj.enabled) {
          viPill.className = 'sensing-pill sensing-pill--offline';
          viPill.textContent = 'Disabled';
          viExp.textContent = 'Vision detection is not running.';
          if (viBadge) viBadge.style.display = 'none';
        } else if (viObj.last_event === 'person') {
          viPill.className = 'sensing-pill sensing-pill--active';
          viPill.textContent = 'Person detected';
          viExp.textContent = 'Zone ' + (viObj.zone || '?') + ' — confidence ' + (viObj.confidence || 0) + '%';
          if (viBadge) { viBadge.style.display = 'inline-flex'; viBadge.className = 'badge danger'; viBadgeText.textContent = 'PERSON'; }
        } else if (viObj.last_event === 'motion') {
          viPill.className = 'sensing-pill sensing-pill--active';
          viPill.textContent = 'Motion detected';
          viExp.textContent = 'Zone ' + (viObj.zone || '?') + ' — confidence ' + (viObj.confidence || 0) + '%';
          if (viBadge) { viBadge.style.display = 'inline-flex'; viBadge.className = 'badge warning'; viBadgeText.textContent = 'MOTION'; }
        } else {
          viPill.className = 'sensing-pill sensing-pill--quiet';
          viPill.textContent = 'Idle';
          viExp.textContent = 'No motion detected.';
          if (viBadge) viBadge.style.display = 'none';
        }

        // Zone grid heatmap (10x8) — click to toggle zone mask
        const VISION_ZONES = 80;
        if (viGrid && !viGrid.dataset.built) {
          viGrid.innerHTML = '';
          for (let i = 0; i < VISION_ZONES; i++) {
            const cell = document.createElement('div');
            cell.style.cssText = 'aspect-ratio:1;border-radius:2px;background:var(--border);transition:background 0.4s,opacity 0.3s;cursor:pointer;position:relative;';
            cell.onclick = ((idx) => () => viToggleZone(idx))(i);
            viGrid.appendChild(cell);
          }
          viGrid.dataset.built = '1';
        }
        if (viGrid) {
          const grid = viObj.grid || [];
          const age = viObj.last_event_age_ms;
          const isPerson = viObj.last_event === 'person' && age >= 0 && age < 5000;
          const zm = viZoneMask;
          const cells = viGrid.children;
          for (let i = 0; i < cells.length; i++) {
            const enabled = (zm[i >> 3] >> (i & 7)) & 1;
            const v = grid[i] || 0;
            if (v < 3 || !enabled) {
              cells[i].style.background = 'var(--border)';
            } else {
              const t = Math.min(v / 80, 1);
              const h = isPerson ? (0 + t * 10) : (180 - t * 20);
              const s = 60 + t * 30;
              const l = 15 + t * 40;
              cells[i].style.background = 'hsl(' + h + ',' + s + '%,' + l + '%)';
            }
            cells[i].style.opacity = enabled ? '1' : '0.25';
          }
        }

        const vst = viObj.stats || {};
        set('viFrames', vst.frames_analyzed || 0);
        set('viL1',     vst.layer1_passes || 0);
        set('viL2',     vst.layer2_passes || 0);
        set('viL3',     vst.layer3_passes || 0);
        set('viMotion', vst.motion_events || 0);
        set('viPerson', vst.person_events || 0);
      } else if (viCard) {
        viCard.style.display = 'none';
      }

      // Show/hide vision settings card alongside the detection card
      const viSettingsCard = document.getElementById('visionSettingsCard');
      if (viObj && viObj.enabled) {
        if (viSettingsCard) {
          viSettingsCard.style.display = '';
          if (!viSettingsCard.dataset.loaded) viLoadConfig();
        }
      } else if (viSettingsCard) {
        viSettingsCard.style.display = 'none';
      }

      const thCard = document.getElementById('tempCard');
      const tempObj = data.temp;
      if (tempObj) {
        if (thCard) thCard.style.display = '';
        const thPill = document.getElementById('tempPill');
        const thExp  = document.getElementById('tempExplain');
        const thAge  = tempObj.last_event_age_ms;

        if (!tempObj.enabled) {
          thPill.className = 'sensing-pill sensing-pill--offline';
          thPill.textContent = 'Sensor offline';
          thExp.textContent = 'The internal temp sensor failed to start. Check the device serial log.';
        } else if (!tempObj.baseline_locked) {
          thPill.className = 'sensing-pill sensing-pill--quiet';
          thPill.textContent = 'Calibrating';
          thExp.textContent = 'Sampling baseline temperature — usually takes the first few minutes after boot.';
        } else if (thAge >= 0 && thAge < 300000) {
          thPill.className = 'sensing-pill sensing-pill--active';
          thPill.textContent = '⚠ Thermal drift';
          thExp.textContent = 'A sudden ±5 °C step from baseline — consistent with the case being opened or the device being moved.';
        } else {
          thPill.className = 'sensing-pill sensing-pill--quiet';
          thPill.textContent = 'Stable';
          thExp.textContent = 'The internal temperature is steady — no tamper indicators.';
        }

        const est = tempObj.stats || {};
        set('thBase',    (est.baseline_c === undefined) ? '--' : est.baseline_c);
        set('thNow',     (est.last_c === undefined)     ? '--' : est.last_c);
        set('thSamples', est.samples_taken || 0);
        set('thDrift',   est.drift_events || 0);
      } else if (thCard) {
        thCard.style.display = 'none';
      }

      // ─── Power & wake (lowpower HAL) ────────────────────────────────
      const lpCard = document.getElementById('powerCard');
      const lp = data.lowpower;
      if (lp) {
        if (lpCard) lpCard.style.display = '';
        set('lpWake', lp.wake_reason || 'cold_boot');
        set('lpWakePad', (lp.wake_touch_pad === undefined || lp.wake_touch_pad < 0)
                          ? '--' : lp.wake_touch_pad);
        // caps bitmask: 1=timer, 2=touch, 4=ext0, 8=ext1, 16=ulp_riscv, 32=ulp_fsm
        const caps = lp.caps | 0;
        const list = [];
        if (caps & 1)  list.push('timer');
        if (caps & 2)  list.push('touch');
        if (caps & 4)  list.push('ext0');
        if (caps & 8)  list.push('ext1');
        if (caps & 16) list.push('ulp-riscv');
        if (caps & 32) list.push('ulp-fsm');
        set('lpCaps', list.join(', ') || '--');
      } else if (lpCard) {
        lpCard.style.display = 'none';
      }
    }

    // ════════════════════════════════════════════════════════════════
    // Microphone test panel: live RMS meter + alarm-pattern self-test
    // ════════════════════════════════════════════════════════════════
    // The level meter publishes the SAME 20 ms RMS scalar the on/off
    // hysteresis uses — not a new audio path. We only poll while the
    // <details> is open AND the Sensing tab is visible, so the meter is
    // not left running while the user is on another tab.
    let acMeterTimer = null;
    let acSelftestTimer = null;
    let acMeterMaxRms = 4096;  // running auto-scale (caps at 65535)

    function audioTestPanelOpen() {
      const card = document.getElementById('acousticCard');
      if (!card || card.style.display === 'none') return false;
      const details = card.querySelector('details');
      return !!(details && details.open);
    }

    async function pollAudioLevel() {
      // Only poll while the user has the Sensing tab open AND has
      // unfolded the test panel — never leave a "loudness number" in
      // memory longer than the user explicitly asked for.
      if (currentPanel !== 'sensing' || !audioTestPanelOpen()) {
        stopAudioLevelPoll();
        return;
      }
      try {
        const r = await api('/api/audio/level', 'GET');
        if (!r || r.ok === false) return;
        const rms = r.rms | 0;
        const on  = r.rms_on_threshold | 0;
        const off = r.rms_off_threshold | 0;
        // Auto-scale: keep the bar usable for quiet rooms too. Headroom
        // ~2× current peak; capped at the int16 RMS ceiling.
        if (rms > acMeterMaxRms) acMeterMaxRms = Math.min(rms * 2, 65535);
        const denom = Math.max(acMeterMaxRms, on * 2, 800);
        const pct = Math.min(100, Math.round(rms * 100 / denom));
        const fill = document.getElementById('acMeterFill');
        if (fill) fill.style.width = pct + '%';
        const offNotch = document.getElementById('acMeterOff');
        const onNotch  = document.getElementById('acMeterOn');
        if (offNotch) offNotch.style.left = Math.min(99, Math.round(off * 100 / denom)) + '%';
        if (onNotch)  onNotch.style.left  = Math.min(99, Math.round(on  * 100 / denom)) + '%';
        const rmsEl = document.getElementById('acMeterRms');
        if (rmsEl) rmsEl.textContent = 'RMS ' + rms;
        const flagsEl = document.getElementById('acMeterFlags');
        if (flagsEl) {
          const parts = [];
          if (r.muted) parts.push('muted');
          else if (r.envelope_high) parts.push('ALARM-LEVEL');
          else parts.push('quiet');
          if (typeof r.age_ms === 'number' && r.age_ms >= 0 && r.age_ms < 5000) {
            parts.push(r.age_ms + ' ms ago');
          }
          flagsEl.textContent = parts.join(' · ');
        }
        // Cadence trace: render the most-recent transitions as colored bars.
        const trace = document.getElementById('acTrace');
        if (trace && Array.isArray(r.transitions)) {
          trace.innerHTML = '';
          // Newest is index 0; show oldest-left → newest-right.
          const reversed = r.transitions.slice().reverse();
          for (const t of reversed) {
            const seg = document.createElement('div');
            seg.className = 'audio-trace__seg' + (t.on ? ' audio-trace__seg--on' : '');
            seg.title = (t.on ? 'ON ' : 'OFF ') + (t.dur_ms || 0) + ' ms (age ' + (t.age_ms || 0) + ' ms)';
            trace.appendChild(seg);
          }
        }
      } catch (e) { /* harmless polling error */ }
    }

    function startAudioLevelPoll() {
      stopAudioLevelPoll();
      pollAudioLevel();
      acMeterTimer = setInterval(pollAudioLevel, 200);  // 5 Hz
    }
    function stopAudioLevelPoll() {
      if (acMeterTimer !== null) { clearInterval(acMeterTimer); acMeterTimer = null; }
    }

    // Bind the <details> open/close to start/stop the poller. Use a
    // delegated listener so we don't have to find the element on every
    // refreshSensing() call.
    document.addEventListener('toggle', (e) => {
      if (!e.target || e.target.tagName !== 'DETAILS') return;
      const card = document.getElementById('acousticCard');
      if (!card || !card.contains(e.target)) return;
      if (e.target.open && currentPanel === 'sensing') startAudioLevelPoll();
      else stopAudioLevelPoll();
    }, true);

    // ─── Vision zone mask + config sliders ──────────────────────────
    let viZoneMask = [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF];

    async function viToggleZone(idx) {
      const byteIdx = idx >> 3;
      const bit = 1 << (idx & 7);
      viZoneMask[byteIdx] ^= bit;
      const r = await api('/api/vision/config', 'POST', { zone_mask: viZoneMask });
      if (r && r.ok) viApplyConfig(r);
    }

    const viFields = {
      jpeg_delta_pct:       { el: 'viJpeg',    lbl: 'viTJpeg',     suffix: '%' },
      block_change_pct:     { el: 'viBlock',    lbl: 'viTBlock',    suffix: '%' },
      luminance_threshold:  { el: 'viLum',      lbl: 'viTLum',      suffix: '' },
      person_confidence_min:{ el: 'viPerson2',  lbl: 'viTPerson',   suffix: '%' },
      process_interval_ms:  { el: 'viInterval', lbl: 'viTInterval', suffix: ' ms' },
      motion_hold_ms:       { el: 'viHold',     lbl: 'viTHold',     suffix: ' ms' },
      duty_active_pct:      { el: 'viDuty',     lbl: 'viTDuty',     suffix: '% active' },
    };

    function viApplyConfig(cfg) {
      for (const [key, f] of Object.entries(viFields)) {
        if (cfg[key] !== undefined) {
          const el = document.getElementById(f.el);
          const lbl = document.getElementById(f.lbl);
          if (el) el.value = cfg[key];
          if (lbl) lbl.textContent = cfg[key] + f.suffix;
        }
      }
      if (cfg.zone_mask) viZoneMask = cfg.zone_mask.slice();
      const card = document.getElementById('visionSettingsCard');
      if (card) card.dataset.loaded = '1';
      const ss = document.getElementById('viSaveStatus');
      if (ss) ss.textContent = cfg.saved ? 'Saved' : 'Unsaved changes';
      if (ss) ss.style.color = cfg.saved ? 'var(--success)' : 'var(--warning)';
    }

    async function viLoadConfig() {
      try {
        const cfg = await api('/api/vision/config');
        if (cfg && cfg.ok) viApplyConfig(cfg);
      } catch (e) {}
    }

    let viDebounce = null;
    let viPending = {};
    function viSlider(field, value, lblId, suffix) {
      const lbl = document.getElementById(lblId);
      if (lbl) lbl.textContent = value + suffix;
      viPending[field] = parseInt(value, 10);
      const ss = document.getElementById('viSaveStatus');
      if (ss) { ss.textContent = 'Unsaved changes'; ss.style.color = 'var(--warning)'; }
      clearTimeout(viDebounce);
      viDebounce = setTimeout(async () => {
        const body = Object.assign({}, viPending);
        viPending = {};
        const r = await api('/api/vision/config', 'POST', body);
        if (r && r.ok) viApplyConfig(r);
      }, 300);
    }

    async function viSaveConfig() {
      const btn = document.getElementById('viSaveBtn');
      if (btn) btn.disabled = true;
      try {
        const r = await api('/api/vision/config/save', 'POST', {});
        if (r && r.ok) viApplyConfig(r);
      } finally {
        if (btn) btn.disabled = false;
      }
    }

    async function viResetDefaults() {
      const r = await api('/api/vision/config', 'POST', { reset: true });
      if (r && r.ok) viApplyConfig(r);
    }

    async function toggleMicMute() {
      const btn = document.getElementById('acMicMuteBtn');
      if (!btn) return;
      // Best-effort read of current state — fall back to the dot class.
      const dot = document.getElementById('acMicDot');
      const currentlyMuted = dot && dot.classList.contains('audio-mic-row__dot--muted');
      const wantMuted = !currentlyMuted;
      if (wantMuted) {
        const ok = confirm(
          'Mute the microphone?\n\n' +
          'While muted, the Canary will NOT detect smoke or CO alarm ' +
          'cadences. Your existing UL-listed alarms keep working — this ' +
          'only stops the Canary from helping notice them.\n\n' +
          'Mute persists across reboots.'
        );
        if (!ok) return;
      }
      btn.disabled = true;
      try {
        const r = await api('/api/audio/mute', 'POST', { muted: wantMuted });
        if (!r || r.ok === false) {
          alert('Mute request failed: ' + (r && r.error ? r.error : 'unknown'));
          return;
        }
        // Trigger an immediate refresh so the pill/dot update right away.
        refreshSensing();
      } finally {
        btn.disabled = false;
      }
    }

    async function startAudioSelftest() {
      const btn = document.getElementById('acTestBtn');
      const statusEl = document.getElementById('acTestStatus');
      if (!btn || !statusEl) return;
      btn.disabled = true;
      statusEl.textContent = 'Starting…';
      try {
        const r = await api('/api/audio/test/start', 'POST', { duration_ms: 30000 });
        if (!r || r.ok === false) {
          statusEl.textContent = 'Failed to start: ' + (r && r.error ? r.error : 'unknown');
          btn.disabled = false;
          return;
        }
        statusEl.textContent = 'Listening for an alarm cadence — press your alarm\'s TEST button now.';
        if (acSelftestTimer !== null) clearInterval(acSelftestTimer);
        acSelftestTimer = setInterval(pollAudioSelftest, 500);
      } catch (e) {
        statusEl.textContent = 'Network error.';
        btn.disabled = false;
      }
    }

    async function pollAudioSelftest() {
      const statusEl = document.getElementById('acTestStatus');
      const btn = document.getElementById('acTestBtn');
      try {
        const r = await api('/api/audio/test/status', 'GET');
        if (!r) return;
        if (r.active) {
          const sec = Math.max(0, Math.round((r.remaining_ms || 0) / 1000));
          const matchedNow = r.matched && r.matched !== 'none' && r.matched !== 'unknown';
          if (matchedNow) {
            statusEl.textContent =
              'Matched ' + r.matched + ' (' + (r.confidence | 0) + '% confidence). ' +
              'Continuing to listen until the test window ends — ' + sec + ' s left.';
          } else {
            statusEl.textContent = 'Listening… ' + sec + ' s left. Transitions seen: ' + (r.transitions_seen | 0) + '.';
          }
        } else {
          // Test ended.
          if (acSelftestTimer !== null) { clearInterval(acSelftestTimer); acSelftestTimer = null; }
          if (btn) btn.disabled = false;
          if (r.matched && r.matched !== 'none' && r.matched !== 'unknown') {
            statusEl.textContent =
              '✓ Matched ' + r.matched + ' (' + (r.confidence | 0) + '% confidence). ' +
              'No Home Assistant automation was triggered.';
          } else {
            const seen = r.transitions_seen | 0;
            if (seen === 0) {
              statusEl.textContent =
                '✗ No sound transitions seen at all. Move the Canary closer to your alarm or check the I2S errors stat.';
            } else {
              statusEl.textContent =
                '✗ Heard ' + seen + ' on/off transitions but no T3/T4 cadence matched. ' +
                'Possible reasons: alarm is too far away, room is too noisy, or your alarm uses a non-standard cadence.';
            }
          }
        }
      } catch (e) { /* harmless polling error */ }
    }

    // ════════════════════════════════════════════════════════════════
    // Synthetic T3 / T4 tone generator (Web Audio API)
    // ════════════════════════════════════════════════════════════════
    // Lets a user verify the cadence detector works without setting off
    // a real alarm. We schedule a series of OscillatorNode start/stop
    // events at audioCtx.currentTime offsets so the cadence is sample-
    // accurate; a setTimeout that just turns a gain node on/off would
    // drift across cycles and break the detector's ±200 ms tolerance.
    //
    // T3: three 0.5 s tones @ 3.2 kHz, 0.5 s gaps, 1.5 s pause. 4.0 s cycle.
    // T4: four  0.1 s tones @ 3.2 kHz, 0.1 s gaps, 5.0 s pause. 5.7 s cycle.
    //
    // We deliberately use a single AudioContext + a fresh OscillatorNode
    // per cycle (oscillators can only start() once per Web Audio spec).
    let acAudioCtx = null;
    let acTonePattern = null;       // 't3' | 't4' | null
    let acToneScheduleAt = 0;       // currentTime of the next cycle start
    let acToneScheduleTimer = null; // setTimeout handle that drives scheduling

    function audioCtx() {
      if (!acAudioCtx) {
        const Ctx = window.AudioContext || window.webkitAudioContext;
        if (!Ctx) return null;
        acAudioCtx = new Ctx();
      }
      return acAudioCtx;
    }

    function scheduleBeep(ctx, startSec, durSec, freqHz) {
      const osc  = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.type = 'sine';
      osc.frequency.value = freqHz;
      // Tiny 5 ms ramps so we don't get clicks that could be misread as
      // an extra transition by the envelope tracker.
      const ramp = 0.005;
      gain.gain.setValueAtTime(0, startSec);
      gain.gain.linearRampToValueAtTime(0.5, startSec + ramp);
      gain.gain.setValueAtTime(0.5, startSec + durSec - ramp);
      gain.gain.linearRampToValueAtTime(0, startSec + durSec);
      osc.connect(gain).connect(ctx.destination);
      osc.start(startSec);
      osc.stop(startSec + durSec + 0.01);
    }

    function scheduleNextCycle() {
      const ctx = audioCtx();
      if (!ctx || !acTonePattern) return;
      const t0 = acToneScheduleAt;
      const freq = 3200;
      let cycleLen;
      if (acTonePattern === 't3') {
        // 3 × (0.5 s beep + 0.5 s gap) + 1.5 s pause
        for (let i = 0; i < 3; i++) {
          scheduleBeep(ctx, t0 + i * 1.0, 0.5, freq);
        }
        cycleLen = 4.0;
      } else {
        // 4 × (0.1 s beep + 0.1 s gap) + 5.0 s pause
        for (let i = 0; i < 4; i++) {
          scheduleBeep(ctx, t0 + i * 0.2, 0.1, freq);
        }
        cycleLen = 5.7;
      }
      acToneScheduleAt = t0 + cycleLen;
      // Schedule the next round just before this one ends so we never
      // run dry. setTimeout drift doesn't matter — the audio events
      // are all scheduled against currentTime in the audio context.
      const msUntilNext = Math.max(50, (cycleLen - 0.2) * 1000);
      acToneScheduleTimer = setTimeout(scheduleNextCycle, msUntilNext);
    }

    function startTone(pattern) {
      const ctx = audioCtx();
      if (!ctx) {
        document.getElementById('acToneStatus').textContent =
          'Web Audio not available in this browser.';
        return false;
      }
      // Mobile Safari and Chrome require a resume() inside a user gesture.
      if (ctx.state === 'suspended') ctx.resume();
      stopTone();   /* ensure no overlap */
      acTonePattern = pattern;
      acToneScheduleAt = ctx.currentTime + 0.1;
      scheduleNextCycle();
      return true;
    }

    function stopTone() {
      if (acToneScheduleTimer !== null) {
        clearTimeout(acToneScheduleTimer);
        acToneScheduleTimer = null;
      }
      acTonePattern = null;
      /* Already-scheduled events finish their tail (≤ 1 s); cheaper
       * than rebuilding the audio graph just to silence them. */
    }

    async function toggleToneT3() {
      const btn = document.getElementById('acToneT3Btn');
      const btn4 = document.getElementById('acToneT4Btn');
      const st = document.getElementById('acToneStatus');
      if (acTonePattern === 't3') {
        stopTone();
        btn.textContent = 'Play T3 (smoke)';
        st.textContent = 'Tone off.';
        return;
      }
      /* Auto-arm the matcher so the user doesn't have to click two
       * buttons. Best-effort; we don't await before starting the tone. */
      api('/api/audio/test/start', 'POST', { duration_ms: 60000 }).then(() => {
        if (acSelftestTimer !== null) clearInterval(acSelftestTimer);
        acSelftestTimer = setInterval(pollAudioSelftest, 500);
        const sBtn = document.getElementById('acTestBtn');
        if (sBtn) sBtn.disabled = true;
      }).catch(() => {});
      if (!startTone('t3')) return;
      btn.textContent = 'Stop T3';
      btn4.textContent = 'Play T4 (CO)';
      st.textContent = 'Playing T3 (smoke) cadence — hold your device near the Canary.';
    }

    async function toggleToneT4() {
      const btn = document.getElementById('acToneT4Btn');
      const btn3 = document.getElementById('acToneT3Btn');
      const st = document.getElementById('acToneStatus');
      if (acTonePattern === 't4') {
        stopTone();
        btn.textContent = 'Play T4 (CO)';
        st.textContent = 'Tone off.';
        return;
      }
      api('/api/audio/test/start', 'POST', { duration_ms: 60000 }).then(() => {
        if (acSelftestTimer !== null) clearInterval(acSelftestTimer);
        acSelftestTimer = setInterval(pollAudioSelftest, 500);
        const sBtn = document.getElementById('acTestBtn');
        if (sBtn) sBtn.disabled = true;
      }).catch(() => {});
      if (!startTone('t4')) return;
      btn.textContent = 'Stop T4';
      btn3.textContent = 'Play T3 (smoke)';
      st.textContent = 'Playing T4 (CO) cadence — hold your device near the Canary.';
    }

    // ════════════════════════════════════════════════════════════════
    // Live Sensing summary on the Status tab
    // ════════════════════════════════════════════════════════════════
    // Pulls /api/sensing and lights up the five quick-glance
    // indicators (smoke / CO / panic / tamper / appliance) plus the
    // motion/breathing/RSSI numbers. Called as part of refreshStatus()
    // so the user sees live state without ever touching the Sensing
    // nav button. Card auto-hides cleanly if the build has no sensing.
    async function refreshLiveSensing() {
      const card = document.getElementById('liveSensingCard');
      const data = await api('/api/sensing');
      if (!data || !data.ok) {
        // Build has no sensing endpoint at all — keep card hidden.
        if (card) card.style.display = 'none';
        return;
      }
      if (card) card.style.display = '';

      // Hero pill
      const label = data.label || 'offline';
      const pill = document.getElementById('liveActivityPill');
      if (pill) {
        pill.className = 'sensing-pill sensing-pill--' + label;
        pill.textContent = label;
      }

      // Numeric tiles
      const setT = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; };
      setT('liveMotion', (data.motion | 0));
      setT('liveBreath', (data.breathing | 0));
      setT('liveRssi',
           (data.rssi_dbm === undefined || data.rssi_dbm === 0) ? '--' : data.rssi_dbm);

      // Five indicator chips. "on" = critical/alarming (red);
      // "info" = informational (blue); "off" = grey.
      const setInd = (id, state) => {
        const e = document.getElementById(id);
        if (e) e.dataset.state = state;
      };
      const ac = data.acoustic || {};
      const tc = data.touch || {};
      const tm = data.temp || {};
      const ir = data.ir || {};
      const acAge = ac.last_event_age_ms;
      const tcAge = tc.last_event_age_ms;
      const tmAge = tm.last_event_age_ms;
      const irAge = ir.last_event_age_ms;

      setInd('liveIndSmoke',
             (ac.last_event === 'smoke_alarm_t3' && acAge >= 0 && acAge < 30000) ? 'on' : 'off');
      setInd('liveIndCO',
             (ac.last_event === 'co_alarm_t4' && acAge >= 0 && acAge < 30000) ? 'on' : 'off');
      setInd('liveIndPanic',
             (tc.last_event === 'silent_panic' && tcAge >= 0 && tcAge < 60000) ? 'on' : 'off');
      const tamperOn =
            (tc.last_event === 'enclosure_tamper' && tcAge >= 0 && tcAge < 60000) ||
            (tmAge >= 0 && tmAge < 300000 && tm.confidence > 0);
      setInd('liveIndTamper', tamperOn ? 'on' : 'off');
      setInd('liveIndIR',
             (irAge >= 0 && irAge < 10000 && ir.last_protocol && ir.last_protocol !== 'none')
             ? 'info' : 'off');
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
    refreshLiveSensing();   // Status-tab live sensing summary
    loadChain();
    loadWifiStatus();
    refreshOpera();
    refreshChirpStatus();
    refreshBtStatus();
    loadBtSettings();
    loadBtPairedDevices();
    updateResolutionUI();
    setInterval(refreshStatus, 2000);
    /* Live sensing updates only matter when the user is actually on
     * the Status tab — every 2 s is plenty (matches the existing
     * refreshStatus cadence without doubling network load). */
    setInterval(() => {
      if (currentPanel === 'status') refreshLiveSensing();
    }, 2000);
    setInterval(loadWifiStatus, 5000);
    setInterval(() => {
      if (currentPanel === 'logs') loadLogs();
      else if (currentPanel === 'witness') loadWitness();
      else if (currentPanel === 'opera') refreshOpera();
      else if (currentPanel === 'community') refreshChirpStatus();
      else if (currentPanel === 'bluetooth') refreshBtStatus();
    }, 5000);
    /* Sensing panel polls at 1 Hz to match the CSI window cadence so the
     * gauges feel live without flooding the device with HTTP. */
    setInterval(() => {
      if (currentPanel === 'sensing') refreshSensing();
    }, 1000);
  </script>
</body>
</html>
)rawliteral";
