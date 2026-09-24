/**
 * @file tamper_events_module.h
 * @brief csi_event module for the device's own integrity story (system.integrity).
 *
 * The Home Assistant integration has spoken a per-kind tamper vocabulary
 * since day one (custom_components/securacv/const.py: power_loss, sd_remove,
 * sd_error, watchdog, unexpected_reboot, …) — and no WAP firmware ever
 * emitted one: tamper reached the wire only as a bare flag. This module
 * gives the kinds a producer, THROUGH the chokepoint, so a tamper rides the
 * same rails as every other event: the ring, /api/events/today,
 * /api/csi/stream, the Ed25519 witness chain, the SD event log, and
 * csi_mqtt's HA republish — zero serializer edits, the kind rides `state`.
 * Sealed IMMEDIATELY: every accepted emit force-closes its bundle
 * (csi_bundler_flush_key), because a record whose subject is "the power
 * just failed" cannot afford the bundler's two-minute RAM buffer.
 *
 * DOCTRINE (const.py's own): emit ONLY kinds this hardware can truly
 * detect. No Canary lane ships an accelerometer or a GPS-jamming detector,
 * so `motion` and `gps_jamming` are never emitted here, and advertising a
 * tamper sensor that cannot fire would be a false promise of protection.
 * `enclosure` is emitted only when a host feeds a REAL contact state
 * (tamper_events_watch_contact below): a build with FEATURE_TAMPER_GPIO=1
 * reading a reed/hall switch on the board map's TAMPER_PIN_DEFAULT through
 * contact_tamper.h's debounce. No shipped profile sets that flag until the
 * pin is bench-validated, and a build that feeds no contact state never
 * emits the kind. `gpio` stays unemitted on purpose: it names the
 * mechanism, and would be a second kind for the one physical fact
 * `enclosure` already names. What it CAN say honestly:
 *
 *   unexpected_reboot — last reset was a crash (panic)
 *   watchdog          — last reset was a watchdog (a system hang)
 *   power_loss        — last reset was a brownout
 *   sd_error          — the SD state machine went MOUNTED → ERROR
 *   sd_remove         — the card left while mounted (MOUNTED → ABSENT)
 *   enclosure         — the enclosure contact went CLOSED → OPEN
 *
 * Kind ids verbatim from const.py — csi_mqtt republishes committed events
 * and the HA per-type sensors key on these exact strings. The set is a
 * gated vocabulary: scripts/lint_dictionary_sync.py requires the kind
 * literals in tamper_events_module.cpp and the table above to equal
 * spec/witness_dictionary.json's system_integrity_kinds, each one a
 * const.py TAMPER_* word with its own HA sensor. A new kind starts in the
 * dictionary.
 *
 * Hosts: the canary-wap sketch feeds its SdState; the canary PIO tree
 * feeds its storage lane's live state through
 * sd_mount_policy::sd_state_for_tamper (ERROR is a mounted card given up
 * on after consecutive write failures, ABSENT is anything else not
 * mounted), so both narrate the same SD stories from the same two
 * triggers.
 *
 * The module owns every transition rule; the host's main loop feeds it the
 * facts only that loop can see together (reset classification + SD state)
 * via tamper_events_watch(). Loop-task-only, like every csi_event emitter.
 */

#ifndef SECURACV_TAMPER_EVENTS_MODULE_H
#define SECURACV_TAMPER_EVENTS_MODULE_H

#include "csi_event.h"   /* values/emit/categories + module types */

#ifdef __cplusplus
extern "C" {
#endif

/* Module manifest for csi_module_register(). */
const csi_module_t* tamper_events_module(void);

/* Feed the watcher once per main loop. Boot classification emits at most
 * once per boot (on the first call, from the reset facts); the SD kinds
 * emit on state-machine TRANSITIONS only — the first call adopts the
 * current SD state silently, so booting with no card is not a removal.
 * A host with no SD state machine feeds a constant (the watcher adopts it
 * and never emits an SD kind) — never invent a detector.
 * Safe to call before csi init: an emit before the module is registered is
 * silently dropped by the chokepoint, and the boot classification is
 * re-attempted until one emit is accepted. */
void tamper_events_watch(int reset_was_crash, int reset_was_watchdog,
                         int reset_was_brownout, uint8_t sd_state);

/* Enclosure contact states for tamper_events_watch_contact(). Pinned
 * numbers: hosts pass them across a plain-typed ABI. */
enum {
  TAMPER_CONTACT_NONE   = 0,  /* no contact input on this build */
  TAMPER_CONTACT_CLOSED = 1,  /* enclosure shut (debounced) */
  TAMPER_CONTACT_OPEN   = 2,  /* enclosure open (debounced) */
};

/* Feed the enclosure contact's DEBOUNCED state (contact_tamper.h owns the
 * debounce) once per main loop, on builds that read one. Same transition
 * doctrine as the SD kinds: the first call adopts silently (booting with
 * the lid off is a configuration), `enclosure` emits on CLOSED → OPEN
 * only, and OPEN → CLOSED ends the standing condition without a row. A
 * host with no contact input never calls this, or feeds
 * TAMPER_CONTACT_NONE — either way the kind is never emitted. Loop-task
 * only. */
void tamper_events_watch_contact(uint8_t contact_state);

/* The tamper condition standing RIGHT NOW, as a kind word — or "" when the
 * device has nothing to confess. Level state for wires that need the
 * present tense (the /api/events/today envelope): every tamper row is
 * sealed-and-closed the moment it commits (durability over bundling), so
 * a reader can no longer infer "still standing" from an open bundle.
 * Boot kinds stand for the whole boot — "this boot began with a crash"
 * stays true until the next one. SD kinds clear on recovery: the card
 * coming back ends that condition. `enclosure` stands while the contact
 * reads open and ends when it closes. Precedence when several stand:
 * enclosure (a physical intrusion happening now) over the SD story (newer
 * news, actionable) over the boot story; each lower story resurfaces when
 * the one above it ends. */
const char* tamper_events_active_kind(void);

/* Tests / diagnostics only: forget boot/SD/contact memory so transitions
 * replay. */
void tamper_events_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_TAMPER_EVENTS_MODULE_H */
