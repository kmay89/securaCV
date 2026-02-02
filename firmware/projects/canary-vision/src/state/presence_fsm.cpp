#include "canary/state/presence_fsm.h"
#include "canary/config.h"

namespace canary::state {

void PresenceFSM::reset() {
  presence_=false;
  dwelling_=false;
  presence_start_ms_=0;
  last_seen_ms_=0;
  dwell_start_ms_=0;
  last_leave_ms_=0;

  interaction_candidate_=false;

  dwell_latch_=false;
  interaction_latch_=false;
  interaction_emitted_=false;
  last_leave_seen_=0;

  bbox_ = BBox{};
  confidence_=0;

  voxel_tracker_.reset();
}

static inline bool emit(EventMsg& out, const char* name, const char* reason=nullptr) {
  out.event_name = name;
  out.reason = reason;
  return true;
}

bool PresenceFSM::tick(const VisionSample& vs, uint32_t now_ms, EventMsg& out_event) {
  out_event = EventMsg{};

  bbox_ = vs.bbox;
  confidence_ = vs.person_now ? vs.bbox.score : 0;

  if (vs.person_now) {
    last_seen_ms_ = now_ms;
    voxel_tracker_.update(vs.voxel, now_ms);

    if (!presence_) {
      presence_ = true;
      dwelling_ = false;
      presence_start_ms_ = now_ms;
      interaction_candidate_ = false;
      dwell_latch_ = false;
      interaction_latch_ = false;
      interaction_emitted_ = false;
      return emit(out_event, "presence_started");
    }

    if (!dwelling_ && (now_ms - presence_start_ms_) >= DWELL_START_MS) {
      dwelling_ = true;
      dwell_start_ms_ = now_ms;
      return emit(out_event, "dwell_started");
    }

    if (!interaction_candidate_ && (now_ms - voxel_tracker_.stable_enter_ms()) >= ZONE_INTERACTION_MS) {
      interaction_candidate_ = true;
    }

    if (dwelling_) dwell_latch_ = true;
    if (interaction_candidate_) interaction_latch_ = true;

    return false;
  }

  if (presence_ && (now_ms - last_seen_ms_) > LOST_TIMEOUT_MS) {
    if (dwelling_) {
      if (DWELL_END_GRACE_MS == 0 || (now_ms - last_seen_ms_) >= DWELL_END_GRACE_MS) {
        dwelling_ = false;
        return emit(out_event, "dwell_ended");
      }
      dwelling_ = false;
    }

    presence_ = false;
    last_leave_ms_ = now_ms;
    return emit(out_event, "presence_ended");
  }

  if (!presence_ && last_leave_ms_ != 0 && last_leave_ms_ != last_leave_seen_) {
    last_leave_seen_ = last_leave_ms_;
    interaction_emitted_ = false;
  }

  if (!presence_ && last_leave_ms_ != 0 && !interaction_emitted_) {
    const bool qualified = (dwell_latch_ || interaction_latch_);
    if (qualified && (now_ms - last_leave_ms_) <= INTERACTION_AFTER_LEAVE_WINDOW_MS) {
      interaction_emitted_ = true;
      const char* reason = dwell_latch_ ? "dwell_then_left" : "zone_interaction_then_left";
      dwell_latch_ = false;
      interaction_latch_ = false;
      return emit(out_event, "interaction_likely", reason);
    }
    if ((now_ms - last_leave_ms_) > INTERACTION_AFTER_LEAVE_WINDOW_MS) {
      interaction_emitted_ = true;
      dwell_latch_ = false;
      interaction_latch_ = false;
    }
  }

  return false;
}

StateSnapshot PresenceFSM::snapshot(uint32_t now_ms, const char* last_event) const {
  StateSnapshot s{};
  s.presence = presence_;
  s.dwelling = dwelling_;
  s.presence_ms = presence_ ? (now_ms - presence_start_ms_) : 0;
  s.dwell_ms    = dwelling_ ? (now_ms - dwell_start_ms_) : 0;

  s.confidence = confidence_;
  s.voxel = voxel_tracker_.stable();
  s.bbox  = bbox_;

  s.last_event = last_event ? last_event : "boot";
  s.uptime_s   = now_ms / 1000;
  s.ts_ms      = now_ms;
  return s;
}

} // namespace
