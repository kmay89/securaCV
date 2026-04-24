/*
 * SecuraCV Canary — CSI Feature Extractor
 * Version 0.1.0
 *
 * Converts a stream of CSI frames (provided by csi_hal) into a fixed
 * 32-dim int8 feature vector per 1-second window. Implements the contract
 * defined in firmware/common/rf_sensing/csi.h.
 *
 * ALLOCATION-FREE: all state lives in static buffers, sized at compile time.
 *
 * FEATURES PRODUCED (matches csi.h layout):
 *   [ 0..7]  Subcarrier amplitude variance (8 frequency bands)
 *   [ 8..11] Phase-difference Doppler (4 sign-aware bands)
 *   [12..19] Breathing/micro-motion FFT (0.1–0.5 Hz, 8 bins)
 *   [20..23] RSSI stats over window: mean, std, max, min
 *   [24..27] Frame-rate health: frames, dropped_estimate, channel, bw_code
 *   [28..31] Reserved for v2.1 (C6 sounding), v2.2 (phase unwrap)
 */

#ifndef SECURACV_CSI_FEATURES_H
#define SECURACV_CSI_FEATURES_H

#include <stdint.h>
#include "../../../../common/rf_sensing/csi.h"

namespace csi_features {

/* Reset the aggregator at the start of a new window. */
void reset();

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
