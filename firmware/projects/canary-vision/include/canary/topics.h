#pragma once
#include <stdio.h>
#include "canary/config.h"

struct Topics {
  char events[96];
  char state[96];
  char status[96];
  // Witness surfaces (same schema as canary-sense): health carries
  // public_key for HA's TOFU pinning; chain is the retained signed head.
  char health[96];
  char chain[96];
  // Firmware update entity (signed pull-OTA) — same topic schema as the
  // other Canary variants: retained state, HA writes "install" / ON / OFF.
  char update_state[96];
  char update_cmd[96];
  char update_auto[96];
  char update_auto_cmd[96];
  // Runtime detection settings (NVS-backed; HA number entities write the
  // /set topics, the retained state mirrors canary::cfg::detect()).
  char cfg_state[96];
  char cfg_target_cmd[96];
  char cfg_score_cmd[96];
  char cfg_lost_cmd[96];
  char cfg_dwell_cmd[96];
  char cfg_profile_cmd[96];
  // Aim assist (bench/aiming): boxes-only live channel — non-retained,
  // off by default, auto-off. HA switch writes aim/set; aim/state is the
  // retained switch state; aim carries the per-frame box payload.
  char aim[96];
  char aim_state[96];
  char aim_cmd[96];
  // Identify (the wizard's "which device is which" moment — HAP-style):
  // HA's identify button / the companion app writes identify/set; the
  // non-retained echo mirrors the blink window so dashboards can pulse
  // the device card in sync with the physical LED.
  char identify_cmd[96];
  char identify_echo[96];
};

// device_id comes from canary::cfg::get() — NVS-backed, so topics stay
// stable across OTA installs of generic release builds.
static inline Topics build_topics(const char* device_id) {
  Topics t{};
  snprintf(t.events, sizeof(t.events), "securacv/%s/events", device_id);
  snprintf(t.state,  sizeof(t.state),  "securacv/%s/state",  device_id);
  snprintf(t.status, sizeof(t.status), "securacv/%s/status", device_id);
  snprintf(t.health, sizeof(t.health), "securacv/%s/health", device_id);
  snprintf(t.chain,  sizeof(t.chain),  "securacv/%s/chain",  device_id);
  snprintf(t.update_state,    sizeof(t.update_state),    "securacv/%s/update/state",    device_id);
  snprintf(t.update_cmd,      sizeof(t.update_cmd),      "securacv/%s/update/cmd",      device_id);
  snprintf(t.update_auto,     sizeof(t.update_auto),     "securacv/%s/update/auto",     device_id);
  snprintf(t.update_auto_cmd, sizeof(t.update_auto_cmd), "securacv/%s/update/auto/cmd", device_id);
  snprintf(t.aim,       sizeof(t.aim),       "securacv/%s/aim",       device_id);
  snprintf(t.aim_state, sizeof(t.aim_state), "securacv/%s/aim/state", device_id);
  snprintf(t.aim_cmd,   sizeof(t.aim_cmd),   "securacv/%s/aim/set",   device_id);
  snprintf(t.cfg_state,      sizeof(t.cfg_state),      "securacv/%s/cfg/state",      device_id);
  snprintf(t.cfg_target_cmd, sizeof(t.cfg_target_cmd), "securacv/%s/cfg/target/set", device_id);
  snprintf(t.cfg_score_cmd,  sizeof(t.cfg_score_cmd),  "securacv/%s/cfg/score/set",  device_id);
  snprintf(t.cfg_lost_cmd,   sizeof(t.cfg_lost_cmd),   "securacv/%s/cfg/lost/set",   device_id);
  snprintf(t.cfg_dwell_cmd,  sizeof(t.cfg_dwell_cmd),  "securacv/%s/cfg/dwell/set",  device_id);
  snprintf(t.cfg_profile_cmd, sizeof(t.cfg_profile_cmd), "securacv/%s/cfg/profile/set", device_id);
  snprintf(t.identify_cmd,  sizeof(t.identify_cmd),  "securacv/%s/identify/set", device_id);
  snprintf(t.identify_echo, sizeof(t.identify_echo), "securacv/%s/identify",     device_id);
  return t;
}
