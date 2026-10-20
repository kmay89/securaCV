/* IDF's NVS C API, as csi_module_settings_nvs.h's namespace probe calls it
 * (sweeps F125, F201) and the sketch's key read asks for the stored key's
 * length (stored_key_length(), sweep F360), answered from the same store as
 * Preferences.h beside it. A read-only nvs_open() of a namespace NVS does not
 * hold answers ESP_ERR_NVS_NOT_FOUND and logs nothing, as IDF's does not log
 * that answer at error level (Arduino's Preferences::begin() is what logs
 * it). fail_begin (NVS refusing every open) answers
 * ESP_ERR_NVS_NOT_INITIALIZED. Each open counts in host_nvs().probes and gets
 * its own handle, which names its namespace until nvs_close();
 * idf_handles is the handles not yet closed. As IDF answers, nvs_get_blob()
 * of a key the namespace does not hold is ESP_ERR_NVS_NOT_FOUND, of a key
 * stored as another type ESP_ERR_NVS_TYPE_MISMATCH, and with a null buffer it
 * answers the length; length_fault makes it fail with that error instead. */
#ifndef STUB_FIRST_BOOT_NVS_H
#define STUB_FIRST_BOOT_NVS_H

#include <stdint.h>
#include <string.h>

#include "Preferences.h"
#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

inline esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out) {
  HostFirstBootNvs& n = host_nvs();
  n.probes++;
  if (n.fail_begin || name == nullptr) return ESP_ERR_NVS_NOT_INITIALIZED;
  if (mode == NVS_READONLY && !n.has_namespace(name)) return ESP_ERR_NVS_NOT_FOUND;
  if (mode == NVS_READWRITE) n.created.insert(name);
  const nvs_handle_t h = ++n.last_handle;
  n.handle_ns[h] = name;
  *out = h;
  return ESP_OK;
}

inline void nvs_close(nvs_handle_t h) { host_nvs().handle_ns.erase(h); }

inline esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len) {
  HostFirstBootNvs& n = host_nvs();
  auto open = n.handle_ns.find(h);
  if (open == n.handle_ns.end() || key == nullptr || len == nullptr) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  if (n.length_fault != ESP_OK) return n.length_fault;
  auto it = n.rows.find(open->second + "/" + key);
  if (it == n.rows.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (it->second.type != 'b') return ESP_ERR_NVS_TYPE_MISMATCH;
  if (out == nullptr) {
    *len = it->second.bytes.size();
    return ESP_OK;
  }
  if (*len < it->second.bytes.size()) return ESP_ERR_NVS_INVALID_LENGTH;
  memcpy(out, it->second.bytes.data(), it->second.bytes.size());
  *len = it->second.bytes.size();
  return ESP_OK;
}

#endif
