#pragma once
#include <stdio.h>
#include "canary/config.h"

struct Topics {
  char events[96];
  char state[96];
  char status[96];
};

static inline Topics build_topics() {
  Topics t{};
  snprintf(t.events, sizeof(t.events), "securacv/%s/events", DEVICE_ID);
  snprintf(t.state,  sizeof(t.state),  "securacv/%s/state",  DEVICE_ID);
  snprintf(t.status, sizeof(t.status), "securacv/%s/status", DEVICE_ID);
  return t;
}
