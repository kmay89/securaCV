// Host stand-in for <emscripten.h>: the one macro the Vision core's browser
// ABI (canary-local/emulator/vision/vision_core_bindings.cpp) uses. Under
// em++ it keeps a symbol alive through dead-code elimination; a hosted link
// keeps every extern "C" function anyway.
#pragma once
#define EMSCRIPTEN_KEEPALIVE
