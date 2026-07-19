// canary-local/emulator/src/emu_bindings.cpp — the scenario console.
//
// Exported knobs the page uses to stage teaching moments: virtual-time
// scale (watch staleness deadlines and the bird's slow feelings without
// waiting an hour), NVS preseed/wipe (provisioned device vs true first
// boot), deterministic RNG for reproducible screenshots. Link-state and
// MQTT injection live next to their shims (emu_net.cpp, emu_mqtt.cpp).
#include <emscripten.h>
#include <stdint.h>

#include "emu_bus.h"

extern "C" {

EMSCRIPTEN_KEEPALIVE void emu_time_scale(double scale) {
  emu_clock_set_scale(scale);
}
EMSCRIPTEN_KEEPALIVE double emu_time_scale_get(void) {
  return emu_clock_get_scale();
}
EMSCRIPTEN_KEEPALIVE void emu_time_step_ms(double ms) { emu_clock_step(ms); }
EMSCRIPTEN_KEEPALIVE void emu_epoch_offset(double s) {
  emu_clock_set_epoch_offset(s);
}
EMSCRIPTEN_KEEPALIVE void emu_seed(unsigned int seed) { emu_rng_seed(seed); }

// Hex transport keeps arbitrary bytes intact across the JS string
// boundary (uint32 NVS values legitimately contain NULs).
EMSCRIPTEN_KEEPALIVE void emu_nvs_preseed_hex(const char* ns, const char* key,
                                              const char* hexstr) {
  uint8_t buf[256];
  int n = 0;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (int i = 0; hexstr[i] && hexstr[i + 1] && n < (int)sizeof(buf); i += 2) {
    const int hi = nib(hexstr[i]), lo = nib(hexstr[i + 1]);
    if (hi < 0 || lo < 0) return;
    buf[n++] = (uint8_t)((hi << 4) | lo);
  }
  emu_nvs_put(ns, key, buf, n);
}
EMSCRIPTEN_KEEPALIVE void emu_nvs_reset(void) { emu_nvs_wipe(); }

}  // extern "C"
