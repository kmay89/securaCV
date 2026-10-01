/**
 * @file csi_event_egress.h
 * @brief Where a committed csi_event goes after the chokepoint: the SD event
 *        log, and the MQTT `events` / `tamper` topics, in id order, with the
 *        delivery watermark the reconnect backfill keeps (backlog F78).
 *
 * The canary-wap's counterpart of the canary PIO tree's
 * src/csi_event_egress.cpp, on the same pure rules:
 * common/csi/src/csi_event_backfill.h's Planner (staged copy, byte-gated by
 * check_csi_sync.sh), host-tested in firmware/tests_host against a model of
 * Home Assistant's replay gate and here, against the real SD event log and
 * CSI library, in tests_host/test_wap_event_egress.cpp.
 *
 * Threading. csi_event_on_committed() (csi_integration.cpp) can run on the
 * NimBLE host task (ble.scout's admit can close a bundle there), under the
 * chokepoint's commit lock. So on_committed() only COPIES the row into a
 * bounded FreeRTOS queue: it never blocks (a full queue drops the row,
 * counted), never publishes, never touches the card or the watermark.
 * pump(), on the loop task, is the one place that logs, publishes and moves
 * the watermark. Before this split the hook published live and appended to
 * the card on whichever task committed, while the backfill ran on the loop
 * task, and both wrote the watermark with no lock between them.
 *
 * Order. Home Assistant's replay gate refuses an event_id below the last
 * one it verified, so a row that overtakes an older one gets the older one
 * refused. A row goes out live only when nothing older waits:
 *   - a row the SD log keeps (a ring row: stateless and ambient commits) is
 *     appended to the card; if anything older is still waiting, on the card
 *     or in RAM, it waits on the card and the backfill sends it in turn;
 *   - a row the card does not keep (a closed bundle, which never enters the
 *     ring, backlog F77; any row while the card is not open; a row whose
 *     append failed) waits in RAM, at most kHeldMax of them, oldest dropped
 *     first, while anything older waits or the link is down, and goes out in
 *     id order with the card's rows. "Anything older" includes a card that is
 *     not open but may hold older rows: from boot until its log first opens
 *     (a slow card mounts after boot), and from a close while rows waited on
 *     it until it opens again (an SD error's remount). That wait is bounded
 *     by kCardWaitMs; past it the RAM rows go, and rows still on a card that
 *     comes back later are skipped (HA would refuse them).
 *   - an ambient row (csi_event.h: "never persisted, drives live UI only")
 *     is never held in RAM: one that cannot go out now is dropped, counted.
 *     With a card in, ambient rows are still logged and replayed like any
 *     ring row (open question, filed with the sweep).
 * On reconnect the backfill walks the card from the watermark, a bounded
 * amount per loop pass, and, while the card stays open or comes back within
 * kCardWaitMs, nothing committed meanwhile overtakes it. With no broker
 * configured, or after the broker changes (csi_mqtt::destination_epoch:
 * host, port, user or topic prefix), the rows waiting are owed to nobody,
 * so the new broker is not sent stale history (the planner's rule, and the
 * canary's).
 *
 * The watermark is the highest event id handed to the broker. Across a
 * reboot it comes back from a CEILING in NVS (csi_mqtt::NVS_KEY_DELIVERED,
 * F47), written before an id is handed over and capped at the id
 * allocator's persisted floor (csi_event_backfill::ceiling_for); begin()
 * restores it with Planner::begin's rule (csi_event_backfill::restore). A
 * row held in RAM never writes the ceiling while card rows wait: route()
 * does not hand it to the planner then, and a row whose card append failed
 * (which the planner's not-on-card route would persist before handing it
 * over) has that write held back until it goes. The backfill's own
 * hand-over still writes it up to kStride ids ahead of the row it sends
 * (F47's trade), so a reboot mid-backfill can skip up to kStride card rows.
 */

#ifndef SECURACV_WAP_CSI_EVENT_EGRESS_H
#define SECURACV_WAP_CSI_EVENT_EGRESS_H

#include <csi_event.h>
#include <stdint.h>

#include "csi_event_backfill.h"   /* staged copy — the planner's Stats */

namespace csi_event_egress {

/* How long rows the card does not keep wait for a card that is not open but
 * may hold older rows (see Order above). An SD error's remount comes up to
 * SD_RECHECK_INTERVAL_MS (30 s, hardware_state.h) later, plus the mount
 * itself; a boot-time mount that outlives its 4 s budget is adopted on a
 * later loop pass. A device with no card waits this long once per boot. */
constexpr uint32_t kCardWaitMs = 45000;

/* Setup, loop task: csi_integration::init calls it once the event-id floor
 * is back from NVS (it reads csi_integration::event_id_floor_stored) and
 * before the modules register (register_v1_modules), so any row they commit
 * finds the queue. Allocates the egress and its queue and restores the
 * delivery watermark. Once per boot: a second call is a no-op. */
void begin();

/* The chokepoint's commit hook, after csi_integration's privacy gate. Any
 * task, under the commit lock: copies the row into the queue, nothing
 * else. */
void on_committed(uint32_t                  event_id,
                  const char*               module_id,
                  const char*               type_name,
                  csi_event_category_t      category,
                  csi_privacy_class_t       privacy,
                  const csi_event_values_t* values);

/* Loop task, every pass (csi_mqtt::loop): the card's state and the broker's
 * (no broker, or a changed one: nothing waiting is owed), up to kPumpBudget
 * queued rows (each one's tamper bridge first, then the row), one bounded
 * backfill pass, then rows waiting in RAM once nothing older waits on the
 * card. */
void pump();

/* The delivery watermark (diagnostics and host tests). */
uint32_t watermark();

struct Stats {
  uint32_t dropped;          /* commits the full queue refused */
  uint32_t held_dropped;     /* rows the RAM hold dropped, oldest first */
  uint32_t ambient_dropped;  /* ambient rows that could not go out at once (never held) */
  csi_event_backfill::Stats planner;
};
Stats stats();

#ifdef CSI_TEST_HOST_BUILD
/* Host tests only: forget this "boot"'s RAM state, to simulate a reboot. */
void test_reset();
#endif

}  // namespace csi_event_egress

#endif  // SECURACV_WAP_CSI_EVENT_EGRESS_H
