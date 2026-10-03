// canary-local/emulator/test/runtime_turn/lv_conf.h — the display's lv_conf.h
// for runtime_turn.sh's native full boot (F222), with a larger builtin pool.
// A 64-bit build's LVGL objects are about twice a 32-bit build's, and the
// nightlight's face with the splash behind it outgrows the 64 KiB pool the
// boards (and the wasm32 dist) fit in. Nothing else differs: every other key
// is the display's own, read from the file the firmware builds with.
#pragma once
#include "../../../../firmware/projects/canary-display/include/lv_conf.h"
#undef LV_MEM_SIZE
#define LV_MEM_SIZE (512U * 1024U)
