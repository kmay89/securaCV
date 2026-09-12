/* Host stand-in for <esp_err.h> — see README.md. Values match ESP-IDF. */
#ifndef SECURACV_CSI_HOST_STUB_ESP_ERR_H
#define SECURACV_CSI_HOST_STUB_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK                   0
#define ESP_FAIL                 (-1)
#define ESP_ERR_WIFI_BASE        0x3000
#define ESP_ERR_WIFI_NOT_INIT    (ESP_ERR_WIFI_BASE + 1)
#define ESP_ERR_WIFI_NOT_STARTED (ESP_ERR_WIFI_BASE + 2)
#define ESP_ERR_WIFI_NOT_CONNECT (ESP_ERR_WIFI_BASE + 15)

#endif /* SECURACV_CSI_HOST_STUB_ESP_ERR_H */
