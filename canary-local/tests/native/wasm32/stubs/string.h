// canary-local/tests/native/wasm32/stubs/string.h: the declaration
// canary/detect_profiles.h includes <string.h> for (its profile lookup by
// name), for the freestanding wasm32 build in ../../wasm32.js (sweep A49).
// Declared, never defined: the pipeline probe never calls it, and wasm-ld
// links with no libc and refuses an undefined symbol, so a pipeline change
// that started calling it would fail that build by name instead of linking
// against something that is not the firmware's.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int strcmp(const char* a, const char* b);
#ifdef __cplusplus
}
#endif
