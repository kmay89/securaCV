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
 * - WPA2-PSK encryption with device-unique password
 * - TLS 1.2+ on all connections (port 443)
 * - HTTP port 80 redirects to HTTPS (301)
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
 * REST API Endpoints:
 *
 * ── Public (no auth required) ──
 * GET  /                         - Dashboard UI (HTML)
 * GET  /api/device-info          - Non-sensitive device metadata
 * GET  /api/provisioning-receipt - Provisioning receipt (physical BOOT button or Bearer token)
 * GET  /api/help-qr              - SVG QR deep link to the Help Desk for the current
 *                                  verdict (coarse anchor only; selftest-parity boundary)
 *
 * ── Authenticated (Bearer token required) ──
 * GET  /api/status          - Device status JSON
 * GET  /api/health          - System health metrics + auth stats
 * GET  /api/pairing-qr      - SVG QR of the pairing receipt (carries the API token)
 * GET  /api/identity        - Device identity (public key, fingerprint)
 * GET  /api/chain           - Chain state (head hash, sequence)
 *
 * GET  /api/witness         - List witness records (paginated)
 * GET  /api/witness/:seq    - Get specific witness record
 * GET  /api/witness/stats   - Witness statistics
 *
 * GET  /api/logs            - List health logs (paginated, filterable)
 * GET  /api/logs/unacked    - List unacknowledged logs
 * POST /api/logs/:seq/ack   - Acknowledge a log entry
 * POST /api/logs/ack-all    - Acknowledge all logs up to level
 *
 * GET  /api/gps             - Current GPS status
 * GET  /api/time            - Time synchronization status
 *
 * POST /api/export          - Create export bundle
 * GET  /api/export/download - Download export bundle
 *
 * GET  /api/config          - Get current configuration
 * POST /api/config          - Update configuration
 *
 * POST /api/reboot          - Reboot device
 *
 * Mesh Network (Opera) Endpoints:
 * GET  /api/mesh            - Mesh status and opera info
 * GET  /api/mesh/peers      - List opera peers with status
 * GET  /api/mesh/alerts     - Recent alerts from opera
 * POST /api/mesh/enable     - Enable/disable mesh networking
 * POST /api/mesh/pair/start - Start pairing (initiator mode)
 * POST /api/mesh/pair/join  - Start pairing (joiner mode)
 * POST /api/mesh/pair/confirm - Confirm pairing code
 * POST /api/mesh/pair/cancel  - Cancel ongoing pairing
 * POST /api/mesh/leave      - Leave current opera
 * POST /api/mesh/remove     - Remove peer from opera
 * POST /api/mesh/name       - Set opera name
 *
 * Chirp Channel (Community Witness Network) Endpoints:
 * GET  /api/chirp            - Chirp channel status
 * GET  /api/chirp/nearby     - Count of nearby chirp devices
 * GET  /api/chirp/recent     - Recent community chirps (last 30 min)
 * POST /api/chirp/enable     - Enable chirp channel
 * POST /api/chirp/disable    - Disable chirp channel
 * POST /api/chirp/send       - Send chirp (requires human confirmation)
 * POST /api/chirp/ack        - Acknowledge a chirp
 * POST /api/chirp/dismiss    - Dismiss a chirp from display
 * POST /api/chirp/mute       - Mute for duration (15/30/60/120 min)
 * POST /api/chirp/unmute     - Unmute chirps
 * POST /api/chirp/settings   - Update chirp settings (relay, filter)
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
