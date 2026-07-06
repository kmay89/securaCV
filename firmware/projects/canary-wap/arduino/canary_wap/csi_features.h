/*
 * SecuraCV Canary — CSI Feature Extractor
 * Version 0.1.0
 *
 * Converts a stream of CSI frames (provided by csi_hal) into a fixed
 * 32-dim int8 feature vector per 1-second window. Implements the contract
 * defined in csi_types.h.
 *
 * ALLOCATION-FREE: all state lives in static buffers, sized at compile time.
 *
 * FEATURES PRODUCED (matches csi.h layout):
 *   [ 0..7]  Per-subcarrier TEMPORAL amplitude variance, AGC-normalized,
 *            averaged per band (8 frequency bands) — motion energy
 *   [ 8..11] CFO-corrected band rotation (4 sign-aware bands) — the
 *            frame-pair phase rotation of each band RELATIVE to the
 *            all-band common rotation, so the ESP32's per-frame phase
 *            offset cancels and a static channel reads 0
 *   [12..19] Breathing spectrum (0.10–0.45 Hz, 8 Goertzel bins) measured
 *            over a cross-window envelope ring (~64 s), NOT within one
 *            window; bin i ↔ 0.10+0.05·i Hz ↔ (6+3·i) BPM. Zero until
 *            the ring holds ≥ BREATH_MIN_WINDOWS samples.
 *   [20..23] RSSI stats over window: mean, std, max, min
 *   [24..27] Frame-rate health: frames, dropped_estimate, channel, bw_code
 *   [28..31] Reserved for v2.1 (C6 sounding), v2.2 (phase unwrap)
 */

#ifndef SECURACV_CSI_FEATURES_H
#define SECURACV_CSI_FEATURES_H

#include <stdint.h>
#include "csi_types.h"

namespace csi_features {

/* Reset the aggregator at the start of a new window. Keeps the
 * cross-window breathing envelope ring (it spans windows by design). */
void reset();

/* Full scrub: reset() plus the cross-window breathing envelope ring.
 * Call when sensing STOPS (csi_hal stop/deinit) so no envelope shape
 * survives a mute/stop boundary — same privacy contract as the
 * amplitude history. */
void reset_history();

/*
 * Accumulate one CSI frame into the current window.
 *   iq              interleaved int8 I/Q pairs (length = subcarrier_cnt * 2)
 *   subcarrier_cnt  number of valid subcarriers in iq
 *   rssi_dbm        frame RSSI (already filtered by csi_hal)
 *   channel         WiFi channel
 *   bw_code         0 = HT20, 1 = HT40
 */
void accumulate(const int8_t* iq, uint8_t subcarrier_cnt,
                int8_t rssi_dbm, uint8_t channel, uint8_t bw_code);

/*
 * Close the window and write the feature vector into `out`. The output is
 * mean-centered and clipped to int8 range. `frames_in_window` should be the
 * count of frames accumulated since the last reset(); it is copied into
 * the output's frames_in_window field for sanity checks downstream.
 */
void finalize(csi_features_t* out, uint32_t frames_in_window);

/* Lightweight introspection — used by csi_hal conformance. */
uint32_t current_frame_count();

}  /* namespace csi_features */

#endif  /* SECURACV_CSI_FEATURES_H */
