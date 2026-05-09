/**
 * @file csi_event_log.h
 * @brief Append-only line-delimited JSON log of committed CSI events on SD.
 *
 * Without this layer the firmware's view of "today" lives entirely in
 * the in-memory ring (csi_event_recent), which is wiped by a power
 * cycle. The dashboard's Today sheet shows nothing after a reboot,
 * and the MQTT bridge re-publishes whatever happens NEXT but loses
 * everything between the last successful HA update and the outage.
 *
 * Wire format (one event per line, terminated by '\n'):
 *
 *   {"id":12345,"first":<ms>,"last":<ms>,"cat":"event","priv":"p0",
 *    "module":"core.presence","type":"presence_changed","bundled":1,
 *    "state":"active","conf":"observed","motion":62,"breathing":18,
 *    "bpm":0,"dur":7,"tb":54,"dom":"motion","dismissed":0}
 *
 * Path: /EVENTS/today.ndjson (sibling of /WITNESS, /HEALTH, /CHAIN).
 *
 * Lifecycle:
 *   - load_into_ring() runs once at boot after sd mount; reads
 *     all committed records and re-injects them via csi_event_inject
 *     so /api/events/today returns yesterday's tail before any new
 *     event commits this boot.
 *   - append() is called from csi_event_on_committed() in
 *     csi_integration.cpp; one fsync-style flush per event so a
 *     hard power cut at most loses the in-flight line.
 *   - iterate_since(event_id, cb) is called from csi_mqtt's
 *     MQTT_EVENT_CONNECTED handler to replay any events that
 *     committed during an HA outage so HA's history backfills
 *     instead of just resuming.
 *
 * Privacy: only fields the chokepoint already cleared for export
 * land in the log. Raw feature vectors never touch SD here. P2 is
 * still gated upstream so this layer never sees a P2 record unless
 * the user explicitly raised the privacy ceiling. */

#ifndef SECURACV_CSI_EVENT_LOG_H
#define SECURACV_CSI_EVENT_LOG_H

#include <csi_event.h>
#include <stddef.h>
#include <stdint.h>

namespace csi_event_log {

/** Path prefix on SD; "/EVENTS/today.ndjson" today, daily rotation
 *  is reserved for a future commit. */
constexpr const char* LOG_PATH = "/EVENTS/today.ndjson";

/** Hard cap on file size. At the per-module hourly ceiling defined
 *  in the chokepoint (~6/hr) and ~64 bytes per line we'd emit ~10 KB
 *  per day on a normal home. 256 KB gives 25+ days of headroom
 *  before head-truncation kicks in. */
constexpr size_t MAX_BYTES = 256u * 1024u;

/** Cap on how many events backfill replays in one shot — bounds the
 *  HA "you missed N events" burst that follows a long outage. */
constexpr size_t BACKFILL_MAX = 64;

/**
 * Cold-boot init. Idempotent. Creates /EVENTS/ if missing. No-op when
 * the SD card isn't mounted (hooks for sd_is_available are sketch-side
 * so we accept a callable predicate rather than depend on hardware_state.h
 * here). Returns true on success / disabled-without-SD; false only on
 * filesystem error worth surfacing.
 */
bool init();

/**
 * Append one record to the log. Best-effort: returns false if the SD
 * card is unavailable, the file rolled over and we couldn't truncate,
 * or the write returned short. Callers (the chokepoint hook) should
 * NOT propagate the failure into csi_event_on_committed's return —
 * losing the on-disk copy must never block the live event from
 * reaching the dashboard or the MQTT bridge.
 */
bool append(const csi_event_record_t* rec);

/**
 * Read the on-disk log and call csi_event_inject for each parseable
 * line. Caps at CSI_EVENT_RING_CAP rows so a long log doesn't trample
 * a fresh boot's first events. Returns the number of records loaded.
 */
size_t load_into_ring();

/**
 * Iterate events with id strictly greater than `since_event_id` and
 * call `cb(record, user)` for each, oldest-first, up to BACKFILL_MAX.
 * Stops on the first cb that returns false (so the MQTT publisher
 * can bail mid-replay if the broker disconnects again).
 */
typedef bool (*iterate_cb_t)(const csi_event_record_t* rec, void* user);
size_t iterate_since(uint32_t since_event_id, iterate_cb_t cb, void* user);

}  /* namespace csi_event_log */

#endif  /* SECURACV_CSI_EVENT_LOG_H */
