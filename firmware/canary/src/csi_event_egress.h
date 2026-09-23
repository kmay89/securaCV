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
 * Persistence: the MQTT layer's bounded offline queue (F9) carries events
 * across a broker outage. There is no SD event log / reconnect backfill on
 * this tree yet (the canary-wap's csi_event_log is SD.h-bound and single-
 * writer rules differ here) — a recorded follow-up, not a silent gap.
 *
 * Event-id continuity: csi_event_on_id_advance persists the allocator's
 * floor to NVS every 10 ids and begin() restores it, so ids stay monotonic
 * across reboots (HA's replay detection keys on them), exactly as the
 * canary-wap does.
 */

#ifndef SECURACV_CSI_EVENT_EGRESS_H
#define SECURACV_CSI_EVENT_EGRESS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Setup, loop task, before the CSI modules register: restore the event-id
 * floor from NVS and — on FEATURE_HA_MQTT builds — hand the witness
 * identity to device_signature and create the egress queue. */
void csi_event_egress_begin(void);

/* Loop task, after mqtt_loop(): publish up to a few queued commits on
 * securacv/<id>/events (and a system.integrity tamper on
 * securacv/<id>/tamper). A no-op without FEATURE_HA_MQTT. */
void csi_event_egress_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_EVENT_EGRESS_H */
