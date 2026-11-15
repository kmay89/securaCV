#pragma once
#include "canary/types.h"

namespace canary::vision {
  void init();
  bool ready();
  // 1 when the Grove Vision AI module answered at init() (the library's
  // begin() and then a fresh NAME? round trip), 0 when it did not. Never
  // the module's own id: no code of the firmware's own asks for it, and
  // what the SSCMA library's begin() asks for once (its I2C begin() is
  // `ID(false) && name(false)`) stays in the library's private buffer,
  // which nothing here reads (vision_mgr.cpp, sweep F384).
  // 1, not true, so a Flasher that gates on `module_id > 0` keeps working.
  int module_id();
  // The same verdict as a bool: the `module_ok` of the `j` proof record.
  bool module_ok();
  bool sample(VisionSample& out); // returns true if sample is valid
}
