/**
 * @file vault_events_module.h
 * @brief csi_event module for the sealed-snapshot vault's lifecycle events.
 *
 * The vault seals camera frames on life-safety acoustic triggers
 * (vault_snapshot.cpp). NO image data ever crosses the CSI chokepoint —
 * this module chains only the EXISTENCE and INTEGRITY of a sealed frame:
 * the note carries "<trigger tag> <first 16 hex of the ciphertext's
 * SHA-256>" (verifiable off-device with `tools/unseal_snapshot.py
 * inspect`). Everything else about the frame stays inside the encrypted
 * .svlt file on SD. The event is ANOMALY-category (bypasses quiet hours,
 * like the acoustic life-safety detections that trigger it) and stateless
 * (bypasses the bundler, so every sealed file gets its own hash row).
 *
 * Loop-task-only, like every csi_event emitter (the chokepoint's bundler
 * and ceilings are single-threaded on the main loop).
 */

#ifndef SECURACV_VAULT_EVENTS_MODULE_H
#define SECURACV_VAULT_EVENTS_MODULE_H

#include "csi_event.h"   /* values/emit/categories + module types */

#ifdef __cplusplus
extern "C" {
#endif

/* Module manifest for csi_module_register(). */
const csi_module_t* vault_events_module(void);

/* Emit one frame_sealed event. trigger_tag is vault_logic::trigger_tag()
 * output ("smoke"/"co"/"glass"/"test"); ct_hash_hex16 is the first 16 hex
 * chars of SHA-256(ciphertext), NUL-terminated. Both land in the note as
 * "<tag> <hash16>". */
void vault_events_emit_frame_sealed(const char* trigger_tag,
                                    const char* ct_hash_hex16);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_VAULT_EVENTS_MODULE_H */
