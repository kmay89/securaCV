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
 * + 8 us a byte at the 1 Mbps fallback rate, the ~59 B of ESP-NOW framing
 * included, airtime_governor.h), not a measurement of the air.
 */
#ifndef SECURACV_PROBE_AIRTIME_H
#define SECURACV_PROBE_AIRTIME_H

#include <stddef.h>
#include <stdint.h>

#include "airtime_governor.h"

namespace probe_airtime {

/* The probe starts no frame once the governor's window reads 1.60 % of its
 * 10 s (160 000 us). One frame is 792 us, under 0.01 % of the window, so the
 * probe stops within one frame of the line (the window holds 160 784 us at
 * most when its last frame lands) and about 0.39 % (39 216 us) of the 2 %
 * routine cap stays for the other routine senders, framed like the probe:
 * the 30 s mesh heartbeat (one 1576 us signed frame to each connected
 * peer, 25 216 us for a full Opera of 16), the 60 s chirp presence
 * (1120 us) and the Beacon self-test (1624 us, a routine reservation,
 * beacon_channel.cpp emit_selftest) — 27 960 us together. Without the
 * ceiling a probe asking for more than the cap took every microsecond the
 * window freed and those were refused (host-measured: 0 of 9 in 180 s at
 * 160 frames/s). 1.60, not lower: one paired peer at the full 20 Hz is
 * estimated at 1.58 % and keeps a steady supply (it skips about one frame
 * when a heartbeat lands); at 1.50 it was throttled (host-measured, framed:
 * 228 of 3600 frames skipped in 180 s, some seconds down to 12). Not higher
 * either: 1.60 % is also the Beacon's airtime_saturated trouble line
 * (beacon_channel.cpp, > 160 x100, NORMAL -> TROUBLE), so a probe held
 * above it would keep the Beacon in TROUBLE once the peer table fills. At
 * this value the probe alone never takes the window over that line (its
 * frames stop at 160 x100); only another sender landing after its last
 * frame does, and on a window the probe saturates it stays over until as
 * much probe airtime ages out, which can take seconds (host-measured,
 * eight probe peers for 170 s: over the line 25 s with the heartbeat
 * charged a frame per connected peer to eight peers; 9 s when it was
 * charged as one unframed send). The window counts urgent and Beacon sends
 * too, so during an alert storm the probe yields first — intended.
 * test_csi_probe_airtime static_asserts the value and pins the gate on
 * both sides of the line. */
constexpr uint16_t PROBE_CEILING_PCT_X100 = 160;

/* csi_probe::Config::airtime_gate (test_csi_probe_airtime.cpp
 * static_asserts the signature). */
inline bool reserve_probe_frame(uint32_t now_ms, size_t payload_bytes) {
  if (airtime_governor::airtime_pct_x100(now_ms) >= PROBE_CEILING_PCT_X100) {
    return false;
  }
  /* The payload only: the governor adds the ESP-NOW framing to every
   * caller's frame (airtime_governor::ESPNOW_FRAME_OVERHEAD_BYTES), so the
   * 16 B probe payload is charged as 75 B, 792 us, and adding it here too
   * would count it twice. */
  return airtime_governor::try_reserve_routine(now_ms, payload_bytes);
}

/* The governor's window is a PSRAM ring that only mesh_network::init
 * allocates. A build or boot where the mesh did not initialize never
 * allocates it, and the governor fails open: every reservation passes.
 * That is the DEV profile (FEATURE_MESH_NETWORK 0), or a FULL boot in safe
 * mode or with ESP-NOW refused before the mesh reached the governor.
 * (MINIMAL never runs the probe: it has no HTTP server, so
 * csi_integration::init is never called.) The probe brings the governor up
 * itself in that case. On a mesh build the ring already exists and this
 * does nothing, so it never resets the mesh's window. */
inline void ensure_governor() {
  if (!airtime_governor::ring_ok()) {
    airtime_governor::init(airtime_governor::DEFAULT_CAP_PCT);
  }
}

}  // namespace probe_airtime

#endif  // SECURACV_PROBE_AIRTIME_H
