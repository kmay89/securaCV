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

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_EVENT_EGRESS_H */
