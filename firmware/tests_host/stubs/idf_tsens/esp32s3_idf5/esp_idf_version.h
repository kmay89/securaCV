/* ESP-IDF 5.5.4, the IDF under the pioarduino core 3 line ([env:full]), as
 * esp_idf_version.h states it, for test_camera_thermal.cpp. The provider
 * picks driver/temperature_sensor.h on it. */
#ifndef STUB_IDF_TSENS_ESP_IDF_VERSION_IDF5_H
#define STUB_IDF_TSENS_ESP_IDF_VERSION_IDF5_H

#define ESP_IDF_VERSION_MAJOR 5
#define ESP_IDF_VERSION_MINOR 5
#define ESP_IDF_VERSION_PATCH 4
#define ESP_IDF_VERSION_VAL(major, minor, patch) ((major << 16) | (minor << 8) | (patch))
#define ESP_IDF_VERSION \
  ESP_IDF_VERSION_VAL(ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH)

#endif
