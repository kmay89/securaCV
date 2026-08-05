// canary-local/emulator/shim/esp_system.h — reset lineage for a browser tab.
// A wasm "boot" is always a fresh cold start: there is no RTC power domain
// to survive anything and no brownout detector to trip, so the reset reason
// is POWERON and the no-init attribute decays to a plain zero-initialized
// global (its marker therefore always reads "absent" — which classifies as
// the cold boot it truly is).
#pragma once
#include <stdint.h>

typedef enum {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON,
  ESP_RST_EXT,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
  ESP_RST_BROWNOUT,
  ESP_RST_SDIO,
} esp_reset_reason_t;

static inline esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }

#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#endif
