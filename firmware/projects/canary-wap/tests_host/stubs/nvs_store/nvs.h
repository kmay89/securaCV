/* IDF's NVS C API for test_nvs_store_lock.cpp, as nvs_store.h reaches it:
 * NvsSession's read-only open asks csi_module_settings_nvs.h's namespace
 * probe before Preferences::begin() (sweep F220). That suite drives
 * NvsManager, NvsMainSession and the nvs_store:: helpers, which never probe;
 * this answers from the same store as Preferences.h beside it all the same:
 * a read-only nvs_open() of a namespace holding no entry answers
 * ESP_ERR_NVS_NOT_FOUND (and logs nothing, as IDF's does not), any other
 * answers ESP_OK. Each call counts in g_host_nvs_probes. The suite that
 * counts what a first boot logs is test_wap_first_boot_nvs.cpp, over
 * stubs/first_boot. */
#ifndef STUB_NVS_STORE_NVS_H
#define STUB_NVS_STORE_NVS_H

#include <stdint.h>

#include <string>

#include "Preferences.h"

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_ERR_NVS_BASE
#define ESP_ERR_NVS_BASE 0x1100
#endif
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#endif

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

inline int g_host_nvs_probes = 0;

inline esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out) {
  ++g_host_nvs_probes;
  if (name == nullptr) return ESP_ERR_NVS_NOT_FOUND;
  if (mode == NVS_READONLY) {
    const std::string prefix = std::string(name) + "/";
    auto it = g_host_nvs.lower_bound(prefix);
    if (it == g_host_nvs.end() || it->first.compare(0, prefix.size(), prefix) != 0) {
      return ESP_ERR_NVS_NOT_FOUND;
    }
  }
  *out = 1;
  return ESP_OK;
}
inline void nvs_close(nvs_handle_t) {}

#endif
