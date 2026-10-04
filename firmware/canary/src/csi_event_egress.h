/*
 * SecuraCV Canary — committed csi_event egress (backlog F29)
 *
 * Until this module the canary tree overrode neither weak csi_event hook,
 * so a committed event (presence, breathing, a system.integrity tamper)
 * reached only the RAM ring: no MQTT, no Home Assistant. This gives the
 * canary the canary-wap's csi_mqtt shape: the SAME `events` body
 * (common/csi/src/csi_event_wire.h, shared byte-for-byte), signed with the
 * device's witness Ed25519 key through common/identity/device_signature,
 * and the same system.integrity -> `tamper` topic bridge HA's per-type
 * tamper sensors match (SD and enclosure kinds: this tree's boot story
 * already reaches that topic through canary_power_events.h).
 *
 * Threading: csi_event_on_committed can fire off the loop task (BLE
 * Scout's arrivals run on the NimBLE host task), and PubSubClient must
 * only be driven from the loop task. So the strong override only COPIES
 * the committed row into a bounded FreeRTOS queue (never blocks, counts
 * what it drops); csi_event_egress_pump() drains it on the loop task.
 *
 * Persistence (backlog F37): with a card in, every row the pump drains is
 * appended to the SD event log (/EVENTS/today.ndjson, the canary-wap's line
 * format — src/csi_event_log.cpp, loop task only under this tree's
 * single-writer SD rule). A row goes out live when nothing older waits on
 * the card; otherwise it waits there, and once the broker is back and the
 * MQTT layer's offline queue has drained, the backfill replays the card in
 * id order, a bounded amount per loop pass, marked `"replay":true` unless
 * the row was committed while the link was up. It never sends an id at or
 * below the highest one already handed to the broker (HA's replay gate
 * would refuse it); that watermark survives a reboot as an NVS ceiling
 * written with the event-id floor's policy. The rules are the pure
 * common/csi/src/csi_event_backfill.h, host-tested. Without a card (or with
 * another device's log on it) rows take the MQTT layer's bounded offline
 * queue (F9) as before: 12 slots, where tamper alerts outrank events
 * (mqtt_offline_queue.h), and a body built while the link is down says
 * `"replay":true`. The tamper-topic bridge publishes at commit either way,
 * so a backlog never delays a tamper alert.
 *
 * A card that is not open may still hold older rows (backlog F104): from
 * boot until its log first opens, and after it closes with rows waiting
 * (an SD error's lost mark, remounted by the storage manager's 30 s
 * recheck). Rows committed then wait in a RAM hold (8 rows, the oldest
 * dropped first) instead of overtaking the card's: they go once the card is
 * back and its backlog sent, in id order with it, or after
 * csi_event_backfill::kCardWaitMs (45 s, the canary-wap's wait), when the
 * card is given up and its rows, if it returns later, are not sent. A row
 * whose card append fails waits in the same hold while older rows wait on
 * the card or in the hold, or while the link is down (backlog F103): it
 * used to go live or into the offline queue at once, past the card's rows.
 * An ambient row that would wait is dropped, counted (csi_event_egress_stats
 * below). A row in the hold writes no NVS delivery ceiling until it goes
 * (the planner writes it then), so a reboot
 * never reads the card's rows as delivered on its account, and with a card
 * open it waits for the link (the ceiling it would write could cover the
 * card rows after it). So the offline queue, which drains later without
 * asking the planner, is never given a row older rows wait ahead of. Rows
 * in the hold do not survive a reboot, and a broker change drops them, so on
 * a build with a card slot but no usable card a row from the first 45 s
 * after boot is lost if the canary reboots (or its broker changes) first. A
 * held row the backfill already sent from the card (a failed append that
 * landed all but its newline) is dropped, not sent twice. Host-tested on
 * the real source by firmware/tests_host/test_canary_event_egress.cpp.
 *
 * Event-id continuity: csi_event_on_id_advance writes the allocator's
 * floor to NVS (common/csi/src/csi_event_id_floor.h: before the first id
 * of each boot and every 10 ids after) and begin() restores it, so ids
 * stay monotonic across reboots (HA's replay detection keys on them), with
 * the same policy as the canary-wap.
 */

#ifndef SECURACV_CSI_EVENT_EGRESS_H
#define SECURACV_CSI_EVENT_EGRESS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Setup, loop task, before the CSI modules register: restore the event-id
 * floor from NVS and — on FEATURE_HA_MQTT builds — hand the witness
 * identity to device_signature, create the egress queue and restore the
 * backfill's delivery watermark. */
void csi_event_egress_begin(void);

/* Loop task, after mqtt_loop(): log up to a few queued commits to the SD
 * event log and publish them on securacv/<id>/events (and a
 * system.integrity tamper on securacv/<id>/tamper), then run one bounded
 * backfill pass. The only caller of the SD event log. A no-op without
 * FEATURE_HA_MQTT. */
void csi_event_egress_pump(void);

#ifdef CSI_TEST_HOST_BUILD
/* Host tests only (firmware/tests_host/test_canary_event_egress.cpp): forget
 * this "boot"'s RAM state, to model a power cycle. NVS and the card stay. */
void csi_event_egress_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "csi_event_backfill.h"   /* the planner's Stats */

/* What the egress did this boot (backlog F109), in the canary-wap's
 * csi_event_egress::Stats shape, field for field, so a bench run or a field
 * report reads both devices' counters by the same names. main.cpp's MQTT
 * health publish carries them as its `csi_event_egress` object. Counted
 * from boot (RAM; a reboot starts them over):
 *   dropped          commits the full egress queue refused (the loop task
 *                    was stuck; the row is on neither the card nor the wire);
 *   held_dropped     rows the RAM hold dropped, the oldest first (kHeldMax),
 *                    or every row that had to wait when the hold has no
 *                    memory;
 *   ambient_dropped  ambient rows that had to wait (they are never held);
 *   unsent_dropped   rows no card kept that were lost unsent where they
 *                    left for the MQTT layer: it refused them (its offline
 *                    queue has no memory, or is full of tamper alerts while
 *                    the link is down) and nothing kept them. (The
 *                    canary-wap counts a body that would not build here; a
 *                    canary body always builds: the 768-byte buffer holds
 *                    the widest, and the signature envelope is bounded.) A
 *                    row the offline queue took and later evicted is not
 *                    here: that is the queue's own dropped_overflow, which
 *                    the health publish carries beside these
 *                    (`offline_queue`);
 *   planner          the backfill planner's own counters
 *                    (csi_event_backfill::Stats: live, held, queued,
 *                    replayed, skipped, untrusted, unsendable,
 *                    truncated_unsent, read_giveups). They count paths, not
 *                    a ledger of rows: docs/csi_developer_api.md says what
 *                    each one counts and what none of them does.
 * Loop task (the pump's), as the health publish is. All zero without
 * FEATURE_HA_MQTT, where nothing leaves the device. Any other task reads
 * csi_event_egress_read_stats() below. */
struct CsiEventEgressStats {
  uint32_t                  dropped;
  uint32_t                  held_dropped;
  uint32_t                  ambient_dropped;
  uint32_t                  unsent_dropped;
  csi_event_backfill::Stats planner;
};
CsiEventEgressStats csi_event_egress_stats();

/* Thirteen u32 counters and nothing else: no padding for the snapshot's
 * byte compare to trip on, and a counter added here or to the planner's
 * Stats fails the build until csi_event_egress_stats_json() spells it. */
static_assert(sizeof(csi_event_backfill::Stats) == 9 * sizeof(uint32_t),
              "the planner's Stats changed: add the counter to csi_event_egress_stats_json()");
static_assert(sizeof(CsiEventEgressStats) == 13 * sizeof(uint32_t),
              "CsiEventEgressStats changed: add the counter to csi_event_egress_stats_json()");

/* Any task (sweep F179): the counters as the last csi_event_egress_pump()
 * left them, a whole copy. The pump publishes them as its last step, every
 * pass (an unchanged copy takes no lock), so this is at most one loop pass
 * old; `dropped`, which a committing task bumps, as of that pass. False,
 * with *out untouched, before the first pump, and on a build without
 * FEATURE_HA_MQTT (no egress runs). GET /api/diagnostics, on the HTTP
 * server's task, reads this (csi_event_egress_diagnostics_json()), never
 * csi_event_egress_stats(): the pump's own state is the loop task's. The
 * canary-wap's csi_event_egress::read_stats() (sweep F149), on the same
 * pattern (its loop_snapshot.h Value<T>). */
bool csi_event_egress_read_stats(CsiEventEgressStats* out);

/* The counters as one JSON object, in the names the MQTT health spells its
 * `csi_event_egress` object with (main.cpp mqtt_publish_health_update(),
 * sweep F109), which are the canary-wap's (csi_event_egress::stats_json()):
 *   {"dropped":N,"held_dropped":N,"ambient_dropped":N,"unsent_dropped":N,
 *    "planner":{"live":N,"held":N,"queued":N,"replayed":N,"skipped":N,
 *               "untrusted":N,"unsendable":N,"truncated_unsent":N,
 *               "read_giveups":N}}
 * kCsiEventEgressStatsJsonMax holds it with every counter at 4294967295
 * (319 bytes and the NUL). Returns its length, or 0 (and `out` holds no
 * partial object) when it does not fit `cap`. Pure: any task, any copy. */
constexpr size_t kCsiEventEgressStatsJsonMax = 384;
inline size_t csi_event_egress_stats_json(const CsiEventEgressStats& s, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  const csi_event_backfill::Stats& p = s.planner;
  const int n = snprintf(out, cap,
      "{\"dropped\":%lu,\"held_dropped\":%lu,\"ambient_dropped\":%lu,"
      "\"unsent_dropped\":%lu,\"planner\":{\"live\":%lu,\"held\":%lu,"
      "\"queued\":%lu,\"replayed\":%lu,\"skipped\":%lu,\"untrusted\":%lu,"
      "\"unsendable\":%lu,\"truncated_unsent\":%lu,\"read_giveups\":%lu}}",
      (unsigned long)s.dropped, (unsigned long)s.held_dropped,
      (unsigned long)s.ambient_dropped, (unsigned long)s.unsent_dropped,
      (unsigned long)p.live, (unsigned long)p.held, (unsigned long)p.queued,
      (unsigned long)p.replayed, (unsigned long)p.skipped,
      (unsigned long)p.untrusted, (unsigned long)p.unsendable,
      (unsigned long)p.truncated_unsent, (unsigned long)p.read_giveups);
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

/* The event-id space is running out (backlog F82): the allocator's next id
 * is at or past csi_event_id_floor::kHoldLimit, or the counter has wrapped
 * (csi_event_id_floor::space_low). Home Assistant refuses a wrapped device's
 * events, so the health publish says so (`event_id_space_low`) before that.
 * What recovers the device (a re-pin in HA and a reset of the floor and the
 * delivery ceiling) is not decided yet; this only warns. Any task. */
bool csi_event_egress_id_space_low();
#endif

#endif /* SECURACV_CSI_EVENT_EGRESS_H */
