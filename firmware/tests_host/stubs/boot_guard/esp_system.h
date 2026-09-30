/* esp_reset_reason() for health/boot_guard.h (host fake). The enum is the
 * ESP-IDF 4.4 set (Arduino core 2.0.17), so the glue cannot lean on a reason
 * the pinned core does not have. The test sets g_fake_reset_reason. */
#ifndef STUB_BOOT_GUARD_ESP_SYSTEM_H
#define STUB_BOOT_GUARD_ESP_SYSTEM_H
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
} esp_reset_reason_t;
static esp_reset_reason_t g_fake_reset_reason = ESP_RST_PANIC;  // one TU per test binary
inline esp_reset_reason_t esp_reset_reason() { return g_fake_reset_reason; }
#endif
