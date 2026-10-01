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
 *   - poll(), append_line() and read_at() are the card half of the events
 *     egress (csi_event_egress.cpp): the egress's csi_event_backfill::Planner
 *     appends every committed ring row and walks the log to replay what the
 *     broker has not seen, in id order (backlog F78). One open and close
 *     per line, so a hard power cut at most loses the line in flight; a
 *     torn last line is sealed with '\n' before the next append, and the
 *     parser refuses it.
 *
 * Threading: LOOP TASK ONLY. Every entry point below runs on the Arduino
 * loop task: the egress's pump and csi_integration's loop and init. The
 * commit hook, which can run on the NimBLE host task (ble.scout), only
 * queues the row for the pump (csi_event_egress.h), so a commit never
 * touches the card. queue_dismissal() is the one call safe from any task.
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

#include "csi_event_backfill.h"   /* staged copy — AppendResult, the planner's card contract */

namespace csi_event_log {

/** Path prefix on SD; "/EVENTS/today.ndjson" today, daily rotation
 *  is reserved for a future commit. */
constexpr const char* LOG_PATH = "/EVENTS/today.ndjson";

/** Hard cap on file size. At the per-module hourly ceiling defined
 *  in the chokepoint (~6/hr) and ~64 bytes per line we'd emit ~10 KB
 *  per day on a normal home. 256 KB gives 25+ days of headroom
 *  before head-truncation kicks in. */
constexpr size_t MAX_BYTES = 256u * 1024u;

/**
 * Cold-boot init. Idempotent. Creates /EVENTS/ if missing. No-op when
 * the SD card isn't mounted (hooks for sd_is_available are sketch-side
 * so we accept a callable predicate rather than depend on hardware_state.h
 * here). Returns true on success / disabled-without-SD; false only on
 * filesystem error worth surfacing.
 */
bool init();

/** What poll() saw. */
enum class CardChange : uint8_t {
  kUnchanged,  /* same state as the last poll */
  kOpened,     /* a (new) card's log is usable: size and last row id reported */
  kClosed,     /* the log stopped being usable (card gone, or a broken rewrite) */
};

/**
 * Re-evaluate the card; the egress calls it once per pump pass, before any
 * other card call. Cheap while nothing changes (the card type and the mount
 * worker's flag, no file-system call). A card is looked at once while it
 * stays in: one that is not this device's (a canary base's owner file) or
 * whose log cannot be opened stays closed until it is pulled. On kOpened,
 * `*size` is the log's size (0 when it does not exist yet) and `*tail_id`
 * the id of its last row: the last whole line that is not a dismissal,
 * looked for up to TAIL_SCAN_MAX bytes back (0 when there is none).
 */
CardChange poll(uint32_t* size, uint32_t* tail_id);

/**
 * Append one line (with its '\n') to the log, only while poll() has it open.
 * Past MAX_BYTES the oldest quarter is dropped first (to a line start), and
 * `cut` says how many bytes; `size` is the log's size afterwards, whatever
 * happened. A torn last line is sealed with '\n' first. A rewrite whose
 * rename failed leaves the survivors in the .tmp file for the next mount's
 * reconcile, so the log closes until the card is pulled.
 */
csi_event_backfill::AppendResult append_line(const char* line, size_t len);

/** Read up to `cap` bytes at byte offset `off`; the count read, 0 on error. */
size_t read_at(uint32_t off, char* buf, size_t cap);

/** How far back from the end poll() looks for the log's last row. */
constexpr size_t TAIL_SCAN_MAX = 16u * 1024u;

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
 * owner file) is left alone, as poll() leaves it.
 *
 * The rows it restores are the log's newest, not today's: see "today is a
 * name, not a bound" above.
 *
 * It restores what the log holds, and the log holds less than the Today
 * sheet shows live: the egress logs only ring rows (csi_event_find), and a
 * bundle the bundler closes (csi_bundler.cpp commit_closed, through the
 * chokepoint's csi_event_commit_bundle_) never enters the ring, so closed
 * bundles are neither on the card nor restored (backlog F77). Only direct (stateless /
 * ambient) commits are. Found 2026-09 while wiring this. Since backlog F46
 * a closed bundle takes its event id from the same persisted allocator as
 * every other row, so the old reason not to log it (ids from a separate,
 * unpersisted space) is gone; putting it in the ring is still open.
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
 * queues; flush_dismissals(), on the loop task where append_line() runs,
 * appends the dismissed ring row as one more line in the usual format, with
 * "dismissed":1. That line adds one fact to the card: that the owner
 * acknowledged this event, and roughly when (by where it falls in the log;
 * it carries no time of its own beyond the record's). It is the owner's own
 * action, kept on the owner's own card and not replayed to MQTT: local, as
 * Invariant IV (local ownership, spec/invariants.md) asks. Best-effort: with
 * no open log (no card, a card that is not ours), a log at MAX_BYTES (a
 * dismissal never cuts the log; the next committed row's append does) or a
 * failed write, the dismissal holds for this boot only. queue_dismissal()
 * is false when the queue (8) is full. The egress's backfill never replays
 * a dismissal line (csi_event_egress.cpp send_backfill).
 *
 * The egress always logs "dismissed":0 (the original), even when the ring
 * row was dismissed between the commit and the hook's copy of it, so only a
 * flush_dismissals() line ever says 1 (csi_event_log.cpp, is_dismissal()).
 */
bool queue_dismissal(uint32_t event_id);
size_t flush_dismissals();

#ifdef CSI_TEST_HOST_BUILD
/** Host tests only: forget this "boot"'s RAM state (the load latch and the
 *  open card), to simulate a reboot. */
void test_rearm_load();
#endif

}  /* namespace csi_event_log */

#endif  /* SECURACV_CSI_EVENT_LOG_H */
