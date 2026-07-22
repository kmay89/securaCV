#pragma once
#include "canary/types.h"

namespace canary::vision {
  void init();
  bool ready();
  int module_id();
  bool sample(VisionSample& out); // returns true if sample is valid
}
