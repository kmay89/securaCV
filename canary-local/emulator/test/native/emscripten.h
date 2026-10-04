// canary-local/emulator/test/native/emscripten.h — host stand-in for
// <emscripten.h>, so glass_turn_test.cpp compiles the emulator's REAL
// display HAL (src/emu_hal_display.cpp) with g++. Two macros: KEEPALIVE
// keeps nothing alive a hosted link would drop, and an EM_JS function (a
// call into the page) becomes a call to emu_native_em_js(name), which the
// test counts — the JS body has no meaning here. Every EM_JS the HAL
// declares returns void, which is all this stand-in can express.
// emscripten_sleep is declared (never defined) so the other emulator
// sources can be syntax-checked against this header too.
#pragma once

#define EMSCRIPTEN_KEEPALIVE

#ifdef __cplusplus
extern "C" {
#endif
void emu_native_em_js(const char* name);
void emscripten_sleep(unsigned int ms);
#ifdef __cplusplus
}
#endif

#define EM_JS(ret, name, params, ...) \
  ret name params { emu_native_em_js(#name); }
