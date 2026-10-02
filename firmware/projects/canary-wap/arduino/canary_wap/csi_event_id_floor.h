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
 * before the id reaches any consumer. At boot the host restores `stored`
 * from the persisted value and the allocator's floor from boot_floor(),
 * which is at or above it, so the boot's first allocation (at or past
 * `stored`) writes again before that id leaves the device. Cost: one NVS write per boot that
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
 *
 * ONE ID SPACE (backlog F46). Every committed row, bundled or direct, takes
 * its id from csi_event.cpp's allocator when it commits. Before F46 the
 * bundler kept its own allocator (0x80000000 upward, restarted every boot,
 * no floor), so Home Assistant's replay gate refused chokepoint ids after
 * any bundle, and every bundle after a reboot. The allocator now starts at
 * kIdSpaceBase on every device, fresh or upgraded. That is above every id
 * an earlier firmware handed out: its chokepoint ids counted up from 1 and
 * its bundler's from 0x80000000 each boot, and neither comes within 2^30 of
 * it. So Home Assistant's stored mark for an upgraded device, wherever the
 * old firmware left it, is below the first new id, and nothing has to be
 * reset: not HA's mark, not the delivery ceiling (csi.evsent). A constant
 * is needed here, not a per-device value: a canary-wap from before #1754
 * kept no record of the bundler ids it sent.
 *
 * Headroom: 2^30 ids (1,073,741,824) before the 32-bit counter wraps.
 * Every committed row takes one, ambient rows included. The most a device
 * can commit in a day is the sum of two things:
 *   - one uncapped ambient module at one row per CSI window:
 *     wifi.channel_activity has no hourly ceiling and its cooldown goes
 *     down to 1 s (86,400);
 *   - the other 15 modules at the 255/hour override's maximum
 *     (15 x 255 x 24 = 91,800; the two uncapped meta modules emit about
 *     once a day). Bundled rows are inside that: every bundle opening
 *     spends its module's ceiling, a reopen after a quiet gap or a
 *     10-minute window included (sweep F80; before it a refunded refresh
 *     could reopen a bundle uncounted, up to 8 x 720 = 5,760 more a day).
 * That is 178,200 a day: about 16.5 years (6,025 days). At the shipped
 * defaults (a 5 s cooldown; manifest ceilings of at most 60 an hour) it is
 * under 17,280 + 15 x 60 x 24 = 38,880 a day: about 75 years. At
 * exhaustion the counter wraps. Ids restart at 1 (0 is never handed out),
 * and each later boot first reissues 0xFFFFFFFF (floor_for saturates).
 * From then on Home Assistant refuses the device's events. Both trees' MQTT
 * health says `event_id_space_low` from kHoldLimit on (space_low() below,
 * backlog F82); what recovers the device is still a decision. A boot loop
 * costs what it cost before
 * F46: one floor write per boot that allocates (one write, not more, even
 * when boot_floor() holds the floor above the delivery ceiling: the boot's
 * first allocation is the write), and up to kStride skipped ids. A boot
 * every 5 s that commits one row writes 17,280 times a day and burns
 * 172,800 ids a day: about 17 years of space, and the write rate is the
 * one F29 already accepted. test_csi_event_id_floor.cpp pins both.
 */

#ifndef SECURACV_CSI_EVENT_ID_FLOOR_H
#define SECURACV_CSI_EVENT_ID_FLOOR_H

#include <stdint.h>

namespace csi_event_id_floor {

// Ids a reboot may skip; also the write cadence while a boot runs.
constexpr uint32_t kStride = 10;

// Where the one event-id space starts (see the file header).
constexpr uint32_t kIdSpaceBase = 0xC0000000u;

// An open bundle has no event id until it commits. Its HANDLE
// (csi_bundler.cpp: csi_event_emit's return for a buffered emit, and the
// `id` of an `"open":1` row on the canary-wap's /api/events/today) lies in
// [kHandleBase, kIdSpaceBase): below every event id this firmware hands
// out, so no client can mistake an open row for a committed one, or the
// reverse. Handles restart every boot and wrap inside the range.
constexpr uint32_t kHandleBase = 0x80000000u;

// boot_floor() never holds the floor above the delivery ceiling once that
// ceiling is past this: 2^28 ids short of the wrap. Only a corrupt ceiling,
// or one an older firmware wrote for a forged card line (its strtol read
// "id":-5 as 0xFFFFFFFB), gets there; following it would bring the
// allocator to the wrap, where ids restart at 1.
constexpr uint32_t kHoldLimit = 0xF0000000u;

static_assert(kHandleBase < kIdSpaceBase && kIdSpaceBase < kHoldLimit,
              "handles below event ids; the hold limit inside the space");

// The floor a host restores at boot (csi_event_set_event_id_floor):
//   - never below kIdSpaceBase (an older firmware's persisted floor, or none,
//     changes nothing);
//   - held at or above the delivery ceiling (`csi.evsent`, the backfill's
//     record of what was handed to the broker, always above every id handed
//     over): if a floor write failed and the ceiling write did not, ids at
//     or past the persisted floor already reached Home Assistant, and
//     reissuing them would be refused;
//   - but never raised to a ceiling past kHoldLimit (see above). Such a
//     device's HA mark is past it too; re-pinning the device in HA resets
//     the mark, and the allocator is still far from the wrap. The backfill
//     drops that ceiling as its record too (ceiling_ignored below), or it
//     would never send a held row again.
// `persisted_floor` / `delivered_ceiling` are what NVS holds (0 = none).
inline uint32_t boot_floor(uint32_t persisted_floor, uint32_t delivered_ceiling) {
  uint32_t f = (persisted_floor > kIdSpaceBase) ? persisted_floor : kIdSpaceBase;
  if (delivered_ceiling > f && delivered_ceiling <= kHoldLimit) f = delivered_ceiling;
  return f;
}

// Did boot_floor() leave the allocator below this delivery ceiling? Only
// for a ceiling past kHoldLimit that is above the persisted floor (see
// kHoldLimit). Such a ceiling no longer bounds this boot's ids, so a backfill
// must not restore its watermark from it: every row this boot commits would
// read as delivered (csi_event_backfill::restore treats it as no record).
inline bool ceiling_ignored(uint32_t persisted_floor, uint32_t delivered_ceiling) {
  return delivered_ceiling > boot_floor(persisted_floor, delivered_ceiling);
}

// Is the id space running out (backlog F82)? `next_id` is the allocator's
// next id (csi_event_get_next_event_id()). True from the moment it reaches
// kHoldLimit, 2^28 ids short of the wrap (about four years before it at the
// most a device can commit, 178,200 a day; about 19 at the shipped
// defaults), and after the counter has wrapped: ids
// restart at 1, below kIdSpaceBase, where no allocation reaches before the
// wrap, and each later boot first reissues 0xFFFFFFFF (floor_for saturates),
// at or past kHoldLimit again. Home Assistant refuses a wrapped device's
// events, so both trees' MQTT health publishes this as `event_id_space_low`.
// It only warns: the recovery (a re-pin in HA, then a reset of the floor
// and of the delivery ceiling csi.evsent) is still a decision.
inline bool space_low(uint32_t next_id) {
  return next_id >= kHoldLimit || next_id < kIdSpaceBase;
}

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
