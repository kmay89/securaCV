/* ESP-IDF 4.4's temperature-sensor driver (driver/temp_sensor.h, as
 * Arduino-ESP32 2.0.17 ships it for the ESP32-S3: tools/sdk/esp32s3/include/
 * driver/esp32s3/include/driver/temp_sensor.h), answered from
 * ../common/host_tsens.h, for test_camera_thermal.cpp. The types and the
 * four calls the provider makes keep the real header's shapes:
 * temp_sensor_set_config() takes the config by value, TSENS_CONFIG_DEFAULT()
 * is {TSENS_DAC_L2, 6}. The provider includes this header inside an
 * extern "C" block, as the real one is C; the fake's C++ is wrapped back. */
#ifndef STUB_IDF_TSENS_DRIVER_TEMP_SENSOR_H
#define STUB_IDF_TSENS_DRIVER_TEMP_SENSOR_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
  TSENS_DAC_L0 = 0,
  TSENS_DAC_L1,
  TSENS_DAC_L2,
  TSENS_DAC_L3,
  TSENS_DAC_L4,
  TSENS_DAC_MAX,
  TSENS_DAC_DEFAULT = TSENS_DAC_L2,
} temp_sensor_dac_offset_t;

typedef struct {
  temp_sensor_dac_offset_t dac_offset;
  uint8_t clk_div;
} temp_sensor_config_t;

#define TSENS_CONFIG_DEFAULT() { TSENS_DAC_L2, 6 }

#ifdef __cplusplus
extern "C++" {
#endif

#include "host_tsens.h"

inline esp_err_t temp_sensor_set_config(temp_sensor_config_t tsens) {
  HostTsens& t = host_tsens();
  t.installs++;
  t.dac_offset = (int)tsens.dac_offset;
  t.clk_div = (int)tsens.clk_div;
  if (t.install_fails) return ESP_FAIL;
  t.installed = true;
  return ESP_OK;
}

inline esp_err_t temp_sensor_start(void) {
  HostTsens& t = host_tsens();
  t.enables++;
  if (!t.installed) return ESP_ERR_INVALID_STATE;
  if (t.enable_fails) return ESP_FAIL;
  t.enabled = true;
  return ESP_OK;
}

inline esp_err_t temp_sensor_stop(void) {
  HostTsens& t = host_tsens();
  t.disables++;
  t.enabled = false;
  return ESP_OK;
}

inline esp_err_t temp_sensor_read_celsius(float* celsius) {
  HostTsens& t = host_tsens();
  t.reads++;
  if (celsius == nullptr) return ESP_ERR_INVALID_ARG;
  if (!t.enabled) return ESP_ERR_INVALID_STATE;
  if (t.read_fails) return ESP_FAIL;
  *celsius = t.celsius;
  return ESP_OK;
}

#ifdef __cplusplus
}
#endif

#endif
