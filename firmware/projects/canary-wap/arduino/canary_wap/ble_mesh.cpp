/*
 * SecuraCV Canary — BLE Mesh control-plane scaffolding (no transport yet)
 *
 * This translation unit deliberately ships a non-functional implementation:
 * the wire-format types and the public API are locked down so consumers
 * (rf_presence, household, chain) can be wired against them, but init()
 * refuses until we pick a transport per docs/BLE_MESH_OPERA_TANDEM.md.
 *
 * Choosing a transport is a design decision — see the doc for the A/B/C
 * options. Each requires a different effort tier and a different sdkconfig
 * footprint, so we don't want to commit to one without explicit sign-off.
 */

#include "build_config.h"
#include "ble_mesh.h"
#include "health_log.h"
#include <string.h>

namespace ble_mesh {

static bool                g_running = false;
static ChainHeadHandler    g_chain_handler  = nullptr;
static BondHandler         g_bond_handler   = nullptr;
static BloomDeltaHandler   g_bloom_handler  = nullptr;
static uint32_t            g_msgs_sent     = 0;
static uint32_t            g_msgs_received = 0;
static uint32_t            g_auth_failures = 0;
static uint32_t            g_replay_drops  = 0;

bool init(const uint8_t /*netkey*/[16],
          const uint8_t /*appkey*/[16],
          uint32_t /*my_sender_id*/) {
  // Transport is not yet wired. Returning false rather than silently
  // succeeding so the call site can fall back to Opera-only sync (Option C
  // in the design doc) until we pick a path.
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
             "BLE Mesh control plane: transport not wired (see docs/BLE_MESH_OPERA_TANDEM.md)",
             nullptr);
  g_running = false;
  return false;
}

void deinit() {
  g_running = false;
  g_chain_handler = nullptr;
  g_bond_handler  = nullptr;
  g_bloom_handler = nullptr;
}

bool is_running() { return g_running; }

bool publish_chain_head(const ChainHeadHeartbeat& /*msg*/)     { return false; }
bool publish_bond(const BondAdvertise& /*msg*/)                { return false; }
bool publish_familiar_delta(const FamiliarBloomDelta& /*msg*/) { return false; }

void set_handlers(ChainHeadHandler h_chain,
                  BondHandler h_bond,
                  BloomDeltaHandler h_bloom) {
  g_chain_handler = h_chain;
  g_bond_handler  = h_bond;
  g_bloom_handler = h_bloom;
}

uint32_t messages_sent()     { return g_msgs_sent; }
uint32_t messages_received() { return g_msgs_received; }
uint32_t auth_failures()     { return g_auth_failures; }
uint32_t replay_drops()      { return g_replay_drops; }

} // namespace ble_mesh
