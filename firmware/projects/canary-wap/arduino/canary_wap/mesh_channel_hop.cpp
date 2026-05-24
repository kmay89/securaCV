/*
 * SecuraCV Canary — Mesh channel-hop wire format + hop tracker
 * Version 0.1.0
 */

#include "mesh_channel_hop.h"
#include <string.h>

namespace mesh_channel_hop {

bool encode(uint8_t   channel,
            Reason    reason,
            uint8_t*  out_buf,
            size_t    out_buf_cap) {
  if (out_buf == nullptr || out_buf_cap < PAYLOAD_LEN) return false;
  if (channel < MIN_CHANNEL || channel > MAX_CHANNEL) return false;
  out_buf[0] = channel;
  out_buf[1] = static_cast<uint8_t>(reason);
  return true;
}

bool decode(const uint8_t*  in_buf,
            size_t          in_len,
            uint8_t*        out_channel,
            Reason*         out_reason) {
  if (in_buf == nullptr || out_channel == nullptr || out_reason == nullptr)
    return false;
  if (in_len != PAYLOAD_LEN) return false;
  uint8_t ch = in_buf[0];
  if (ch < MIN_CHANNEL || ch > MAX_CHANNEL) return false;
  *out_channel = ch;
  *out_reason  = static_cast<Reason>(in_buf[1]);
  return true;
}

uint8_t next_channel(uint8_t current_channel) {
  switch (current_channel) {
    case  1: return  6;
    case  6: return 11;
    case 11: return  1;
    default: return  0;
  }
}

HopTracker make_tracker(uint16_t threshold_pct_x100,
                        uint32_t sustain_ms,
                        uint32_t cooldown_ms) {
  HopTracker t;
  t.threshold_pct_x100 = threshold_pct_x100;
  t.sustain_ms          = sustain_ms;
  t.cooldown_ms         = cooldown_ms;
  t.above_since_ms      = 0;
  t.last_hop_ms         = 0;
  t.tracking_above      = false;
  t.has_hopped          = false;
  t.armed               = true;
  return t;
}

bool tick(HopTracker& t, uint32_t now_ms, uint16_t airtime_pct_x100) {
  if (!t.armed) {
    if (t.has_hopped &&
        (int32_t)(now_ms - t.last_hop_ms) >= (int32_t)t.cooldown_ms) {
      t.armed = true;
    }
    return false;
  }

  if (airtime_pct_x100 >= t.threshold_pct_x100) {
    if (!t.tracking_above) {
      t.above_since_ms = now_ms;
      t.tracking_above = true;
    }
    if ((int32_t)(now_ms - t.above_since_ms) >= (int32_t)t.sustain_ms) {
      return true;
    }
  } else {
    t.tracking_above = false;
  }
  return false;
}

void reset(HopTracker& t, uint32_t now_ms) {
  t.tracking_above = false;
  t.last_hop_ms    = now_ms;
  t.has_hopped     = true;
  t.armed          = false;
}

}  /* namespace mesh_channel_hop */
