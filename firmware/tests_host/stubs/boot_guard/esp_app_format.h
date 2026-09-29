/* The one field of esp_app_desc_t that health/boot_guard.h reads (host fake). */
#ifndef STUB_BOOT_GUARD_ESP_APP_FORMAT_H
#define STUB_BOOT_GUARD_ESP_APP_FORMAT_H
#include <stdint.h>
typedef struct {
  uint8_t app_elf_sha256[32];
} esp_app_desc_t;
#endif
