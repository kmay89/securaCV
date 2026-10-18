/**
 * @file wifi_supervisor.h
 * @brief The headless Canaries' one Wi-Fi supervisor — STA join, backoff
 *        retry, the shared setup portal, recovery, saved networks, the clock.
 *
 * canary-sense, canary-vision and canary-sentinel each carried a
 * `src/net/wifi_mgr.cpp` that was the same 300 lines with a different
 * product name (docs/FIRMWARE_MAP.md §3 — "parity by convention"). This is
 * that file once, compiled by each board's build_src_filter beside
 * setup_portal.cpp; each board keeps a thin `wifi_mgr.cpp` shim that builds
 * the Config from its own canary/config.h and runtime_config and forwards
 * the historical `canary::net::wifi_*` calls, so main.cpp did not change.
 *
 * What it does, in the words of common/network/onboarding_fsm.h:
 *
 *   unprovisioned  no saved network → raise the shared setup portal at boot
 *                  (the Improv door opens beside it on boards with a radio);
 *                  sensing continues underneath.
 *   joining        boot: a bounded blocking join of the saved network (never a
 *                  reboot on timeout — wifi_join_policy.h); after boot: backoff
 *                  retries with jitter, rotating through the saved networks
 *                  (wifi_store.h pick rule) on a NotFound.
 *   provisioned    online. Power policy applied, the clock synced (SNTP: pool,
 *                  nist, the hub as third — time/clock_sync.h), saved-network
 *                  ring persisted (`wifi_nets` blob), the legacy pair kept in
 *                  step through Config::persist_current so every other reader
 *                  of `wifi_ssid` keeps working.
 *   recovery       a never-online node failing for a fixable reason after three
 *                  attempts (wifi_should_open_setup), OR any node with saved
 *                  networks offline ten minutes (onboard::recovery_reraise_due)
 *                  → the portal is re-raised without wiping; the saved networks
 *                  keep being retried quietly underneath it.
 *
 * Plus the BOOT-tap portal under the Bluetooth door (unchanged from the three
 * managers) and `forget_all()`, the credentials half of a factory reset.
 *
 * Status facts for GET /api/status come from state(), ap_up(), saved_count(),
 * clock_source(). Nothing here touches MQTT; the board's broker supervision
 * stays where it was.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "network/onboarding_fsm.h"
#include "network/wifi_join_policy.h"
#include "time/clock_sync.h"

namespace canary {
namespace net {
namespace wifi_sup {

struct Config {
  /// Product name the wizard page shows, e.g. "Canary Sense".
  const char* product_name;
  /// One of onboard::NODE_* — the setup AP's middle word.
  const char* node_type;
  /// Device-unique, MAC-free SSID suffix (>= 4 chars); must outlive begin().
  const char* id_suffix;
  /// Setup AP channel (1, or the board's ESP-NOW fallback channel).
  uint8_t ap_channel;

  /// The credentials runtime_config resolved for this boot (compiled secrets
  /// win over NVS and are persisted; placeholders defer). `seed_configured`
  /// false means "no real network yet" whatever the strings say.
  const char* seed_ssid;
  const char* seed_pass;
  bool seed_configured;
  /// Keep the legacy `wifi_ssid`/`wifi_pass` pair equal to the CURRENT pick,
  /// so runtime_config, the flashers' seed scheme and every other reader
  /// stay right. Called with ("", "") on forget_all(). Return = durable.
  bool (*persist_current)(const char* ssid, const char* pass);
  /// Optional: the wizard's name and time-zone fields (null = not offered).
  bool (*save_identity)(const char* name, const char* tz);

  /// The hub's host for the third SNTP server (null / placeholder → none).
  const char* hub_host;
  /// POSIX TZ string for configTzTime (null → UTC).
  const char* posix_tz;

  /// The boot-time blocking bound (the board's WIFI_BOOT_TIMEOUT_MS).
  uint32_t boot_timeout_ms;
  /// The retry schedule, built by the board's shim from its own tunables by
  /// name (`p.base_ms = WIFI_RETRY_BASE_MS;` …) — scripts/lint_wifi_join_policy.py
  /// holds every shim to that spelling, and this core to applying it only
  /// through wifi_next_action().
  WifiRetryPolicy retry;
  /// Power policy after a join (the board's canary/config.h values).
  bool power_save;
  int8_t tx_power_qdbm;
};

/// Boot-time: adopt the config, join the saved network (bounded) or raise the
/// portal. `idle_poll` (may be null) runs each 300 ms wait tick.
void begin(const Config& cfg, void (*idle_poll)());
/// Every loop() pass.
void loop(uint32_t now_ms);

bool connected();
bool configured();
int rssi();

/// The BOOT-tap portal under the Bluetooth door (see setup_portal.h).
void open_tap_portal();
void close_tap_portal();
bool tap_portal_open();

/// Facts for GET /api/status.
onboard::State state();
bool ap_up();
int saved_count();
time_sync::Source clock_source();
bool ever_online();
/// The portal's own view: a phone handed credentials this AP session.
bool credentials_submitted();

/// The network on the air (or being tried): the ring slot's SSID and
/// password, for the household fleet key (fleet_key.h). False when none.
bool current_network(const char** ssid, const char** pass);

/// The credentials half of a factory reset: the ring, the legacy pair (via
/// persist_current("", "")), the setup AP password. Returns whether the
/// store verifiably let go. The caller reboots.
bool forget_all();

}  // namespace wifi_sup
}  // namespace net
}  // namespace canary
