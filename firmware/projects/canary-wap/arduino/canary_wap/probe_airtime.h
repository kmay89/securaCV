/*
 * SecuraCV Canary WAP — the CSI probe's airtime reservation.
 *
 * The body of csi_probe::Config::airtime_gate as probe_pump() installs it
 * (csi_integration.cpp), kept in a header so
 * tests_host/test_csi_probe_airtime.cpp can link it against the REAL probe
 * scheduler and the REAL governor. Pure: no Arduino, time injected by the
 * caller.
 *
 * Every figure here is the governor's ESTIMATE of airtime (192 us preamble
 * + 8 us a byte at the 1 Mbps fallback rate, airtime_governor.h), not a
 * measurement of the air.
 */
#ifndef SECURACV_PROBE_AIRTIME_H
#define SECURACV_PROBE_AIRTIME_H

#include <stddef.h>
#include <stdint.h>

#include "airtime_governor.h"

namespace probe_airtime {

/* ESP-NOW MAC/action-frame framing the governor's estimate does not add on
 * its own (csi_probe.h's honest airtime math): the 16 B probe payload is
 * charged as 75 B, 792 us. */
constexpr size_t PROBE_FRAME_OVERHEAD_BYTES = 59;

/* The probe starts no frame once the governor's window reads 1.60 % of its
 * 10 s, so the probe's share tops out within one frame (792 us) of that and
 * at least 0.32 % (32 000 us) of the 2 % routine cap stays for the 30 s mesh
 * heartbeat, the 60 s chirp presence and the Beacon self-test. Without the
 * ceiling a probe asking for more than the cap took every microsecond the
 * window freed and those were refused (host-measured: 0 of 9 in 180 s at
 * 160 frames/s). 1.60, not lower: one paired peer at the full 20 Hz is
 * estimated at 1.58 % and keeps a steady supply; at 1.50 it was throttled.
 * The window counts urgent and Beacon sends too, so during an alert storm
 * the probe yields first — intended. */
constexpr uint16_t PROBE_CEILING_PCT_X100 = 160;

/* csi_probe::Config::airtime_gate (test_csi_probe_airtime.cpp
 * static_asserts the signature). */
inline bool reserve_probe_frame(uint32_t now_ms, size_t payload_bytes) {
  if (airtime_governor::airtime_pct_x100(now_ms) >= PROBE_CEILING_PCT_X100) {
    return false;
  }
  return airtime_governor::try_reserve_routine(
      now_ms, payload_bytes + PROBE_FRAME_OVERHEAD_BYTES);
}

/* The governor's window is a PSRAM ring that only mesh_network::init
 * allocates. A build or boot without the mesh (the DEV and MINIMAL
 * profiles, FEATURE_MESH_NETWORK 0; or ESP-NOW refused before the mesh
 * reached the governor) never allocates it, and the governor fails open:
 * every reservation passes. The probe brings the governor up itself in that
 * case. On a mesh build the ring already exists and this does nothing, so it
 * never resets the mesh's window. */
inline void ensure_governor() {
  if (!airtime_governor::ring_ok()) {
    airtime_governor::init(airtime_governor::DEFAULT_CAP_PCT);
  }
}

}  // namespace probe_airtime

#endif  // SECURACV_PROBE_AIRTIME_H
