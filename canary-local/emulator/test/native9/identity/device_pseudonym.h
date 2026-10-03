// canary-local/emulator/test/native9/identity/device_pseudonym.h — the one
// call splash.cpp makes into firmware/common/identity/device_pseudonym.h
// (F246): device_id_hex(), the pseudonym the first meeting's script names.
// The real one derives it from a salt in NVS through mbedTLS; here it is
// HEX_LEN '2's (test_splash_scenes' default pseudonym). HEX_LEN is the real
// header's TOKEN_BYTES * 2, which glass_turn_lvgl9.sh holds before it builds.
#pragma once
#include <stddef.h>
#include <string.h>

namespace device_pseudonym {
constexpr size_t TOKEN_BYTES = 8;
constexpr size_t HEX_LEN = TOKEN_BYTES * 2;
inline bool device_id_hex(char* out_hex, size_t out_len) {
  if (out_hex == nullptr || out_len < HEX_LEN + 1) return false;
  memset(out_hex, '2', HEX_LEN);
  out_hex[HEX_LEN] = '\0';
  return true;
}
}  // namespace device_pseudonym
