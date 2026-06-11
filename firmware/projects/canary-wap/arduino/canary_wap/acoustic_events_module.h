/**
 * @file acoustic_events_module.h
 * @brief csi_event module for PDM-microphone acoustic detections, plus the
 *        emit helpers the audio callbacks in canary_wap.ino call.
 *
 * Routes smoke/CO/knock/doorbell/glass-break detections and mic mute
 * toggles through the csi_event chokepoint so they land in the Today
 * timeline, the SD event log, and the MQTT /events stream alongside
 * presence events — with the same per-event allow-list enforcement.
 * Every payload is privacy-bounded by construction: a state tag, a
 * confidence word, and a 10-minute time bucket. No audio content exists
 * to leak (see securacv_audio.h).
 *
 * The implementation is compiled out with FEATURE_ACOUSTIC_EVENTS; the
 * declarations stay visible so gated call sites type-check either way.
 */

#ifndef SECURACV_ACOUSTIC_EVENTS_MODULE_H
#define SECURACV_ACOUSTIC_EVENTS_MODULE_H

#include "csi_event.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Module descriptor for csi_module_register(). */
const csi_module_t* acoustic_events_module(void);

/**
 * Emit one acoustic detection. event_type is an audio_event_type_t value
 * from securacv_audio.h (taken as uint8_t so this header doesn't depend
 * on it); confidence is the matcher's 0..100 score, mapped to the
 * chokepoint's tentative/observed/confirmed vocabulary. Call from the
 * main loop only (the audio event callback runs there).
 */
void acoustic_events_emit_detection(uint8_t event_type, uint8_t confidence);

/**
 * Emit a mic hard-mute toggle so the timeline shows when the device
 * stopped/resumed listening — the same "muted before or in response?"
 * question the witness-chain record answers, surfaced where users look.
 */
void acoustic_events_emit_mute(bool muted);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_ACOUSTIC_EVENTS_MODULE_H */
