/*
 * SecuraCV Canary — Web UI
 * Version 3.1.0 — Added device health dashboard with thermals, PSRAM, CPU monitoring
 *
 * Professional dashboard for device monitoring, log review, and management.
 * Embedded as PROGMEM string for flash storage efficiency.
 *
 * Navigation: Status | Camera | Community | Records | Settings
 *
 * Copyright (c) 2024-2025 SecuraCV Project Contributors
 * Licensed under the MIT License
 * SPDX-License-Identifier: MIT
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
    .brand h1 { font-size: 1.25rem; font-weight: 600; letter-spacing: -0.02em; }
    .brand span { font-size: 0.75rem; color: var(--muted); display: block; }
    .status-badges { display: flex; gap: 0.5rem; flex-wrap: wrap; }
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

    /* Navigation - Clean 5-tab design */
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
      padding: 0.7rem 0.5rem;
      border: none;
      background: transparent;
      color: var(--muted);
      font-size: 0.8rem;
      font-weight: 500;
      border-radius: 8px;
      cursor: pointer;
      transition: all 0.15s ease;
      white-space: nowrap;
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
      margin-left: 0.3rem;
      background: var(--danger);
      color: #fff;
      font-size: 0.6rem;
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
      flex-wrap: wrap;
      gap: 0.5rem;
    }
    .card-title { font-size: 0.9rem; font-weight: 600; color: var(--text); }
    .card-subtitle { font-size: 0.75rem; color: var(--muted); margin-top: 0.15rem; }

    /* Feature info box */
    .feature-info {
      background: linear-gradient(135deg, rgba(79,209,197,0.1) 0%, rgba(99,179,237,0.1) 100%);
      border: 1px solid rgba(79,209,197,0.2);
      border-radius: 8px;
      padding: 1rem;
      margin-bottom: 1rem;
    }
    .feature-info h4 { font-size: 0.85rem; color: var(--accent); margin-bottom: 0.5rem; }
    .feature-info p { font-size: 0.8rem; color: var(--muted); margin-bottom: 0.5rem; }
    .feature-info ul { font-size: 0.75rem; color: var(--muted); padding-left: 1.2rem; margin: 0.5rem 0; }
    .feature-info li { margin-bottom: 0.25rem; }
    .feature-info .privacy-note {
      display: flex;
      align-items: center;
      gap: 0.5rem;
      margin-top: 0.75rem;
      padding-top: 0.75rem;
      border-top: 1px solid rgba(255,255,255,0.1);
      font-size: 0.75rem;
      color: var(--success);
    }

    /* Sub-navigation for combined panels */
    .sub-nav {
      display: flex;
      gap: 0.5rem;
      margin-bottom: 1rem;
      flex-wrap: wrap;
    }
    .sub-nav-btn {
      padding: 0.5rem 1rem;
      border: 1px solid var(--border);
      background: transparent;
      color: var(--muted);
      font-size: 0.8rem;
      border-radius: 6px;
      cursor: pointer;
      transition: all 0.15s ease;
    }
    .sub-nav-btn:hover { border-color: var(--accent); color: var(--text); }
    .sub-nav-btn.active { background: var(--accent-dim); border-color: var(--accent); color: var(--accent); }

    /* Stats Grid */
    .stats-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
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
    .stat-value { font-size: 1.25rem; font-weight: 600; font-family: var(--mono); }
    .stat-unit { font-size: 0.75rem; color: var(--muted); margin-left: 0.25rem; }

    /* System Health Card */
    .health-section { margin-bottom: 1rem; }
    .health-section-label { margin-bottom: 0.5rem; }
    .health-temp-display {
      display: flex;
      align-items: center;
      gap: 1rem;
      padding: 0.75rem;
      background: rgba(0,0,0,0.2);
      border-radius: 8px;
    }
    .health-temp-value {
      font-size: 2rem;
      font-weight: 600;
      font-family: var(--mono);
    }
    .health-temp-unit {
      font-size: 1rem;
      color: var(--muted);
    }
    .health-temp-secondary {
      font-size: 0.75rem;
      color: var(--muted);
      margin-top: 0.25rem;
    }
    .health-temp-bar-container { margin-top: 0.5rem; }
    .health-temp-bar-labels {
      display: flex;
      justify-content: space-between;
      font-size: 0.65rem;
      color: var(--muted);
      margin-bottom: 0.25rem;
    }
    .health-temp-bar {
      height: 8px;
      background: linear-gradient(to right,#63b3ed 0%,#63b3ed 5%,#68d391 5%,#68d391 65%,#f6ad55 65%,#f6ad55 80%,#fc8181 80%);
      border-radius: 4px;
      position: relative;
    }
    .health-temp-marker {
      position: absolute;
      top: -2px;
      width: 4px;
      height: 12px;
      background: white;
      border-radius: 2px;
      transform: translateX(-50%);
      left: 50%;
    }
    .health-memory-bar {
      height: 4px;
      background: rgba(255,255,255,0.1);
      border-radius: 2px;
      margin-top: 0.25rem;
    }
    .health-memory-bar-fill {
      height: 100%;
      background: var(--accent);
      border-radius: 2px;
      width: 0%;
      transition: width 0.3s, background 0.3s;
    }
    .health-memory-bar-fill.psram { background: var(--info); }

    /* Identity Grid */
    .identity-grid { display: grid; gap: 0.5rem; }
    .identity-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 0.5rem 0;
      border-bottom: 1px solid rgba(255,255,255,0.05);
    }
    .identity-row:last-child { border-bottom: none; }
    .identity-label { font-size: 0.8rem; color: var(--muted); }
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
    .chain-block { display: flex; flex-direction: column; align-items: center; min-width: 80px; }
    .chain-hash {
      font-family: var(--mono);
      font-size: 0.65rem;
      color: var(--accent);
      padding: 0.3rem 0.5rem;
      background: var(--accent-dim);
      border-radius: 4px;
    }
    .chain-seq { font-size: 0.7rem; color: var(--muted); margin-top: 0.25rem; }
    .chain-arrow { color: var(--muted); font-size: 1.2rem; }

    /* GPS Grid */
    .gps-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.5rem; }
    .gps-item { padding: 0.5rem; background: rgba(0,0,0,0.2); border-radius: 6px; }
    .gps-label { font-size: 0.65rem; color: var(--muted); text-transform: uppercase; }
    .gps-value { font-family: var(--mono); font-size: 0.9rem; }

    /* Peek/Camera Preview */
    .peek-container { display: flex; flex-direction: column; }
    .peek-frame {
      position: relative;
      width: 100%;
      aspect-ratio: 4 / 3;
      background: #0a0e1a;
      border-radius: 8px;
      overflow: hidden;
      border: 1px solid var(--border);
    }
    .peek-stream { width: 100%; height: 100%; object-fit: contain; display: block; }
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
    .peek-offline svg { opacity: 0.5; }

    /* Log list */
    .log-list { display: flex; flex-direction: column; gap: 0.5rem; max-height: 500px; overflow-y: auto; }
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

    .log-content { display: flex; flex-direction: column; gap: 0.2rem; }
    .log-message { font-size: 0.85rem; color: var(--text); }
    .log-detail { font-size: 0.75rem; color: var(--muted); font-family: var(--mono); }
    .log-meta { font-size: 0.7rem; color: var(--muted); }
    .log-actions { display: flex; gap: 0.25rem; }

    /* Witness records */
    .witness-item {
      display: grid;
      grid-template-columns: 60px 1fr auto;
      gap: 0.75rem;
      padding: 0.6rem;
      background: rgba(0,0,0,0.2);
      border-radius: 8px;
    }
    .witness-seq { font-family: var(--mono); font-size: 0.85rem; font-weight: 600; color: var(--accent); }
    .witness-type { font-size: 0.7rem; color: var(--muted); }
    .witness-hash { font-family: var(--mono); font-size: 0.7rem; color: var(--muted); }
    .witness-verified {
      font-size: 0.7rem;
      padding: 0.2rem 0.4rem;
      border-radius: 4px;
      background: var(--success-dim);
      color: var(--success);
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

    /* Form elements */
    .form-group { margin-bottom: 1rem; }
    .form-label { display: block; font-size: 0.8rem; color: var(--muted); margin-bottom: 0.3rem; }
    .form-input, .form-select {
      width: 100%;
      padding: 0.6rem;
      background: rgba(0,0,0,0.3);
      border: 1px solid var(--border);
      border-radius: 6px;
      color: var(--text);
      font-size: 0.9rem;
    }
    .form-input:focus, .form-select:focus { outline: none; border-color: var(--accent); }

    /* Toggle switch */
    .toggle-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 0.75rem;
      background: rgba(0,0,0,0.2);
      border-radius: 8px;
      margin-bottom: 0.5rem;
    }
    .toggle-row .toggle-info { flex: 1; }
    .toggle-row .toggle-title { font-size: 0.85rem; font-weight: 500; }
    .toggle-row .toggle-desc { font-size: 0.75rem; color: var(--muted); }

    /* Resolution selector */
    .resolution-selector { display: flex; gap: 0.5rem; flex-wrap: wrap; margin-top: 0.5rem; }
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
    .resolution-btn:hover { border-color: var(--accent); color: var(--text); }
    .resolution-btn.active { background: var(--accent-dim); border-color: var(--accent); color: var(--accent); }

    /* Panels */
    .panel { display: none; }
    .panel.active { display: block; }
    .sub-panel { display: none; }
    .sub-panel.active { display: block; }

    /* Empty state */
    .empty-state { text-align: center; padding: 2rem; color: var(--muted); }
    .empty-icon { font-size: 2rem; margin-bottom: 0.5rem; opacity: 0.5; }

    /* Loading */
    .loading { display: flex; align-items: center; justify-content: center; padding: 2rem; }
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
    .modal-close { background: none; border: none; color: var(--muted); font-size: 1.25rem; cursor: pointer; }
    .modal-body { padding: 1rem; }
    .modal-footer {
      display: flex;
      gap: 0.5rem;
      justify-content: flex-end;
      padding: 1rem;
      border-top: 1px solid var(--border);
    }

    /* Section divider */
    .section-title {
      font-size: 0.7rem;
      font-weight: 600;
      color: var(--muted);
      text-transform: uppercase;
      letter-spacing: 0.1em;
      margin: 1.5rem 0 0.75rem;
      padding-bottom: 0.5rem;
      border-bottom: 1px solid var(--border);
    }

    /* Auth Modal — Full-screen overlay for token entry */
    .auth-overlay {
      position: fixed;
      inset: 0;
      background: var(--bg);
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      z-index: 2000;
      padding: 1rem;
    }
    .auth-overlay.hidden { display: none; }
    .auth-box {
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 2rem;
      width: 100%;
      max-width: 420px;
      box-shadow: var(--shadow);
    }
    .auth-brand {
      text-align: center;
      margin-bottom: 1.5rem;
    }
    .auth-brand h2 { font-size: 1.3rem; font-weight: 600; margin-bottom: 0.25rem; }
    .auth-brand p { font-size: 0.8rem; color: var(--muted); }
    .auth-divider {
      display: flex;
      align-items: center;
      gap: 0.75rem;
      margin: 1rem 0;
      font-size: 0.75rem;
      color: var(--muted);
    }
    .auth-divider::before, .auth-divider::after {
      content: '';
      flex: 1;
      height: 1px;
      background: var(--border);
    }
    .auth-status {
      text-align: center;
      font-size: 0.8rem;
      padding: 0.5rem;
      border-radius: 6px;
      margin-top: 0.75rem;
      display: none;
    }
    .auth-status.error { display: block; background: var(--danger-dim); color: var(--danger); }
    .auth-status.success { display: block; background: var(--success-dim); color: var(--success); }
    .auth-status.info { display: block; background: var(--accent-dim); color: var(--accent); }
    .auth-footer {
      margin-top: 1.5rem;
      text-align: center;
      font-size: 0.72rem;
      color: var(--muted);
    }
    .auth-footer span { margin: 0 0.3rem; }

    /* BLE signal bars (Find My-style) — four ascending bars, each lit if RSSI
       crosses a threshold. CSS-only so it stays cheap to render hundreds of
       results on a phone. */
    .signal-bars { width: 18px; }
    .signal-bars span {
      width: 3px; background: var(--border); border-radius: 1px; opacity: 0.6;
    }
    .signal-bars span:nth-child(1) { height: 4px; }
    .signal-bars span:nth-child(2) { height: 7px; }
    .signal-bars span:nth-child(3) { height: 10px; }
    .signal-bars span:nth-child(4) { height: 13px; }
    .signal-bars.s1 span:nth-child(1),
    .signal-bars.s2 span:nth-child(-n+2),
    .signal-bars.s3 span:nth-child(-n+3),
    .signal-bars.s4 span:nth-child(-n+4) { background: var(--accent); opacity: 1; }
    .signal-bars.weak span:nth-child(-n+1) { background: var(--danger); opacity: 1; }
    .ble-row { display:flex; align-items:center; gap:0.6rem; padding:0.6rem 0.75rem; }
    .ble-row + .ble-row { border-top: 1px solid var(--border); }
    .ble-row .ble-icon { font-size:1.3rem; }
    .ble-row .ble-meta { flex:1; min-width:0; }
    .ble-row .ble-name { font-weight:600; font-size:0.9rem; }
    .ble-row .ble-sub  { font-size:0.7rem; color: var(--muted); font-family: var(--mono); }
    .ble-row .ble-dist { text-align:right; min-width:70px; }
    .ble-row .ble-dist .ble-dist-m { font-weight:600; font-size:0.85rem; }
    .ble-row .ble-dist .ble-dist-l { font-size:0.65rem; color: var(--muted); }

    /* Responsive */
    @media (max-width: 600px) {
      .stats-grid { grid-template-columns: repeat(2, 1fr); }
      .gps-grid { grid-template-columns: 1fr; }
      .header-content { flex-direction: column; align-items: flex-start; }
      .nav-btn { font-size: 0.75rem; padding: 0.6rem 0.3rem; }
    }
  </style>
</head>
<body>

  <!-- Auth Modal — shown on first load, requires token to proceed -->
  <div class="auth-overlay" id="authOverlay">
    <div class="auth-box">
      <div class="auth-brand">
        <h2>SecuraCV Canary Dashboard</h2>
        <p>Enter your API token to connect</p>
      </div>
      <div class="form-group">
        <label class="form-label">API Token</label>
        <input type="text" class="form-input" id="tokenInput" placeholder="cv_..." autocomplete="off" spellcheck="false" autocapitalize="off" autocorrect="off" style="font-family:var(--mono);letter-spacing:0.02em;">
        <p style="font-size:0.7rem;color:var(--muted);margin-top:0.3rem;">Type the token exactly as displayed — don't substitute 0/O or 1/I/l. (New tokens omit those glyphs; older tokens may still contain them.)</p>
      </div>
      <button class="btn btn-primary" style="width:100%" onclick="connectWithToken()">Connect</button>
      <div class="auth-divider">or</div>
      <button class="btn btn-secondary" style="width:100%" onclick="requestFromDevice()">Request from Device</button>
      <p style="font-size:0.72rem;color:var(--muted);text-align:center;margin-top:0.5rem;">
        Press BOOT button on device, then click above
      </p>
      <div class="auth-status" id="authStatus"></div>
      <div class="auth-footer">
        <span id="authDeviceId">--</span> |
        <span id="authFw">--</span> |
        <span id="authTls">TLS: --</span>
      </div>
    </div>
  </div>

  <header>
    <div class="header-content">
      <div class="brand">
        <div class="brand-icon">🐥</div>
        <div>
          <h1>SecuraCV Canary</h1>
          <span id="deviceId">Loading...</span>
        </div>
      </div>
      <div class="status-badges">
        <div class="badge success" id="chainBadge"><span class="badge-dot"></span><span>Chain OK</span></div>
        <div class="badge info" id="tempBadge"><span class="badge-dot"></span><span id="tempStatus">--°C</span></div>
        <div class="badge info" id="gpsBadge"><span class="badge-dot"></span><span id="gpsStatus">GPS</span></div>
        <div class="badge success" id="sdBadge"><span class="badge-dot"></span><span>SD OK</span></div>
        <div class="badge info" id="rfBadge"><span class="badge-dot"></span><span id="rfStatus">RF</span></div>
        <div class="badge info" id="wpBadge" title="WiFi Presence — nearby device count"><span class="badge-dot"></span><span id="wpBadgeText">WP</span></div>
      </div>
    </div>
  </header>

  <div class="container">
    <!-- Clean 5-Tab Navigation -->
    <nav>
      <button class="nav-btn active" data-panel="status">Status</button>
      <button class="nav-btn" data-panel="camera">Camera</button>
      <button class="nav-btn" data-panel="presence">Presence</button>
      <button class="nav-btn" data-panel="community">Community<span class="count" id="communityCount" style="display:none">0</span></button>
      <button class="nav-btn" data-panel="records">Records<span class="count" id="recordsCount" style="display:none">0</span></button>
      <button class="nav-btn" data-panel="settings">Settings</button>
    </nav>

    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <!-- STATUS PANEL - Device Health, GPS, RF Presence -->
    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <div class="panel active" id="panel-status">
      <!-- Device Health Card -->
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

      <!-- System Health Card (Temperature, Memory, CPU) -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">System Health</div>
            <div class="card-subtitle">Temperature, memory, and CPU metrics</div>
          </div>
          <button class="btn btn-ghost btn-sm" onclick="refreshSystemHealth()">↻ Refresh</button>
        </div>
        <!-- Temperature Section -->
        <div class="health-section">
          <div class="stat-label health-section-label">TEMPERATURE</div>
          <div class="health-temp-display">
            <div style="flex:1;">
              <div class="health-temp-value">
                <span id="sysTemp">--</span><span class="health-temp-unit">°C</span>
                <span class="health-temp-unit" style="margin-left:0.5rem;">(<span id="sysTempF">--</span>°F)</span>
              </div>
              <div class="health-temp-secondary">
                Min: <span id="sysTempMin">--</span>°C · Max: <span id="sysTempMax">--</span>°C · Avg: <span id="sysTempAvg">--</span>°C
              </div>
            </div>
            <div id="sysTempState" class="badge info"><span class="badge-dot"></span><span id="sysTempStateText">--</span></div>
          </div>
          <!-- Temperature Bar -->
          <div class="health-temp-bar-container">
            <div class="health-temp-bar-labels">
              <span>0°C</span><span>COLD</span><span>NORMAL</span><span>HOT</span><span>100°C</span>
            </div>
            <div class="health-temp-bar">
              <div id="sysTempMarker" class="health-temp-marker"></div>
            </div>
          </div>
        </div>
        <!-- Memory Section -->
        <div class="health-section">
          <div class="stat-label health-section-label">MEMORY</div>
          <div class="stats-grid">
            <div class="stat-item">
              <div class="stat-label">Heap Used</div>
              <div class="stat-value"><span id="sysHeapPct">0</span><span class="stat-unit">%</span></div>
              <div class="health-memory-bar">
                <div id="sysHeapBar" class="health-memory-bar-fill"></div>
              </div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Heap Free</div>
              <div class="stat-value"><span id="sysHeapFree">0</span><span class="stat-unit">KB</span></div>
            </div>
            <div class="stat-item">
              <div class="stat-label">PSRAM Used</div>
              <div class="stat-value"><span id="sysPsramPct">--</span><span class="stat-unit">%</span></div>
              <div class="health-memory-bar">
                <div id="sysPsramBar" class="health-memory-bar-fill psram"></div>
              </div>
            </div>
            <div class="stat-item">
              <div class="stat-label">PSRAM Free</div>
              <div class="stat-value"><span id="sysPsramFree">--</span><span class="stat-unit">MB</span></div>
            </div>
          </div>
        </div>
        <!-- CPU/Device Info Section -->
        <div>
          <div class="stat-label health-section-label">DEVICE INFO</div>
          <div class="identity-grid">
            <div class="identity-row">
              <span class="identity-label">Chip</span>
              <span class="identity-value" id="sysChip">--</span>
            </div>
            <div class="identity-row">
              <span class="identity-label">CPU</span>
              <span class="identity-value"><span id="sysCores">--</span> cores @ <span id="sysFreq">--</span> MHz</span>
            </div>
            <div class="identity-row">
              <span class="identity-label">Flash</span>
              <span class="identity-value"><span id="sysFlash">--</span> MB</span>
            </div>
            <div class="identity-row">
              <span class="identity-label">Sketch</span>
              <span class="identity-value"><span id="sysSketch">--</span> KB used</span>
            </div>
            <div class="identity-row">
              <span class="identity-label">MAC</span>
              <span class="identity-value" id="sysMac">--</span>
            </div>
            <div class="identity-row">
              <span class="identity-label">Reset</span>
              <span class="identity-value" id="sysReset">--</span>
            </div>
          </div>
        </div>
      </div>

      <!-- Device Identity Card -->
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
            <span class="identity-label">Firmware</span>
            <span class="identity-value" id="firmware">3.1.0</span>
          </div>
        </div>
      </div>

      <!-- Hash Chain Card -->
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

      <!-- RF Presence Card (REPORTED VALUES) -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">RF Presence Detection</div>
            <div class="card-subtitle" id="rfSubtitle">Privacy-preserving presence sensing</div>
          </div>
          <div class="badge info" id="rfStateBadge">
            <span class="badge-dot"></span>
            <span id="rfStateText">--</span>
          </div>
        </div>
        <div class="stats-grid">
          <div class="stat-item">
            <div class="stat-label">State</div>
            <div class="stat-value" style="font-size:0.9rem;" id="rfState">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Confidence</div>
            <div class="stat-value" style="font-size:0.9rem;" id="rfConfidence">--</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Device Count</div>
            <div class="stat-value" id="rfDeviceCount">0</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Dwell Class</div>
            <div class="stat-value" style="font-size:0.9rem;" id="rfDwellClass">--</div>
          </div>
        </div>
        <div class="toggle-row" style="margin-top:1rem;">
          <div class="toggle-info">
            <div class="toggle-title">Enable RF Presence</div>
            <div class="toggle-desc">Detect nearby devices without storing identifiers</div>
          </div>
          <label style="cursor:pointer;">
            <input type="checkbox" id="rfEnabled" onchange="toggleRfEnabled()">
          </label>
        </div>
      </div>

      <!-- GPS Status Card -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">GPS Status</div>
            <div class="card-subtitle" id="gpsSubtitle">Waiting for fix...</div>
          </div>
        </div>
        <div class="gps-grid">
          <div class="gps-item"><div class="gps-label">Latitude</div><div class="gps-value" id="gpsLat">--</div></div>
          <div class="gps-item"><div class="gps-label">Longitude</div><div class="gps-value" id="gpsLon">--</div></div>
          <div class="gps-item"><div class="gps-label">Altitude</div><div class="gps-value" id="gpsAlt">--</div></div>
          <div class="gps-item"><div class="gps-label">Speed</div><div class="gps-value" id="gpsSpeed">--</div></div>
          <div class="gps-item"><div class="gps-label">Satellites</div><div class="gps-value" id="gpsSats">--</div></div>
          <div class="gps-item"><div class="gps-label">HDOP</div><div class="gps-value" id="gpsHdop">--</div></div>
        </div>
      </div>
    </div>

    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <!-- CAMERA PANEL - Preview for Setup -->
    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <div class="panel" id="panel-camera">
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Camera Preview</div>
            <div class="card-subtitle">Live view for positioning (not recorded)</div>
          </div>
          <div style="display:flex;gap:0.5rem;align-items:center;">
            <span id="peekStatus" style="font-size:0.75rem;color:var(--muted);">Ready</span>
            <button class="btn btn-primary btn-sm" id="peekToggle" onclick="togglePeek()">▶ Start</button>
          </div>
        </div>
        <div class="peek-container">
          <div class="peek-frame">
            <img id="peekStream" class="peek-stream" style="display:none;" alt="Camera preview">
            <div id="peekOffline" class="peek-offline">
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="48" height="48">
                <path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z"/>
                <circle cx="12" cy="13" r="4"/>
              </svg>
              <p id="peekOfflineText">Click Start to preview</p>
            </div>
          </div>
        </div>
        <p style="font-size:0.8rem;color:var(--muted);margin-top:0.75rem;">
          <strong>Note:</strong> This preview is for camera positioning only. No frames are stored — SecuraCV records semantic events, not video.
        </p>
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
          <button class="btn btn-secondary" onclick="takeSnapshot()">📷 Snapshot</button>
        </div>
        <div style="margin-top:1rem;">
          <div class="form-label">Resolution</div>
          <div class="resolution-selector">
            <button class="resolution-btn" data-size="4" onclick="setResolution(4)">320×240</button>
            <button class="resolution-btn active" data-size="8" onclick="setResolution(8)">640×480</button>
            <button class="resolution-btn" data-size="9" onclick="setResolution(9)">800×600</button>
            <button class="resolution-btn" data-size="10" onclick="setResolution(10)">1024×768</button>
          </div>
          <p id="resolutionStatus" style="font-size:0.7rem;color:var(--muted);margin-top:0.5rem;">Current: 640×480</p>
        </div>

        <!--
          Sensor tuning panel — every control here writes straight through to
          the OV2640 / OV3660 sensor_t via /api/peek/sensor. Values are read
          back from the sensor on poll, never fabricated.
        -->
        <div style="margin-top:1.25rem;">
          <div class="form-label">Picture Quality</div>
          <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:0.6rem 1.25rem;font-size:0.82rem;">
            <label>JPEG Quality (lower = sharper, larger): <span id="sensorQualityVal">—</span>
              <input type="range" id="sensorQuality" min="4" max="40" step="1" oninput="onSensorSlider('quality', this.value, 'sensorQualityVal')">
            </label>
            <label>Brightness: <span id="sensorBrightnessVal">—</span>
              <input type="range" id="sensorBrightness" min="-2" max="2" step="1" oninput="onSensorSlider('brightness', this.value, 'sensorBrightnessVal')">
            </label>
            <label>Contrast: <span id="sensorContrastVal">—</span>
              <input type="range" id="sensorContrast" min="-2" max="2" step="1" oninput="onSensorSlider('contrast', this.value, 'sensorContrastVal')">
            </label>
            <label>Saturation: <span id="sensorSaturationVal">—</span>
              <input type="range" id="sensorSaturation" min="-2" max="2" step="1" oninput="onSensorSlider('saturation', this.value, 'sensorSaturationVal')">
            </label>
            <label>AE Level (exposure bias): <span id="sensorAeLevelVal">—</span>
              <input type="range" id="sensorAeLevel" min="-2" max="2" step="1" oninput="onSensorSlider('ae_level', this.value, 'sensorAeLevelVal')">
            </label>
            <label>Sharpness (OV3660 only): <span id="sensorSharpnessVal">—</span>
              <input type="range" id="sensorSharpness" min="-2" max="2" step="1" oninput="onSensorSlider('sharpness', this.value, 'sensorSharpnessVal')">
            </label>
            <label>Frame delay (ms, ↓ = more fps): <span id="sensorFrameDelayVal">—</span>
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

      <!-- Real-time camera info: every value is read from /api/peek/status (esp_camera state). -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Camera Info</div>
            <div class="card-subtitle">Live sensor + stream metrics — updates every second while streaming</div>
          </div>
          <span id="camInfoLive" class="badge info" style="display:none;"><span class="badge-dot"></span>LIVE</span>
        </div>

        <!-- Init diagnostics + retry. Shown only when esp_camera_init failed
             at boot — gives the user a way to recover from a half-seated
             camera connector or transient PSRAM brownout without rebooting. -->
        <div id="camInitDiag" style="display:none;margin-bottom:0.75rem;padding:0.6rem 0.75rem;border-radius:6px;border:1px solid #b45309;background:rgba(180,83,9,0.12);color:#fde68a;">
          <div style="font-weight:600;margin-bottom:0.25rem;">Camera not initialized</div>
          <div id="camInitDiagText" style="font-size:0.78rem;line-height:1.4;">—</div>
          <div style="margin-top:0.5rem;display:flex;gap:0.4rem;flex-wrap:wrap;">
            <button class="btn btn-sm btn-primary" id="camReinitBtn" onclick="reinitCamera()">↻ Re-initialize Camera</button>
          </div>
        </div>
        <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:0.6rem 1rem;font-size:0.82rem;">
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Sensor</div><div id="camSensor">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Pixel Format</div><div id="camPixFmt">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Resolution</div><div id="camRes">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">JPEG Quality</div><div id="camQuality" title="0–63, lower is higher quality">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">XCLK</div><div id="camXclk">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">PSRAM</div><div id="camPsram">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Frame Buffers</div><div id="camFbCount">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Frame Rate</div><div id="camFps">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Last Frame</div><div id="camLastFrame">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Avg Frame</div><div id="camAvgFrame">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Throughput</div><div id="camKbps">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Frames Sent</div><div id="camFrameCount">—</div></div>
          <div><div style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.05em;">Stream Uptime</div><div id="camUptime">—</div></div>
        </div>
        <p style="font-size:0.7rem;color:var(--muted);margin-top:0.75rem;">All values are read directly from the on-device camera driver (esp_camera) — no placeholder data.</p>
      </div>
    </div>

    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <!-- PRESENCE PANEL - WiFi Presence Detection + Audible Chirp -->
    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <div class="panel" id="panel-presence">
      <!-- Headline: Who's Home (auto-context indicator + manual override) -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Who's Home</div>
            <div class="card-subtitle" id="presenceSubtitle">Loading…</div>
          </div>
          <div class="badge info" id="presenceBadge">
            <span class="badge-dot"></span>
            <span id="presenceBadgeText">—</span>
          </div>
        </div>
        <div style="margin-top:0.6rem;display:flex;gap:0.4rem;flex-wrap:wrap;">
          <button class="btn btn-sm btn-secondary" onclick="setPresenceOverride('home')">I'm home</button>
          <button class="btn btn-sm btn-secondary" onclick="setPresenceOverride('away')">I'm away</button>
          <button class="btn btn-sm btn-secondary" onclick="setPresenceOverride('quiet')">Quiet hours</button>
          <button class="btn btn-sm btn-ghost" id="presenceClearOverrideBtn" onclick="clearPresenceOverride()" style="display:none;">Clear override</button>
        </div>
        <div id="presenceOverrideHint" style="display:none;margin-top:0.5rem;font-size:0.7rem;color:var(--muted);"></div>
      </div>

      <!-- Household Devices: paired phones with role selectors -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Household Devices</div>
            <div class="card-subtitle">Tag your own phone <strong>Owner</strong> so the device knows you're home and stops alerting on your arrivals.</div>
          </div>
        </div>
        <div id="householdList" class="log-list" style="max-height:260px;">
          <p style="color:var(--muted);font-size:0.85rem;text-align:center;padding:0.75rem;">Loading…</p>
        </div>
      </div>

      <!-- WiFi Presence Detection -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">WiFi Presence Detection</div>
            <div class="card-subtitle">Privacy-preserving device counting via probe requests</div>
          </div>
          <div class="badge info" id="wifiPresenceBadge">
            <span class="badge-dot"></span>
            <span id="wifiPresenceState">Loading...</span>
          </div>
        </div>
        <div class="stats-grid">
          <div class="stat-item">
            <div class="stat-label">Devices Now</div>
            <div class="stat-value" id="wpCurrentCount">0</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Last Bucket</div>
            <div class="stat-value" id="wpLastCount">0</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Peak</div>
            <div class="stat-value" id="wpPeakCount">0</div>
          </div>
          <div class="stat-item">
            <div class="stat-label">Total Probes</div>
            <div class="stat-value" id="wpTotalProbes">0</div>
          </div>
        </div>
        <!-- History sparkline -->
        <div style="margin-top:1rem;">
          <div class="form-label">History (last 10 minutes)</div>
          <div id="wpSparkline" style="display:flex;align-items:flex-end;gap:2px;height:40px;padding:0.5rem 0;"></div>
        </div>
        <div class="toggle-row" style="margin-top:1rem;">
          <div class="toggle-info">
            <div class="toggle-title">WiFi Presence Monitoring</div>
            <div class="toggle-desc">Listen for probe requests from nearby devices</div>
          </div>
          <label style="cursor:pointer;">
            <input type="checkbox" id="wpEnabled" onchange="toggleWifiPresence()">
          </label>
        </div>
        <div style="margin-top:0.75rem;padding:0.75rem;background:rgba(99,179,237,0.1);border-radius:8px;font-size:0.75rem;color:var(--muted);">
          <strong>Privacy:</strong> MAC addresses are hashed immediately and never stored.
          Only anonymous device counts are recorded. Hashes rotate every bucket window
          so devices cannot be tracked across time.
        </div>
      </div>

      <!-- BLE Presence -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Bluetooth Presence</div>
            <div class="card-subtitle" id="blePresenceSubtitle">BLE device detection</div>
          </div>
        </div>
        <div id="blePresenceContent">
          <div style="padding:1rem;background:rgba(246,173,85,0.1);border-radius:8px;font-size:0.8rem;">
            <strong>BLE Status:</strong>
            <span id="blePresenceStatus">Checking...</span>
          </div>
        </div>
      </div>

      <!-- Audible Chirp Controls -->
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">Canary Chirp</div>
            <div class="card-subtitle" id="chirpSubtitleText">Local audible/visual alerts</div>
          </div>
          <div class="badge info" id="chirpHwBadge">
            <span class="badge-dot"></span>
            <span id="chirpHwState">Loading...</span>
          </div>
        </div>
        <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
          <button class="btn btn-primary btn-sm" onclick="playAudibleChirp('confirm')" title="Play two short beeps — device confirmation">Test</button>
          <button class="btn btn-secondary btn-sm" onclick="playAudibleChirp('alert')" title="Play rising three-tone alert">Alert</button>
          <button class="btn btn-secondary btn-sm" onclick="playAudibleChirp('success')" title="Play pleasant ascending tone">Success</button>
          <button class="btn btn-secondary btn-sm" onclick="playAudibleChirp('error')" title="Play descending error tone">Error</button>
        </div>
        <div id="chirpHwResult" style="margin-top:0.5rem;font-size:0.8rem;min-height:1.2em;"></div>
        <div style="margin-top:0.75rem;font-size:0.8rem;color:var(--muted);">
          <span id="chirpHwMode">Mode: Checking...</span><br>
          <span style="font-size:0.75rem;">Connect a passive buzzer to the configured GPIO for audio alerts.
          Without a buzzer, chirp uses LED blink patterns.</span>
        </div>
        <div class="toggle-row" style="margin-top:0.75rem;">
          <div class="toggle-info">
            <div class="toggle-title">Visual Only Mode</div>
            <div class="toggle-desc">Use LED blinks instead of buzzer tones</div>
          </div>
          <label style="cursor:pointer;">
            <input type="checkbox" id="chirpVisualOnly" onchange="setChirpVisualOnly()">
          </label>
        </div>
      </div>
    </div>

    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <!-- COMMUNITY PANEL - Opera (Mesh) + Chirp Combined -->
    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <div class="panel" id="panel-community">
      <!-- Sub-navigation -->
      <div class="sub-nav">
        <button class="sub-nav-btn active" onclick="switchCommunityTab('opera')">🔗 Opera Network</button>
        <button class="sub-nav-btn" onclick="switchCommunityTab('chirp')">🐦 Chirp Channel</button>
        <button class="sub-nav-btn" onclick="switchCommunityTab('ble-discovery')">📡 BLE Discovery</button>
      </div>

      <!-- BLE DISCOVERY SUB-PANEL -->
      <div class="sub-panel" id="community-ble-discovery">
        <div class="feature-info">
          <h4>📡 BLE Discovery (Opera/Chirp/Nearby)</h4>
          <p>Bluetooth Low Energy subsystem for device presence, broadcast alerts, and proximity detection.</p>
          <ul>
            <li><strong>Opera:</strong> Advertises device presence — discoverable by other Canaries and phones</li>
            <li><strong>Chirp:</strong> Connectionless broadcast alerts between Canary devices</li>
            <li><strong>Nearby:</strong> Scans for and tracks other Canary devices by signal strength</li>
          </ul>
          <div class="privacy-note">🔒 Non-Canary BLE devices are counted only — never individually logged. No MAC addresses stored.</div>
        </div>

        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">BLE Status</div>
              <div class="card-subtitle" id="bleSubtitle">BLE discovery subsystem</div>
            </div>
            <div class="badge info" id="bleStateBadge">
              <span class="badge-dot"></span>
              <span id="bleStateText">Loading...</span>
            </div>
          </div>
          <div class="stats-grid">
            <div class="stat-item">
              <div class="stat-label">Opera</div>
              <div class="stat-value" style="font-size:0.9rem;" id="bleOperaState">--</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Chirp Sent</div>
              <div class="stat-value" id="bleChirpSent">0</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Chirp Recv</div>
              <div class="stat-value" id="bleChirpRecv">0</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Connections</div>
              <div class="stat-value" id="bleConnections">0</div>
            </div>
          </div>
        </div>

        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Nearby Canaries</div>
              <div class="card-subtitle" id="bleNearbySubtitle">Scanning...</div>
            </div>
          </div>
          <div id="bleNearbyList">
            <div class="empty-state"><div class="empty-icon">📡</div><p>No nearby Canaries detected</p></div>
          </div>
        </div>

        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Send Chirp Alert</div>
              <div class="card-subtitle">Broadcast an alert to nearby Canaries</div>
            </div>
          </div>
          <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
            <button class="btn btn-primary" onclick="sendBleChirp('alert')">🚨 Alert</button>
            <button class="btn btn-secondary" onclick="sendBleChirp('heartbeat')">💓 Heartbeat</button>
          </div>
          <div id="bleChirpResult" style="margin-top:0.5rem;font-size:0.85rem;color:var(--muted);"></div>
        </div>
      </div>

      <!-- OPERA SUB-PANEL -->
      <div class="sub-panel active" id="community-opera">
        <!-- What is Opera? Info Box -->
        <div class="feature-info">
          <h4>🔗 What is Opera Network?</h4>
          <p>Opera is a secure mesh network connecting your SecuraCV devices for mutual protection.</p>
          <ul>
            <li><strong>What it does:</strong> Devices alert each other of tamper events or power loss</li>
            <li><strong>How it works:</strong> Direct device-to-device communication (ESP-NOW), no internet required</li>
            <li><strong>Privacy:</strong> Only communicates with devices YOU pair — requires physical confirmation</li>
          </ul>
          <div class="privacy-note">🔒 No cloud servers. Your opera, your devices, your control.</div>
        </div>

        <!-- Opera Status Card -->
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Opera Status</div>
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
              <div class="stat-label">Peers Online</div>
              <div class="stat-value"><span id="peersOnline">0</span> / <span id="peersTotal">0</span></div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Alerts</div>
              <div class="stat-value" id="alertsReceived">0</div>
            </div>
          </div>
          <div class="toggle-row" style="margin-top:1rem;">
            <div class="toggle-info">
              <div class="toggle-title">Enable Mesh Network</div>
              <div class="toggle-desc">Communicate with other canaries in your opera</div>
            </div>
            <label style="cursor:pointer;">
              <input type="checkbox" id="meshEnabled" onchange="toggleMeshEnabled()">
            </label>
          </div>
        </div>

        <!-- Opera Members Card -->
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Opera Members</div>
              <div class="card-subtitle" id="peersSubtitle">Devices in your opera</div>
            </div>
            <button class="btn btn-ghost btn-sm" onclick="refreshOpera()">Refresh</button>
          </div>
          <div id="peersList" class="log-list" style="max-height:250px;">
            <div class="empty-state"><div class="empty-icon">🐦</div><p>No opera configured</p></div>
          </div>
        </div>

        <!-- Opera Alerts Card -->
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Opera Alerts</div>
              <div class="card-subtitle">Alerts from other canaries</div>
            </div>
            <button class="btn btn-ghost btn-sm" onclick="clearOperaAlerts()">Clear</button>
          </div>
          <div id="operaAlertsList" class="log-list" style="max-height:200px;">
            <div class="empty-state"><div class="empty-icon">✓</div><p>No alerts</p></div>
          </div>
        </div>

        <!-- Opera Management Card -->
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Opera Management</div>
              <div class="card-subtitle">Create, join, or leave opera</div>
            </div>
          </div>
          <div id="operaNoOpera">
            <p style="color:var(--muted);margin-bottom:1rem;font-size:0.85rem;">
              Create a new opera or join an existing one.
            </p>
            <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
              <button class="btn btn-primary" onclick="startPairing('init')">Create Opera</button>
              <button class="btn btn-secondary" onclick="startPairing('join')">Join Opera</button>
            </div>
          </div>
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
          <div id="operaPairing" style="display:none;">
            <div style="text-align:center;padding:1rem;">
              <div class="spinner" style="margin:0 auto 1rem;"></div>
              <p id="pairingStatus" style="color:var(--muted);margin-bottom:1rem;">Searching...</p>
              <div id="pairingCode" style="display:none;margin-bottom:1rem;">
                <p style="font-size:0.85rem;color:var(--muted);margin-bottom:0.5rem;">Confirm code matches:</p>
                <div style="font-family:var(--mono);font-size:2rem;font-weight:bold;color:var(--accent);letter-spacing:0.2em;" id="pairingCodeValue">------</div>
              </div>
              <div style="display:flex;gap:0.5rem;justify-content:center;">
                <button class="btn btn-primary" id="pairingConfirmBtn" onclick="confirmPairing()" style="display:none;">Confirm</button>
                <button class="btn btn-secondary" onclick="cancelPairing()">Cancel</button>
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- CHIRP SUB-PANEL -->
      <div class="sub-panel" id="community-chirp">
        <!-- What is Chirp? Info Box -->
        <div class="feature-info">
          <h4>🐦 What is Chirp Channel?</h4>
          <p>Chirp is an anonymous community alert system for sharing local awareness.</p>
          <ul>
            <li><strong>What it does:</strong> Share and receive community alerts using pre-defined templates</li>
            <li><strong>What it doesn't do:</strong> NO free text, NO tracking, NO permanent records</li>
            <li><strong>Privacy:</strong> New anonymous identity each session — emojis, not names</li>
            <li><strong>Philosophy:</strong> "Witness authority, not neighbors" — NO surveillance templates</li>
          </ul>
          <div class="privacy-note">🔒 Ephemeral identity. Template-only messages. 3-hop max range.</div>
        </div>

        <!-- Chirp Status Card -->
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Chirp Status</div>
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
              <div class="stat-label">Nearby</div>
              <div class="stat-value" id="chirpNearbyCount">0</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Recent</div>
              <div class="stat-value" id="chirpRecentCount">0</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Cooldown</div>
              <div class="stat-value" id="chirpCooldown" style="font-size:0.9rem;">Ready</div>
            </div>
          </div>
          <div class="toggle-row" style="margin-top:1rem;">
            <div class="toggle-info">
              <div class="toggle-title">Enable Chirp Channel</div>
              <div class="toggle-desc">Join anonymous community network (new identity each session)</div>
            </div>
            <label style="cursor:pointer;">
              <input type="checkbox" id="chirpEnabled" onchange="toggleChirpEnabled()">
            </label>
          </div>
        </div>

        <!-- Send Chirp Card -->
        <div class="card" id="chirpSendCard" style="display:none;">
          <div class="card-header">
            <div>
              <div class="card-title">Share with Community</div>
              <div class="card-subtitle">Template-based alerts only</div>
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
              </optgroup>
              <optgroup label="Infrastructure">
                <option value="16">⚡ power outage</option>
                <option value="17">💧 water service disruption</option>
                <option value="18">🔥 gas smell - evacuate?</option>
                <option value="20">🚧 road closed or blocked</option>
              </optgroup>
              <optgroup label="Emergency">
                <option value="32">🔥 fire or smoke visible</option>
                <option value="33">🚑 medical emergency scene</option>
                <option value="35">📢 evacuation in progress</option>
              </optgroup>
              <optgroup label="All Clear">
                <option value="128">✅ situation resolved</option>
                <option value="129">✅ area appears safe now</option>
              </optgroup>
            </select>
          </div>
          <div class="form-group">
            <label class="form-label">Urgency</label>
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
          <p style="font-size:0.75rem;color:var(--muted);margin-bottom:1rem;">
            Requires 2 neighbor confirmations before spreading. No free text — privacy by design.
          </p>
          <button class="btn btn-primary" id="chirpSendBtn" onclick="sendChirp()" style="width:100%;">Send Chirp</button>
          <p id="chirpPresenceHint" style="font-size:0.75rem;color:var(--warning);margin-top:0.5rem;display:none;text-align:center;">
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
          <div id="chirpList" class="log-list" style="max-height:300px;">
            <div class="empty-state"><div class="empty-icon">🐦</div><p>No community alerts</p></div>
          </div>
        </div>

        <!-- Chirp Settings Card -->
        <div class="card" id="chirpSettingsCard" style="display:none;">
          <div class="card-header">
            <div>
              <div class="card-title">Chirp Settings</div>
              <div class="card-subtitle">Customize your experience</div>
            </div>
          </div>
          <div class="toggle-row">
            <div class="toggle-info">
              <div class="toggle-title">Relay others' chirps</div>
              <div class="toggle-desc">Help extend range by forwarding</div>
            </div>
            <label style="cursor:pointer;">
              <input type="checkbox" id="chirpRelayEnabled" checked onchange="updateChirpSettings()">
            </label>
          </div>
          <div style="margin-top:0.5rem;display:flex;gap:0.5rem;flex-wrap:wrap;">
            <button class="btn btn-secondary btn-sm" onclick="muteChirps(15)">Mute 15m</button>
            <button class="btn btn-secondary btn-sm" onclick="muteChirps(30)">Mute 30m</button>
            <button class="btn btn-secondary btn-sm" onclick="muteChirps(60)">Mute 1h</button>
            <button class="btn btn-ghost btn-sm" onclick="unmuteChirps()" id="chirpUnmuteBtn" style="display:none;">Unmute</button>
          </div>
        </div>
      </div>
    </div>

    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <!-- RECORDS PANEL - Logs + Witness Combined -->
    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <div class="panel" id="panel-records">
      <!-- Sub-navigation -->
      <div class="sub-nav">
        <button class="sub-nav-btn active" onclick="switchRecordsTab('logs')">📋 System Logs</button>
        <button class="sub-nav-btn" onclick="switchRecordsTab('witness')">🔐 Witness Chain</button>
      </div>

      <!-- LOGS SUB-PANEL -->
      <div class="sub-panel active" id="records-logs">
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">System Logs</div>
              <div class="card-subtitle" id="logsSubtitle">All events and diagnostics</div>
            </div>
            <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
              <button class="btn btn-secondary btn-sm" onclick="filterLogs('all')">All</button>
              <button class="btn btn-secondary btn-sm" onclick="filterLogs('unread')">Unread</button>
              <button class="btn btn-danger btn-sm" onclick="ackAllLogs()" title="Acknowledge all unread log entries">Ack All</button>
            </div>
          </div>
          <div class="log-list" id="logList">
            <div class="loading"><div class="spinner"></div></div>
          </div>
        </div>
      </div>

      <!-- WITNESS SUB-PANEL -->
      <div class="sub-panel" id="records-witness">
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Witness Records</div>
              <div class="card-subtitle" id="witnessSubtitle">Cryptographically signed events</div>
            </div>
            <button class="btn btn-primary btn-sm" onclick="exportWitness()" title="Download witness record bundle for verification">⬇ Export</button>
          </div>
          <div class="log-list" id="witnessList">
            <div class="loading"><div class="spinner"></div></div>
          </div>
        </div>
        <div class="feature-info">
          <h4>🔐 About Witness Records</h4>
          <p>Each witness record is cryptographically signed and chained to previous records, creating tamper-evident proof of events.</p>
          <ul>
            <li>Records cannot be modified without breaking the chain</li>
            <li>Export for verification by third parties</li>
            <li>Chain hash proves integrity of all previous records</li>
          </ul>
        </div>
      </div>
    </div>

    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <!-- SETTINGS PANEL - WiFi + Bluetooth + Device Config All Together -->
    <!-- ═══════════════════════════════════════════════════════════════════════ -->
    <div class="panel" id="panel-settings">
      <!-- Sub-navigation -->
      <div class="sub-nav">
        <button class="sub-nav-btn active" onclick="switchSettingsTab('wifi')">📶 WiFi</button>
        <button class="sub-nav-btn" onclick="switchSettingsTab('bluetooth')">📱 Bluetooth</button>
        <button class="sub-nav-btn" onclick="switchSettingsTab('device')">⚙️ Device</button>
        <button class="sub-nav-btn" onclick="switchSettingsTab('rf')">📡 RF Presence</button>
      </div>

      <!-- WIFI SETTINGS -->
      <div class="sub-panel active" id="settings-wifi">
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
          <div class="stats-grid" style="margin-bottom:1rem;">
            <div class="stat-item">
              <div class="stat-label">Device AP</div>
              <div class="stat-value" style="font-size:0.85rem;" id="wifiApSsid">--</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">AP IP</div>
              <div class="stat-value" style="font-size:0.85rem;" id="wifiApIp">--</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Home WiFi</div>
              <div class="stat-value" style="font-size:0.85rem;" id="wifiStaSsid">Not configured</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Home IP</div>
              <div class="stat-value" style="font-size:0.85rem;" id="wifiStaIp">--</div>
            </div>
          </div>
          <div id="wifiRssiBar" style="margin-bottom:1rem;display:none;">
            <div class="stat-label">Signal Strength</div>
            <div style="display:flex;align-items:center;gap:0.5rem;">
              <span id="wifiRssiBars" style="font-family:monospace;letter-spacing:1px;font-size:0.95rem;color:var(--success);">▮▮▮▮</span>
              <div style="flex:1;height:8px;background:rgba(0,0,0,0.3);border-radius:4px;overflow:hidden;">
                <div id="wifiRssiLevel" style="height:100%;background:var(--success);width:0%;transition:width 0.3s;"></div>
              </div>
              <span id="wifiRssiQuality" style="font-size:0.75rem;color:var(--muted);min-width:4.5em;text-align:right;">--</span>
              <span id="wifiRssiValue" style="font-size:0.75rem;color:var(--muted);">-- dBm</span>
            </div>
          </div>
          <div id="wifiFailReason" style="display:none;margin-bottom:0.75rem;padding:0.5rem 0.75rem;border-radius:6px;background:var(--danger-dim);color:var(--danger);font-size:0.85rem;"></div>
          <div id="wifiSetupSection">
            <div class="form-group">
              <label class="form-label">Home WiFi Network</label>
              <div style="display:flex;gap:0.5rem;">
                <select class="form-input" id="wifiSsidSelect" style="flex:1;">
                  <option value="">-- Select network --</option>
                </select>
                <button class="btn btn-secondary" onclick="scanWifi()" id="wifiScanBtn">Scan</button>
              </div>
              <input type="text" class="form-input" id="wifiSsidInput" placeholder="Or enter SSID manually" style="margin-top:0.5rem;">
            </div>
            <div class="form-group">
              <label class="form-label">Password</label>
              <input type="password" class="form-input" id="wifiPassword" placeholder="WiFi password">
            </div>
            <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
              <button class="btn btn-primary" onclick="connectWifi()" id="wifiConnectBtn">Connect</button>
              <button class="btn btn-secondary" onclick="disconnectWifi()" id="wifiDisconnectBtn" style="display:none;">Disconnect</button>
              <button class="btn btn-danger" onclick="forgetWifi()" id="wifiForgetBtn" style="display:none;">Remove credentials</button>
            </div>
          </div>
          <div id="wifiProgress" style="display:none;margin-top:1rem;">
            <div style="display:flex;align-items:center;gap:0.75rem;">
              <div class="spinner"></div>
              <span id="wifiProgressText" style="color:var(--muted);">Connecting...</span>
            </div>
          </div>
        </div>
      </div>

      <!-- BLUETOOTH SETTINGS -->
      <div class="sub-panel" id="settings-bluetooth">
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Bluetooth Status</div>
              <div class="card-subtitle" id="btSubtitle">BLE connectivity</div>
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
              <div class="stat-label">TX Power</div>
              <div class="stat-value" id="btTxPower">--<span class="stat-unit">dBm</span></div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Paired</div>
              <div class="stat-value" id="btPairedCount">0</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Nearby</div>
              <div class="stat-value" id="btNearbyCount">0</div>
            </div>
            <div class="stat-item">
              <div class="stat-label">MTU</div>
              <div class="stat-value" id="btMtu">--<span class="stat-unit">B</span></div>
            </div>
            <div class="stat-item">
              <div class="stat-label">Battery</div>
              <div class="stat-value" id="btBattery">--<span class="stat-unit">%</span></div>
            </div>
          </div>
          <!-- Live connection panel (Find My-style: name, signal bars, distance) -->
          <div id="btConnLive" style="display:none;margin-top:1rem;padding:0.85rem;border:1px solid var(--border);border-radius:8px;background:var(--card-hover,#1a2340);">
            <div style="display:flex;align-items:center;gap:0.75rem;">
              <div style="font-size:1.6rem;">📱</div>
              <div style="flex:1;min-width:0;">
                <div style="font-weight:600;" id="btConnName">Connected device</div>
                <div style="font-size:0.7rem;color:var(--muted);font-family:var(--mono,monospace);" id="btConnAddr">--</div>
              </div>
              <div style="text-align:right;">
                <div style="font-weight:600;" id="btConnDistance">— m</div>
                <div style="font-size:0.7rem;color:var(--muted);" id="btConnDistanceLabel">unknown</div>
              </div>
            </div>
            <div style="display:flex;align-items:center;gap:0.5rem;margin-top:0.5rem;">
              <div class="signal-bars" id="btConnBars" style="display:inline-flex;gap:2px;align-items:flex-end;height:14px;">
                <span></span><span></span><span></span><span></span>
              </div>
              <span style="font-size:0.75rem;color:var(--muted);" id="btConnRssi">— dBm</span>
              <span style="margin-left:auto;font-size:0.7rem;color:var(--muted);" id="btConnSecurity">—</span>
            </div>
            <div style="margin-top:0.5rem;display:flex;gap:0.5rem;">
              <button class="btn btn-sm btn-danger" onclick="btDisconnect()">Disconnect</button>
            </div>
          </div>
          <div class="toggle-row" style="margin-top:1rem;">
            <div class="toggle-info">
              <div class="toggle-title">Bluetooth Enabled</div>
              <div class="toggle-desc">Allow BLE connections</div>
            </div>
            <label style="cursor:pointer;">
              <input type="checkbox" id="btEnabled" onchange="toggleBtEnabled()">
            </label>
          </div>
        </div>

        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Bluetooth Controls</div>
              <div class="card-subtitle">Advertising and pairing</div>
            </div>
          </div>
          <div style="display:flex;gap:0.5rem;flex-wrap:wrap;">
            <button class="btn btn-primary" id="btAdvBtn" onclick="toggleBtAdvertising()">Start Advertising</button>
            <button class="btn btn-secondary" id="btPairBtn" onclick="btStartPairing()">Start Pairing</button>
          </div>
          <div id="btPairingBox" style="display:none;margin-top:1rem;padding:0.75rem;background:var(--surface-2,#222);border:1px solid var(--border);border-radius:6px;">
            <div style="font-size:0.8rem;color:var(--muted);margin-bottom:0.25rem;" id="btPairingState">Pairing mode active</div>
            <div style="font-family:monospace;font-size:1.6rem;letter-spacing:0.2em;text-align:center;" id="btPairingPin">------</div>
            <div style="font-size:0.75rem;color:var(--muted);margin-top:0.25rem;text-align:center;" id="btPairingHint">Open Bluetooth on your phone, connect to this device, and confirm the PIN.</div>
            <!-- Confirm / Reject only render in the 'confirming' state. The
                 user MUST visually compare the PIN above against the one on
                 their phone before tapping Numbers match — that's the bit
                 that prevents an MITM from coercing the bond. -->
            <div id="btPairingConfirmRow" style="display:none;gap:0.5rem;margin-top:0.6rem;justify-content:center;">
              <button class="btn btn-sm btn-primary" onclick="btConfirmPairing()">Numbers match</button>
              <button class="btn btn-sm btn-danger" onclick="btRejectPairing()">Don't match</button>
            </div>
            <div style="display:flex;gap:0.5rem;margin-top:0.5rem;justify-content:center;">
              <button class="btn btn-sm btn-secondary" onclick="btCancelPairing()">Cancel Pairing</button>
            </div>
          </div>
          <div class="toggle-row" style="margin-top:1rem;">
            <div class="toggle-info">
              <div class="toggle-title">Auto-advertise on boot</div>
              <div class="toggle-desc">Automatically start advertising</div>
            </div>
            <label style="cursor:pointer;">
              <input type="checkbox" id="btAutoAdv" onchange="saveBtSettings()">
            </label>
          </div>
          <div class="toggle-row">
            <div class="toggle-info">
              <div class="toggle-title">Allow new pairings</div>
              <div class="toggle-desc">Accept pairing requests</div>
            </div>
            <label style="cursor:pointer;">
              <input type="checkbox" id="btAllowPairing" onchange="saveBtSettings()">
            </label>
          </div>
          <div class="toggle-row">
            <div class="toggle-info">
              <div class="toggle-title">Long-range mode (Coded PHY S=8)</div>
              <div class="toggle-desc">~4× range, ~125 kbps. Peer must support BLE 5.0 LR.</div>
            </div>
            <label style="cursor:pointer;">
              <input type="checkbox" id="btLongRange" onchange="saveBtSettings()">
            </label>
          </div>
        </div>

        <!-- BLE OTA status. Hidden when idle; surfaces when a paired
             client begins streaming a signed firmware image over GATT. -->
        <div class="card" id="btOtaCard" style="display:none;">
          <div class="card-header">
            <div>
              <div class="card-title">BLE Firmware Update</div>
              <div class="card-subtitle" id="btOtaSubtitle">Receiving image over BLE…</div>
            </div>
            <div class="badge info" id="btOtaBadge">
              <span class="badge-dot"></span>
              <span id="btOtaStateText">idle</span>
            </div>
          </div>
          <div style="margin-top:0.75rem;">
            <div style="display:flex;justify-content:space-between;font-size:0.75rem;color:var(--muted);margin-bottom:0.25rem;">
              <span id="btOtaProgressLabel">0%</span>
              <span id="btOtaBytesLabel">0 / 0 B</span>
            </div>
            <div style="height:6px;background:var(--surface-2,#222);border-radius:3px;overflow:hidden;">
              <div id="btOtaProgressBar" style="height:100%;width:0;background:var(--accent);transition:width 0.3s;"></div>
            </div>
            <div id="btOtaError" style="display:none;margin-top:0.5rem;font-size:0.75rem;color:var(--danger,#e44);"></div>
          </div>
        </div>

        <!-- Nearby Bluetooth scanner — mirrors the WiFi scan presentation
             so people get the same "X devices nearby" affordance for both. -->
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Nearby Bluetooth Devices</div>
              <div class="card-subtitle" id="btScanSubtitle">Tap Scan to see what's around</div>
            </div>
            <div style="display:flex;gap:0.5rem;">
              <button class="btn btn-secondary btn-sm" id="btScanBtn" onclick="btStartScan()">Scan</button>
              <button class="btn btn-ghost btn-sm" onclick="btClearScan()" title="Clear results">Clear</button>
            </div>
          </div>
          <div id="btScanList" class="log-list" style="max-height:280px;">
            <p style="color:var(--muted);font-size:0.85rem;text-align:center;padding:1rem;">No scan yet</p>
          </div>
        </div>

        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">Paired Devices</div>
              <div class="card-subtitle" id="btPairedSubtitle">Trusted connections</div>
            </div>
            <button class="btn btn-danger btn-sm" onclick="btClearAllPaired()">Clear All</button>
          </div>
          <div id="btPairedList" class="log-list" style="max-height:200px;">
            <p style="color:var(--muted);font-size:0.85rem;text-align:center;padding:1rem;">No paired devices</p>
          </div>
        </div>
      </div>

      <!-- DEVICE SETTINGS -->
      <div class="sub-panel" id="settings-device">
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
          <div style="display:flex;gap:0.5rem;margin-top:1rem;flex-wrap:wrap;">
            <button class="btn btn-primary" onclick="saveConfig()" title="Save current settings to device NVS">Save Configuration</button>
            <button class="btn btn-danger" onclick="confirmReboot()" title="Restart the device (witness chain preserved)">Reboot Device</button>
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
            <button class="btn btn-secondary" onclick="rotateOldLogs()" title="Delete health logs older than 30 days from SD card">Rotate Old Logs (30+ days)</button>
          </div>
        </div>
      </div>

      <!-- RF PRESENCE SETTINGS -->
      <div class="sub-panel" id="settings-rf">
        <div class="feature-info">
          <h4>📡 About RF Presence Detection</h4>
          <p>RF Presence detects nearby BLE/WiFi devices to determine occupancy without storing any identifying information.</p>
          <ul>
            <li><strong>What it stores:</strong> Anonymous aggregate counts only</li>
            <li><strong>What it NEVER stores:</strong> MAC addresses, device names, any identifiers</li>
            <li><strong>Session rotation:</strong> Internal tokens rotate automatically for privacy</li>
          </ul>
          <div class="privacy-note">🔒 Conformance-tested privacy guarantees. No surveillance.</div>
        </div>

        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">RF Presence Settings</div>
              <div class="card-subtitle">Configure detection parameters</div>
            </div>
          </div>
          <div class="form-group">
            <label class="form-label">Presence Threshold (seconds)</label>
            <input type="number" class="form-input" id="rfPresenceThreshold" value="5" min="1" max="60">
            <p style="font-size:0.7rem;color:var(--muted);margin-top:0.25rem;">Time before detecting presence</p>
          </div>
          <div class="form-group">
            <label class="form-label">Dwell Threshold (seconds)</label>
            <input type="number" class="form-input" id="rfDwellThreshold" value="30" min="10" max="300">
            <p style="font-size:0.7rem;color:var(--muted);margin-top:0.25rem;">Time before classifying as dwelling</p>
          </div>
          <div class="form-group">
            <label class="form-label">Lost Timeout (seconds)</label>
            <input type="number" class="form-input" id="rfLostTimeout" value="60" min="30" max="600">
            <p style="font-size:0.7rem;color:var(--muted);margin-top:0.25rem;">Time before marking presence lost</p>
          </div>
          <div class="toggle-row">
            <div class="toggle-info">
              <div class="toggle-title">Emit impulse events</div>
              <div class="toggle-desc">Generate events on presence changes</div>
            </div>
            <label style="cursor:pointer;">
              <input type="checkbox" id="rfEmitImpulse" checked onchange="saveRfSettings()">
            </label>
          </div>
          <div style="margin-top:1rem;display:flex;gap:0.5rem;flex-wrap:wrap;">
            <button class="btn btn-primary" onclick="saveRfSettings()">Save RF Settings</button>
            <button class="btn btn-secondary" onclick="rotateRfSession()">Rotate Session</button>
          </div>
        </div>
      </div>
    </div>

    <!-- Production Footer -->
    <footer style="margin-top:2rem;padding:1.5rem;border-top:1px solid var(--border);text-align:center;">
      <div style="display:flex;align-items:center;justify-content:center;gap:0.5rem;margin-bottom:0.75rem;">
        <span style="font-size:1.25rem;">🐥</span>
        <span style="font-weight:600;font-size:0.9rem;">SecuraCV Canary</span>
        <span style="color:var(--muted);font-size:0.75rem;">v3.1.0</span>
      </div>
      <p style="font-size:0.7rem;color:var(--muted);margin-bottom:0.5rem;">
        Privacy-first witness device for accountability without surveillance.
      </p>
      <p style="font-size:0.65rem;color:var(--muted);margin-bottom:0.5rem;">
        © 2024-2025 SecuraCV Project Contributors. Licensed under MIT License.
      </p>
      <div style="font-size:0.6rem;color:var(--muted);opacity:0.7;">
        <p>This device records semantic events, not continuous video. No cloud storage.</p>
        <p style="margin-top:0.25rem;">For support and documentation: <a href="https://github.com/securacv" style="color:var(--accent);">github.com/securacv</a></p>
      </div>
    </footer>
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
          Mark this entry as reviewed. Original preserved in append-only log.
        </p>
        <div class="form-group">
          <label class="form-label">Reason (optional)</label>
          <input type="text" class="form-input" id="ackReason" placeholder="e.g., Expected behavior">
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

    // ═══════════════════════════════════════════════════════════════
    // AUTH — Token stored ONLY in JS variable. No localStorage/cookies.
    // ═══════════════════════════════════════════════════════════════
    let apiToken = null;

    async function secureFetch(endpoint, options = {}) {
      const headers = options.headers || {};
      if (apiToken) {
        headers['Authorization'] = 'Bearer ' + apiToken;
      }
      options.headers = headers;
      try {
        const resp = await fetch(API_BASE + endpoint, options);
        if (resp.status === 401 || resp.status === 403) {
          apiToken = null;
          showAuthModal('Invalid or expired token. Please re-enter.');
          throw new Error('auth_required');
        }
        if (resp.status === 429) {
          const data = await resp.json();
          showAuthModal('Too many failed attempts. Retry in ' + data.retry_after_s + ' seconds.');
          throw new Error('rate_limited');
        }
        return resp;
      } catch (e) {
        if (e.message === 'auth_required' || e.message === 'rate_limited') throw e;
        throw e;
      }
    }

    function showAuthModal(msg) {
      document.getElementById('authOverlay').classList.remove('hidden');
      if (msg) {
        const s = document.getElementById('authStatus');
        s.textContent = msg;
        s.className = 'auth-status error';
      }
    }

    function hideAuthModal() {
      document.getElementById('authOverlay').classList.add('hidden');
      document.getElementById('authStatus').className = 'auth-status';
    }

    async function connectWithToken() {
      const input = document.getElementById('tokenInput').value.trim();
      if (!input.startsWith('cv_')) {
        const s = document.getElementById('authStatus');
        s.textContent = 'Token must start with cv_';
        s.className = 'auth-status error';
        return;
      }
      apiToken = input;
      try {
        const resp = await secureFetch('/api/status');
        if (resp.ok) {
          hideAuthModal();
          startDashboard();
        }
      } catch (e) {
        apiToken = null;
      }
    }

    async function requestFromDevice() {
      const s = document.getElementById('authStatus');
      s.textContent = 'Waiting... Press BOOT button on device now.';
      s.className = 'auth-status info';
      for (let i = 0; i < 30; i++) {
        try {
          const resp = await fetch(API_BASE + '/api/provisioning-receipt');
          if (resp.status === 200) {
            const receipt = await resp.json();
            apiToken = receipt.token;
            hideAuthModal();
            startDashboard();
            return;
          }
        } catch (e) { /* network error, keep trying */ }
        await new Promise(r => setTimeout(r, 2000));
      }
      s.textContent = 'Timeout. Press BOOT button and try again.';
      s.className = 'auth-status error';
    }

    // Load device info (no auth required) for the auth footer
    async function loadAuthDeviceInfo() {
      try {
        const resp = await fetch(API_BASE + '/api/device-info');
        if (resp.ok) {
          const d = await resp.json();
          document.getElementById('authDeviceId').textContent = d.device_id || '--';
          document.getElementById('authFw').textContent = 'FW: ' + (d.firmware || '--');
          document.getElementById('authTls').textContent = 'TLS: ' + (d.tls_enabled ? 'Yes' : 'No');
        }
      } catch (e) { /* ignore */ }
    }

    // Handle Enter key in token input
    document.addEventListener('DOMContentLoaded', () => {
      const ti = document.getElementById('tokenInput');
      if (ti) ti.addEventListener('keydown', (e) => { if (e.key === 'Enter') connectWithToken(); });
      loadAuthDeviceInfo();
    });

    // Refresh all dashboard data
    function refreshAll() {
      refreshStatus();
      refreshSystemHealth();
      loadChain();
      refreshRfStatus();
      refreshWifiPresence();
      refreshAudibleChirpStatus();
    }

    function startDashboard() {
      refreshAll();
      if (!window._refreshInterval) {
        window._refreshInterval = setInterval(refreshAll, 5000);
      }
    }

    let currentPanel = 'status';
    let currentCommunityTab = 'opera';
    let currentRecordsTab = 'logs';
    let currentSettingsTab = 'wifi';
    let pendingAckSeq = null;
    let logFilter = 'all';
    let peekActive = false;
    let cameraReady = false;
    let currentResolution = 8;

    // Navigation
    document.querySelectorAll('.nav-btn').forEach(btn => {
      btn.addEventListener('click', () => switchPanel(btn.dataset.panel));
    });

    function switchPanel(panel) {
      document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
      document.querySelector(`[data-panel="${panel}"]`).classList.add('active');
      document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
      document.getElementById(`panel-${panel}`).classList.add('active');
      currentPanel = panel;

      if (panel === 'records') { loadLogs(); loadWitness(); }
      else if (panel === 'camera') { refreshPeekStatus(); refreshSensorState(); }
      else if (panel === 'presence') { refreshPresence(); refreshHousehold(); refreshWifiPresence(); refreshAudibleChirpStatus(); }
      else if (panel === 'community') { refreshOpera(); refreshChirpStatus(); refreshBleDiscovery(); }
      else if (panel === 'settings') { loadWifiStatus(); refreshBtStatus(); loadBtPairedDevices(); refreshBtOtaStatus(); loadRfSettings(); }

      if (panel !== 'camera' && peekActive) stopPeek();
      if (panel !== 'camera') stopCamInfoPolling();
    }

    function switchCommunityTab(tab) {
      currentCommunityTab = tab;
      document.querySelectorAll('#panel-community .sub-nav-btn').forEach(b => b.classList.remove('active'));
      event.target.classList.add('active');
      document.querySelectorAll('#panel-community .sub-panel').forEach(p => p.classList.remove('active'));
      document.getElementById(`community-${tab}`).classList.add('active');
      if (tab === 'opera') refreshOpera();
      else if (tab === 'ble-discovery') refreshBleDiscovery();
      else refreshChirpStatus();
    }

    function switchRecordsTab(tab) {
      currentRecordsTab = tab;
      document.querySelectorAll('#panel-records .sub-nav-btn').forEach(b => b.classList.remove('active'));
      event.target.classList.add('active');
      document.querySelectorAll('#panel-records .sub-panel').forEach(p => p.classList.remove('active'));
      document.getElementById(`records-${tab}`).classList.add('active');
      if (tab === 'logs') loadLogs();
      else loadWitness();
    }

    function switchSettingsTab(tab) {
      currentSettingsTab = tab;
      document.querySelectorAll('#panel-settings .sub-nav-btn').forEach(b => b.classList.remove('active'));
      event.target.classList.add('active');
      document.querySelectorAll('#panel-settings .sub-panel').forEach(p => p.classList.remove('active'));
      document.getElementById(`settings-${tab}`).classList.add('active');
      if (tab === 'wifi') loadWifiStatus();
      else if (tab === 'bluetooth') { refreshBtStatus(); loadBtPairedDevices(); refreshBtOtaStatus(); }
      else if (tab === 'rf') loadRfSettings();
    }

    // API helper — uses secureFetch for authenticated requests
    async function api(endpoint, method = 'GET', body = null) {
      const opts = { method, headers: {} };
      if (body) { opts.headers['Content-Type'] = 'application/json'; opts.body = JSON.stringify(body); }
      try {
        const res = await secureFetch(endpoint, opts);
        const text = await res.text();
        try { return JSON.parse(text); }
        catch { return { ok: false, error: text.slice(0, 100) }; }
      } catch (e) {
        if (e.message === 'auth_required' || e.message === 'rate_limited') return { ok: false, error: e.message };
        return { ok: false, error: 'Network error' };
      }
    }

    // ══════════════════════════════════════════════════════════════════
    // STATUS & RF PRESENCE
    // ══════════════════════════════════════════════════════════════════
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
      document.getElementById('firmware').textContent = data.firmware || '3.0.0';

      if (data.gps) updateGps(data.gps);
      updateBadges(data);

      const unacked = data.unacked_count || 0;
      const recordsCount = document.getElementById('recordsCount');
      if (unacked > 0) { recordsCount.textContent = unacked; recordsCount.style.display = 'inline-flex'; }
      else { recordsCount.style.display = 'none'; }

      document.getElementById('sdFree').textContent = Math.round((data.sd_free || 0) / (1024 * 1024));
      document.getElementById('sdTotal').textContent = Math.round((data.sd_total || 0) / (1024 * 1024));
      document.getElementById('sdUsed').textContent = Math.round((data.sd_used || 0) / (1024 * 1024));

      if (typeof data.camera_ready !== 'undefined') cameraReady = data.camera_ready;

      // Update WiFi presence badges from status response
      if (data.presence) {
        const wpBadge = document.getElementById('wpBadge');
        const wpText = document.getElementById('wpBadgeText');
        if (data.presence.wifi_enabled && data.presence.wifi_count > 0) {
          wpBadge.className = 'badge success';
          wpText.textContent = data.presence.wifi_count + ' WP';
        } else if (data.presence.wifi_enabled) {
          wpBadge.className = 'badge info';
          wpText.textContent = '0 WP';
        } else {
          wpBadge.className = 'badge info';
          wpText.textContent = 'WP';
        }
      }
    }

    // Helper function to get temperature badge class based on state
    function getTempBadgeClass(state) {
      if (state.includes('CRIT')) return 'badge danger';
      if (state.includes('WARN')) return 'badge warning';
      return 'badge success';
    }

    // Helper function to get memory bar color based on usage percentage
    function getMemoryBarColor(pct, isDefault = 'var(--accent)') {
      if (pct > 85) return 'var(--danger)';
      if (pct > 70) return 'var(--warning)';
      return isDefault;
    }

    async function refreshSystemHealth() {
      const data = await api('/api/system');
      if (!data.temperature) return;

      // Temperature
      const temp = data.temperature.celsius;
      document.getElementById('sysTemp').textContent = temp.current?.toFixed(1) || '--';
      document.getElementById('sysTempF').textContent = data.temperature.fahrenheit?.current?.toFixed(1) || '--';
      document.getElementById('sysTempMin').textContent = temp.min?.toFixed(1) || '--';
      document.getElementById('sysTempMax').textContent = temp.max?.toFixed(1) || '--';
      document.getElementById('sysTempAvg').textContent = temp.avg?.toFixed(1) || '--';

      // Temperature state badge (using helper function)
      const state = data.temperature.state || 'NORMAL';
      document.getElementById('sysTempStateText').textContent = state;
      document.getElementById('sysTempState').className = getTempBadgeClass(state);

      // Temperature marker position (0-100°C range)
      const markerPct = Math.max(0, Math.min(100, temp.current || 50));
      document.getElementById('sysTempMarker').style.left = markerPct + '%';

      // Header temperature badge (using helper function)
      document.getElementById('tempStatus').textContent = (temp.current?.toFixed(0) || '--') + '°C';
      document.getElementById('tempBadge').className = getTempBadgeClass(state);

      // Memory - Heap
      if (data.memory?.heap) {
        const heap = data.memory.heap;
        const heapPct = heap.used_pct?.toFixed(0) || 0;
        document.getElementById('sysHeapPct').textContent = heapPct;
        const heapBar = document.getElementById('sysHeapBar');
        heapBar.style.width = heapPct + '%';
        heapBar.style.background = getMemoryBarColor(heapPct, 'var(--accent)');
        document.getElementById('sysHeapFree').textContent = Math.round((heap.free || 0) / 1024);
      }

      // Memory - PSRAM (with color-coding for consistency)
      if (data.memory?.psram?.available) {
        const psram = data.memory.psram;
        const psramUsed = psram.total - psram.free;
        const psramPct = psram.total > 0 ? ((psramUsed / psram.total) * 100).toFixed(0) : 0;
        document.getElementById('sysPsramPct').textContent = psramPct;
        const psramBar = document.getElementById('sysPsramBar');
        psramBar.style.width = psramPct + '%';
        psramBar.style.background = getMemoryBarColor(psramPct, 'var(--info)');
        document.getElementById('sysPsramFree').textContent = (psram.free / (1024 * 1024)).toFixed(1);
      } else {
        document.getElementById('sysPsramPct').textContent = 'N/A';
        document.getElementById('sysPsramFree').textContent = '--';
        document.getElementById('sysPsramBar').style.width = '0%';
      }

      // Device Info
      if (data.device) {
        const dev = data.device;
        document.getElementById('sysChip').textContent = (dev.model || 'ESP32') + ' rev ' + (dev.revision || '?');
        document.getElementById('sysCores').textContent = dev.cores || '--';
        document.getElementById('sysFreq').textContent = dev.freq_mhz || '--';
        document.getElementById('sysFlash').textContent = ((dev.flash_size || 0) / (1024 * 1024)).toFixed(0);
        document.getElementById('sysSketch').textContent = Math.round((data.memory?.sketch?.size || 0) / 1024);
        document.getElementById('sysMac').textContent = dev.mac || '--';
        document.getElementById('sysReset').textContent = (dev.reset_reason || 'unknown').replace(/_/g, ' ');
      }
    }

    async function refreshRfStatus() {
      const data = await api('/api/rf/status');
      if (!data.state) return;

      document.getElementById('rfEnabled').checked = data.enabled;
      document.getElementById('rfState').textContent = data.state;
      document.getElementById('rfConfidence').textContent = data.confidence;
      document.getElementById('rfDeviceCount').textContent = data.device_count;
      document.getElementById('rfDwellClass').textContent = data.dwell_class;

      const badge = document.getElementById('rfStateBadge');
      const text = document.getElementById('rfStateText');
      if (data.state === 'present' || data.state === 'dwelling') {
        badge.className = 'badge success';
        text.textContent = data.state;
      } else if (data.state === 'absent') {
        badge.className = 'badge info';
        text.textContent = 'Absent';
      } else {
        badge.className = 'badge info';
        text.textContent = data.enabled ? data.state : 'Disabled';
      }

      // Update header badge
      const rfBadge = document.getElementById('rfBadge');
      const rfStatus = document.getElementById('rfStatus');
      if (data.enabled && data.device_count > 0) {
        rfBadge.className = 'badge success';
        rfStatus.textContent = data.device_count + ' RF';
      } else {
        rfBadge.className = 'badge info';
        rfStatus.textContent = 'RF';
      }
    }

    async function toggleRfEnabled() {
      const enabled = document.getElementById('rfEnabled').checked;
      await api(enabled ? '/api/rf/enable' : '/api/rf/disable', 'POST');
      refreshRfStatus();
    }

    async function loadRfSettings() {
      const data = await api('/api/rf/settings');
      if (data.presence_threshold_sec) {
        document.getElementById('rfPresenceThreshold').value = data.presence_threshold_sec;
        document.getElementById('rfDwellThreshold').value = data.dwell_threshold_sec;
        document.getElementById('rfLostTimeout').value = data.lost_timeout_sec;
        document.getElementById('rfEmitImpulse').checked = data.emit_impulse_events;
      }
    }

    async function saveRfSettings() {
      const settings = {
        presence_threshold_sec: parseInt(document.getElementById('rfPresenceThreshold').value),
        dwell_threshold_sec: parseInt(document.getElementById('rfDwellThreshold').value),
        lost_timeout_sec: parseInt(document.getElementById('rfLostTimeout').value),
        emit_impulse_events: document.getElementById('rfEmitImpulse').checked
      };
      const data = await api('/api/rf/settings', 'POST', settings);
      alert(data.success ? 'RF settings saved!' : 'Failed: ' + (data.error || 'Unknown'));
    }

    async function rotateRfSession() {
      const data = await api('/api/rf/rotate', 'POST');
      alert(data.success ? 'Session rotated!' : 'Failed');
      refreshRfStatus();
    }

    function updateGps(gps) {
      // Backend emits gps.fix (bool) — gps.valid never existed; the previous
      // check "gps.valid && ..." was always false, which is why the UI showed
      // "Waiting for fix" even with valid coordinates.
      const hasFix = !!(gps && gps.fix && gps.quality > 0);
      const stationary = !!(gps && gps.stationary);
      document.getElementById('gpsStatus').textContent = hasFix ? (gps.satellites + ' Sats') : 'No Fix';
      // When the motion filter has us locked, surface that explicitly so the
      // 0 m/s reading reads as "intentionally pinned" rather than "broken".
      let subtitle;
      if (!hasFix) {
        subtitle = 'Waiting for fix...';
      } else if (stationary) {
        subtitle = `Stationary · Fix ${gps.fix_mode || '?'}D`;
      } else {
        subtitle = `Fix: ${gps.fix_mode || '?'}D`;
      }
      document.getElementById('gpsSubtitle').textContent = subtitle;
      document.getElementById('gpsBadge').className = 'badge ' + (hasFix ? 'success' : 'warning');
      document.getElementById('gpsLat').textContent = (gps.lat != null) ? Number(gps.lat).toFixed(6) : '--';
      document.getElementById('gpsLon').textContent = (gps.lon != null) ? Number(gps.lon).toFixed(6) : '--';
      document.getElementById('gpsAlt').textContent = gps.alt ? Number(gps.alt).toFixed(1) + ' m' : '--';
      // 0.0 m/s is a valid reading (anchor lock) — render it instead of '--'.
      document.getElementById('gpsSpeed').textContent = (hasFix && gps.speed != null)
        ? Number(gps.speed).toFixed(1) + ' m/s' + (stationary ? ' · still' : '')
        : '--';
      document.getElementById('gpsSats').textContent = gps.satellites || '--';
      document.getElementById('gpsHdop').textContent = (gps.hdop != null) ? Number(gps.hdop).toFixed(1) : '--';
    }

    function updateBadges(data) {
      document.getElementById('chainBadge').className = 'badge ' + (data.crypto_healthy ? 'success' : 'danger');
      const sdBadge = document.getElementById('sdBadge');
      // Distinguish "no card present" (informational, not an error) from a real
      // SD failure. The firmware exposes sd_state ∈ {ABSENT, MOUNTED, ERROR};
      // we previously painted any non-mounted state red, which made "SD ERR"
      // stick after acknowledging the latest log even though the card was
      // simply not inserted.
      const sdState = data.sd_state || (data.sd_mounted ? 'MOUNTED' : 'ABSENT');
      let cls = 'info', label = 'No SD';
      if (sdState === 'MOUNTED') { cls = 'success'; label = 'SD OK'; }
      else if (sdState === 'ERROR') { cls = 'danger'; label = 'SD ERR'; }
      sdBadge.className = 'badge ' + cls;
      sdBadge.querySelector('span:last-child').textContent = label;
      sdBadge.title = sdState === 'ABSENT'
        ? 'No SD card inserted (witness logging continues in RAM)'
        : sdState === 'ERROR'
          ? 'SD card mounted but reporting errors — check the card'
          : 'SD card mounted and healthy';
    }

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

    // ══════════════════════════════════════════════════════════════════
    // CAMERA/PEEK
    // ══════════════════════════════════════════════════════════════════
    let camInfoTimer = null;

    function fmtBytes(n) {
      if (n == null || isNaN(n)) return '—';
      if (n < 1024) return n + ' B';
      if (n < 1024*1024) return (n/1024).toFixed(1) + ' KB';
      return (n/1048576).toFixed(2) + ' MB';
    }
    function fmtDuration(ms) {
      if (!ms || ms < 0) return '—';
      const s = Math.floor(ms / 1000);
      if (s < 60) return s + 's';
      const m = Math.floor(s / 60);
      const rs = s % 60;
      return m + 'm ' + rs + 's';
    }
    function setText(id, v) { const el = document.getElementById(id); if (el) el.textContent = v; }

    function applyCameraInfo(data) {
      if (!data || !data.ok) return;
      const sensor = data.sensor_model && data.sensor_model !== 'unknown'
        ? data.sensor_model + ' (PID 0x' + (data.sensor_pid||0).toString(16).toUpperCase() + ')'
        : 'PID 0x' + (data.sensor_pid||0).toString(16).toUpperCase();
      setText('camSensor', data.sensor_pid ? sensor : 'unavailable');
      setText('camPixFmt', data.pixel_format || '—');
      setText('camRes', data.resolution_name || '—');
      setText('camQuality', (data.jpeg_quality != null) ? (data.jpeg_quality + ' / 63') : '—');
      setText('camXclk', data.xclk_hz ? (data.xclk_hz/1000000).toFixed(0) + ' MHz' : '—');
      setText('camPsram', (data.psram === true) ? 'Yes' : (data.psram === false ? 'No' : '—'));
      setText('camFbCount', data.fb_count != null ? String(data.fb_count) : '—');
      // Init-failure diagnostics card. Only visible when camera_initialized=false.
      const diag = document.getElementById('camInitDiag');
      const diagText = document.getElementById('camInitDiagText');
      if (diag && diagText) {
        if (!data.camera_initialized) {
          let msg = '';
          if (data.last_init_label && data.last_init_label !== 'never') {
            msg += 'Last attempt: <code>' + data.last_init_label + '</code> → ' +
                   (data.last_init_err_name || ('0x' + (data.last_init_err||0).toString(16)));
          } else {
            msg += 'Camera init has not run yet.';
          }
          if (data.psram_found === false) {
            msg += '<br>PSRAM not detected — on XIAO ESP32-S3 Sense this usually means the sketch was flashed without <b>Tools → PSRAM → OPI PSRAM</b>.';
          }
          if (typeof data.init_attempts !== 'undefined') {
            msg += '<br><span style="color:var(--muted);font-size:0.72rem;">Attempts since boot: ' + data.init_attempts +
                   (typeof data.free_heap !== 'undefined' ? ' · Free heap: ' + Math.round(data.free_heap/1024) + ' KB' : '') + '</span>';
          }
          diagText.innerHTML = msg;
          diag.style.display = 'block';
        } else {
          diag.style.display = 'none';
        }
      }
      // Live stream metrics: when peek_active=true, show LIVE numbers; when
      // a stream just ended (frame_count>0 but inactive), surface the final
      // real measurements from that stream so the user can verify quality.
      // When the device has never streamed yet, show "idle" — never a fake number.
      const hasMetrics = (data.frame_count != null) && (data.frame_count > 0);
      const live = document.getElementById('camInfoLive');
      if (data.peek_active) {
        setText('camFps', (data.fps != null) ? (data.fps + ' fps') : '—');
        setText('camLastFrame', fmtBytes(data.last_frame_bytes));
        setText('camAvgFrame', fmtBytes(data.avg_frame_bytes));
        setText('camKbps', (data.avg_kbps != null) ? (data.avg_kbps + ' kbps') : '—');
        setText('camFrameCount', String(data.frame_count));
        setText('camUptime', fmtDuration(data.stream_uptime_ms));
        if (live) { live.textContent = 'LIVE'; live.className = 'badge info'; live.style.display = 'inline-flex'; }
      } else if (hasMetrics) {
        setText('camFps', (data.fps != null) ? (data.fps + ' fps') : '—');
        setText('camLastFrame', fmtBytes(data.last_frame_bytes));
        setText('camAvgFrame', fmtBytes(data.avg_frame_bytes));
        setText('camKbps', (data.avg_kbps != null) ? (data.avg_kbps + ' kbps') : '—');
        setText('camFrameCount', String(data.frame_count));
        setText('camUptime', fmtDuration(data.stream_uptime_ms));
        if (live) { live.textContent = 'LAST STREAM'; live.className = 'badge'; live.style.display = 'inline-flex'; }
      } else {
        ['camFps','camLastFrame','camAvgFrame','camKbps','camFrameCount','camUptime']
          .forEach(id => setText(id, 'idle'));
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

    async function refreshPeekStatus() {
      const data = await api('/api/peek/status');
      if (data && data.ok) {
        const wasReady = cameraReady;
        cameraReady = data.camera_initialized;
        peekActive = data.peek_active;
        if (typeof data.resolution !== 'undefined') { currentResolution = data.resolution; updateResolutionUI(); }
        applyCameraInfo(data);
        updatePeekUI();
        if (peekActive) startCamInfoPolling(); else stopCamInfoPolling();
        // The camera just became available — pull sensor state so the
        // tuning sliders show the on-chip values rather than blanks.
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
        btn.disabled = true; btn.textContent = '⚠ No Camera';
        status.textContent = 'Camera unavailable';
        offlineText.textContent = 'Camera not initialized';
        stream.style.display = 'none'; offline.style.display = 'flex';
        return;
      }
      btn.disabled = false;
      if (peekActive) {
        btn.textContent = '⏹ Stop'; btn.className = 'btn btn-danger btn-sm';
        status.textContent = 'Streaming...';
        stream.style.display = 'block'; offline.style.display = 'none';
      } else {
        btn.textContent = '▶ Start'; btn.className = 'btn btn-primary btn-sm';
        status.textContent = 'Ready';
        stream.style.display = 'none'; offline.style.display = 'flex';
        offlineText.textContent = 'Click Start to preview';
      }
    }

    function togglePeek() { peekActive ? stopPeek() : startPeek(); }

    async function startPeek() {
      if (!cameraReady) { alert('Camera not available'); return; }
      const stream = document.getElementById('peekStream');
      document.getElementById('peekStatus').textContent = 'Connecting...';
      stream.src = API_BASE + '/api/peek/stream?t=' + Date.now() + '&token=' + encodeURIComponent(apiToken);
      stream.onload = () => { peekActive = true; updatePeekUI(); startCamInfoPolling(); setTimeout(refreshPeekStatus, 300); };
      stream.onerror = () => { if (peekActive) setTimeout(() => { if (peekActive) stream.src = API_BASE + '/api/peek/stream?t=' + Date.now() + '&token=' + encodeURIComponent(apiToken); }, 2000); };
      peekActive = true; updatePeekUI();
      startCamInfoPolling();
    }

    async function stopPeek() {
      peekActive = false;
      document.getElementById('peekStream').src = '';
      await api('/api/peek/stop', 'POST');
      stopCamInfoPolling();
      updatePeekUI();
      refreshPeekStatus();
    }

    async function reinitCamera() {
      const btn = document.getElementById('camReinitBtn');
      const diagText = document.getElementById('camInitDiagText');
      if (btn) { btn.disabled = true; btn.textContent = '… initializing'; }
      if (diagText) diagText.innerHTML = 'Re-running esp_camera_init() through PSRAM-XGA → PSRAM-VGA → DRAM-QVGA…';
      try {
        const r = await api('/api/peek/init', 'POST');
        if (r && r.ok) {
          // Pull a fresh status so the diag panel hides and the metrics re-populate.
          await refreshPeekStatus();
          await refreshSensorState();
        } else if (r) {
          if (diagText) {
            diagText.innerHTML = 'Re-init failed: <code>' + (r.last_init_err_name || '0x' + (r.last_init_err||0).toString(16)) +
                                 '</code> on attempt <code>' + (r.last_init_label || '?') + '</code>.' +
                                 (r.psram_found === false ? '<br>PSRAM still not visible — flash with PSRAM=OPI in Arduino IDE.' : '');
          }
        }
      } catch (e) {
        if (diagText) diagText.textContent = 'Re-init request failed: ' + e;
      } finally {
        if (btn) { btn.disabled = false; btn.textContent = '↻ Re-initialize Camera'; }
      }
    }

    async function takeSnapshot() {
      if (!cameraReady) { alert('Camera not available'); return; }
      const img = document.getElementById('snapshotImg');
      img.src = API_BASE + '/api/peek/snapshot?t=' + Date.now() + '&token=' + encodeURIComponent(apiToken);
      img.onload = () => { document.getElementById('snapshotPreview').style.display = 'block'; };
    }

    function updateResolutionUI() {
      document.querySelectorAll('.resolution-btn').forEach(btn => {
        btn.classList.toggle('active', parseInt(btn.dataset.size) === currentResolution);
      });
      const names = { 4: '320×240', 8: '640×480', 9: '800×600', 10: '1024×768' };
      document.getElementById('resolutionStatus').textContent = 'Current: ' + (names[currentResolution] || 'Unknown');
    }

    async function setResolution(size) {
      const data = await api('/api/peek/resolution', 'POST', { size });
      if (data.ok) {
        currentResolution = size; updateResolutionUI();
        if (peekActive) document.getElementById('peekStream').src = API_BASE + '/api/peek/stream?t=' + Date.now() + '&token=' + encodeURIComponent(apiToken);
      }
    }

    // ──────────────────────────────────────────────────────────────────
    // SENSOR TUNING
    // Real-time read/write of OV2640/OV3660 sensor parameters via
    // /api/peek/sensor. Slider events are coalesced into one POST per ~200ms
    // so dragging doesn't spam the device.
    // ──────────────────────────────────────────────────────────────────
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

    function setVal(id, v) { const el = document.getElementById(id); if (el != null && v != null) el.value = v; }
    function setChecked(id, v) { const el = document.getElementById(id); if (el != null) el.checked = !!v; }

    function applySensorState(data) {
      if (!data || !data.ok) return;
      // Sliders
      setVal('sensorQuality',     data.quality);     setText('sensorQualityVal',    data.quality);
      setVal('sensorBrightness',  data.brightness);  setText('sensorBrightnessVal', data.brightness);
      setVal('sensorContrast',    data.contrast);    setText('sensorContrastVal',   data.contrast);
      setVal('sensorSaturation',  data.saturation);  setText('sensorSaturationVal', data.saturation);
      setVal('sensorAeLevel',     data.ae_level);    setText('sensorAeLevelVal',    data.ae_level);
      setVal('sensorSharpness',   data.sharpness);   setText('sensorSharpnessVal',  data.sharpness);
      setVal('sensorFrameDelay',  data.frame_delay_ms); setText('sensorFrameDelayVal', data.frame_delay_ms);
      // Toggles
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
      // Selects
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

    // ══════════════════════════════════════════════════════════════════
    // OPERA (MESH)
    // ══════════════════════════════════════════════════════════════════
    let pairingPollingInterval = null;

    async function refreshOpera() {
      const data = await api('/api/mesh');
      if (!data.ok) return;

      document.getElementById('operaStatus').textContent = data.state || 'DISABLED';
      document.getElementById('peersOnline').textContent = data.peers_online || 0;
      document.getElementById('peersTotal').textContent = data.peers_total || 0;
      document.getElementById('alertsReceived').textContent = data.alerts_received || 0;

      const badge = document.getElementById('operaBadge');
      const stateText = document.getElementById('operaState');
      if (data.state === 'ACTIVE') { badge.className = 'badge success'; stateText.textContent = 'Active'; }
      else if (data.state === 'CONNECTING') { badge.className = 'badge warning'; stateText.textContent = 'Connecting'; }
      else { badge.className = 'badge info'; stateText.textContent = data.state || 'Disabled'; }

      document.getElementById('meshEnabled').checked = data.enabled !== false;

      const hasOpera = data.has_opera || (data.peers_total > 0);
      const isPairing = data.state && data.state.startsWith('PAIRING');
      document.getElementById('operaNoOpera').style.display = (!hasOpera && !isPairing) ? 'block' : 'none';
      document.getElementById('operaHasOpera').style.display = (hasOpera && !isPairing) ? 'block' : 'none';
      document.getElementById('operaPairing').style.display = isPairing ? 'block' : 'none';

      if (hasOpera && data.opera_name) document.getElementById('operaNameInput').value = data.opera_name;
      if (hasOpera) loadPeers();
      loadOperaAlerts();

      // Community badge
      const count = (data.alerts_received || 0);
      const communityCount = document.getElementById('communityCount');
      if (count > 0) { communityCount.textContent = count; communityCount.style.display = 'inline-flex'; }
      else { communityCount.style.display = 'none'; }
    }

    async function loadPeers() {
      const data = await api('/api/mesh/peers');
      const list = document.getElementById('peersList');
      if (!data.ok || !data.peers?.length) {
        list.innerHTML = '<div class="empty-state"><div class="empty-icon">🐦</div><p>No peers</p></div>';
        return;
      }
      document.getElementById('peersSubtitle').textContent = data.peers.length + ' device(s)';
      list.innerHTML = data.peers.map(peer => {
        const icon = peer.state === 'CONNECTED' ? '🟢' : peer.state === 'STALE' ? '🟡' : peer.state === 'ALERT' ? '🔴' : '⚫';
        return `<div class="log-item"><div style="font-size:1.5rem;">${icon}</div>
          <div class="log-content">
            <div class="log-message">${escapeHtml(peer.name || 'Unknown')}</div>
            <div class="log-meta">${peer.state} · ${peer.rssi ? peer.rssi + ' dBm' : '--'}</div>
          </div>
          <button class="btn btn-ghost btn-sm" onclick="removePeer('${peer.fingerprint}')">✕</button>
        </div>`;
      }).join('');
    }

    async function loadOperaAlerts() {
      const data = await api('/api/mesh/alerts');
      const list = document.getElementById('operaAlertsList');
      if (!data.ok || !data.alerts?.length) {
        list.innerHTML = '<div class="empty-state"><div class="empty-icon">✓</div><p>No alerts</p></div>';
        return;
      }
      list.innerHTML = data.alerts.map(a => `<div class="log-item ${a.severity >= 4 ? 'error' : ''}">
        <div class="log-level ${a.severity >= 4 ? 'error' : 'warning'}">${a.type || 'ALERT'}</div>
        <div class="log-content">
          <div class="log-message">From: ${escapeHtml(a.sender_name || 'Unknown')}</div>
          <div class="log-detail">${escapeHtml(a.detail || '')}</div>
        </div>
      </div>`).join('');
    }

    async function startPairing(mode) {
      const endpoint = mode === 'init' ? '/api/mesh/pair/start' : '/api/mesh/pair/join';
      const data = await api(endpoint, 'POST');
      if (!data.ok) { alert('Failed: ' + (data.error || 'Unknown')); return; }
      document.getElementById('operaNoOpera').style.display = 'none';
      document.getElementById('operaHasOpera').style.display = 'none';
      document.getElementById('operaPairing').style.display = 'block';
      document.getElementById('pairingStatus').textContent = mode === 'init' ? 'Waiting for device...' : 'Searching...';
      document.getElementById('pairingCode').style.display = 'none';
      document.getElementById('pairingConfirmBtn').style.display = 'none';
      startPairingPolling();
    }

    function startPairingPolling() {
      if (pairingPollingInterval) clearInterval(pairingPollingInterval);
      pairingPollingInterval = setInterval(async () => {
        const data = await api('/api/mesh');
        if (!data.ok) return;
        if (data.state === 'PAIRING_CONFIRM' && data.pairing_code) {
          document.getElementById('pairingStatus').textContent = 'Verify code:';
          document.getElementById('pairingCodeValue').textContent = String(data.pairing_code).padStart(6, '0');
          document.getElementById('pairingCode').style.display = 'block';
          document.getElementById('pairingConfirmBtn').style.display = 'inline-flex';
        } else if (data.state === 'ACTIVE' || data.state === 'CONNECTING') {
          stopPairingPolling(); refreshOpera();
        } else if (data.state === 'NO_FLOCK' || data.state === 'DISABLED') {
          stopPairingPolling(); refreshOpera();
        }
      }, 1000);
    }

    function stopPairingPolling() { if (pairingPollingInterval) { clearInterval(pairingPollingInterval); pairingPollingInterval = null; } }
    async function confirmPairing() { await api('/api/mesh/pair/confirm', 'POST'); document.getElementById('pairingStatus').textContent = 'Completing...'; }
    async function cancelPairing() { stopPairingPolling(); await api('/api/mesh/pair/cancel', 'POST'); refreshOpera(); }
    async function saveOperaName() { const name = document.getElementById('operaNameInput').value.trim(); if (name) await api('/api/mesh/name', 'POST', { name }); refreshOpera(); }
    async function leaveOpera() { if (confirm('Leave opera?')) { await api('/api/mesh/leave', 'POST'); refreshOpera(); } }
    async function removePeer(fp) { if (confirm('Remove device?')) { await api('/api/mesh/remove', 'POST', { fingerprint: fp }); loadPeers(); } }
    async function toggleMeshEnabled() { const enabled = document.getElementById('meshEnabled').checked; await api('/api/mesh/enable', 'POST', { enabled }); refreshOpera(); }
    async function clearOperaAlerts() { await api('/api/mesh/alerts', 'DELETE'); loadOperaAlerts(); }

    // ══════════════════════════════════════════════════════════════════
    // CHIRP
    // ══════════════════════════════════════════════════════════════════
    async function refreshChirpStatus() {
      const data = await api('/api/chirp');
      if (!data.state) return;

      document.getElementById('chirpSessionEmoji').textContent = data.session_emoji || '--';
      document.getElementById('chirpNearbyCount').textContent = data.nearby_count || 0;
      document.getElementById('chirpRecentCount').textContent = data.recent_chirps || 0;

      const enabled = data.state !== 'disabled';
      document.getElementById('chirpEnabled').checked = enabled;

      if (!data.presence_met) {
        document.getElementById('chirpCooldown').textContent = 'Warming up...';
        document.getElementById('chirpSendBtn').disabled = true;
        document.getElementById('chirpPresenceHint').style.display = 'block';
      } else if (data.cooldown_remaining_sec > 0) {
        const mins = Math.floor(data.cooldown_remaining_sec / 60);
        const secs = data.cooldown_remaining_sec % 60;
        document.getElementById('chirpCooldown').textContent = `${mins}:${secs.toString().padStart(2, '0')}`;
        document.getElementById('chirpSendBtn').disabled = true;
        document.getElementById('chirpPresenceHint').style.display = 'none';
      } else {
        document.getElementById('chirpCooldown').textContent = 'Ready';
        document.getElementById('chirpSendBtn').disabled = false;
        document.getElementById('chirpPresenceHint').style.display = 'none';
      }

      const badge = document.getElementById('chirpBadge');
      const stateText = document.getElementById('chirpState');
      if (data.state === 'active') { badge.className = 'badge success'; stateText.textContent = 'Active'; }
      else if (data.state === 'muted') { badge.className = 'badge warning'; stateText.textContent = 'Muted'; }
      else { badge.className = 'badge info'; stateText.textContent = enabled ? data.state : 'Disabled'; }

      document.getElementById('chirpSendCard').style.display = enabled ? 'block' : 'none';
      document.getElementById('chirpSettingsCard').style.display = enabled ? 'block' : 'none';

      if (data.muted) { document.getElementById('chirpUnmuteBtn').style.display = 'inline-flex'; }
      else { document.getElementById('chirpUnmuteBtn').style.display = 'none'; }

      document.getElementById('chirpRelayEnabled').checked = data.relay_enabled !== false;
      loadChirps();
    }

    async function loadChirps() {
      const data = await api('/api/chirp/recent');
      const list = document.getElementById('chirpList');
      if (!data.chirps?.length) {
        list.innerHTML = '<div class="empty-state"><div class="empty-icon">🐦</div><p>No alerts</p></div>';
        return;
      }
      list.innerHTML = data.chirps.map(c => {
        const urgColor = c.urgency === 'urgent' ? '#e67e22' : c.urgency === 'caution' ? '#f4b942' : '#63b3ed';
        const catIcon = c.category === 'authority' ? '🚔' : c.category === 'infrastructure' ? '⚡' : c.category === 'emergency' ? '🚨' : '👁️';
        return `<div class="log-item" style="border-left-color:${urgColor};">
          <div style="font-size:1.5rem;">${catIcon}</div>
          <div class="log-content">
            <div class="log-message"><span style="opacity:0.8;">${c.emoji}</span> witnessed</div>
            <div class="log-detail" style="font-weight:500;">${escapeHtml(c.template_text || 'alert')}</div>
            <div class="log-meta">${c.category} · ${c.hop_count} hop(s)</div>
          </div>
          <div class="log-actions">
            ${!c.validated ? `<button class="btn btn-ghost btn-sm" onclick="confirmChirp('${c.nonce}')">👁️</button>` : ''}
            <button class="btn btn-ghost btn-sm" onclick="dismissChirp('${c.nonce}')">✕</button>
          </div>
        </div>`;
      }).join('');
    }

    function refreshChirps() { loadChirps(); }
    function updateChirpPreview() {}

    async function toggleChirpEnabled() {
      const enabled = document.getElementById('chirpEnabled').checked;
      await api(enabled ? '/api/chirp/enable' : '/api/chirp/disable', 'POST');
      refreshChirpStatus();
    }

    async function sendChirp() {
      const template_id = parseInt(document.getElementById('chirpTemplate').value);
      const urgency = document.querySelector('input[name="chirpUrgency"]:checked').value;
      if (!confirm('Send this alert?')) return;
      const data = await api('/api/chirp/send', 'POST', { template_id, urgency, ttl_minutes: 15 });
      alert(data.success ? 'Sent!' : 'Failed: ' + (data.message || data.error));
      refreshChirpStatus();
    }

    async function confirmChirp(nonce) { await api('/api/chirp/confirm', 'POST', { nonce }); loadChirps(); }
    async function dismissChirp(nonce) { await api('/api/chirp/dismiss', 'POST', { nonce }); loadChirps(); }
    async function muteChirps(mins) { await api('/api/chirp/mute', 'POST', { duration_minutes: mins }); refreshChirpStatus(); }
    async function unmuteChirps() { await api('/api/chirp/unmute', 'POST'); refreshChirpStatus(); }
    async function updateChirpSettings() { await api('/api/chirp/settings', 'POST', { relay_enabled: document.getElementById('chirpRelayEnabled').checked }); }

    // ══════════════════════════════════════════════════════════════════
    // LOGS & WITNESS
    // ══════════════════════════════════════════════════════════════════
    async function loadLogs() {
      const filter = logFilter === 'unread' ? '?unacked=true' : '';
      const data = await api('/api/logs' + filter);
      const list = document.getElementById('logList');
      if (!data.ok || !data.logs?.length) {
        list.innerHTML = '<div class="empty-state"><div class="empty-icon">📋</div><p>No logs</p></div>';
        return;
      }
      document.getElementById('logsSubtitle').textContent = `${data.logs.length} entries`;
      list.innerHTML = data.logs.map(log => `<div class="log-item ${log.ack_status === 'unread' ? 'unread' : ''} ${log.level >= 4 ? 'error' : ''}">
        <div class="log-level ${getLevelClass(log.level)}">${log.level_name}</div>
        <div class="log-content">
          <div class="log-message">${escapeHtml(log.message)}</div>
          <div class="log-meta">${log.category} · ${formatTimestamp(log.timestamp_ms)} · #${log.seq}</div>
        </div>
        <div class="log-actions">
          ${log.ack_status !== 'acknowledged' ? `<button class="btn btn-ghost btn-sm" onclick="openAckModal(${log.seq})">✓</button>` : '<span style="color:var(--success);">✓</span>'}
        </div>
      </div>`).join('');
    }

    function filterLogs(f) { logFilter = f; loadLogs(); }
    function getLevelClass(l) { return l <= 2 ? 'info' : l === 3 ? 'warning' : 'error'; }

    async function loadWitness() {
      const data = await api('/api/witness');
      const list = document.getElementById('witnessList');
      if (!data.ok || !data.records?.length) {
        list.innerHTML = '<div class="empty-state"><div class="empty-icon">🔐</div><p>No records</p></div>';
        return;
      }
      document.getElementById('witnessSubtitle').textContent = `${data.records.length} records`;
      list.innerHTML = data.records.slice(-50).reverse().map(r => `<div class="witness-item">
        <div><div class="witness-seq">#${r.seq}</div><div class="witness-type">${r.type_name}</div></div>
        <div><div class="witness-hash">Chain: ${truncateHash(r.chain_hash, 16)}</div><div class="log-meta">TB: ${r.time_bucket}</div></div>
        <div class="witness-verified">${r.verified ? '✓' : '⚠'}</div>
      </div>`).join('');
    }

    async function exportWitness() {
      const data = await api('/api/export', 'POST');
      if (data.ok && data.download_url) window.location.href = data.download_url;
      else alert('Export failed');
    }

    function openAckModal(seq) { pendingAckSeq = seq; document.getElementById('ackReason').value = ''; document.getElementById('ackModal').classList.add('active'); }
    function closeAckModal() { pendingAckSeq = null; document.getElementById('ackModal').classList.remove('active'); }
    async function submitAck() {
      if (pendingAckSeq === null) return;
      await api(`/api/logs/${pendingAckSeq}/ack`, 'POST', { reason: document.getElementById('ackReason').value });
      closeAckModal(); loadLogs(); refreshStatus();
    }
    async function ackAllLogs() { if (confirm('Acknowledge all?')) { await api('/api/logs/ack-all', 'POST', { level: 3 }); loadLogs(); refreshStatus(); } }

    // ══════════════════════════════════════════════════════════════════
    // WIFI PRESENCE DETECTION
    // ══════════════════════════════════════════════════════════════════
    // ── Household + presence (auto-context) ─────────────────────────────────
    function fmtAgo(ms) {
      if (ms == null) return 'never';
      const sec = Math.floor(ms / 1000);
      if (sec < 60)   return sec + 's ago';
      const min = Math.floor(sec / 60);
      if (min < 60)   return min + ' min ago';
      const hr  = Math.floor(min / 60);
      if (hr  < 24)   return hr + ' h ago';
      return Math.floor(hr / 24) + ' d ago';
    }

    async function refreshPresence() {
      const data = await api('/api/presence');
      if (!data || !data.effective_context) return;

      const badge = document.getElementById('presenceBadge');
      const text  = document.getElementById('presenceBadgeText');
      const sub   = document.getElementById('presenceSubtitle');
      const clearBtn = document.getElementById('presenceClearOverrideBtn');
      const hint  = document.getElementById('presenceOverrideHint');

      const ctx = data.effective_context;
      text.textContent = ctx.toUpperCase();
      // Color cue: HOME = info (calm), AWAY = success (armed), QUIET/TRAVELING = warning
      badge.className = 'badge ' + (
        ctx === 'home' ? 'info' :
        ctx === 'away' ? 'success' :
        'warning'
      );

      let s;
      if (data.owner_seen_recently) {
        s = 'Owner phone seen ' + fmtAgo(data.ms_since_any_owner) + '. Quieter alerts.';
      } else if (data.ms_since_any_owner == null) {
        s = 'No owner-tagged device yet. Pair one and tag it Owner below to enable auto-quiet at home.';
      } else {
        s = 'No owner phone seen for ' + fmtAgo(data.ms_since_any_owner) + '. Alerting on anything new.';
      }
      sub.textContent = s;

      if (data.override_active) {
        clearBtn.style.display = '';
        hint.style.display = '';
        const remaining = Math.round((data.override_ms_remaining || 0) / 60000);
        hint.textContent = 'Manual override active for ' + remaining + ' more minutes (auto-resumes after).';
      } else {
        clearBtn.style.display = 'none';
        hint.style.display = 'none';
      }
    }

    async function setPresenceOverride(ctx) {
      const r = await api('/api/presence/override', 'POST', { context: ctx });
      if (r && r.success === false) alert('Override: ' + (r.error || 'failed'));
      refreshPresence();
    }
    async function clearPresenceOverride() {
      await api('/api/presence/override', 'DELETE');
      refreshPresence();
    }

    function roleBadgeStyle(role) {
      if (role === 'owner')  return 'background:rgba(102,179,255,0.15);color:#66b3ff;';
      if (role === 'family') return 'background:rgba(72,187,120,0.15);color:#48bb78;';
      return                        'background:rgba(160,160,160,0.15);color:var(--muted);';
    }

    async function refreshHousehold() {
      const data = await api('/api/household');
      const list = document.getElementById('householdList');
      if (!list) return;
      if (!data || !Array.isArray(data.devices) || data.devices.length === 0) {
        list.innerHTML = '<p style="color:var(--muted);font-size:0.85rem;text-align:center;padding:0.75rem;">No paired devices yet. Tap Start Pairing on the Bluetooth panel to add one.</p>';
        return;
      }
      list.innerHTML = data.devices.map(d => {
        const seen = (d.last_seen_ago_ms == null)
          ? 'never seen this boot'
          : 'seen ' + fmtAgo(d.last_seen_ago_ms);
        return (
          '<div style="padding:0.6rem 0.75rem;border-bottom:1px solid var(--border);display:flex;align-items:center;gap:0.6rem;">' +
            '<div style="flex:1;min-width:0;">' +
              '<div style="font-weight:600;">' + (d.label && d.label.length ? d.label : 'Slot ' + d.slot) + '</div>' +
              '<div style="font-size:0.7rem;color:var(--muted);">' + seen + '</div>' +
            '</div>' +
            '<span class="badge" style="font-size:0.65rem;padding:0.15rem 0.45rem;' + roleBadgeStyle(d.role) + '">' + d.role + '</span>' +
            '<select onchange="setHouseholdRole(' + d.slot + ', this.value)" style="background:var(--surface-2,#222);color:var(--text);border:1px solid var(--border);border-radius:4px;padding:0.2rem 0.4rem;font-size:0.7rem;">' +
              '<option value="guest"' + (d.role === 'guest'  ? ' selected' : '') + '>Guest</option>' +
              '<option value="family"' + (d.role === 'family' ? ' selected' : '') + '>Family</option>' +
              '<option value="owner"'  + (d.role === 'owner'  ? ' selected' : '') + '>Owner</option>' +
            '</select>' +
            '<button class="btn btn-sm btn-ghost" onclick="forgetHousehold(' + d.slot + ', \'' + (d.label || '').replace(/'/g, '\\\'') + '\')" title="Forget this device" style="font-size:0.7rem;">×</button>' +
          '</div>'
        );
      }).join('');
    }

    async function setHouseholdRole(slot, role) {
      const r = await api('/api/household/role', 'POST', { slot: slot, role: role });
      if (r && r.success === false) alert('Role: ' + (r.error || 'failed'));
      refreshHousehold();
      refreshPresence();
    }
    async function forgetHousehold(slot, label) {
      if (!confirm('Forget ' + (label || 'this paired device') + '? It will need to re-pair to be recognized again.')) return;
      const r = await api('/api/household', 'DELETE', { slot: slot });
      if (r && r.success === false) alert('Forget: ' + (r.error || 'failed'));
      refreshHousehold();
      refreshPresence();
    }

    async function refreshWifiPresence() {
      const data = await api('/api/presence/wifi');
      if (!data.feature_available && !data.enabled && data.enabled === undefined) return;

      const badge = document.getElementById('wifiPresenceBadge');
      const state = document.getElementById('wifiPresenceState');

      if (data.enabled) {
        badge.className = 'badge success';
        state.textContent = data.current_count + ' devices';
      } else {
        badge.className = 'badge info';
        state.textContent = data.feature_available ? 'Stopped' : 'Unavailable';
      }

      document.getElementById('wpEnabled').checked = data.enabled || false;
      document.getElementById('wpCurrentCount').textContent = data.current_count || 0;
      document.getElementById('wpLastCount').textContent = data.last_count || 0;
      document.getElementById('wpPeakCount').textContent = data.peak_count || 0;
      document.getElementById('wpTotalProbes').textContent = data.total_probes || 0;

      // Render sparkline
      if (data.history) {
        const sparkline = document.getElementById('wpSparkline');
        const max = Math.max(1, ...data.history);
        sparkline.innerHTML = data.history.map(v => {
          const pct = Math.max(4, (v / max) * 100);
          const color = v > 0 ? 'var(--accent)' : 'var(--border)';
          return '<div style="flex:1;background:' + color + ';height:' + pct + '%;border-radius:2px;min-width:4px;" title="' + v + ' devices"></div>';
        }).join('');
      }

      // BLE presence status
      const bleStatus = document.getElementById('blePresenceStatus');
      const bleSubtitle = document.getElementById('blePresenceSubtitle');
      try {
        const presData = await api('/api/presence');
        if (presData.ble?.available) {
          bleStatus.textContent = 'BLE scanning available via BLE Discovery subsystem';
          bleSubtitle.textContent = 'Active';
        } else {
          bleStatus.innerHTML = '<strong>Disabled at compile time</strong> (security: no BT binary blobs).<br>' +
            'To enable: rebuild firmware with FEATURE_BLE=1 and CONFIG_BT_ENABLED=y in sdkconfig.';
          bleSubtitle.textContent = 'Compile-time disabled';
        }
      } catch(e) { console.error('Failed to fetch BLE presence data:', e); }
    }

    async function toggleWifiPresence() {
      const enabled = document.getElementById('wpEnabled').checked;
      const data = await api(enabled ? '/api/presence/wifi/start' : '/api/presence/wifi/stop', 'POST');
      if (!data.success) {
        document.getElementById('wpEnabled').checked = !enabled;
        alert('Failed: ' + (data.error || 'Unknown error'));
      }
      refreshWifiPresence();
    }

    // ══════════════════════════════════════════════════════════════════
    // AUDIBLE CHIRP
    // ══════════════════════════════════════════════════════════════════
    async function refreshAudibleChirpStatus() {
      const data = await api('/api/audible-chirp');
      if (!data.feature_available && !data.available) return;

      const badge = document.getElementById('chirpHwBadge');
      const state = document.getElementById('chirpHwState');
      const mode = document.getElementById('chirpHwMode');

      if (data.available) {
        badge.className = 'badge success';
        state.textContent = 'Ready';
        mode.textContent = data.visual_only
          ? 'Mode: Visual only (LED blink) — connect buzzer to GPIO ' + data.gpio + ' for audio'
          : 'Mode: Audio (GPIO ' + data.gpio + ')';
        document.getElementById('chirpVisualOnly').checked = data.visual_only;
      } else {
        badge.className = 'badge info';
        state.textContent = 'Unavailable';
        mode.textContent = 'Mode: Not compiled (FEATURE_AUDIBLE_CHIRP=0)';
      }
    }

    async function playAudibleChirp(pattern) {
      const result = document.getElementById('chirpHwResult');
      result.textContent = 'Playing...';
      result.style.color = 'var(--muted)';
      const data = await api('/api/audible-chirp/play', 'POST', { pattern });
      if (data.success) {
        result.style.color = 'var(--success)';
        result.textContent = 'Played: ' + data.pattern + (data.visual_only ? ' (LED blink)' : ' (audio)');
      } else {
        result.style.color = 'var(--danger)';
        result.textContent = 'Failed: ' + (data.error || 'Unknown error');
      }
      setTimeout(() => { result.textContent = ''; }, 3000);
    }

    async function setChirpVisualOnly() {
      const visual = document.getElementById('chirpVisualOnly').checked;
      await api('/api/audible-chirp/config', 'POST', { visual_only: visual });
      refreshAudibleChirpStatus();
    }

    // ══════════════════════════════════════════════════════════════════
    // WIFI
    // ══════════════════════════════════════════════════════════════════
    let wifiState = null;
    let wifiPollingInterval = null;

    // Map an RSSI (dBm) to a 0-4 bar count. Common rule of thumb:
    //   >= -55 dBm  excellent (4)    >= -65 dBm  good (3)
    //   >= -75 dBm  fair (2)         >= -85 dBm  weak (1)   else none (0)
    function rssiBarLevel(rssi) {
      if (rssi == null || rssi === 0) return 0;
      if (rssi >= -55) return 4;
      if (rssi >= -65) return 3;
      if (rssi >= -75) return 2;
      if (rssi >= -85) return 1;
      return 0;
    }
    function rssiQualityLabel(level) {
      return ['No signal','Weak','Fair','Good','Excellent'][level] || '--';
    }
    function rssiBarString(level) {
      // Filled vs empty unicode block characters, easy to render in <option> too.
      return '▮'.repeat(level) + '▯'.repeat(4 - level);
    }
    function rssiBarColor(level) {
      if (level >= 3) return 'var(--success)';
      if (level === 2) return 'var(--warning)';
      return 'var(--danger)';
    }

    async function loadWifiStatus() {
      const data = await api('/api/wifi');
      if (!data.ok) return;
      wifiState = data;
      document.getElementById('wifiApSsid').textContent = data.ap_ssid || '--';
      document.getElementById('wifiApIp').textContent = data.ap_ip || '--';
      document.getElementById('wifiStaSsid').textContent = data.configured ? data.sta_ssid : 'Not configured';
      document.getElementById('wifiStaIp').textContent = data.sta_connected ? data.sta_ip : '--';

      const badge = document.getElementById('wifiBadge');
      const state = document.getElementById('wifiState');
      if (data.sta_connected) { badge.className = 'badge success'; state.textContent = 'Connected'; }
      else if (data.state === 'connecting') { badge.className = 'badge warning'; state.textContent = 'Connecting'; }
      else if (data.state === 'failed') { badge.className = 'badge danger'; state.textContent = 'Failed'; }
      else { badge.className = 'badge info'; state.textContent = data.configured ? 'Disconnected' : 'AP Only'; }

      const rssiBar = document.getElementById('wifiRssiBar');
      if (data.sta_connected && data.rssi) {
        rssiBar.style.display = 'block';
        const lvl = rssiBarLevel(data.rssi);
        const pct = Math.max(0, Math.min(100, (data.rssi + 90) * 1.67));
        const lvlEl = document.getElementById('wifiRssiLevel');
        lvlEl.style.width = pct + '%';
        lvlEl.style.background = rssiBarColor(lvl);
        const barsEl = document.getElementById('wifiRssiBars');
        barsEl.textContent = rssiBarString(lvl);
        barsEl.style.color = rssiBarColor(lvl);
        document.getElementById('wifiRssiQuality').textContent = rssiQualityLabel(lvl);
        document.getElementById('wifiRssiValue').textContent = data.rssi + ' dBm';
      } else { rssiBar.style.display = 'none'; }

      // Surface the most recent failure reason from the firmware so the user
      // knows why "always fails" is happening (wrong password, not in range, …).
      const failBox = document.getElementById('wifiFailReason');
      if (failBox) {
        if (!data.sta_connected && data.state === 'failed' && data.fail_reason) {
          failBox.textContent = 'Last attempt failed: ' + data.fail_reason;
          failBox.style.display = 'block';
        } else if (data.sta_connected) {
          failBox.style.display = 'none';
        }
      }

      document.getElementById('wifiConnectBtn').style.display = data.sta_connected ? 'none' : 'inline-flex';
      document.getElementById('wifiDisconnectBtn').style.display = data.sta_connected ? 'inline-flex' : 'none';
      document.getElementById('wifiForgetBtn').style.display = data.configured ? 'inline-flex' : 'none';
      document.getElementById('wifiProgress').style.display = data.state === 'connecting' ? 'block' : 'none';
    }

    async function scanWifi() {
      const btn = document.getElementById('wifiScanBtn');
      const select = document.getElementById('wifiSsidSelect');
      btn.disabled = true; btn.textContent = 'Scanning...';
      select.innerHTML = '<option value="">Scanning...</option>';

      let data, attempts = 0;
      while (attempts < 20) {
        data = await api('/api/wifi/scan');
        if (!data.ok || !data.scanning) break;
        await new Promise(r => setTimeout(r, 500));
        attempts++;
      }
      btn.disabled = false; btn.textContent = 'Scan';
      if (!data.ok || data.scanning) { select.innerHTML = '<option value="">Scan failed</option>'; return; }

      select.innerHTML = '<option value="">-- Select --</option>';
      if (data.networks?.length) {
        data.networks.sort((a, b) => b.rssi - a.rssi);
        data.networks.forEach(n => {
          if (n.ssid) {
            const lvl = rssiBarLevel(n.rssi);
            const lock = (n.security && n.security !== 'open') ? '🔒' : '  ';
            const opt = document.createElement('option');
            opt.value = n.ssid;
            // Bars first so users can scan vertically; raw dBm preserved for power users.
            opt.textContent = `${rssiBarString(lvl)} ${lock} ${n.ssid}  ·  ${rssiQualityLabel(lvl)} (${n.rssi} dBm)`;
            select.appendChild(opt);
          }
        });
      }
    }

    document.getElementById('wifiSsidSelect').addEventListener('change', function() { if (this.value) document.getElementById('wifiSsidInput').value = this.value; });

    async function connectWifi() {
      const ssid = document.getElementById('wifiSsidInput').value.trim() || document.getElementById('wifiSsidSelect').value;
      const password = document.getElementById('wifiPassword').value;
      if (!ssid) { alert('Enter network name'); return; }

      // Hide any previous failure banner; the firmware will repopulate it
      // if the new attempt also fails.
      const failBox = document.getElementById('wifiFailReason');
      if (failBox) failBox.style.display = 'none';

      document.getElementById('wifiProgress').style.display = 'block';
      document.getElementById('wifiProgressText').textContent = 'Connecting…';
      const data = await api('/api/wifi/connect', 'POST', { ssid, password });
      if (!data.ok) {
        document.getElementById('wifiProgress').style.display = 'none';
        alert('Failed: ' + (data.error || 'unknown error'));
        return;
      }
      startWifiPolling();
    }

    async function disconnectWifi() { if (confirm('Disconnect?')) { await api('/api/wifi/disconnect', 'POST'); loadWifiStatus(); } }

    function clearWifiInputs() {
      document.getElementById('wifiPassword').value = '';
      document.getElementById('wifiSsidInput').value = '';
      const sel = document.getElementById('wifiSsidSelect');
      if (sel) sel.selectedIndex = 0;
      const failBox = document.getElementById('wifiFailReason');
      if (failBox) { failBox.style.display = 'none'; failBox.textContent = ''; }
      document.getElementById('wifiProgress').style.display = 'none';
    }

    async function forgetWifi() {
      if (!confirm('Remove saved WiFi credentials from this Canary?')) return;
      stopWifiPolling();
      const data = await api('/api/wifi/forget', 'POST');
      if (!data.ok) { alert('Failed to remove credentials'); return; }
      // Wipe the input fields so the next user doesn't see the prior password.
      clearWifiInputs();
      await loadWifiStatus();
    }

    function startWifiPolling() {
      if (wifiPollingInterval) clearInterval(wifiPollingInterval);
      let count = 0;
      wifiPollingInterval = setInterval(async () => {
        await loadWifiStatus();
        count++;
        if (wifiState?.sta_connected) {
          stopWifiPolling();
          document.getElementById('wifiProgress').style.display = 'none';
          alert('Connected to ' + (wifiState.sta_ssid || 'home WiFi') + '!');
        } else if (wifiState?.state === 'failed') {
          stopWifiPolling();
          document.getElementById('wifiProgress').style.display = 'none';
          // loadWifiStatus already rendered the fail_reason banner; nudge user.
          if (wifiState.fail_reason) {
            alert('Connection failed: ' + wifiState.fail_reason);
          }
        } else if (count > 25) {
          // Slightly longer than the firmware's 15s connect timeout to allow
          // for AP-channel renegotiation when STA joins on a different channel.
          stopWifiPolling();
          document.getElementById('wifiProgress').style.display = 'none';
        }
      }, 1000);
    }
    function stopWifiPolling() { if (wifiPollingInterval) { clearInterval(wifiPollingInterval); wifiPollingInterval = null; } }

    // ══════════════════════════════════════════════════════════════════
    // BLUETOOTH
    // ══════════════════════════════════════════════════════════════════
    // ── BLE Discovery (Opera/Chirp/Nearby) ──
    let bleDiscoveryTimer = null;

    async function refreshBleDiscovery() {
      try {
        const status = await api('/api/ble/status');
        if (!status) return;

        const badge = document.getElementById('bleStateBadge');
        const text = document.getElementById('bleStateText');
        if (status.ble_available) {
          badge.className = 'badge success';
          text.textContent = 'Active';
        } else {
          badge.className = 'badge warning';
          text.textContent = 'Unavailable';
        }

        document.getElementById('bleOperaState').textContent = status.opera?.active ? 'Active' : 'Off';
        document.getElementById('bleChirpSent').textContent = status.chirp?.sent || 0;
        document.getElementById('bleChirpRecv').textContent = status.chirp?.received || 0;
        document.getElementById('bleConnections').textContent = status.opera?.connections_total || 0;
        document.getElementById('bleSubtitle').textContent = status.opera?.device_name || 'BLE discovery';
      } catch(e) { console.error('Failed to fetch BLE status:', e); }

      try {
        const nearby = await api('/api/nearby');
        if (!nearby) return;

        const list = document.getElementById('bleNearbyList');
        const canaries = nearby.canaries || [];
        const nonCanary = nearby.non_canary_device_count || 0;

        document.getElementById('bleNearbySubtitle').textContent =
          canaries.length + ' Canaries, ' + nonCanary + ' other BLE devices';

        if (canaries.length === 0) {
          list.innerHTML = '<div class="empty-state"><div class="empty-icon">📡</div><p>No nearby Canaries detected</p></div>';
          return;
        }

        let html = '';
        canaries.forEach(c => {
          const rssi = c.rssi_current;
          const quality = c.rssi_quality;
          const barPct = Math.max(0, Math.min(100, (rssi + 100) * 2));
          const barColor = quality === 'excellent' ? 'var(--success)' :
                           quality === 'good' ? 'var(--accent)' :
                           quality === 'fair' ? 'var(--warning)' : 'var(--danger)';
          html += '<div style="display:flex;align-items:center;gap:0.75rem;padding:0.6rem 0;border-bottom:1px solid var(--border);">';
          html += '<div style="min-width:60px;font-family:var(--mono);font-weight:600;color:var(--accent);">SCV-' + c.device_id_prefix + '</div>';
          html += '<div style="flex:1;">';
          html += '<div style="background:var(--border);border-radius:4px;height:8px;overflow:hidden;">';
          html += '<div style="background:' + barColor + ';width:' + barPct + '%;height:100%;border-radius:4px;transition:width 0.3s;"></div>';
          html += '</div></div>';
          html += '<div style="min-width:80px;text-align:right;font-size:0.8rem;">';
          html += '<span style="color:var(--text);">' + rssi + ' dBm</span><br>';
          html += '<span style="color:var(--muted);">' + quality + '</span>';
          html += '</div>';
          html += '<div style="min-width:40px;text-align:right;font-size:0.75rem;color:var(--muted);">' + c.last_seen_sec_ago + 's</div>';
          html += '</div>';
        });
        list.innerHTML = html;
      } catch(e) { console.error('Failed to fetch nearby devices:', e); }

      // Auto-refresh every 10 seconds when tab is visible
      clearTimeout(bleDiscoveryTimer);
      if (currentCommunityTab === 'ble-discovery') {
        bleDiscoveryTimer = setTimeout(refreshBleDiscovery, 10000);
      }
    }

    async function sendBleChirp(type) {
      const result = document.getElementById('bleChirpResult');
      result.textContent = 'Sending...';
      try {
        const resp = await secureFetch('/api/chirp/send', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({type: type})
        });
        const data = await resp.json();
        if (data.ok) {
          result.style.color = 'var(--success)';
          result.textContent = 'Chirp sent: ' + data.chirp_type + ' (total: ' + data.chirps_sent + ')';
        } else {
          result.style.color = 'var(--danger)';
          result.textContent = 'Failed: ' + (data.error || 'unknown error');
        }
      } catch(e) {
        result.style.color = 'var(--danger)';
        result.textContent = 'Error: ' + e.message;
      }
      setTimeout(() => { result.textContent = ''; }, 5000);
    }

    let btState = null;
    let btScanPolling = null;

    // RSSI -> bar count (1..4). Mirrors Apple's Bluetooth meter: -55 dBm and
    // up is "right next to it"; below ~-90 is "too far to be useful".
    function rssiBarCount(rssi) {
      if (rssi == null || rssi === 0) return 0;
      if (rssi >= -55) return 4;
      if (rssi >= -70) return 3;
      if (rssi >= -85) return 2;
      return 1;
    }
    function distancePretty(m) {
      if (!m || m <= 0) return '—';
      if (m < 1) return m.toFixed(1) + ' m';
      if (m < 10) return m.toFixed(1) + ' m';
      return Math.round(m) + ' m';
    }
    function deviceIcon(d) {
      if (d.is_securacv) return '🔒';
      switch (d.type) {
        case 'phone':    return '📱';
        case 'tablet':   return '📱';
        case 'computer': return '💻';
        case 'wearable': return '⌚';
        case 'securacv': return '🔒';
        default:         return '📶';
      }
    }

    async function refreshBtStatus() {
      const data = await api('/api/bluetooth');
      if (!data || !data.state) {
        // Backend not present. Three failure modes land here:
        //   1. FEATURE_BLUETOOTH=0 (MINIMAL profile)
        //   2. NimBLEDevice.h missing (library not installed)
        //   3. NimBLE-Arduino is 1.x — the bluetooth_channel TU silently
        //      compiled empty against the wrong callback API and
        //      register_routes() was never called.
        // Surface (3) explicitly because it bit us before and the symptom
        // (no /api/bluetooth response) looks identical to (1) and (2).
        const subtitle = document.getElementById('btSubtitle');
        if (subtitle) subtitle.textContent = 'BLE radio not registered. Flash a FULL or DEV build with NimBLE-Arduino ≥ 2.3.8.';
        const badge = document.getElementById('btStateBadge');
        const text = document.getElementById('btStateText');
        if (badge && text) { badge.className = 'badge warning'; text.textContent = 'Unavailable'; }
        const conn = document.getElementById('btConnLive');
        if (conn) conn.style.display = 'none';
        return;
      }
      btState = data;

      document.getElementById('btStateVal').textContent = data.state;
      document.getElementById('btDeviceName').textContent = data.device_name || '--';
      document.getElementById('btTxPower').innerHTML = (data.tx_power >= 0 ? '+' : '') + data.tx_power + '<span class="stat-unit">dBm</span>';
      // MTU only meaningful while connected; shows the negotiated value
      // (defaults to 23 — ATT minimum — when no peer is present).
      const mtuEl = document.getElementById('btMtu');
      if (mtuEl) {
        if (data.connected && data.mtu) {
          mtuEl.innerHTML = data.mtu + '<span class="stat-unit">B</span>';
        } else {
          mtuEl.innerHTML = '--<span class="stat-unit">B</span>';
        }
      }
      // Battery comes from the SIG Battery Service. Mains-powered devices
      // sit at 100 % indefinitely; a future battery-sense driver can call
      // ble_standard_profiles::set_battery_level() to update the value.
      const battEl = document.getElementById('btBattery');
      if (battEl) {
        const pct = (typeof data.battery_pct === 'number') ? data.battery_pct : null;
        battEl.innerHTML = (pct === null ? '--' : pct) + '<span class="stat-unit">%</span>';
      }
      document.getElementById('btPairedCount').textContent = data.paired_count || 0;
      // The scan-list polling also writes this — fall back to the status-side
      // count so the stat shows something between scans.
      const nearbyEl = document.getElementById('btNearbyCount');
      if (nearbyEl && nearbyEl.dataset.live !== '1') nearbyEl.textContent = data.scanned_count || 0;
      document.getElementById('btEnabled').checked = data.enabled;

      const badge = document.getElementById('btStateBadge');
      const text = document.getElementById('btStateText');
      if (data.connected) { badge.className = 'badge success'; text.textContent = 'Connected'; }
      else if (data.advertising) { badge.className = 'badge info'; text.textContent = 'Advertising'; }
      else { badge.className = 'badge info'; text.textContent = data.enabled ? 'Idle' : 'Disabled'; }

      // Live connection panel (Find My-like card)
      const connEl = document.getElementById('btConnLive');
      if (data.connected && data.connection) {
        connEl.style.display = '';
        document.getElementById('btConnName').textContent = data.connection.name || 'Unnamed device';
        document.getElementById('btConnAddr').textContent = data.connection.address || '';
        document.getElementById('btConnRssi').textContent = (data.connection.rssi || 0) + ' dBm';
        document.getElementById('btConnDistance').textContent = distancePretty(data.connection.distance_m);
        document.getElementById('btConnDistanceLabel').textContent = data.connection.distance_label || '';
        document.getElementById('btConnSecurity').textContent = data.connection.security || '';
        const bars = document.getElementById('btConnBars');
        bars.className = 'signal-bars s' + rssiBarCount(data.connection.rssi);
      } else {
        connEl.style.display = 'none';
      }

      const advBtn = document.getElementById('btAdvBtn');
      advBtn.textContent = data.advertising ? 'Stop Advertising' : 'Start Advertising';
      advBtn.className = data.advertising ? 'btn btn-danger' : 'btn btn-primary';

      // Pairing PIN / state display. The backend only includes data.pairing
      // when a pairing session is active.
      const box = document.getElementById('btPairingBox');
      const confirmRow = document.getElementById('btPairingConfirmRow');
      if (data.pairing && data.pairing.state) {
        box.style.display = '';
        document.getElementById('btPairingState').textContent = 'Pairing: ' + data.pairing.state;
        const pinEl = document.getElementById('btPairingPin');
        const isConfirming = (data.pairing.state === 'confirming');
        if (data.pairing.pin !== undefined && data.pairing.pin !== null) {
          pinEl.textContent = String(data.pairing.pin).padStart(6, '0');
          window.__btPairingPin = data.pairing.pin;
          document.getElementById('btPairingHint').textContent = isConfirming
            ? 'Compare this 6-digit code to the code on your phone. If they match, tap "Numbers match".'
            : 'Confirm this PIN on your phone.';
        } else {
          pinEl.textContent = 'waiting…';
          window.__btPairingPin = null;
          document.getElementById('btPairingHint').textContent =
            'Open Bluetooth on your phone, connect to ' + (data.device_name || 'this device') +
            ', and a PIN will appear here.';
        }
        confirmRow.style.display = isConfirming ? 'flex' : 'none';
      } else {
        box.style.display = 'none';
        confirmRow.style.display = 'none';
        window.__btPairingPin = null;
      }

      // While pairing or advertising, poll faster so the PIN appears promptly.
      if (data.advertising || (data.pairing && data.pairing.state)) {
        if (!window.__btPollTimer) {
          window.__btPollTimer = setInterval(refreshBtStatus, 1500);
        }
      } else if (window.__btPollTimer) {
        clearInterval(window.__btPollTimer);
        window.__btPollTimer = null;
      }
    }

    async function loadBtSettings() {
      const data = await api('/api/bluetooth/settings');
      if (data.auto_advertise !== undefined) {
        document.getElementById('btAutoAdv').checked = data.auto_advertise;
        document.getElementById('btAllowPairing').checked = data.allow_pairing;
        const lr = document.getElementById('btLongRange');
        if (lr) lr.checked = !!data.long_range_mode;
      }
    }

    async function saveBtSettings() {
      const lr = document.getElementById('btLongRange');
      await api('/api/bluetooth/settings', 'POST', {
        auto_advertise: document.getElementById('btAutoAdv').checked,
        allow_pairing: document.getElementById('btAllowPairing').checked,
        long_range_mode: lr ? lr.checked : false
      });
    }

    async function toggleBtEnabled() {
      const enabled = document.getElementById('btEnabled').checked;
      await api(enabled ? '/api/bluetooth/enable' : '/api/bluetooth/disable', 'POST');
      refreshBtStatus();
    }

    async function toggleBtAdvertising() {
      const isAdv = btState?.advertising;
      const ep = isAdv ? '/api/bluetooth/advertise/stop' : '/api/bluetooth/advertise/start';
      const data = await api(ep, 'POST');
      if (data && data.success === false) {
        alert('Bluetooth: ' + (data.error || 'unknown error'));
      }
      refreshBtStatus();
    }

    async function btStartPairing() {
      const data = await api('/api/bluetooth/pair/start', 'POST');
      if (data && data.success === false) {
        alert('Pairing: ' + (data.error || 'unknown error'));
      }
      refreshBtStatus();
    }

    async function btCancelPairing() {
      await api('/api/bluetooth/pair/cancel', 'POST');
      refreshBtStatus();
    }

    async function btConfirmPairing() {
      // Send back the same PIN the firmware showed us. The backend
      // cross-checks before injecting confirmation into NimBLE — a mismatch
      // is treated as a reject, not a retry.
      const pin = window.__btPairingPin;
      if (pin === null || pin === undefined) {
        alert('No pairing PIN to confirm.');
        return;
      }
      const data = await api('/api/bluetooth/pair/confirm', 'POST', { pin: pin });
      if (data && data.success === false) {
        alert('Confirm failed: ' + (data.error || 'unknown error'));
      }
      refreshBtStatus();
    }

    async function btRejectPairing() {
      await api('/api/bluetooth/pair/reject', 'POST');
      refreshBtStatus();
    }

    // Poll OTA status alongside BT status. The card stays hidden when no
    // session is active, surfaces with a progress bar during receive, and
    // shows the verifier's reason on failure.
    async function refreshBtOtaStatus() {
      const data = await api('/api/bluetooth/ota');
      const card = document.getElementById('btOtaCard');
      if (!card) return;
      const state = data && data.state ? data.state : 'idle';
      if (state === 'idle') {
        card.style.display = 'none';
        if (window.__btOtaPollTimer) {
          clearInterval(window.__btOtaPollTimer);
          window.__btOtaPollTimer = null;
        }
        return;
      }
      card.style.display = '';
      document.getElementById('btOtaStateText').textContent = state;
      const badge = document.getElementById('btOtaBadge');
      badge.className = 'badge ' + (state === 'failed' ? 'warning' : (state === 'rebooting' ? 'success' : 'info'));
      const pct = data.progress_pct || 0;
      document.getElementById('btOtaProgressLabel').textContent = pct + '%';
      document.getElementById('btOtaBytesLabel').textContent =
        (data.bytes_received || 0) + ' / ' + (data.image_size || 0) + ' B';
      document.getElementById('btOtaProgressBar').style.width = pct + '%';
      const errEl = document.getElementById('btOtaError');
      if (data.last_error) {
        errEl.style.display = '';
        errEl.textContent = 'Error: ' + data.last_error;
      } else {
        errEl.style.display = 'none';
      }
      // Faster poll while a session is active
      if (!window.__btOtaPollTimer && (state === 'receiving' || state === 'verifying')) {
        window.__btOtaPollTimer = setInterval(refreshBtOtaStatus, 800);
      }
    }

    async function btDisconnect() {
      await api('/api/bluetooth/disconnect', 'POST');
      refreshBtStatus();
    }

    // Nearby Bluetooth scan: kick off a 10s scan and poll results so the list
    // populates in real time, matching the WiFi scan UX.
    async function btStartScan() {
      const btn = document.getElementById('btScanBtn');
      btn.disabled = true;
      btn.textContent = 'Scanning…';
      document.getElementById('btScanSubtitle').textContent = 'Listening for advertisements…';
      document.getElementById('btScanList').innerHTML =
        '<div class="loading" style="padding:1rem;text-align:center;"><div class="spinner"></div></div>';
      const start = await api('/api/bluetooth/scan/start', 'POST', { duration_sec: 10 });
      if (start && start.success === false) {
        btn.disabled = false;
        btn.textContent = 'Scan';
        document.getElementById('btScanList').innerHTML =
          '<p style="color:var(--danger);font-size:0.85rem;text-align:center;padding:1rem;">' +
          (start.error || 'Scan failed') + '</p>';
        return;
      }
      // Poll for incremental results so devices fade in as the scanner sees them.
      if (btScanPolling) clearInterval(btScanPolling);
      btScanPolling = setInterval(async () => {
        const r = await api('/api/bluetooth/scan/results');
        if (r && r.devices) renderBtScanList(r.devices);
        if (r && !r.scanning) {
          clearInterval(btScanPolling); btScanPolling = null;
          btn.disabled = false;
          btn.textContent = 'Scan';
        }
      }, 1000);
      // Hard cutoff: never leave the spinner spinning.
      setTimeout(() => {
        if (btScanPolling) { clearInterval(btScanPolling); btScanPolling = null; }
        btn.disabled = false;
        btn.textContent = 'Scan';
      }, 14000);
    }

    function renderBtScanList(devices) {
      const list = document.getElementById('btScanList');
      const sub  = document.getElementById('btScanSubtitle');
      const live = document.getElementById('btNearbyCount');
      sub.textContent = devices.length + ' device' + (devices.length === 1 ? '' : 's') + ' nearby';
      if (live) { live.textContent = devices.length; live.dataset.live = '1'; }
      if (!devices.length) {
        list.innerHTML = '<p style="color:var(--muted);font-size:0.85rem;text-align:center;padding:1rem;">No advertisements heard yet</p>';
        return;
      }
      // Sort by signal strength (closest first) so the most relevant devices
      // float to the top — same behaviour as Apple Find My.
      devices.sort((a, b) => (b.rssi || -200) - (a.rssi || -200));
      list.innerHTML = devices.map(d => {
        const bars = rssiBarCount(d.rssi);
        const barsCls = 'signal-bars s' + bars + (bars === 1 ? ' weak' : '');
        const securaBadge = d.is_securacv ?
          ' <span class="badge success" style="font-size:0.6rem;">SecuraCV</span>' : '';
        return '<div class="ble-row">' +
          '<div class="ble-icon">' + deviceIcon(d) + '</div>' +
          '<div class="ble-meta">' +
            '<div class="ble-name">' + escapeHtml(d.name || 'Unknown device') + securaBadge + '</div>' +
            '<div class="ble-sub">' + escapeHtml(d.address || '') + ' · ' + (d.rssi || '?') + ' dBm</div>' +
          '</div>' +
          '<div class="' + barsCls + '"><span></span><span></span><span></span><span></span></div>' +
          '<div class="ble-dist">' +
            '<div class="ble-dist-m">' + distancePretty(d.distance_m) + '</div>' +
            '<div class="ble-dist-l">' + escapeHtml(d.distance_label || '') + '</div>' +
          '</div>' +
        '</div>';
      }).join('');
    }

    async function btClearScan() {
      await api('/api/bluetooth/scan/results', 'DELETE');
      const live = document.getElementById('btNearbyCount');
      if (live) { live.textContent = '0'; delete live.dataset.live; }
      document.getElementById('btScanList').innerHTML =
        '<p style="color:var(--muted);font-size:0.85rem;text-align:center;padding:1rem;">No scan yet</p>';
      document.getElementById('btScanSubtitle').textContent = 'Tap Scan to see what’s around';
    }

    async function loadBtPairedDevices() {
      const data = await api('/api/bluetooth/paired');
      if (!data.devices) return;
      const list = document.getElementById('btPairedList');
      document.getElementById('btPairedSubtitle').textContent = (data.count || 0) + ' paired';
      if (!data.devices.length) {
        list.innerHTML = '<p style="color:var(--muted);text-align:center;padding:1rem;">No paired devices</p>';
        return;
      }
      list.innerHTML = data.devices.map(d => `<div class="log-item" style="padding:0.75rem;">
        <div><strong>${d.name || 'Unknown'}</strong><div style="font-size:0.75rem;color:var(--muted);">${d.address}</div></div>
        <button class="btn btn-danger btn-sm" onclick="btRemovePaired('${d.address}')">Remove</button>
      </div>`).join('');
    }

    async function btRemovePaired(addr) { if (confirm('Remove?')) { await api('/api/bluetooth/paired', 'DELETE', { address: addr }); loadBtPairedDevices(); } }
    async function btClearAllPaired() { if (confirm('Remove all?')) { await api('/api/bluetooth/paired/all', 'DELETE'); loadBtPairedDevices(); } }

    // ══════════════════════════════════════════════════════════════════
    // DEVICE CONFIG
    // ══════════════════════════════════════════════════════════════════
    async function saveConfig() {
      const config = {
        record_interval_ms: parseInt(document.getElementById('configRecordInterval').value),
        time_bucket_ms: parseInt(document.getElementById('configTimeBucket').value),
        log_level: parseInt(document.getElementById('configLogLevel').value)
      };
      const data = await api('/api/config', 'POST', config);
      alert(data.ok ? 'Saved!' : 'Failed');
    }

    function confirmReboot() { if (confirm('Reboot device?')) { api('/api/reboot', 'POST'); alert('Rebooting...'); } }
    async function rotateOldLogs() { if (confirm('Delete logs > 30 days?')) { const data = await api('/api/logs/rotate', 'POST', { max_age_days: 30 }); alert(data.ok ? `Rotated ${data.deleted_count || 0}` : 'Failed'); } }

    // ══════════════════════════════════════════════════════════════════
    // UTILITIES
    // ══════════════════════════════════════════════════════════════════
    function formatUptime(sec) {
      const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
      return `${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}:${s.toString().padStart(2,'0')}`;
    }
    function formatTimestamp(ms) { return ms ? new Date(ms).toLocaleTimeString() : '--'; }
    function truncateHash(h, len) { return (!h || h.length <= len) ? (h || '--') : h.substring(0, len) + '...'; }
    function escapeHtml(str) { const d = document.createElement('div'); d.textContent = str || ''; return d.innerHTML; }

    // ══════════════════════════════════════════════════════════════════
    // INIT
    // ══════════════════════════════════════════════════════════════════
    refreshStatus();
    refreshSystemHealth();
    loadChain();
    refreshRfStatus();
    refreshWifiPresence();
    refreshAudibleChirpStatus();
    loadWifiStatus();
    refreshOpera();
    refreshChirpStatus();
    refreshBtStatus();
    loadBtSettings();
    loadBtPairedDevices();
    updateResolutionUI();

    setInterval(refreshStatus, 2000);
    setInterval(refreshSystemHealth, 5000);
    setInterval(refreshRfStatus, 3000);
    setInterval(loadWifiStatus, 5000);
    setInterval(() => {
      if (currentPanel === 'presence') { refreshPresence(); refreshHousehold(); refreshWifiPresence(); }
      else if (currentPanel === 'records' && currentRecordsTab === 'logs') loadLogs();
      else if (currentPanel === 'records' && currentRecordsTab === 'witness') loadWitness();
      else if (currentPanel === 'community' && currentCommunityTab === 'opera') refreshOpera();
      else if (currentPanel === 'community' && currentCommunityTab === 'chirp') refreshChirpStatus();
      else if (currentPanel === 'settings' && currentSettingsTab === 'bluetooth') refreshBtStatus();
    }, 5000);
  </script>
</body>
</html>
)rawliteral";

#endif // SECURACV_WEB_UI_H
