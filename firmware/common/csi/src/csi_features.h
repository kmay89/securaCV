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
 *            window; bin i ↔ 0.10+0.05·i Hz ↔ (6+3·i) BPM. The envelope
 *            is each subcarrier band's share of the AGC-normalized frame
 *            (gain-invariant), one sample per second on a fixed grid
 *            keyed by each window's close timestamp. Zero until the ring
 *            holds ≥ BREATH_MIN_WINDOWS samples.
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
 *
 * `close_ms` is the window's close time (millis()). The breathing ring is
 * fed on a FIXED 1 Hz grid keyed by that clock, not by how often the loop
 * manages to close a window: a close that lands in the same one-second
 * slot as the previous one is averaged into it (an early window), and a
 * close that skips slots first holds the previous sample across the gap
 * (a late window — loop stall, quiet radio, power gating). So the
 * Goertzel bank's one-sample-per-second assumption is true by
 * construction and the reported rate no longer drifts with loop latency.
 */
void finalize(csi_features_t* out, uint32_t frames_in_window,
              uint32_t close_ms);

/* Untimed close: assumes this window is exactly one second after the
 * previous one and pushes one envelope sample. For hosts without a clock
 * and for synthetic test windows; do not mix with the timed overload on
 * the same ring (it drops the grid anchor). */
void finalize(csi_features_t* out, uint32_t frames_in_window);

/* Hold the newest envelope sample `missed` more times — for untimed hosts
 * that know a gap elapsed (the timed finalize does this itself from the
 * timestamps). Capped at the ring size; a longer gap simply restarts the
 * spectrum from the held value. */
void note_missed_windows(uint32_t missed);

/* Envelope samples currently in the breathing ring (test introspection). */
size_t envelope_len();

/* Cadence bookkeeping for csi_stats_t — how far the loop's real pace was
 * from one window per second. Counted since boot; a sensing stop/start
 * re-anchors the grid but does not clear these. */
uint32_t held_windows();      /* grid slots filled by holding the previous sample */
uint32_t merged_windows();    /* closes averaged into an already-filled slot */
uint32_t window_period_ms();  /* mean close-to-close interval (ms); 0 until two timed closes */

/* Lightweight introspection — used by csi_hal conformance. */
uint32_t current_frame_count();

}  /* namespace csi_features */

#endif  /* SECURACV_CSI_FEATURES_H */
