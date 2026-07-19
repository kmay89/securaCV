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
#include "canary/ui/character.h"
#include "canary/glass_settings.h"

extern "C" {

// ── Character knobs (the lab's style rail) ──────────────────────────────
// Same three moves the on-glass picker makes (settings_ui.cpp): mutate the
// setting, mark it dirty (the debounced committer persists it, so an
// emulated reboot keeps the choice), apply the look. The render tick's
// ground-flip rebuild then repaints the live face — the page never touches
// pixels, it turns the same knob a finger would. Names/captions/ring order
// are read back from the firmware table so the page can never drift from
// the truth it demos.
EMSCRIPTEN_KEEPALIVE void emu_apply_character(int n) {
  canary::glass::settings_mut().character = (uint8_t)n;
  canary::glass::settings_mark_dirty();
  canary::ui::character_apply((canary::ui::Character)n);
}
EMSCRIPTEN_KEEPALIVE int emu_character_count(void) {
  return (int)canary::ui::character_count();
}
EMSCRIPTEN_KEEPALIVE int emu_character_active(void) {
  return (int)canary::ui::active_character();
}
EMSCRIPTEN_KEEPALIVE int emu_character_at_ring(int pos) {
  return (int)canary::ui::character_at_ring((uint8_t)pos);
}
EMSCRIPTEN_KEEPALIVE const char* emu_character_name(int n) {
  return canary::ui::character_name((canary::ui::Character)n);
}
EMSCRIPTEN_KEEPALIVE const char* emu_character_caption(int n) {
  return canary::ui::character_caption((canary::ui::Character)n);
}

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
