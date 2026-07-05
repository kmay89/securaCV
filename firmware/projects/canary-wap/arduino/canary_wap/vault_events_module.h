/**
 * @file vault_events_module.h
 * @brief csi_event module for the sealed-snapshot vault's lifecycle events.
 *
 * The vault seals camera frames on life-safety acoustic triggers
 * (vault_snapshot.cpp). NO image data ever crosses the CSI chokepoint —
 * this module chains only the EXISTENCE and INTEGRITY of a sealed frame:
 * state_name carries the trigger tag, note carries the first 16 hex chars
 * of the ciphertext's SHA-256 (verifiable off-device with
 * `tools/unseal_snapshot.py inspect`). Everything else about the frame
 * stays inside the encrypted .svlt file on SD.
 *
 * Loop-task-only, like every csi_event emitter (the chokepoint's bundler
 * and ceilings are single-threaded on the main loop).
 */

#ifndef SECURACV_VAULT_EVENTS_MODULE_H
#define SECURACV_VAULT_EVENTS_MODULE_H

#include "csi_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Module manifest for csi_module_register(). */
const csi_module_t* vault_events_module(void);

/* Emit one frame_sealed event. trigger_tag is vault_logic::trigger_tag()
 * output ("smoke"/"co"/"glass"/"test"); ct_hash_hex16 is the first 16 hex
 * chars of SHA-256(ciphertext), NUL-terminated. */
void vault_events_emit_frame_sealed(const char* trigger_tag,
                                    const char* ct_hash_hex16);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_VAULT_EVENTS_MODULE_H */
