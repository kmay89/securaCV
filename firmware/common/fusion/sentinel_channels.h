/**
 * @file sentinel_channels.h
 * @brief sentinel.channels — pure adapters from raw sensor readings to a
 *        fusion `Vote`. Header-only, no Arduino / pin dependency.
 *
 * The fusion engine (sentinel_fusion.h) is deliberately blind to hardware: it
 * consumes `Vote`s. This header is the board-agnostic translation layer that
 * turns each sensor driver's native output into the coarse Weak/Strong/Denied/
 * None vocabulary — the ONE place that decides "what counts as evidence" per
 * channel, so it is unit-testable without a bench.
 *
 * A driver's board wiring (which GPIO the PIR is on, which UART the radar
 * speaks) lives under boards/ and is bound in the project; only the already-
 * read scalars arrive here. Keeping this in common/fusion (not the project)
 * means the adapters are covered by the same host tests as the scorer.
 *
 * The `Denied` mappings are the fraud-detection primitive in action: a stalled
 * radar UART or a blinded light sensor becomes evidence of evasion, not silence.
 */

#ifndef SECURACV_FUSION_SENTINEL_CHANNELS_H
#define SECURACV_FUSION_SENTINEL_CHANNELS_H

#include "sentinel_fusion.h"

#include <stdint.h>
#include <stdbool.h>

namespace securacv {
namespace fusion {
namespace channels {

/**
 * PIR (passive infrared) motion pin.
 *  @param motion_now   the pin is asserting motion this instant.
 *  @param since_edge_ms time since the last motion edge (for the settle tail).
 *  @param settle_ms    how long after motion stops PIR still votes Weak.
 * Live motion is Strong; a recent-but-settled edge lingers as Weak (bodies
 * pause); long-quiet is None. PIR cannot be "Denied" — an idle PIR is
 * indistinguishable from a working one, so it never claims to be blinded.
 */
inline Vote pir_vote(bool motion_now, uint32_t since_edge_ms, uint32_t settle_ms) {
  if (motion_now) return Vote::Strong;
  if (since_edge_ms <= settle_ms) return Vote::Weak;
  return Vote::None;
}

/**
 * 60GHz radar presence.
 *  @param uart_stalled the radar UART produced no frame within its stall window
 *                      — the module is unplugged / jammed / dead: Denied.
 *  @param present      the radar asserts a target in range.
 *  @param settling     a just-cleared edge (brief tail before None).
 */
inline Vote radar_vote(bool uart_stalled, bool present, bool settling) {
  if (uart_stalled) return Vote::Denied;
  if (present) return Vote::Strong;
  if (settling) return Vote::Weak;
  return Vote::None;
}

/**
 * WiFi CSI device-free motion/breathing core verdict.
 *  @param motion_confirmed the CSI core reports confirmed motion/breathing.
 *  @param motion_observed  a weaker single-window observation.
 * CSI going quiet is genuine None (an empty room really does perturb nothing).
 */
inline Vote csi_vote(bool motion_confirmed, bool motion_observed) {
  if (motion_confirmed) return Vote::Strong;
  if (motion_observed) return Vote::Weak;
  return Vote::None;
}

/**
 * Aggregate device-count channels (WiFi-RF, BLE). Privacy-preserving: the
 * caller passes only a COUNT (never a MAC), from canary-wap's rf_presence.
 *  @param device_count   unique devices seen this window.
 *  @param weak_threshold  >= this many -> Weak.
 *  @param strong_threshold>= this many -> Strong (a phone right at the door).
 */
inline Vote count_vote(uint8_t device_count, uint8_t weak_threshold,
                       uint8_t strong_threshold) {
  if (strong_threshold != 0 && device_count >= strong_threshold) return Vote::Strong;
  if (weak_threshold != 0 && device_count >= weak_threshold) return Vote::Weak;
  return Vote::None;
}

/**
 * Ambient-light corroboration / blinding.
 *  @param blinded    the sensor is saturated or pinned dark in a way consistent
 *                    with being covered while the scene shouldn't be: Denied.
 *  @param abs_delta_lux magnitude of the light change vs the rolling baseline.
 *  @param weak_delta a change this large -> Weak corroboration (shadow crossing,
 *                    a light toggled, a hand over the sill).
 * Light is corroboration only — it is never Strong on its own (a cloud is not a
 * person), so the strongest a genuine light change votes is Weak.
 */
inline Vote light_vote(bool blinded, uint16_t abs_delta_lux, uint16_t weak_delta) {
  if (blinded) return Vote::Denied;
  if (weak_delta != 0 && abs_delta_lux >= weak_delta) return Vote::Weak;
  return Vote::None;
}

/**
 * Door/window reed contact (Heavy tier). Open is unambiguous Strong evidence.
 * `fault` (a wire cut / magnet removed to defeat it) is Denied.
 */
inline Vote contact_vote(bool open, bool fault) {
  if (fault) return Vote::Denied;
  return open ? Vote::Strong : Vote::None;
}

/**
 * Enclosure tamper (accelerometer, Heavy tier). Any disturbance is Strong; a
 * sensor that has gone unresponsive when it should answer is Denied.
 */
inline Vote tamper_vote(bool disturbed, bool unresponsive) {
  if (unresponsive) return Vote::Denied;
  return disturbed ? Vote::Strong : Vote::None;
}

/** Coarse occupant bucket from a raw target count (radar). Never a track. */
inline Occupancy occupancy_from_count(uint8_t targets) {
  if (targets == 0) return Occupancy::Zero;
  if (targets == 1) return Occupancy::One;
  return Occupancy::TwoPlus;
}

/**
 * Coarse range band from a raw distance in centimeters (radar). The centimeter
 * value is consumed HERE and never leaves — only the band does.
 */
inline RangeBand range_from_cm(uint16_t distance_cm, uint16_t near_cm,
                               uint16_t mid_cm) {
  if (distance_cm == 0) return RangeBand::Unknown;
  if (distance_cm <= near_cm) return RangeBand::Near;
  if (distance_cm <= mid_cm) return RangeBand::Mid;
  return RangeBand::Far;
}

}  // namespace channels
}  // namespace fusion
}  // namespace securacv

#endif  // SECURACV_FUSION_SENTINEL_CHANNELS_H
