#include "canary/state/voxel_tracker.h"

namespace canary::state {

static constexpr uint8_t VOXEL_STABLE_N = 3;

void VoxelTracker::reset() {
  cur_ = Voxel{-1,-1,0,0};
  stable_ = Voxel{-1,-1,0,0};
  stable_frames_=0;
  stable_enter_ms_=0;
}

void VoxelTracker::update(const Voxel& v, uint32_t now_ms) {
  if (v.r != cur_.r || v.c != cur_.c) {
    cur_ = v;
  }

  if (stable_.r == -1 && stable_.c == -1) {
    stable_ = cur_;
    stable_frames_=0;
    stable_enter_ms_=now_ms;
    return;
  }

  if (cur_.r == stable_.r && cur_.c == stable_.c) {
    stable_frames_=0;
    return;
  }

  stable_frames_++;
  if (stable_frames_ >= VOXEL_STABLE_N) {
    stable_ = cur_;
    stable_frames_=0;
    stable_enter_ms_=now_ms;
  }
}

} // namespace
