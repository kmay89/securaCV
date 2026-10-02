// tests_host/fake_lvgl/identity/device_pseudonym.h — the one call splash.cpp
// makes into firmware/common/identity/device_pseudonym.h: device_id_hex(),
// the per-device pseudonym the first meeting's script names. The real one
// derives it from a salt in NVS (its host branch wants OpenSSL); here a test
// picks it (fake_pseudonym::id()), so the bubble can be read with the
// pseudonym that wraps the worst. HEX_LEN is the real header's,
// TOKEN_BYTES * 2: test_splash_scenes.cpp reads that header and holds this
// number to it.
#pragma once
#include <stddef.h>
#include <string.h>

#include <string>

namespace device_pseudonym {
constexpr size_t HEX_LEN = 16;
}  // namespace device_pseudonym

namespace fake_pseudonym {
inline std::string& id() {
  static std::string s(device_pseudonym::HEX_LEN, '2');
  return s;
}
}  // namespace fake_pseudonym

namespace device_pseudonym {
inline bool device_id_hex(char* out_hex, size_t out_len) {
  const std::string& s = fake_pseudonym::id();
  if (out_hex == nullptr || out_len < HEX_LEN + 1 || s.size() != HEX_LEN)
    return false;
  memcpy(out_hex, s.c_str(), HEX_LEN + 1);
  return true;
}
}  // namespace device_pseudonym
