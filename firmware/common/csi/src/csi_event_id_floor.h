/**
 * @file csi_event_id_floor.h
 * @brief When to write the event-id floor to NVS so that no event_id is
 *        ever handed out twice across a reboot. Pure, Arduino-free,
 *        host-tested (firmware/tests_host/test_csi_event_id_floor.cpp).
 *
 * Both firmware trees persist csi_event.cpp's id allocator through the
 * csi_event_on_id_advance hook and restore it at boot with
 * csi_event_set_event_id_floor(): the canary-wap in csi_integration.cpp,
 * the canary PIO tree in src/csi_event_egress.cpp. It matters because
 * Home Assistant's replay gate refuses a signed `events` body whose
 * event_id is below the last one it verified for that device, and the
 * canary-wap's reconnect backfill keys its watermark on the same ids. An
 * id handed out a second time is a real event thrown away.
 *
 * The invariant: once an id has been handed out, NVS already holds a
 * value above it. The host tracks the value NVS actually holds
 * (`stored`, 0 before the first write). An allocation at or past it
 * writes `id + kStride` first. The hook runs inside the allocator,
 * before the id reaches any consumer. At boot the host restores the
 * allocator's floor AND `stored` from the same persisted value, so the
 * boot's first allocation (the persisted value itself) writes again
 * before that id leaves the device. Cost: one NVS write per boot that
 * allocates anything, plus one per kStride ids. A reboot skips at most
 * kStride ids and reuses none.
 *
 * What this replaces: both trees used to remember the id at which they
 * last wrote and write again only kStride ids later. A boot that
 * allocated fewer than kStride ids therefore left NVS where it started,
 * and the next boot handed the same ids out again (review of backlog F29).
 *
 * A failed write (NVS unavailable) must leave `stored` unchanged, so the
 * next allocation tries again. The id still goes out; the allocator
 * cannot wait on flash.
 */

#ifndef SECURACV_CSI_EVENT_ID_FLOOR_H
#define SECURACV_CSI_EVENT_ID_FLOOR_H

#include <stdint.h>

namespace csi_event_id_floor {

// Ids a reboot may skip; also the write cadence while a boot runs.
constexpr uint32_t kStride = 10;

// Must the allocation of `new_id` write the floor before the id goes out?
// `stored` is the value NVS holds (0 = nothing written yet).
inline bool must_persist(uint32_t stored, uint32_t new_id) {
  return new_id >= stored;
}

// The floor to write for `new_id`: kStride past it, saturating instead of
// wrapping into ids that are already in use.
inline uint32_t floor_for(uint32_t new_id) {
  return (new_id > UINT32_MAX - kStride) ? UINT32_MAX : new_id + kStride;
}

}  // namespace csi_event_id_floor

#endif  // SECURACV_CSI_EVENT_ID_FLOOR_H
