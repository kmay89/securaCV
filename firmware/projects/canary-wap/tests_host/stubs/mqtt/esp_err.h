/* The ESP-IDF error type and the codes the MQTT bridge host build's NVS
 * (nvs.h beside this file) answers with; esp_http_server.h beside it
 * defines the same ones. */
#ifndef STUB_MQTT_ESP_ERR_H
#define STUB_MQTT_ESP_ERR_H
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_FAIL -1
#endif
#endif
