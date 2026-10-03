// canary-local/emulator/test/runtime_turn/emscripten.h — host stand-in for
// <emscripten.h> in runtime_turn.sh's native full boot (F222). Every EM_JS
// (a call into the page) becomes a plain C declaration that
// runtime_turn_test.cpp defines with the same signature, so the driver plays
// the page; emscripten_sleep and emscripten_get_now are the driver's virtual
// clock. (glass_turn.sh's stand-in, test/native/emscripten.h, only counts its
// EM_JS calls; this one hands the driver their arguments.)
#pragma once

#define EMSCRIPTEN_KEEPALIVE

#ifdef __cplusplus
extern "C" {
#endif
void emscripten_sleep(unsigned int ms);
double emscripten_get_now(void);
#ifdef __cplusplus
}
#define EM_JS(ret, name, params, ...) extern "C" ret name params;
#else
#define EM_JS(ret, name, params, ...) ret name params;
#endif
