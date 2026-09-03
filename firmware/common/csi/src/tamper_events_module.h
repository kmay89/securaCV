/**
 * @file tamper_events_module.h
 * @brief csi_event module for the device's own integrity story (system.integrity).
 *
 * The Home Assistant integration has spoken a per-kind tamper vocabulary
 * since day one (custom_components/securacv/const.py: power_loss, sd_remove,
 * sd_error, watchdog, unexpected_reboot, …) — and no WAP firmware ever
 * emitted one: tamper reached the wire only as a bare flag. This module
 * gives the kinds a producer, THROUGH the chokepoint, so a tamper rides the
 * same rails as every other event: the ring, /api/events/today (open
 * bundles first), /api/csi/stream, the Ed25519 witness chain, the SD event
 * log, and csi_mqtt's HA republish — zero serializer edits, the kind rides
 * `state`.
 *
 * DOCTRINE (const.py's own): emit ONLY kinds this hardware can truly
 * detect. No Canary lane ships an accelerometer, an enclosure switch, a
 * tamper GPIO wired in a shipping profile, or a GPS-jamming detector — so
 * `motion`, `enclosure`, `gpio`, and `gps_jamming` are never emitted here,
 * and advertising a tamper sensor that cannot fire would be a false
 * promise of protection. What it CAN say honestly:
 *
 *   unexpected_reboot — last reset was a crash (panic)
 *   watchdog          — last reset was a watchdog (a system hang)
 *   power_loss        — last reset was a brownout
 *   sd_error          — the SD state machine went MOUNTED → ERROR
 *   sd_remove         — the card left while mounted (MOUNTED → ABSENT)
 *
 * Kind ids verbatim from const.py — csi_mqtt republishes committed events
 * and the HA per-type sensors key on these exact strings.
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

/* Tests / diagnostics only: forget boot/SD memory so transitions replay. */
void tamper_events_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_TAMPER_EVENTS_MODULE_H */
