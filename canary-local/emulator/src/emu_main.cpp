// canary-local/emulator/src/emu_main.cpp — the power button.
//
// The firmware's own setup()/loop() (src/main.cpp, compiled verbatim
// into this module) run exactly as on silicon: main() waits for the page
// to press power, then boots once and loops forever. delay() yields to
// the browser through Asyncify, so the splash storyboard's blocking
// pump(), the boot sequence's pacing, and loop()'s 5 ms breather all
// behave like the bench — just visible.
#include <emscripten.h>
#include <Arduino.h>

#include <config.h>  // CD_NIGHTLIGHT: where this flavor keeps its rotation
#include "canary/glass_settings.h"
#ifdef CD_NIGHTLIGHT
#include "canary/care/nightlight_glue.h"
#endif

extern void setup();
extern void loop();

namespace {
volatile int g_power = 0;
int g_saved_rotation = -1;  // a saved rotation staged before power-on (see save_rotation)

// Store a saved rotation the way the glass's own Settings sheet does: the
// settings store loaded from whatever flash the page staged, the rotation
// changed, the blob committed (settings_loop past its debounce). setup()
// then reads it back in settings_init() and main.cpp wears it before the
// splash — lvgl_port_set_rotation() on the dash glass — like any unit that
// saved it (F184). Runs after every preseed and restored image, before the
// firmware's first line.
//
// The nightlight keeps its rotation in its own namespace instead (scv-nl,
// key "rot": care/nightlight.cpp), written by the one apply path a turn of
// the unit takes (nightlight_set_rotation), so this stores it there, the
// same way: its prefs loaded from the staged flash, the rotation set. setup()
// loads them again (nightlight_begin) and wears the rotation before the
// splash — display_set_rotation() and lvgl_port_set_panel_rotation() (F204).
void save_rotation(int rot) {
#ifdef CD_NIGHTLIGHT
  canary::care::nightlight_begin();
  canary::care::nightlight_set_rotation((uint8_t)rot);
#else
  canary::glass::settings_init();
  canary::glass::settings_mut().rotation = (uint8_t)rot;
  canary::glass::settings_mark_dirty();
  canary::glass::settings_loop(millis() + 60000u);
#endif
}
}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void emu_power_on(void) { g_power = 1; }

// The bench's mounting bracket (F184): stage a saved rotation (0..3) for the
// boot that power-on starts. 1 when staged, 0 when refused (out of range, or
// after power-on: a running unit turns from its Settings sheet). The dash
// glass wears it as a canary::glass::Rotation (1 = portrait, 480x800); the
// nightlight as its own quarter turns (io/orientation.h's Orient, Arduino_GFX
// numbering: 1 = landscape, 320x180; F204). The other flavors' main.cpp
// ignores the setting, as their hardware does.
extern "C" EMSCRIPTEN_KEEPALIVE int emu_preset_rotation(int rot) {
  if (g_power || rot < 0 || rot > 3) return 0;
  g_saved_rotation = rot;
  return 1;
}

int main() {
  while (!g_power) emscripten_sleep(30);
  if (g_saved_rotation >= 0) save_rotation(g_saved_rotation);
  setup();
  for (;;) loop();
}
