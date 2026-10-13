// canary-local/emulator/test/native/emscripten.h — host stand-in for
// <emscripten.h>, so glass_turn_test.cpp compiles the emulator's REAL
// display HAL (src/emu_hal_display.cpp) with g++. Two macros: KEEPALIVE
// keeps nothing alive a hosted link would drop, and an EM_JS function (a
// call into the page) becomes a plain C declaration that
// glass_turn_test.cpp (and backlight_cap_test.cpp, which links the same HAL
// object) defines with the same signature, so the test plays
// the page and reads every argument the HAL hands it: js_display_ready's
// shape AND the quarter turns it carries (sweep A64; the JS body has no
// meaning here). A call the HAL adds without a definition in the test fails
// the link, naming it. runtime_turn/emscripten.h is the same stand-in for
// the full boot. emscripten_sleep is declared (never defined) so the other
// emulator sources can be syntax-checked against this header too.
#pragma once

#define EMSCRIPTEN_KEEPALIVE

#ifdef __cplusplus
extern "C" {
#endif
void emscripten_sleep(unsigned int ms);
#ifdef __cplusplus
}
#define EM_JS(ret, name, params, ...) extern "C" ret name params;
#else
#define EM_JS(ret, name, params, ...) ret name params;
#endif
