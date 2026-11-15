/* ESP-IDF 5's temperature-sensor driver (driver/temperature_sensor.h,
 * components/esp_driver_tsens at v5.5.1), answered from
 * ../common/host_tsens.h, for test_camera_thermal.cpp. The handle, the
 * config and the five calls the provider makes keep the real header's
 * shapes; TEMPERATURE_SENSOR_CONFIG_DEFAULT(min, max) fills range_min,
 * range_max and clk_src (positionally here: the real macro's designators
 * are a C++20 extension clang warns on, and the firmware compiles it with
 * GCC). IDF allows one installed instance: a second install is
 * ESP_ERR_INVALID_STATE, which is why the provider exists. */
#ifndef STUB_IDF_TSENS_DRIVER_TEMPERATURE_SENSOR_H
#define STUB_IDF_TSENS_DRIVER_TEMPERATURE_SENSOR_H

#include <stdint.h>

#include "esp_err.h"
#include "host_tsens.h"

typedef enum {
  TEMPERATURE_SENSOR_CLK_SRC_DEFAULT = 0,
} temperature_sensor_clk_src_t;

struct temperature_sensor_obj_t {
  int id;
};
typedef struct temperature_sensor_obj_t* temperature_sensor_handle_t;

typedef struct {
  int range_min;
  int range_max;
  temperature_sensor_clk_src_t clk_src;
  struct {
    uint32_t allow_pd;
  } flags;
} temperature_sensor_config_t;

#define TEMPERATURE_SENSOR_CONFIG_DEFAULT(min, max) \
  { (min), (max), TEMPERATURE_SENSOR_CLK_SRC_DEFAULT, {0} }

inline temperature_sensor_obj_t g_host_tsens_obj{1};

inline esp_err_t temperature_sensor_install(const temperature_sensor_config_t* tsens_config,
                                            temperature_sensor_handle_t* ret_tsens) {
  HostTsens& t = host_tsens();
  t.installs++;
  if (tsens_config == nullptr || ret_tsens == nullptr) return ESP_ERR_INVALID_ARG;
  if (t.installed) return ESP_ERR_INVALID_STATE;
  t.range_min = tsens_config->range_min;
  t.range_max = tsens_config->range_max;
  if (t.install_fails) return ESP_FAIL;
  t.installed = true;
  *ret_tsens = &g_host_tsens_obj;
  return ESP_OK;
}

inline esp_err_t temperature_sensor_uninstall(temperature_sensor_handle_t tsens) {
  HostTsens& t = host_tsens();
  t.uninstalls++;
  if (tsens != &g_host_tsens_obj || !t.installed) return ESP_ERR_INVALID_ARG;
  t.installed = false;
  t.enabled = false;
  return ESP_OK;
}

inline esp_err_t temperature_sensor_enable(temperature_sensor_handle_t tsens) {
  HostTsens& t = host_tsens();
  t.enables++;
  if (tsens != &g_host_tsens_obj || !t.installed) return ESP_ERR_INVALID_ARG;
  if (t.enable_fails) return ESP_FAIL;
  t.enabled = true;
  return ESP_OK;
}

inline esp_err_t temperature_sensor_disable(temperature_sensor_handle_t tsens) {
  HostTsens& t = host_tsens();
  t.disables++;
  if (tsens != &g_host_tsens_obj || !t.installed) return ESP_ERR_INVALID_ARG;
  t.enabled = false;
  return ESP_OK;
}

inline esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t tsens,
                                                float* out_celsius) {
  HostTsens& t = host_tsens();
  t.reads++;
  if (tsens != &g_host_tsens_obj || out_celsius == nullptr) return ESP_ERR_INVALID_ARG;
  if (!t.enabled) return ESP_ERR_INVALID_STATE;
  if (t.read_fails) return ESP_FAIL;
  *out_celsius = t.celsius;
  return ESP_OK;
}

#endif
