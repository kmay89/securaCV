// canary-local/emulator/src/emu_main.cpp — the power button.
//
// The firmware's own setup()/loop() (src/main.cpp, compiled verbatim
// into this module) run exactly as on silicon: main() waits for the page
// to press power, then boots once and loops forever. delay() yields to
// the browser through Asyncify, so the splash storyboard's blocking
// pump(), the boot sequence's pacing, and loop()'s 5 ms breather all
// behave like the bench — just visible.
#include <emscripten.h>

extern void setup();
extern void loop();

namespace {
volatile int g_power = 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE void emu_power_on(void) { g_power = 1; }

int main() {
  while (!g_power) emscripten_sleep(30);
  setup();
  for (;;) loop();
}
