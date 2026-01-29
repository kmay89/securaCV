#pragma once
#include "canary/types.h"

namespace canary::state {

class VoxelTracker {
public:
  VoxelTracker()
    : cur_(Voxel{255, 255, 0, 0}),
      stable_(Voxel{255, 255, 0, 0}),
      stable_frames_(0),
      stable_enter_ms_(0) {}

  void reset();
  void update(const Voxel& v, uint32_t now_ms);

  Voxel stable() const { return stable_; }
  uint32_t stable_enter_ms() const { return stable_enter_ms_; }

private:
  Voxel cur_;
  Voxel stable_;
  uint8_t stable_frames_;
  uint32_t stable_enter_ms_;
};

} // namespace canary::state
