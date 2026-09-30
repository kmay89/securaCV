/* ESP-IDF error codes health/boot_guard.h compares against (host fake). */
#ifndef STUB_BOOT_GUARD_ESP_ERR_H
#define STUB_BOOT_GUARD_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#endif
