/*
 * SecuraCV Canary — WiFi Access Point & HTTP Server
 *
 * Provides local-only access for:
 * - Device health monitoring
 * - Witness record review
 * - Log management and acknowledgment
 * - Configuration
 * - Export functionality
 * - Mesh network (opera) management
 *
 * Security considerations:
 * - AP mode only (no internet connection required)
 * - WPA2/WPA3 transition (PMF capable) with a device-unique password; WPA2-PSK
 *   where the core lacks SoftAP SAE (ap_security_policy.h, F16)
 * - HTTPS on port 443 once first-boot setup is done (where the build has the
 *   HTTPS server); plain HTTP during setup, and if the certificate fails
 * - While HTTPS is up, port 80 serves the probes and /api/fleet and redirects
 *   the rest to HTTPS (301)
 * - Bearer token authentication on protected endpoints
 * - Constant-time token comparison (timing attack mitigation)
 * - Exponential backoff on auth failures (brute-force mitigation)
 * - Physical BOOT button required for provisioning receipt
 * - No sensitive data in URLs (POST for actions, Authorization header for tokens)
 * - CORS restricted to same-origin
 * - Rate limiting on sensitive endpoints
 * - Max 1 AP client for security isolation
 * - Mesh pairing requires physical confirmation
 */

#ifndef SECURACV_WAP_SERVER_H
#define SECURACV_WAP_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_http_server.h"
#include "esp_https_server.h"

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

namespace wap_server {

// Access Point configuration
static const char* AP_SSID_PREFIX    = "SecuraCV-";      // + last 4 chars of MAC
static const char* AP_HOSTNAME       = "canary";
static const int   AP_CHANNEL        = 1;
static const int   AP_MAX_CLIENTS    = 1;                // Hardened: max 1 client
static const bool  AP_HIDDEN         = false;

// Server configuration
static const int   HTTPS_PORT       = 443;              // Primary (TLS)
static const int   HTTP_PORT        = 80;               // Redirect to HTTPS
static const int   HTTP_MAX_URI_LEN = 512;
static const int   HTTP_MAX_RESP_HDR = 512;

// Rate limiting
static const uint32_t RATE_LIMIT_WINDOW_MS = 60000;  // 1 minute window
static const int      RATE_LIMIT_REQUESTS  = 120;    // Max requests per window
static const int      RATE_LIMIT_ACTIONS   = 30;     // Max POST actions per window

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

enum ServerState : uint8_t {
  SERVER_STATE_STOPPED   = 0,
  SERVER_STATE_STARTING  = 1,
  SERVER_STATE_RUNNING   = 2,
  SERVER_STATE_ERROR     = 3
};

struct ServerStatus {
  ServerState state;
  bool ap_active;
  bool mdns_active;
  uint8_t connected_clients;
  char ap_ssid[32];
  char ap_ip[16];
  uint32_t requests_served;
  uint32_t requests_rejected;
  uint32_t uptime_sec;
  uint32_t last_request_ms;
};

struct RateLimitEntry {
  uint32_t window_start_ms;
  uint16_t request_count;
  uint16_t action_count;
};

// ════════════════════════════════════════════════════════════════════════════
// API ENDPOINTS
// ════════════════════════════════════════════════════════════════════════════

/*
 * HTTP routes, as canary_wap.ino registers them on its one primary server
 * (the TLS server when HTTPS is up, else plain HTTP): register_api_routes()
 * with csi_integration::init() inside it, then start_http_server() from its
 * register_extra_routes: label, each X_api::register_routes() inline. While
 * HTTPS is up the port-80 server serves only the six probes and GET/OPTIONS
 * /api/fleet, and 301-redirects the rest; it is not listed here.
 *
 * Every entry below is served, and every route served is an entry or falls
 * under a family entry (METHOD `*`, path ending in `*`): firmware/tests_host/
 * test_dashboard_route_match.test.js holds both against the route table it
 * reads, and that an entry under an #if names it on its line or in its
 * section's heading (sweep F237, which removed eight entries nothing served:
 * /api/health, /api/identity, /api/witness/<seq>, /api/witness/stats,
 * /api/logs/unacked, /api/gps, /api/time, /api/export/download). Each
 * handler checks its own credential; the public ones are marked.
 *
 * ── Pages ──
 * GET    /                                - Sensing dashboard (csi_dashboard_html.h); the pair landing without a session
 * GET    /sense                           - 301 to /
 * GET    /admin                           - Tabbed admin dashboard (web_ui.h)
 * GET    /settings                        - The admin dashboard, for its Settings tab
 * GET    /tune                            - Tuning Lab (302 to / without a session)
 * GET    /mqtt                            - Hub (MQTT) settings page
 * GET    /companion                       - Companion PWA (public, static)
 * GET    /companion-sw.js                 - Its service worker
 * GET    /companion-manifest.webmanifest  - Its manifest
 * GET    /manifest.webmanifest            - The Sensing dashboard's manifest
 * GET    /sw.js                           - The Sensing dashboard's service worker
 * GET    /enroll                          - Public key card, HTML (public)
 * GET    /hotspot-detect.html             - Captive probe (Apple)
 * GET    /library/test/success.html       - Captive probe (Apple, older)
 * GET    /generate_204                    - Captive probe (Android)
 * GET    /gen_204                         - Captive probe (Android, short)
 * GET    /connecttest.txt                 - Captive probe (Windows)
 * GET    /ncsi.txt                        - Captive probe (Windows, legacy)
 *
 * ── Device, chain, records ──
 * GET    /api/device/enroll               - Public key card, JSON (public)
 * GET    /api/device-info                 - Non-sensitive device metadata (public)
 * GET    /api/provisioning-receipt        - Provisioning receipt (physical BOOT button or Bearer token)
 * GET    /api/pairing-qr                  - SVG QR of the pairing receipt (carries the API token)
 * GET    /api/help-qr                     - SVG QR deep link to the Help Desk for the current verdict (public)
 * GET    /api/status                      - Device status JSON
 * GET    /api/system                      - System metrics (FEATURE_SYS_MONITOR)
 * GET    /api/diagnostics                 - Diagnostics (FEATURE_SYS_MONITOR)
 * GET    /api/battery/history             - Battery history (FEATURE_POWER_MONITOR)
 * GET    /api/chain                       - Chain state (head hash, sequence)
 * GET    /api/witness                     - List witness records (paginated)
 * GET    /api/v1/witness                  - The shared witness-page contract (spec/witness_api_v1.md):
 *                                           newest N signed records with their chain-hash pre-image,
 *                                           from the RAM ring (witness_page.h); ?last=N, 1..100
 * GET    /api/logs                        - List health logs (paginated, filterable)
 * POST   /api/logs/ack-all                - Acknowledge all logs up to level
 * POST   /api/logs/rotate                 - Rotate old logs
 * POST   /api/logs/<seq>/ack              - Acknowledge a log entry (registered as /api/logs/*, after the
 *                                           two above; the handler reads <seq>/ack, sweep F214)
 * GET    /api/config                      - Get current configuration
 * POST   /api/config                      - Update configuration
 * POST   /api/export                      - Create export bundle
 * POST   /api/reboot                      - Reboot device (bearer alone, F361)
 * POST   /api/safe-mode/retry             - Leave safe mode and retry (bearer alone, F361)
 * POST   /api/identify                    - Blink to identify
 * POST   /api/device-name                 - Set the device name
 * GET    /api/selftest                    - Run the self-test
 * GET    /api/fleet                       - Coarse fleet presence (public; also on the port-80 server)
 * OPTIONS /api/fleet                      - Its CORS preflight
 * GET    /api/fleet/qr                    - Fleet provisioning QR
 * GET    /api/fleet/scan                  - Fleet LAN discovery (mDNS browse, cached)
 *
 * ── Sensing (csi_integration.cpp) ──
 * *      /api/csi/*                       - CSI stream, window and calibration
 * *      /api/events/*                    - Today's events, dismiss
 * *      /api/settings                    - Sensing settings, GET and POST (Pet Mode today)
 * GET    /api/privacy-budget              - Outbound privacy budget
 * *      /api/tune/*                      - Tuning Lab coefficients and presets
 * GET    /api/pair/token                  - A fresh pair token for the companion PWA
 * *      /api/mqtt/*                      - Hub (MQTT) config and test
 *
 * ── Microphone (FEATURE_ACOUSTIC_EVENTS) ──
 * *      /api/audio/*                     - Status, mute, self-test, config, transitions
 *
 * ── Wi-Fi and updates ──
 * GET    /api/wifi                        - Wi-Fi status
 * *      /api/wifi/*                      - Scan, pair token, AP-only, connect, disconnect, forget, reconnect
 *                                           (after setup connect, AP-only, disconnect and forget: bearer alone, F334)
 * *      /api/wifi/qr-scan                - Camera QR provisioning (FEATURE_QR_PROVISION; the POST start during setup only, F334)
 * *      /api/ota/*                       - Signed pull-OTA: status, check, install, config
 *
 * ── Camera (FEATURE_CAMERA_PEEK) and sealed snapshots (FEATURE_VAULT_SNAPSHOT) ──
 * *      /api/peek/*                      - Camera peek (FEATURE_CAMERA_PEEK)
 * *      /api/vault/*                     - Sealed snapshots (FEATURE_VAULT_SNAPSHOT)
 *
 * ── Mesh network, opera (FEATURE_MESH_NETWORK) ──
 * GET    /api/mesh                        - Mesh status and opera info
 * GET    /api/mesh/peers                  - List opera peers with status
 * GET    /api/mesh/alerts                 - Recent alerts from opera
 * DELETE /api/mesh/alerts                 - Clear them
 * POST   /api/mesh/enable                 - Enable/disable mesh networking
 * POST   /api/mesh/pair/start             - Start pairing (initiator mode)
 * POST   /api/mesh/pair/join              - Start pairing (joiner mode)
 * POST   /api/mesh/pair/confirm           - Confirm pairing code (bearer alone, F346)
 * POST   /api/mesh/pair/cancel            - Cancel ongoing pairing
 * POST   /api/mesh/leave                  - Leave current opera
 * POST   /api/mesh/remove                 - Remove peer from opera
 * POST   /api/mesh/name                   - Set opera name
 *
 * ── Chirp channel, community witness network (chirp_api.h, FEATURE_MESH_NETWORK) ──
 * GET    /api/chirp                       - Chirp channel status
 * GET    /api/chirp/nearby                - Count of nearby chirp devices
 * GET    /api/chirp/recent                - Recent community chirps (last 30 min)
 * GET    /api/chirp/templates             - The chirp templates
 * POST   /api/chirp/enable                - Enable chirp channel
 * POST   /api/chirp/disable               - Disable chirp channel
 * POST   /api/chirp/send                  - Send chirp (requires human confirmation)
 * POST   /api/chirp/confirm               - Confirm a pending chirp
 * POST   /api/chirp/ack                   - Acknowledge a chirp
 * POST   /api/chirp/dismiss               - Dismiss a chirp from display
 * POST   /api/chirp/mute                  - Mute for duration (15/30/60/120 min)
 * POST   /api/chirp/unmute                - Unmute chirps
 * POST   /api/chirp/settings              - Update chirp settings (relay, filter)
 *
 * ── Bluetooth and BLE discovery ──
 * *      /api/bluetooth*                  - Pairing channel (bluetooth_api.h, FEATURE_BLUETOOTH)
 * GET    /api/ble/status                  - BLE discovery status (FEATURE_BLE)
 * GET    /api/nearby                      - Nearby Canaries (FEATURE_BLE)
 * POST   /api/ble/chirp/send              - BLE discovery chirp (FEATURE_BLE)
 *
 * ── Presence, RF, household, audible chirp, beacon ──
 * *      /api/presence*                   - Combined and Wi-Fi presence, overrides (wifi_presence_api.h, household_api.h)
 * *      /api/rf/*                        - RF presence (rf_presence_api.h)
 * *      /api/household*                  - Household roles (household_api.h)
 * *      /api/audible-chirp*              - Audible chirp (audible_chirp_api.h)
 * *      /api/beacon*                     - Beacon channel (beacon_api.h, FEATURE_BEACON_CHANNEL)
 */

// ════════════════════════════════════════════════════════════════════════════
// FUNCTION DECLARATIONS
// ════════════════════════════════════════════════════════════════════════════

// Initialization
bool init();
void deinit();
bool is_running();
ServerStatus get_status();

// Access Point management
bool start_access_point(const char* ssid = nullptr, const char* password = nullptr);
bool stop_access_point();
bool set_ap_credentials(const char* ssid, const char* password);
uint8_t get_connected_clients();

// HTTP server
bool start_http_server();
bool stop_http_server();

// mDNS
bool start_mdns(const char* hostname = nullptr);
bool stop_mdns();

// Rate limiting
bool check_rate_limit(const char* client_ip, bool is_action = false);
void reset_rate_limits();

// Response helpers
esp_err_t send_json_response(httpd_req_t* req, const char* json);
esp_err_t send_json_error(httpd_req_t* req, int status_code, 
                          const char* error_code, const char* message);
esp_err_t send_file_response(httpd_req_t* req, const char* path, 
                             const char* content_type);

// Security helpers
bool validate_request(httpd_req_t* req);
void set_security_headers(httpd_req_t* req);

} // namespace wap_server

#endif // SECURACV_WAP_SERVER_H
