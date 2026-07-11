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
  return t;
}
