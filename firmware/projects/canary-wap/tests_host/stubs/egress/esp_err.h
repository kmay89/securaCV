/* The ESP-IDF error type and the codes the events-egress host build's NVS
 * (nvs.h beside this file) answers with. */
#ifndef STUB_EGRESS_ESP_ERR_H
#define STUB_EGRESS_ESP_ERR_H
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_FAIL -1
#endif
#endif
