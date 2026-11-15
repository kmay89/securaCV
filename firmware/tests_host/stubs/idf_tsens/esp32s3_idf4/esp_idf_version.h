/* ESP-IDF 4.4.7, the IDF under Arduino-ESP32 2.0.17 (the stock units' core),
 * as esp_idf_version.h states it, for test_camera_thermal.cpp. The provider
 * picks driver/temp_sensor.h on it. */
#ifndef STUB_IDF_TSENS_ESP_IDF_VERSION_IDF4_H
#define STUB_IDF_TSENS_ESP_IDF_VERSION_IDF4_H

#define ESP_IDF_VERSION_MAJOR 4
#define ESP_IDF_VERSION_MINOR 4
#define ESP_IDF_VERSION_PATCH 7
#define ESP_IDF_VERSION_VAL(major, minor, patch) ((major << 16) | (minor << 8) | (patch))
#define ESP_IDF_VERSION \
  ESP_IDF_VERSION_VAL(ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH)

#endif
