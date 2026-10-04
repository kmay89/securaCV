// canary-local/emulator/test/runtime_turn/lv_conf.h — the display's lv_conf.h
// for runtime_turn.sh's native full boot (F222), with a larger builtin pool.
// A 64-bit build's LVGL objects are larger than the boards' (and the wasm32
// dist's), so the headroom the 64 KiB pool leaves on the glass is not this
// build's: the nightlight boots in it today, but a pool this build exhausts
// halts LVGL in LV_ASSERT_MALLOC's endless loop, which would hang the test
// instead of failing it. Nothing else differs: every other key is the
// display's own, read from the file the firmware builds with.
#pragma once
#include "../../../../firmware/projects/canary-display/include/lv_conf.h"
#undef LV_MEM_SIZE
#define LV_MEM_SIZE (512U * 1024U)
