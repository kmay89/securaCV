/* Host stand-in for IDF's NVS C API, as csi_module_settings_nvs.h's
 * namespace probe calls it (sweep F164: mesh_network.cpp's read-only opens
 * of "mesh" go through begin_read_only()), answered from the current
 * device's store (Preferences.h beside this file). A read-only nvs_open()
 * of a namespace the store does not hold answers ESP_ERR_NVS_NOT_FOUND and
 * logs nothing, as IDF's does not log that answer at error level (Arduino's
 * Preferences::begin() is what logs it); a read-write one creates it, as
 * IDF's does. host_sim::nvs_open_fails (NVS refusing every open) answers
 * ESP_ERR_NVS_NOT_INITIALIZED. Each call counts in host_sim::nvs_probes. */
#ifndef STUB_MESH_NET_NVS_H
#define STUB_MESH_NET_NVS_H

#include <stdint.h>

#include "Preferences.h"
#include "esp_now.h"   /* esp_err_t and ESP_OK, which this stub directory defines there */

#ifndef ESP_ERR_NVS_BASE
#define ESP_ERR_NVS_BASE 0x1100
#endif
#ifndef ESP_ERR_NVS_NOT_INITIALIZED
#define ESP_ERR_NVS_NOT_INITIALIZED (ESP_ERR_NVS_BASE + 0x01)
#endif
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#endif

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

inline esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out) {
  ++host_sim::nvs_probes;
  if (host_sim::nvs_open_fails || name == nullptr) return ESP_ERR_NVS_NOT_INITIALIZED;
  if (mode == NVS_READONLY && !host_sim::nvs_has_namespace(name)) return ESP_ERR_NVS_NOT_FOUND;
  if (mode == NVS_READWRITE) host_sim::nvs_create_namespace(name);
  *out = 1;
  return ESP_OK;
}
inline void nvs_close(nvs_handle_t) {}

#endif
