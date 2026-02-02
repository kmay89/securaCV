#pragma once
#include "canary/types.h"
#include "canary/state/voxel_tracker.h"

namespace canary::state {

class PresenceFSM {
public:
  void reset();
  // returns true if an event was emitted
  bool tick(const VisionSample& vs, uint32_t now_ms, EventMsg& out_event);

  StateSnapshot snapshot(uint32_t now_ms, const char* last_event) const;

private:
  // state
  bool presence_=false;
  bool dwelling_=false;

  uint32_t presence_start_ms_=0;
  uint32_t last_seen_ms_=0;
  uint32_t dwell_start_ms_=0;
  uint32_t last_leave_ms_=0;

  bool interaction_candidate_=false;

  // interaction tracking
  bool dwell_latch_=false;
  bool interaction_latch_=false;
  bool interaction_emitted_=false;
  uint32_t last_leave_seen_=0;

  // current
  BBox bbox_{};
  int confidence_=0;

  VoxelTracker voxel_tracker_;
};

} // namespace
