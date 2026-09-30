/* Fake esp_ota_ops for test_boot_guard.cpp: one running partition whose OTA
 * state and app ELF hash the test sets through g_fake_ota. */
#ifndef STUB_BOOT_GUARD_ESP_OTA_OPS_H
#define STUB_BOOT_GUARD_ESP_OTA_OPS_H

#include <string.h>

#include "esp_app_format.h"
#include "esp_err.h"

typedef enum {
  ESP_OTA_IMG_NEW            = 0x0U,
  ESP_OTA_IMG_PENDING_VERIFY = 0x1U,
  ESP_OTA_IMG_VALID          = 0x2U,
  ESP_OTA_IMG_INVALID        = 0x3U,
  ESP_OTA_IMG_ABORTED        = 0x4U,
  ESP_OTA_IMG_UNDEFINED      = 0xFFFFFFFFU,
} esp_ota_img_states_t;

typedef struct {
  const char* label;
} esp_partition_t;

struct FakeOta {
  esp_partition_t running{"ota_0"};
  bool has_state = true;                       // false: not an OTA partition
  esp_ota_img_states_t state = ESP_OTA_IMG_VALID;
  bool desc_ok = true;                         // false: app descriptor unreadable
  uint8_t sha[32] = {0};
};
static FakeOta g_fake_ota;  // one TU per test binary

inline const esp_partition_t* esp_ota_get_running_partition() { return &g_fake_ota.running; }

inline esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* st) {
  if (!g_fake_ota.has_state) return ESP_FAIL;
  *st = g_fake_ota.state;
  return ESP_OK;
}

inline esp_err_t esp_ota_get_partition_description(const esp_partition_t*, esp_app_desc_t* d) {
  if (!g_fake_ota.desc_ok) return ESP_FAIL;
  memcpy(d->app_elf_sha256, g_fake_ota.sha, sizeof(d->app_elf_sha256));
  return ESP_OK;
}

#endif
