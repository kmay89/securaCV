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
  uint32_t last_visit_ms_=0;  // duration of the last completed stay
  // Length of the dwell the latest tick ended, for dwell_ended's rows (sweep
  // F130). tick() clears dwelling_ before the snapshot publish_event_json
  // reads, so without it the dwell_ended row reported dwell_ms 0. Held until
  // the next tick, which zeroes it first.
  uint32_t ended_dwell_ms_=0;

  bool interaction_candidate_=false;

  // interaction tracking
  bool dwell_latch_=false;
  bool interaction_latch_=false;
  bool interaction_emitted_=false;
  uint32_t last_leave_seen_=0;
  // The reason of an ended visit's interaction_likely that is still owed
  // when the next visit starts on the frame right after presence_ended
  // (the one frame interaction_likely would have gone out on); sent on the
  // frame after that, inside the same window, then cleared. nullptr when
  // nothing is owed.
  const char* pending_interaction_reason_=nullptr;
  bool send_pending_interaction(uint32_t now_ms, EventMsg& out_event);

  // The visit boundaries (sweep F186): presence_started and presence_ended,
  // with what each resets or latches. open_visit seeds the new visit's
  // tracker with its first sighting (first_cell, seen at seen_ms).
  bool open_visit(const Voxel& first_cell, uint32_t seen_ms, uint32_t now_ms, EventMsg& out_event);
  bool end_visit(uint32_t now_ms, EventMsg& out_event);
  // Set by dwell_ended, which declares the person gone: the next frame ends
  // the stay (presence_ended) whatever it shows, since a tick sends one
  // event. Before F186 a sighting on that frame kept the stay and started a
  // second dwell in it.
  bool leave_owed_=false;
  // A sighting on that frame belongs to the next visit, which it opens on
  // the frame after (presence_started), unless that frame has a sighting of
  // its own. Its time and cell, so the visit is held present through the
  // lost timeout from the sighting and starts on the sighting's cell.
  bool held_sighting_=false;
  uint32_t held_seen_ms_=0;
  Voxel held_cell_{};

  // current
  BBox bbox_{};
  int confidence_=0;

  // coarse optical extras (live/telemetry tier — carried through to snapshot)
  uint8_t   person_count_=0;
  Posture   posture_=Posture::Unknown;
  Proximity proximity_=Proximity::Unknown;
  uint16_t  voxel_mask_=0;

  VoxelTracker voxel_tracker_;
};

} // namespace
