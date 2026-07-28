#pragma once
#include <stdio.h>

struct Topics {
  char events[96];
  char state[96];
  char status[96];
  // Witness trust surface — same wire schema as canary-wap: HA TOFU-pins
  // the pubkey from the retained health payload and verifies the signed
  // chain head with its existing verify_chain path.
  char chain[96];
  char health[96];
  // Firmware update entity (signed pull-OTA) — same topic schema as the
  // other Canary variants: retained state, HA writes "install" / ON / OFF.
  char update_state[96];
  char update_cmd[96];
  char update_auto[96];
  char update_auto_cmd[96];
  // Identify (the wizard's "which device is which" moment — HAP-style):
  // HA's identify button / the companion app writes identify/set; the
  // non-retained echo mirrors the blink window so dashboards can pulse
  // the device card in sync with the physical LED.
  char identify_cmd[96];
  char identify_echo[96];
  // Runtime radar reflexes (sense_config): retained snapshot + one command
  // topic per knob — the same cfg/* schema canary-vision's dials speak, so
  // HA blueprints and the flasher treat both families identically.
  char cfg_state[96];
  char cfg_debounce_cmd[96];
  char cfg_clear_cmd[96];
  char cfg_stall_cmd[96];
  char cfg_near_cmd[96];
  char cfg_mid_cmd[96];
  char cfg_vlock_cmd[96];
  char cfg_vlost_cmd[96];
  char cfg_bmin_cmd[96];
  char cfg_bmax_cmd[96];
  char cfg_hmin_cmd[96];
  char cfg_hmax_cmd[96];
};

// device_id comes from canary::cfg::get() — NVS-backed, so topics stay
// stable across OTA installs of generic release builds.
static inline Topics build_topics(const char* device_id) {
  Topics t{};
  snprintf(t.events, sizeof(t.events), "securacv/%s/events", device_id);
  snprintf(t.state,  sizeof(t.state),  "securacv/%s/state",  device_id);
  snprintf(t.status, sizeof(t.status), "securacv/%s/status", device_id);
  snprintf(t.chain,  sizeof(t.chain),  "securacv/%s/chain",  device_id);
  snprintf(t.health, sizeof(t.health), "securacv/%s/health", device_id);
  snprintf(t.update_state,    sizeof(t.update_state),    "securacv/%s/update/state",    device_id);
  snprintf(t.update_cmd,      sizeof(t.update_cmd),      "securacv/%s/update/cmd",      device_id);
  snprintf(t.update_auto,     sizeof(t.update_auto),     "securacv/%s/update/auto",     device_id);
  snprintf(t.update_auto_cmd, sizeof(t.update_auto_cmd), "securacv/%s/update/auto/cmd", device_id);
  snprintf(t.identify_cmd,  sizeof(t.identify_cmd),  "securacv/%s/identify/set", device_id);
  snprintf(t.identify_echo, sizeof(t.identify_echo), "securacv/%s/identify",     device_id);
  snprintf(t.cfg_state,        sizeof(t.cfg_state),        "securacv/%s/cfg/state",           device_id);
  snprintf(t.cfg_debounce_cmd, sizeof(t.cfg_debounce_cmd), "securacv/%s/cfg/debounce/set",    device_id);
  snprintf(t.cfg_clear_cmd,    sizeof(t.cfg_clear_cmd),    "securacv/%s/cfg/clear/set",       device_id);
  snprintf(t.cfg_stall_cmd,    sizeof(t.cfg_stall_cmd),    "securacv/%s/cfg/stall/set",       device_id);
  snprintf(t.cfg_near_cmd,     sizeof(t.cfg_near_cmd),     "securacv/%s/cfg/near/set",        device_id);
  snprintf(t.cfg_mid_cmd,      sizeof(t.cfg_mid_cmd),      "securacv/%s/cfg/mid/set",         device_id);
  snprintf(t.cfg_vlock_cmd,    sizeof(t.cfg_vlock_cmd),    "securacv/%s/cfg/vitals_lock/set", device_id);
  snprintf(t.cfg_vlost_cmd,    sizeof(t.cfg_vlost_cmd),    "securacv/%s/cfg/vitals_lost/set", device_id);
  snprintf(t.cfg_bmin_cmd,     sizeof(t.cfg_bmin_cmd),     "securacv/%s/cfg/breath_min/set",  device_id);
  snprintf(t.cfg_bmax_cmd,     sizeof(t.cfg_bmax_cmd),     "securacv/%s/cfg/breath_max/set",  device_id);
  snprintf(t.cfg_hmin_cmd,     sizeof(t.cfg_hmin_cmd),     "securacv/%s/cfg/heart_min/set",   device_id);
  snprintf(t.cfg_hmax_cmd,     sizeof(t.cfg_hmax_cmd),     "securacv/%s/cfg/heart_max/set",   device_id);
  return t;
}
