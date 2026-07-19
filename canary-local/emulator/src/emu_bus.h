// canary-local/emulator/src/emu_bus.h — internal wiring between the shim
// layer, the emulated HAL, and the scenario bindings. Not a firmware
// interface: firmware only ever sees the real canary/* headers.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Scenario link state (set by JS, read by the wifi/mqtt shims).
int emu_bus_wifi_up(void);
int emu_bus_wifi_rssi(void);
int emu_bus_broker_up(void);

// LEDC writes routed by channel (backlight vs chime) in emu_hal_display.
void emu_bus_ledc_write(uint8_t channel, uint32_t duty, uint32_t freq_hz,
                        uint8_t res_bits);

// NVS (scenario preseed / wipe) — defined in emu_support.cpp.
void emu_nvs_put(const char* ns, const char* key, const uint8_t* data, int len);
int emu_nvs_get(const char* ns, const char* key, uint8_t* out, int cap);
void emu_nvs_wipe(void);

// Virtual clock knobs — defined in emu_support.cpp.
void emu_clock_set_scale(double scale);
double emu_clock_get_scale(void);
void emu_clock_step(double ms);
void emu_clock_set_epoch_offset(double s);
void emu_rng_seed(uint32_t seed);

#ifdef __cplusplus
}
#endif
