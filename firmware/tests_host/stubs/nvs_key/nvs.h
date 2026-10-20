/* IDF's NVS C API for test_nvs_load_key.cpp (sweep F360), answered from the
 * store Preferences.h beside it keeps. As IDF answers: a read-only
 * nvs_open() of a namespace never created is ESP_ERR_NVS_NOT_FOUND (and logs
 * nothing), nvs_get_blob() of a key the namespace does not hold is
 * ESP_ERR_NVS_NOT_FOUND, and with a null buffer it answers the length. The
 * faults are Preferences.h's ro_open_fault and length_fault. Opens, closes
 * and the handles left open are counted; the key load writes nothing through
 * IDF, so a read-write open is counted apart. */
#ifndef STUB_NVS_KEY_NVS_H
#define STUB_NVS_KEY_NVS_H

#include <stdint.h>
#include <string.h>

#include "Preferences.h"
#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

inline esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out) {
  HostKeyNvs& n = host_key_nvs();
  n.idf_opens++;
  if (mode != NVS_READONLY) { n.idf_rw_opens++; return ESP_FAIL; }
  if (name == nullptr || n.ns != name) { n.wrong_namespace++; return ESP_ERR_NVS_NOT_FOUND; }
  if (n.ro_open_fault != ESP_OK) return n.ro_open_fault;
  if (!n.ns_exists) return ESP_ERR_NVS_NOT_FOUND;
  n.idf_open_handles++;
  *out = 1;
  return ESP_OK;
}

inline void nvs_close(nvs_handle_t) {
  HostKeyNvs& n = host_key_nvs();
  n.idf_closes++;
  n.idf_open_handles--;
}

inline esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len) {
  HostKeyNvs& n = host_key_nvs();
  if (h != 1 || n.idf_open_handles <= 0 || key == nullptr || len == nullptr)
    return ESP_ERR_NVS_INVALID_HANDLE;
  if (n.length_fault != ESP_OK) return n.length_fault;
  auto it = n.blobs.find(key);
  if (it == n.blobs.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (out == nullptr) {
    *len = it->second.size();
    return ESP_OK;
  }
  if (*len < it->second.size()) return ESP_ERR_NVS_INVALID_LENGTH;
  memcpy(out, it->second.data(), it->second.size());
  *len = it->second.size();
  return ESP_OK;
}

#endif
