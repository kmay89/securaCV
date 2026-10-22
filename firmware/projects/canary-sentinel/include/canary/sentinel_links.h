/**
 * @file sentinel_links.h
 * @brief What canary-sentinel says about its own sensor links: the radar UART
 *        (the retained health payload's `radar` object) and the BH1750 light
 *        sensor (absent, blinded, or reading).
 *
 * Pure, no Arduino: src/main.cpp feeds it millis() and the drivers' answers,
 * and tests_host/test_sentinel_links.cpp runs the same functions on the host.
 * One rule per link, read by both the fusion vote and the health report, so
 * the two can never disagree about the same link.
 */

#pragma once

#include <stdint.h>

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
// dead from power-on reports "down" once judged, not "ok".
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
  // The channel is not wired here and never votes (the README's "an unwired
  // channel never votes"); the boot console says so. Before, every read
  // failed and voted Denied, so a unit without the sensor read as a blinded
  // channel for good, and, on a build with the radar, as an Anomaly
  // whenever the radar saw a body.
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

}  // namespace sentinel
}  // namespace canary
