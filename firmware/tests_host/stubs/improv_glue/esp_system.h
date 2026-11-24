#pragma once
// esp_system.h stand-in: IDF's reset reasons, 4.4.7's list (UNKNOWN through
// SDIO) for core 2 and 5.5.5's for core 3 (the same, then USB, JTAG, eFuse,
// power glitch and CPU lockup, in that order; the glue gates the USB, JTAG
// and glitch reasons it names at IDF 5.1),
// and the reset reason the test sets (a power-on unless it says otherwise:
// test_improv_glue.cpp's reset scenario sets each one it holds).
#include "esp_idf_version.h"

typedef enum {
  ESP_RST_UNKNOWN,
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
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
  ESP_RST_USB,
  ESP_RST_JTAG,
  ESP_RST_EFUSE,
  ESP_RST_PWR_GLITCH,
  ESP_RST_CPU_LOCKUP,
#endif
} esp_reset_reason_t;

inline esp_reset_reason_t g_esp_reset_reason = ESP_RST_POWERON;
inline esp_reset_reason_t esp_reset_reason(void) { return g_esp_reset_reason; }
