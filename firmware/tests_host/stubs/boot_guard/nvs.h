/* Fake NVS for test_boot_guard.cpp. Keeps the semantics boot_guard.h leans
 * on: a namespace exists only once something was written to it (a read-only
 * open of a never-written namespace is ESP_ERR_NVS_NOT_FOUND), values persist
 * across "reboots" (the test simply calls begin() again), and the test can
 * make every open fail (NVS broken) and count writes (flash wear). */
#ifndef STUB_BOOT_GUARD_NVS_H
#define STUB_BOOT_GUARD_NVS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

struct FakeNvs {
  std::map<std::string, std::map<std::string, std::vector<uint8_t>>> ns;
  std::vector<std::string> handles;   // handle -> namespace (index + 1)
  bool broken = false;                // every open fails
  int writes = 0;                     // nvs_set_* calls that stored something
  void reset() { ns.clear(); handles.clear(); broken = false; writes = 0; }
};
static FakeNvs g_fake_nvs;  // one TU per test binary

inline esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out) {
  if (g_fake_nvs.broken) return ESP_ERR_NVS_NOT_INITIALIZED;
  if (mode == NVS_READONLY && g_fake_nvs.ns.find(name) == g_fake_nvs.ns.end())
    return ESP_ERR_NVS_NOT_FOUND;
  g_fake_nvs.ns[name];  // READWRITE creates the namespace
  g_fake_nvs.handles.push_back(name);
  *out = (nvs_handle_t)g_fake_nvs.handles.size();
  return ESP_OK;
}
inline void nvs_close(nvs_handle_t) {}
inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }

inline std::map<std::string, std::vector<uint8_t>>& fake_nvs_ns(nvs_handle_t h) {
  return g_fake_nvs.ns[g_fake_nvs.handles.at(h - 1)];
}

inline esp_err_t nvs_get_u16(nvs_handle_t h, const char* key, uint16_t* out) {
  auto& m = fake_nvs_ns(h);
  auto it = m.find(key);
  if (it == m.end() || it->second.size() != 2) return ESP_ERR_NVS_NOT_FOUND;
  *out = (uint16_t)(it->second[0] | (it->second[1] << 8));
  return ESP_OK;
}
inline esp_err_t nvs_set_u16(nvs_handle_t h, const char* key, uint16_t v) {
  fake_nvs_ns(h)[key] = {(uint8_t)(v & 0xFF), (uint8_t)(v >> 8)};
  g_fake_nvs.writes++;
  return ESP_OK;
}
inline esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len) {
  auto& m = fake_nvs_ns(h);
  auto it = m.find(key);
  if (it == m.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (*len < it->second.size()) return ESP_FAIL;
  memcpy(out, it->second.data(), it->second.size());
  *len = it->second.size();
  return ESP_OK;
}
inline esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* v, size_t len) {
  const uint8_t* p = (const uint8_t*)v;
  fake_nvs_ns(h)[key] = std::vector<uint8_t>(p, p + len);
  g_fake_nvs.writes++;
  return ESP_OK;
}

#endif
