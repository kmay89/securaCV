// canary/tincan/knock_codec.h — the knock: a tapped rhythm, carried across the
// house, replayed on the other wrist exactly as it was tapped. Pure,
// host-testable (no Arduino, no LVGL, no radio).
//
// This is the Tin Can's headline feature and its whole argument
// (docs/design/canary_tincan_kids_watch.md §5.2). The design refuses speech, so
// what the string carries is *rhythm*. That refusal is not a limitation dressed
// up as a virtue:
//
//   * A rhythm is not language. There is nothing here to moderate, filter,
//     store or leak — no slur can be encoded in eight taps, and the firmware
//     never learns what any pattern means.
//   * The vocabulary is invented by the children, not by us. Two siblings will
//     agree that three-short means "come to my room" inside a week, and it will
//     be *theirs*. Shipping a knock dictionary would take that away, so we
//     never ship one — there is deliberately no lookup table in this file.
//   * It is tiny. A full knock is at most 8 bytes on the wire.
//
// Wire format (payload of a LINK_KIND_KNOCK frame):
//
//   byte 0        : tap count, 1..KNOCK_MAX_TAPS
//   bytes 1..n-1  : inter-onset gaps, one byte each, in 40 ms units, 1..63
//
// The first tap is time zero, so a knock of n taps carries n-1 gaps. Gaps are
// quantized because a human wrist cannot reproduce 3 ms of precision and
// pretending otherwise just makes every knock unique-looking noise. 40 ms is
// comfortably under the ~50 ms at which two taps start to fuse perceptually,
// so a quantized knock still *feels* like the one that was tapped.

#ifndef CANARY_TINCAN_KNOCK_CODEC_H
#define CANARY_TINCAN_KNOCK_CODEC_H

#include <stddef.h>
#include <stdint.h>

namespace canary {
namespace tincan {

static constexpr uint8_t KNOCK_MAX_TAPS = 8;
static constexpr uint16_t KNOCK_QUANTUM_MS = 40;

// Below this, two contacts are one tap with a bouncing finger, not a rhythm.
// Debouncing at capture rather than at playback means the sender's own screen
// shows what will actually be sent.
static constexpr uint16_t KNOCK_MIN_GAP_MS = 60;

// The longest gap that fits a 1-byte quantized value (63 * 40 ms).
static constexpr uint16_t KNOCK_MAX_GAP_MS = 63 * KNOCK_QUANTUM_MS;  // 2520

// A knock must finish inside this window; the capture UI shows it filling.
static constexpr uint16_t KNOCK_WINDOW_MS = 2000;

static constexpr size_t KNOCK_MAX_WIRE = KNOCK_MAX_TAPS;  // 1 count + 7 gaps

// A captured or received knock: the count plus the gaps between taps.
struct Knock {
  uint8_t taps = 0;
  uint16_t gap_ms[KNOCK_MAX_TAPS - 1] = {0};

  bool valid() const { return taps >= 1 && taps <= KNOCK_MAX_TAPS; }

  // Total duration from first tap to last.
  uint32_t span_ms() const {
    uint32_t t = 0;
    for (uint8_t i = 0; i + 1 < taps; i++) t += gap_ms[i];
    return t;
  }
};

// Quantize a gap to the wire's 40 ms grid, clamped into the representable
// range. Rounds to nearest so a tap does not systematically drift early.
inline uint8_t knock_quantize(uint16_t gap_ms) {
  if (gap_ms < KNOCK_MIN_GAP_MS) gap_ms = KNOCK_MIN_GAP_MS;
  if (gap_ms > KNOCK_MAX_GAP_MS) gap_ms = KNOCK_MAX_GAP_MS;
  const uint16_t units = (uint16_t)((gap_ms + KNOCK_QUANTUM_MS / 2) / KNOCK_QUANTUM_MS);
  if (units < 1) return 1;
  if (units > 63) return 63;
  return (uint8_t)units;
}

inline uint16_t knock_dequantize(uint8_t units) {
  if (units < 1) units = 1;
  if (units > 63) units = 63;
  return (uint16_t)(units * KNOCK_QUANTUM_MS);
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

// Accumulates taps as they land. The runtime feeds it touch/IMU timestamps;
// everything about what counts as a tap lives here so it is testable.
struct KnockCapture {
  Knock knock;
  uint32_t first_ms = 0;
  uint32_t last_ms = 0;
  bool started = false;

  void reset() {
    knock = Knock();
    first_ms = 0;
    last_ms = 0;
    started = false;
  }

  // Offer a tap at `now_ms`. Returns true if it was recorded.
  //
  // Refuses when: the knock is already full, the tap is inside the debounce
  // interval, or it would fall outside the capture window. Refusing is not a
  // failure — the sender's UI simply stops growing, which is the honest signal
  // that the pattern is complete.
  bool tap(uint32_t now_ms) {
    if (!started) {
      started = true;
      first_ms = now_ms;
      last_ms = now_ms;
      knock.taps = 1;
      return true;
    }
    if (knock.taps >= KNOCK_MAX_TAPS) return false;
    if (now_ms - first_ms > KNOCK_WINDOW_MS) return false;

    const uint32_t gap = now_ms - last_ms;
    if (gap < KNOCK_MIN_GAP_MS) return false;  // finger bounce, not a tap

    uint16_t g = (gap > KNOCK_MAX_GAP_MS) ? KNOCK_MAX_GAP_MS : (uint16_t)gap;
    knock.gap_ms[knock.taps - 1] = g;
    knock.taps++;
    last_ms = now_ms;
    return true;
  }

  // Has the capture window closed? The runtime polls this to auto-send.
  bool complete(uint32_t now_ms) const {
    if (!started) return false;
    if (knock.taps >= KNOCK_MAX_TAPS) return true;
    return (now_ms - first_ms) > KNOCK_WINDOW_MS;
  }
};

// ---------------------------------------------------------------------------
// Wire
// ---------------------------------------------------------------------------

// Serialize. Returns bytes written, or 0 if the knock is unusable.
inline size_t knock_encode(const Knock& k, uint8_t* out, size_t cap) {
  if (!out || !k.valid()) return 0;
  const size_t need = (size_t)k.taps;  // 1 count byte + (taps-1) gap bytes
  if (cap < need) return 0;

  out[0] = k.taps;
  for (uint8_t i = 0; i + 1 < k.taps; i++) {
    out[1 + i] = knock_quantize(k.gap_ms[i]);
  }
  return need;
}

// Parse. Total: rejects anything malformed rather than salvaging it, because a
// half-understood knock replayed on a child's wrist is worse than none.
inline bool knock_decode(const uint8_t* in, size_t len, Knock& out) {
  if (!in || len < 1) return false;
  const uint8_t taps = in[0];
  if (taps < 1 || taps > KNOCK_MAX_TAPS) return false;
  if (len != (size_t)taps) return false;  // exact length, no trailing slack

  out = Knock();
  out.taps = taps;
  for (uint8_t i = 0; i + 1 < taps; i++) {
    const uint8_t units = in[1 + i];
    if (units < 1 || units > 63) return false;
    out.gap_ms[i] = knock_dequantize(units);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

// A pulse schedule: when to fire the motor, measured from playback start.
// The runtime just walks this against millis(), so the timing is testable
// without a motor and identical on-device and on the host.
struct KnockPlayback {
  uint8_t pulses = 0;
  uint32_t at_ms[KNOCK_MAX_TAPS] = {0};
};

inline bool knock_playback(const Knock& k, KnockPlayback& out) {
  if (!k.valid()) return false;
  out = KnockPlayback();
  out.pulses = k.taps;
  uint32_t t = 0;
  out.at_ms[0] = 0;
  for (uint8_t i = 1; i < k.taps; i++) {
    t += k.gap_ms[i - 1];
    out.at_ms[i] = t;
  }
  return true;
}

}  // namespace tincan
}  // namespace canary

#endif  // CANARY_TINCAN_KNOCK_CODEC_H
