/* ESP-IDF's error type and the codes the fake temperature-sensor drivers
 * answer (stubs/idf_tsens), for test_camera_thermal.cpp. The values are
 * IDF's own. */
#ifndef STUB_IDF_TSENS_ESP_ERR_H
#define STUB_IDF_TSENS_ESP_ERR_H

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

#endif
