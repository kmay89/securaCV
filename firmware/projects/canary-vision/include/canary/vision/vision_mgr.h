#pragma once
#include "canary/types.h"

namespace canary::vision {
  void init();
  bool ready();
  // 1 when the Grove Vision AI module answered at init() (the library's
  // begin() and then a fresh NAME? round trip), 0 when it did not. Never
  // the module's own id: nothing in this firmware asks for it (sweep F384).
  // 1, not true, so a Flasher that gates on `module_id > 0` keeps working.
  int module_id();
  // The same verdict as a bool: the `module_ok` of the `j` proof record.
  bool module_ok();
  bool sample(VisionSample& out); // returns true if sample is valid
}
