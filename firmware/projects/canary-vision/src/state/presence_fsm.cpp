#include "canary/state/presence_fsm.h"
#include "canary/config.h"
#include "canary/detect_config.h"

namespace canary::state {

void PresenceFSM::reset() {
  presence_=false;
  dwelling_=false;
  presence_start_ms_=0;
  last_seen_ms_=0;
  dwell_start_ms_=0;
  last_leave_ms_=0;
  last_visit_ms_=0;
  ended_dwell_ms_=0;

  interaction_candidate_=false;

  dwell_latch_=false;
  interaction_latch_=false;
  interaction_emitted_=false;
  last_leave_seen_=0;
  pending_interaction_reason_=nullptr;
  leave_owed_=false;
  held_sighting_=false;
  held_seen_ms_=0;
  held_cell_=Voxel{};

  bbox_ = BBox{};
  confidence_=0;

  person_count_=0;
  posture_=Posture::Unknown;
  proximity_=Proximity::Unknown;
  voxel_mask_=0;

  voxel_tracker_.reset();
}

static inline bool emit(EventMsg& out, const char* name, const char* reason=nullptr) {
  out.event_name = name;
  out.reason = reason;
  return true;
}

// Sends the interaction_likely an ended visit still owes (see open_visit),
// once, if its window is still open.
bool PresenceFSM::send_pending_interaction(uint32_t now_ms, EventMsg& out_event) {
  if (!pending_interaction_reason_) return false;
  const char* reason = pending_interaction_reason_;
  pending_interaction_reason_ = nullptr;
  if ((now_ms - last_leave_ms_) > INTERACTION_AFTER_LEAVE_WINDOW_MS) return false;
  return emit(out_event, "interaction_likely", reason);
}

// presence_started: a new visit, which starts its own tracker (sweep F152):
// the cell the last visit settled in, and the time it settled, say nothing
// about this one. Without it a later visit's interaction clock
// (stable_enter_ms) started in an earlier visit, so almost any later visit
// ended in interaction_likely. The visit's first sighting seeds the settled
// cell, so presence_started names the cell this visit began in, as the
// first visit after boot always did.
bool PresenceFSM::open_visit(const Voxel& first_cell, uint32_t seen_ms, uint32_t now_ms,
                             EventMsg& out_event) {
  voxel_tracker_.reset();
  voxel_tracker_.update(first_cell, seen_ms);
  // The visit that just ended reports interaction_likely on the first
  // frame after its presence_ended. When this visit opens on that very
  // frame, the leave-side code in tick never ran (a latch still set here
  // says so: it is cleared once the report is sent or its window has
  // closed), and clearing the latches would drop a qualified visit's
  // report: hold its reason, and send it on the next frame if the window
  // is still open. That row reads the new visit's frame (present, its
  // confidence and cell); visit_ms still says how long the ended visit
  // lasted.
  pending_interaction_reason_ = dwell_latch_        ? "dwell_then_left"
                                : interaction_latch_ ? "zone_interaction_then_left"
                                                     : nullptr;
  presence_ = true;
  dwelling_ = false;
  presence_start_ms_ = now_ms;
  interaction_candidate_ = false;
  dwell_latch_ = false;
  interaction_latch_ = false;
  interaction_emitted_ = false;
  return emit(out_event, "presence_started");
}

// presence_ended: the stay is over. Latch its duration so the leave-side
// events (presence_ended now, interaction_likely shortly after) can still
// report how long the visit was after presence_ms resets to 0.
bool PresenceFSM::end_visit(uint32_t now_ms, EventMsg& out_event) {
  presence_ = false;
  last_leave_ms_ = now_ms;
  last_visit_ms_ = now_ms - presence_start_ms_;
  return emit(out_event, "presence_ended");
}

bool PresenceFSM::tick(const VisionSample& vs, uint32_t now_ms, EventMsg& out_event) {
  out_event = EventMsg{};
  // Only the tick that ends a dwell reports its length (dwell_ended).
  ended_dwell_ms_ = 0;

  bbox_ = vs.bbox;
  confidence_ = vs.person_now ? vs.bbox.score : 0;

  // Coarse optical extras — pass-through to the live snapshot (never sealed).
  person_count_ = vs.person_count;
  posture_      = vs.posture;
  proximity_    = vs.proximity;
  voxel_mask_   = vs.voxel_mask;

  // The frame after dwell_ended ends the stay, whatever it shows (sweep
  // F186). dwell_ended declared the person gone, unseen for longer than the
  // lost timeout (or the dwell end grace), and presence_ended waits for this
  // frame only because a tick sends one event. Before, a sighting here kept
  // the stay and, the stay being older than the dwell start, took the
  // dwell_started branch again: a second dwell in one stay, a second page
  // from the lingering alert, a second dwell_ended. A sighting here belongs
  // to the next visit: held, it opens that visit on the next frame.
  if (leave_owed_) {
    leave_owed_ = false;
    held_sighting_ = vs.person_now;
    held_seen_ms_ = now_ms;
    held_cell_ = vs.voxel;
    return end_visit(now_ms, out_event);
  }
  // The held sighting opens the next visit here, the frame after the one it
  // was seen on, when this frame has no sighting of its own (one that does
  // opens it below, as any sighting does). The visit's clocks start on this
  // frame, the one that sends its presence_started; it is held present
  // through the lost timeout from the sighting, and starts on its cell.
  if (held_sighting_) {
    held_sighting_ = false;
    if (!vs.person_now) {
      last_seen_ms_ = held_seen_ms_;
      return open_visit(held_cell_, held_seen_ms_, now_ms, out_event);
    }
  }

  if (vs.person_now) {
    last_seen_ms_ = now_ms;
    if (!presence_) return open_visit(vs.voxel, now_ms, now_ms, out_event);
    voxel_tracker_.update(vs.voxel, now_ms);

    if (send_pending_interaction(now_ms, out_event)) return true;

    if (!dwelling_ && (now_ms - presence_start_ms_) >= canary::cfg::detect().dwell_start_ms) {
      // The stay has dwelled: latch it for the leave here, where the dwell
      // starts (sweep F202). The latch used to be set below, on later
      // sighted frames only, and this frame returns before it, so a person
      // last seen on this frame left a stay that had dwelled (dwell_started
      // and dwell_ended both sent) reporting zone_interaction_then_left, or
      // no interaction_likely at all when their settled cell kept moving.
      dwell_latch_ = true;
      dwelling_ = true;
      dwell_start_ms_ = now_ms;
      return emit(out_event, "dwell_started");
    }

    if (!interaction_candidate_ && (now_ms - voxel_tracker_.stable_enter_ms()) >= ZONE_INTERACTION_MS) {
      interaction_candidate_ = true;
    }

    if (interaction_candidate_) interaction_latch_ = true;

    return false;
  }

  if (send_pending_interaction(now_ms, out_event)) return true;

  if (presence_ && (now_ms - last_seen_ms_) > canary::cfg::detect().lost_timeout_ms) {
    if (dwelling_) {
      // The dwell end grace holds a dweller past the lost timeout when it is
      // the longer wait (sweep F154): they stay present and dwelling until
      // they have gone unseen for longer than the grace too, so one who
      // drops out of frame for less keeps the dwell, and a dwell that does
      // end still sends dwell_ended with its length. Before, a grace longer
      // than the lost timeout cleared the dwell silently and ended the stay
      // on this tick. 0, the shipped value, leaves the lost timeout in charge.
      if ((now_ms - last_seen_ms_) <= DWELL_END_GRACE_MS) return false;
      // The stay ends on the next frame (sweep F186, above).
      leave_owed_ = true;
      // Latch the finished dwell, on the clock the running dwell used, so
      // the dwell_ended row says how long it lasted.
      ended_dwell_ms_ = now_ms - dwell_start_ms_;
      dwelling_ = false;
      return emit(out_event, "dwell_ended");
    }

    return end_visit(now_ms, out_event);
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
  // The running dwell; on the tick that ended one (dwell_ended), its length;
  // otherwise 0. dwell_started's own tick reads 0: the dwell starts there.
  s.dwell_ms    = dwelling_ ? (now_ms - dwell_start_ms_) : ended_dwell_ms_;
  s.visit_ms    = last_visit_ms_;

  s.confidence = confidence_;
  s.voxel = voxel_tracker_.stable();
  s.bbox  = bbox_;

  s.person_count = person_count_;
  s.posture      = posture_;
  s.proximity    = proximity_;
  s.voxel_mask   = voxel_mask_;

  s.last_event = last_event ? last_event : "boot";
  s.uptime_s   = now_ms / 1000;
  s.ts_ms      = now_ms;
  return s;
}

} // namespace
