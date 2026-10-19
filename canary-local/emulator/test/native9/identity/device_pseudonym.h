// canary-local/emulator/test/native9/identity/device_pseudonym.h — the one
// call splash.cpp makes into firmware/common/identity/device_pseudonym.h
// (F246): device_id_hex(), the pseudonym the first meeting's script names.
// The real one derives it from a salt in NVS through mbedTLS; here it is
// HEX_LEN copies of one letter: '2' (test_splash_scenes' default pseudonym)
// unless the test picks another with fill() (glass_splash_test.cpp, the LVGL
// 8.4 render, also plays the wide-lettered 'W', F159). HEX_LEN is the real
// header's TOKEN_BYTES * 2, which glass_turn_lvgl9.sh and glass_turn.sh hold
// before they build.
#pragma once
#include <stddef.h>
#include <string.h>

namespace device_pseudonym {
constexpr size_t TOKEN_BYTES = 8;
constexpr size_t HEX_LEN = TOKEN_BYTES * 2;
// The letter the pseudonym repeats.
inline char& fill() {
  static char c = '2';
  return c;
}
inline bool device_id_hex(char* out_hex, size_t out_len) {
  if (out_hex == nullptr || out_len < HEX_LEN + 1) return false;
  memset(out_hex, fill(), HEX_LEN);
  out_hex[HEX_LEN] = '\0';
  return true;
}
}  // namespace device_pseudonym
