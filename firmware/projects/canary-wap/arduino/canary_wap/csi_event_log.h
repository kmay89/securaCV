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
 * Wire format (one event per line, terminated by '\n'; marshalled and
 * parsed by csi_event_log_line.h, shared with the canary PIO tree so a
 * tool reads either device's card the same way):
 *
 *   {"id":12345,"first":<ms>,"last":<ms>,"cat":"event","priv":"p0",
 *    "module":"core.presence","type":"presence_changed","bundled":1,
 *    "state":"active","conf":"observed","motion":62,"breathing":18,
 *    "bpm":0,"dur":7,"tb":54,"dom":"motion","dismissed":0}
 *
 * Path: /EVENTS/today.ndjson (sibling of /WITNESS, /HEALTH, /CHAIN).
 * A card whose /EVENTS holds an owner file (/EVENTS/owner) belongs to a
 * canary base, which binds its log to its witness key there. This device
 * leaves that log alone: it does not append to it, replay it or truncate it.
 *
 * "today" is a name, not a bound. The file has no daily rotation (see
 * LOG_PATH), and a record carries no date: `first`/`last` are the writing
 * boot's monotonic clock and `tb` is only the time of day (10-minute
 * bucket). So the reload below restores the log's newest rows whatever
 * their age, and the Today sheet shows a row from last week at its time of
 * day as if it were today's. Bounding the reload by date needs a date on
 * the record (a wall-clock stamp the device may not have); not done.
 *
 * Lifecycle:
 *   - load_into_ring() runs once per boot, after csi_integration has
 *     restored the privacy ceiling and the event-id floor (arm_load()) and
 *     the card is ready; it reads the log's tail and re-injects each record
 *     via csi_event_inject so /api/events/today returns the earlier boots'
 *     tail before any new event commits this boot.
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
 * Append one record to the log, as committed: its "dismissed" is written 0
 * whatever `rec` says (a dismissal is its own later line; see
 * queue_dismissal()). Best-effort: returns false if the SD
 * card is unavailable, the file rolled over and we couldn't truncate,
 * or the write returned short. Callers (the chokepoint hook) should
 * NOT propagate the failure into csi_event_on_committed's return —
 * losing the on-disk copy must never block the live event from
 * reaching the dashboard or the MQTT bridge.
 */
bool append(const csi_event_record_t* rec);

/** How far back from the end of the log load_into_ring() reads. The ring
 *  holds CSI_EVENT_RING_CAP (512) rows; at the usual 150-250 bytes a line,
 *  the last 128 KB covers it, and the boot never reads the whole 256 KB. */
constexpr size_t LOAD_TAIL_BYTES = 128u * 1024u;

/**
 * Put the tail of the on-card log back into the in-memory ring, so
 * /api/events/today shows the events from before a reboot, not an empty
 * sheet. Reads the last LOAD_TAIL_BYTES of the log oldest-first and hands
 * each parsed line to csi_event_inject (the canonical CSI library), which
 * is where the rules live: it refuses a line whose id this boot could
 * still allocate, a type this build does not register, a type above the
 * current privacy ceiling, a duplicate, and everything once a live event
 * has committed this boot; it re-applies the manifest's allow-list and
 * sanitizing; it fires no witness write, MQTT publish or SD append.
 *
 * It does nothing, and does not latch, until arm_load() says csi_integration
 * has restored the privacy ceiling and the event-id floor from NVS (without
 * the floor every line would be refused, and a latch then would leave the
 * ring empty for the boot). csi_integration::init runs only when the AP and
 * the HTTP server came up, so on a boot where they did not, nothing is ever
 * loaded. Once armed it runs at most once per boot: the first call that
 * finds the card ready latches; a call with no card returns 0 and leaves the
 * next call to try. If a live event is already in the ring by then (a card
 * that mounted late), it latches without reading the card, since inject
 * would refuse every row. A card that is not this device's (a canary base's
 * owner file) is left alone, as append() leaves it.
 *
 * The rows it restores are the log's newest, not today's: see "today is a
 * name, not a bound" above.
 *
 * It restores what the log holds, and the log holds less than the Today
 * sheet shows live: append() is fed from csi_event_find(), and a bundle
 * the bundler closes (csi_bundler.cpp run_commit_hooks) never enters the
 * ring, so closed bundles are neither on the card nor restored. Only
 * direct (stateless / ambient) commits are. Found 2026-09 while wiring
 * this; not changed here, since bundle ids also come from a separate,
 * unpersisted allocator (0x80000000 up, reset every boot).
 *
 * A dismissal survives the reboot: a record the tail also holds a
 * dismissal line for (flush_dismissals() below) is restored dismissed, and
 * the dismissal line itself is not a row. If the tail dismisses more ids
 * than the ring holds, or the set of them cannot be allocated, every row is
 * restored dismissed rather than any dismissal undone.
 *
 * Returns the number of rows injected (the ring keeps its newest
 * CSI_EVENT_RING_CAP of them). Read-only on the card.
 */
size_t load_into_ring();

/**
 * csi_integration::init calls this once the event-id floor (and the privacy
 * ceiling) are back from NVS, before its own load_into_ring(). Until then
 * load_into_ring() neither reads nor latches.
 */
void arm_load();

/**
 * Record on the card that the user dismissed `event_id` (csi_event_dismiss),
 * so load_into_ring() does not bring it back undismissed after a reboot.
 * queue_dismissal() is safe from any task (the HTTP handler) and only
 * queues; flush_dismissals(), on the loop task where append() runs, appends
 * the dismissed ring row as one more line in the usual format, with
 * "dismissed":1. That line adds one fact to the card: that the owner
 * acknowledged this event, and roughly when (by where it falls in the log;
 * it carries no time of its own beyond the record's). It is the owner's own
 * action, kept on the owner's own card and not replayed to MQTT: local, as
 * Invariant IV (local ownership, spec/invariants.md) asks. Best-effort like
 * append(): with no card, a card that is not ours, or a failed write, the
 * dismissal holds for this boot only. queue_dismissal() is false when the queue (8) is full.
 * iterate_since() does not replay dismissal lines.
 *
 * append() always writes "dismissed":0 (the original), even when the ring
 * row was dismissed between the commit and the hook's copy of it, so only a
 * flush_dismissals() line ever says 1 (csi_event_log.cpp, is_dismissal()).
 */
bool queue_dismissal(uint32_t event_id);
size_t flush_dismissals();

#ifdef CSI_TEST_HOST_BUILD
/** Host tests only: forget this "boot"'s load latch, to simulate a reboot. */
void test_rearm_load();
#endif

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
