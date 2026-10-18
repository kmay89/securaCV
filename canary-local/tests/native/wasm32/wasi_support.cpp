// canary-local/tests/native/wasm32/wasi_support.cpp — linked into every
// wasm32-wasi build of a whole Lab core by ../wasi.js (sweep A58), beside the
// core's own sources. It adds two things and nothing the core calls:
//
// - __cxa_allocate_exception and __cxa_throw, which abort. Ubuntu's
//   libc++abi for wasm32-wasi is built without exception support, yet its
//   operator new still names both (to throw std::bad_alloc), so a core that
//   allocates (the Vision shim's in-memory Preferences map) would not link.
//   The cores build with -fno-exceptions, as build.sh's em++ calls do, and
//   emscripten with exceptions off aborts where a throw would be; so do
//   these. Weak, so a sysroot whose C++ runtime defines them keeps its own.
// - lab_wasm32_long_bits(): this build's sizeof(long) in bits, so
//   cores_wasm32.test.js reads the 32 from the module rather than assume it.

#include <stddef.h>
#include <stdlib.h>

extern "C" {

__attribute__((weak)) void* __cxa_allocate_exception(size_t) { abort(); }

__attribute__((weak)) void __cxa_throw(void*, void*, void (*)(void*)) { abort(); }

__attribute__((used, export_name("lab_wasm32_long_bits"))) int lab_wasm32_long_bits(void) {
  return (int)(sizeof(long) * 8);
}

}  // extern "C"
