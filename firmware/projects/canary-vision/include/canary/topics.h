#pragma once
#include <stdio.h>
#include "canary/config.h"

struct Topics {
  char events[96];
  char state[96];
  char status[96];
  // Firmware update entity (signed pull-OTA) — same topic schema as the
  // other Canary variants: retained state, HA writes "install" / ON / OFF.
  char update_state[96];
  char update_cmd[96];
  char update_auto[96];
  char update_auto_cmd[96];
};

// device_id comes from canary::cfg::get() — NVS-backed, so topics stay
// stable across OTA installs of generic release builds.
static inline Topics build_topics(const char* device_id) {
  Topics t{};
  snprintf(t.events, sizeof(t.events), "securacv/%s/events", device_id);
  snprintf(t.state,  sizeof(t.state),  "securacv/%s/state",  device_id);
  snprintf(t.status, sizeof(t.status), "securacv/%s/status", device_id);
  snprintf(t.update_state,    sizeof(t.update_state),    "securacv/%s/update/state",    device_id);
  snprintf(t.update_cmd,      sizeof(t.update_cmd),      "securacv/%s/update/cmd",      device_id);
  snprintf(t.update_auto,     sizeof(t.update_auto),     "securacv/%s/update/auto",     device_id);
  snprintf(t.update_auto_cmd, sizeof(t.update_auto_cmd), "securacv/%s/update/auto/cmd", device_id);
  return t;
}
