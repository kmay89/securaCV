#pragma once
#include <stdio.h>

// The display's OWN topics (it is "a Canary that shows": it publishes a
// retained status heartbeat + LWT and a retained health row so fleet
// tooling sees it) plus the update-entity topics the shared OTA engine
// drives — same wire schema as every other Canary variant.
struct Topics {
  char status[96];
  char health[96];
  char update_state[96];
  char update_cmd[96];
  char update_auto[96];
  char update_auto_cmd[96];
};

// device_id comes from canary::cfg::get() — NVS-backed, so topics stay
// stable across OTA installs of generic release builds.
static inline Topics build_topics(const char* device_id) {
  Topics t{};
  snprintf(t.status, sizeof(t.status), "securacv/%s/status", device_id);
  snprintf(t.health, sizeof(t.health), "securacv/%s/health", device_id);
  snprintf(t.update_state,    sizeof(t.update_state),    "securacv/%s/update/state",    device_id);
  snprintf(t.update_cmd,      sizeof(t.update_cmd),      "securacv/%s/update/cmd",      device_id);
  snprintf(t.update_auto,     sizeof(t.update_auto),     "securacv/%s/update/auto",     device_id);
  snprintf(t.update_auto_cmd, sizeof(t.update_auto_cmd), "securacv/%s/update/auto/cmd", device_id);
  return t;
}

// Fleet subscription patterns — everything any Canary variant publishes
// that a status display can render. Single-level wildcard on device_id;
// the dispatcher drops the display's own echo.
//
//   status        retained heartbeat (+ LWT "offline") — both firmware families
//   availability  retained online/offline LWT — ACTIVE canary tree
//   health        retained battery/heap/fw + public_key (TOFU pin source)
//   events        non-retained witness events (signed on the SPECIALIZED trees)
//   tamper        retained {"state","confidence","kind"} — ACTIVE tree
//   chain         retained signed chain head {v,length,latest_hash,fp,sig}
//   state         retained per-variant snapshot (presence etc.)
struct FleetSubs {
  // Household ack-sync (trailblazer spec §2): one shared retained topic —
  // acknowledge on any display, every display agrees.
  static constexpr const char* FLEET_ACK    = "securacv/fleet/ack";

  static constexpr const char* STATUS       = "securacv/+/status";
  static constexpr const char* AVAILABILITY = "securacv/+/availability";
  static constexpr const char* HEALTH       = "securacv/+/health";
  static constexpr const char* EVENTS       = "securacv/+/events";
  static constexpr const char* TAMPER       = "securacv/+/tamper";
  static constexpr const char* CHAIN        = "securacv/+/chain";
  static constexpr const char* STATE        = "securacv/+/state";
  // Rooms & names (trailblazer spec §8): retained {"name","room"} published
  // by HA / the companion app / mosquitto_pub — the glass speaks "Kitchen".
  static constexpr const char* META         = "securacv/+/meta";
};
