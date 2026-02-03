/**
 * @file web_ui.h
 * @brief Web UI assets and serving
 *
 * Provides embedded web UI assets and serving functionality.
 * The UI is embedded as compressed data in flash to save space.
 */

#pragma once

#include "../core/types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// EMBEDDED ASSETS
// ============================================================================

/**
 * @brief Web asset entry
 */
typedef struct {
    const char* path;           // URI path
    const char* content_type;   // MIME type
    const uint8_t* data;        // Asset data (may be gzipped)
    size_t size;                // Data size
    bool gzipped;               // Data is gzip compressed
} web_asset_t;

/**
 * @brief Get embedded web asset
 * @param path URI path (e.g., "/", "/style.css")
 * @return Asset entry or NULL if not found
 */
const web_asset_t* web_ui_get_asset(const char* path);

/**
 * @brief Get number of embedded assets
 * @return Asset count
 */
size_t web_ui_asset_count(void);

/**
 * @brief Get all embedded assets
 * @return Array of assets
 */
const web_asset_t* web_ui_get_assets(void);

// ============================================================================
// UI REGISTRATION
// ============================================================================

/**
 * @brief Register web UI routes with HTTP server
 *
 * Registers handlers for all embedded assets and the main UI.
 *
 * @return RESULT_OK on success
 */
result_t web_ui_register_routes(void);

// ============================================================================
// UI DATA
// ============================================================================

/**
 * @brief Get main HTML page
 * @return HTML content
 */
const char* web_ui_get_html(void);

/**
 * @brief Get main CSS
 * @return CSS content
 */
const char* web_ui_get_css(void);

/**
 * @brief Get main JavaScript
 * @return JavaScript content
 */
const char* web_ui_get_js(void);

#ifdef __cplusplus
}
#endif

// ============================================================================
// EMBEDDED HTML TEMPLATE
// ============================================================================

#ifdef WEB_UI_IMPLEMENTATION

// Main HTML template - embedded in firmware
static const char WEB_UI_HTML[] = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>SecuraCV Canary</title>
    <style>
        :root {
            --bg: #1a1a2e;
            --fg: #eef;
            --accent: #4a9eff;
            --success: #4ade80;
            --warning: #fbbf24;
            --error: #f87171;
            --card-bg: #16213e;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: var(--bg);
            color: var(--fg);
            line-height: 1.6;
            padding: 1rem;
        }
        .container { max-width: 800px; margin: 0 auto; }
        header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 1rem 0;
            border-bottom: 1px solid #333;
            margin-bottom: 1rem;
        }
        h1 { font-size: 1.5rem; }
        .status-badge {
            padding: 0.25rem 0.75rem;
            border-radius: 9999px;
            font-size: 0.875rem;
            font-weight: 500;
        }
        .status-online { background: var(--success); color: #000; }
        .status-offline { background: var(--error); color: #fff; }
        .card {
            background: var(--card-bg);
            border-radius: 0.5rem;
            padding: 1rem;
            margin-bottom: 1rem;
        }
        .card h2 {
            font-size: 1rem;
            margin-bottom: 0.75rem;
            color: var(--accent);
        }
        .grid { display: grid; gap: 1rem; }
        .grid-2 { grid-template-columns: repeat(2, 1fr); }
        .metric {
            display: flex;
            justify-content: space-between;
            padding: 0.5rem 0;
            border-bottom: 1px solid #333;
        }
        .metric:last-child { border-bottom: none; }
        .metric-value { font-weight: 600; color: var(--accent); }
        .btn {
            display: inline-block;
            padding: 0.5rem 1rem;
            border-radius: 0.25rem;
            border: none;
            cursor: pointer;
            font-size: 0.875rem;
            transition: opacity 0.2s;
        }
        .btn:hover { opacity: 0.8; }
        .btn-primary { background: var(--accent); color: #fff; }
        .btn-danger { background: var(--error); color: #fff; }
        .log-entry {
            font-family: monospace;
            font-size: 0.8rem;
            padding: 0.25rem 0;
            border-bottom: 1px solid #222;
        }
        #logs-container {
            max-height: 300px;
            overflow-y: auto;
        }
        @media (max-width: 600px) {
            .grid-2 { grid-template-columns: 1fr; }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>SecuraCV Canary</h1>
            <span id="status-badge" class="status-badge status-offline">Connecting...</span>
        </header>

        <div class="grid grid-2">
            <div class="card">
                <h2>Device Info</h2>
                <div class="metric"><span>Device ID</span><span id="device-id" class="metric-value">-</span></div>
                <div class="metric"><span>Firmware</span><span id="firmware" class="metric-value">-</span></div>
                <div class="metric"><span>Uptime</span><span id="uptime" class="metric-value">-</span></div>
                <div class="metric"><span>Free Heap</span><span id="heap" class="metric-value">-</span></div>
            </div>

            <div class="card">
                <h2>Witness Chain</h2>
                <div class="metric"><span>Sequence</span><span id="sequence" class="metric-value">-</span></div>
                <div class="metric"><span>Records</span><span id="records" class="metric-value">-</span></div>
                <div class="metric"><span>Verified</span><span id="verified" class="metric-value">-</span></div>
                <div class="metric"><span>Boot Count</span><span id="boots" class="metric-value">-</span></div>
            </div>
        </div>

        <div class="card">
            <h2>GPS Status</h2>
            <div class="grid grid-2">
                <div>
                    <div class="metric"><span>Fix</span><span id="gps-fix" class="metric-value">-</span></div>
                    <div class="metric"><span>Satellites</span><span id="gps-sats" class="metric-value">-</span></div>
                </div>
                <div>
                    <div class="metric"><span>Latitude</span><span id="gps-lat" class="metric-value">-</span></div>
                    <div class="metric"><span>Longitude</span><span id="gps-lon" class="metric-value">-</span></div>
                </div>
            </div>
        </div>

        <div class="card">
            <h2>Recent Logs</h2>
            <div id="logs-container"></div>
        </div>

        <div class="card">
            <h2>Actions</h2>
            <button class="btn btn-primary" onclick="exportRecords()">Export Records</button>
            <button class="btn btn-primary" onclick="refreshStatus()">Refresh</button>
        </div>
    </div>

    <script>
        async function fetchStatus() {
            try {
                const res = await fetch('/api/status');
                const data = await res.json();
                updateUI(data);
                document.getElementById('status-badge').textContent = 'Online';
                document.getElementById('status-badge').className = 'status-badge status-online';
            } catch (e) {
                document.getElementById('status-badge').textContent = 'Offline';
                document.getElementById('status-badge').className = 'status-badge status-offline';
            }
        }

        function updateUI(data) {
            document.getElementById('device-id').textContent = data.device_id || '-';
            document.getElementById('firmware').textContent = data.firmware || '-';
            document.getElementById('uptime').textContent = formatUptime(data.uptime_sec);
            document.getElementById('heap').textContent = formatBytes(data.free_heap);
            document.getElementById('sequence').textContent = data.sequence || '-';
            document.getElementById('records').textContent = data.records_created || '-';
            document.getElementById('verified').textContent = data.records_verified || '-';
            document.getElementById('boots').textContent = data.boot_count || '-';
            if (data.gps) {
                document.getElementById('gps-fix').textContent = data.gps.valid ? 'Yes' : 'No';
                document.getElementById('gps-sats').textContent = data.gps.satellites || '-';
                document.getElementById('gps-lat').textContent = data.gps.lat?.toFixed(6) || '-';
                document.getElementById('gps-lon').textContent = data.gps.lon?.toFixed(6) || '-';
            }
        }

        function formatUptime(sec) {
            if (!sec) return '-';
            const h = Math.floor(sec / 3600);
            const m = Math.floor((sec % 3600) / 60);
            const s = sec % 60;
            return `${h}h ${m}m ${s}s`;
        }

        function formatBytes(b) {
            if (!b) return '-';
            return (b / 1024).toFixed(1) + ' KB';
        }

        async function exportRecords() {
            window.location.href = '/api/witness/export';
        }

        function refreshStatus() {
            fetchStatus();
        }

        fetchStatus();
        setInterval(fetchStatus, 5000);
    </script>
</body>
</html>
)html";

#endif // WEB_UI_IMPLEMENTATION
