// canary-local/emulator/shim/esp_task_wdt.h — watchdog as a no-op.
// The browser tab cannot brown out; the arm/reset calls compile away.
#pragma once
#include <stdint.h>

#define ESP_OK 0
typedef int esp_err_t;

static inline esp_err_t esp_task_wdt_init(uint32_t, bool) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_add(void*) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
