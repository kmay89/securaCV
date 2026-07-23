// Silicon boundary for the small Canary Vision wasm core.
//
// The firmware's detect_config.cpp is compiled verbatim. This file supplies
// only its Arduino NVS/Serial boundary: an in-memory Preferences map and a
// silent diagnostic console. The detection pipeline and state machine remain
// the real firmware sources.

#include <map>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Preferences.h"

namespace {
std::map<std::string, std::vector<uint8_t>>& nvs() {
  static std::map<std::string, std::vector<uint8_t>> values;
  return values;
}

std::string nvs_key(const char* name_space, const char* key) {
  return std::string(name_space ? name_space : "") + "/" + (key ? key : "");
}

bool get_raw(const char* name_space, const char* key, void* out, size_t size) {
  const auto found = nvs().find(nvs_key(name_space, key));
  if (found == nvs().end() || found->second.size() != size) return false;
  memcpy(out, found->second.data(), size);
  return true;
}

void put_raw(const char* name_space, const char* key, const void* value, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(value);
  nvs()[nvs_key(name_space, key)] = std::vector<uint8_t>(bytes, bytes + size);
}
}  // namespace

EmuSerial Serial;
void EmuSerial::write_str(const char*) {}

bool Preferences::begin(const char* name_space, bool) {
  strncpy(ns_, name_space ? name_space : "", sizeof(ns_) - 1);
  ns_[sizeof(ns_) - 1] = '\0';
  return true;
}

void Preferences::end() { ns_[0] = '\0'; }

uint8_t Preferences::getUChar(const char* key, uint8_t def) {
  uint8_t value = 0;
  return get_raw(ns_, key, &value, sizeof(value)) ? value : def;
}

size_t Preferences::putUChar(const char* key, uint8_t value) {
  put_raw(ns_, key, &value, sizeof(value));
  return sizeof(value);
}

uint32_t Preferences::getUInt(const char* key, uint32_t def) {
  uint32_t value = 0;
  return get_raw(ns_, key, &value, sizeof(value)) ? value : def;
}

size_t Preferences::putUInt(const char* key, uint32_t value) {
  put_raw(ns_, key, &value, sizeof(value));
  return sizeof(value);
}
