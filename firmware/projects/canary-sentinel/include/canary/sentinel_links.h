/**
 * @file sentinel_links.h
 * @brief What canary-sentinel says about its own sensor links: the radar UART
 *        (the retained health payload's `radar` object) and the BH1750 light
 *        sensor (absent, blinded, or reading: the vote it gives, and the
 *        state topic's `channel_denied`).
 *
 * Pure, no Arduino: src/main.cpp feeds it millis() and the drivers' answers,
 * and tests_host/test_sentinel_links.cpp runs the same functions on the host
 * (and holds main.cpp to calling them, by its source). One rule per link,
 * read by both the fusion vote and the owner's report. For the radar they
 * agree once it has spoken; a radar silent since boot reads down in Home
 * Assistant while its channel never votes, as an unwired one does. For the
 * light sensor, absent never votes and is reported as a denied channel.
 */

#pragma once

#include <math.h>
#include <stdint.h>

#include "fusion/sentinel_channels.h"
#include "fusion/sentinel_fusion.h"

namespace canary {
namespace sentinel {

// ── The radar link ──────────────────────────────────────────────────────────
//
// A "frame" is any frame the MR60 parser decodes: the same rule read_radar()
// has always used for the fusion vote, where a link that has spoken and then
// stays silent past the preset's SENT_STALE_RADAR votes Denied. The health
// object reports that same rule, never a second one.
struct RadarLink {
  bool     frame_seen    = false;  // a frame has decoded since boot
  uint32_t boot_ms       = 0;      // when the UART was opened
  uint32_t last_frame_ms = 0;      // the newest frame; boot_ms until one arrives
};

// The UART was opened at `now_ms`.
inline void radar_link_open(RadarLink& l, uint32_t now_ms) {
  l.frame_seen = false;
  l.boot_ms = now_ms;
  l.last_frame_ms = now_ms;
}

// A frame decoded at `now_ms`.
inline void radar_link_frame(RadarLink& l, uint32_t now_ms) {
  l.frame_seen = true;
  l.last_frame_ms = now_ms;
}

// Milliseconds since the newest frame (since the UART opened, before one).
// Unsigned and wrap-safe across one millis() rollover, as read_radar() was.
inline uint32_t radar_frame_age_ms(const RadarLink& l, uint32_t now_ms) {
  return now_ms - l.last_frame_ms;
}

// The fusion engine's Denied condition: the link spoke, then went quiet for
// longer than the stall window. A link that has never spoken is not stalled
// (that channel simply never votes).
inline bool radar_stalled(const RadarLink& l, uint32_t now_ms, uint32_t stall_ms) {
  return l.frame_seen && radar_frame_age_ms(l, now_ms) > stall_ms;
}

// Whether the link can be reported yet: a frame has decoded, or a whole
// stall window has passed since the UART opened with none (the boot-time
// broker connect can run before the UART has been drained once). Until then
// the health payload leaves the object out, which Home Assistant reads as
// "unknown", never as "down" (the contract RadarLinkHealth documents in
// include/canary/net/mqtt_mgr.h, the same rule canary-sense applies).
inline bool radar_link_judged(const RadarLink& l, uint32_t now_ms, uint32_t stall_ms) {
  return l.frame_seen || (uint32_t)(now_ms - l.boot_ms) >= stall_ms;
}

// The object's link_ok: a frame arrived inside the stall window. Never true
// for a link that has not spoken since boot, so a radar that is unplugged or
// dead from power-on reports "down" once judged, not "ok". That is the one
// place the object and the vote part: radar_stalled() is false for that link,
// so its channel never votes, as an unwired one does, and the owner learns of
// it from this object alone. Once a frame has decoded, link_ok is false
// exactly when the vote is Denied.
inline bool radar_link_ok(const RadarLink& l, uint32_t now_ms, uint32_t stall_ms) {
  return l.frame_seen && !radar_stalled(l, now_ms, stall_ms);
}

// ── The light sensor ────────────────────────────────────────────────────────
//
// The BH1750 has no id register. Its only presence probe is the POWER_ON
// acknowledgment its driver's begin() sends (common/sensors/bh1750), so
// begin()'s result at boot is the only thing that tells a unit with no light
// sensor from a light sensor that stops answering.
enum class LightLink : uint8_t {
  // Nothing answered begin() at boot: this unit has no working light sensor.
  // The channel never votes, so it adds no suspicion, and a visitor the other
  // channels see is not an Anomaly for it. Before, every read failed and voted
  // Denied, so a unit without the sensor read as a blinded channel for good
  // and, on a build with the radar, as an Anomaly whenever the radar saw a
  // body. It is never a silent gap either (requirement R6): the state topic
  // reports it as a denied channel (channel_denied_reported(), below), which
  // is how a sensor unplugged while the unit was off still reaches the owner.
  Absent,
  // It answered at boot and a read fails now: unplugged, jammed or dead
  // since boot. Denied, suspicion (requirement R2).
  Blinded,
  // A reading in lux (0 in the dark is a reading, not a blinding).
  Reading,
};

inline LightLink light_link(bool answered_at_boot, float lux) {
  if (!answered_at_boot) return LightLink::Absent;
  return (lux < 0.0f) ? LightLink::Blinded : LightLink::Reading;
}

// The light channel between samples: the slow-tracked baseline a change is
// measured against, negative until the first reading.
struct LightChannel {
  float baseline = -1.0f;
};

// A change this large against the baseline is Weak corroboration (a shadow
// crossing, a light toggled, a hand over the sill).
constexpr uint16_t kLightWeakDeltaLux = 50;

// What one light sample gives the fusion engine: a vote and its quality, or
// no vote at all (`votes` false), which is the absent sensor's answer.
struct LightSample {
  bool                   votes   = false;
  securacv::fusion::Vote vote    = securacv::fusion::Vote::None;
  uint8_t                quality = 0;
};

// One light sample, the whole of what read_light() decides: `answered_at_boot`
// is the driver's begin() answer (BH1750::present()) and `lux` its read_lux().
inline LightSample light_sample(LightChannel& c, bool answered_at_boot, float lux) {
  LightSample s;
  switch (light_link(answered_at_boot, lux)) {
    case LightLink::Absent:
      return s;
    case LightLink::Blinded:
      s.votes   = true;
      s.vote    = securacv::fusion::Vote::Denied;
      s.quality = 100;
      return s;
    case LightLink::Reading:
      break;
  }
  if (c.baseline < 0.0f) c.baseline = lux;
  const uint16_t delta = static_cast<uint16_t>(fabsf(lux - c.baseline));
  // Slow-track the baseline so a sunset drift doesn't read as an event.
  c.baseline += (lux - c.baseline) * 0.02f;
  s.votes   = true;
  s.vote    = securacv::fusion::channels::light_vote(/*blinded=*/false, delta,
                                                     kLightWeakDeltaLux);
  s.quality = 80;
  return s;
}

// The state topic's `channel_denied`, which Home Assistant's "Channel blinded"
// problem sensor reads: the engine's denied_any (a channel voting Denied right
// now), or a light sensor that did not answer at boot. The absent sensor adds
// no suspicion to the verdict, but the owner still sees that a channel this
// build carries is not there, as they did when it voted Denied.
inline bool channel_denied_reported(bool engine_denied_any, bool light_absent_at_boot) {
  return engine_denied_any || light_absent_at_boot;
}

}  // namespace sentinel
}  // namespace canary
