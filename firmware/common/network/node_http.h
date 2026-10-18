/**
 * @file node_http.h
 * @brief The small HTTP surface every headless Canary serves once it is on
 *        the network: GET /api/status and GET /api/fleet.
 *
 * canary-sense, canary-vision and canary-sentinel ran NO HTTP server after
 * provisioning (docs/FIRMWARE_MAP.md §4): the Witness Wall that followed
 * their mDNS advert got a refused connection, and nothing on the LAN could
 * ask them how they were. This module is the fix, shared: a WebServer on
 * port 80 that is up exactly while the STA link is up and the setup portal
 * (which owns port 80 while it is raised) is down.
 *
 *   GET /api/status  the coarse body in common/network/node_status.h.
 *   GET /api/fleet   the roll-call body in common/fleet_selfreport/
 *                    fleet_selfreport.h — the contract the Witness Wall polls
 *                    (tvos/discovery/DISCOVERY.md), self row only.
 *   OPTIONS /api/fleet  CORS preflight, as the display answers it.
 *
 * Both carry Access-Control-Allow-Origin: * (the Wall reads from another
 * origin), which is why both bodies are coarse by construction.
 *
 * The module remembers when /api/fleet was last read, so the status body can
 * say the Wall is reading this node directly (`wall: "direct"`).
 *
 * Compiled by each board's build_src_filter beside setup_portal.cpp; the board
 * fills the two bodies through the hooks, and this file owns the server.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "fleet_selfreport/fleet_selfreport.h"
#include "network/node_status.h"

namespace canary {
namespace net {
namespace node_http {

struct Hooks {
  /// Fill the status body. Strings must stay valid until the handler returns
  /// (static buffers in the board's glue are the pattern).
  void (*fill_status)(NodeStatus& out);
  /// Fill the self row of /api/fleet. Same lifetime rule.
  void (*fill_fleet)(FleetSelfDevice& out);
  /// The "kernel" name /api/fleet opens with — the device's own name.
  const char* (*kernel_name)();
};

/// Remember the hooks. Does not start the server.
void begin(const Hooks& hooks);

/// Every loop() pass: start the server when `sta_online && !portal_active`,
/// stop it otherwise, and serve requests.
void loop(uint32_t now_ms, bool sta_online, bool portal_active);

/// True while the server is up.
bool serving();

/// millis() of the last GET /api/fleet, 0 = never this boot.
uint32_t last_fleet_poll_ms();

/// Seconds since the last /api/fleet GET, or -1 when never / unknown.
int fleet_polled_s_ago(uint32_t now_ms);

/// The Wall is reading this node directly if it polled within this window
/// (the Wall polls every 10 s; six missed polls = gone).
static constexpr uint32_t WALL_DIRECT_WINDOW_MS = 60000;
bool wall_polling(uint32_t now_ms);

}  // namespace node_http
}  // namespace net
}  // namespace canary
