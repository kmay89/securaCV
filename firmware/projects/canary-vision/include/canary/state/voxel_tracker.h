#pragma once
#include "canary/types.h"

namespace canary::state {

class VoxelTracker {
public:
  void reset();
  void update(const Voxel& v, uint32_t now_ms);

  Voxel stable() const { return stable_; }
  uint32_t stable_enter_ms() const { return stable_enter_ms_; }

private:
  Voxel cur_ = Voxel::Invalid();
  Voxel stable_ = Voxel::Invalid();
  uint8_t stable_frames_ = 0;
  uint32_t stable_enter_ms_ = 0;
};

} // namespace canary::state
