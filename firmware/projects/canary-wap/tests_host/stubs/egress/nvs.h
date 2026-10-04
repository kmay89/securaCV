/* IDF's NVS C API, as csi_module_settings_nvs.h's namespace probe calls it
 * (sweeps F125, F150), answered from the same store as Preferences.h beside
 * it: a read-only nvs_open() of a namespace NVS does not hold answers
 * ESP_ERR_NVS_NOT_FOUND and logs nothing (Arduino's Preferences::begin() is
 * what logs it). Each call counts in probes. */
#ifndef STUB_EGRESS_NVS_H
#define STUB_EGRESS_NVS_H

#include <stdint.h>

#include "Preferences.h"
#include "esp_err.h"

#ifndef ESP_ERR_NVS_BASE
#define ESP_ERR_NVS_BASE 0x1100
#endif
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#endif

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

inline int& host_nvs_probes() {
  static int probes = 0;
  return probes;
}

inline esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out) {
  host_nvs_probes()++;
  if (name == nullptr) return ESP_FAIL;
  if (mode == NVS_READONLY && !host_nvs().has_namespace(name)) return ESP_ERR_NVS_NOT_FOUND;
  if (mode == NVS_READWRITE) host_nvs().created.insert(name);
  *out = 1;
  return ESP_OK;
}
inline void nvs_close(nvs_handle_t) {}

#endif
