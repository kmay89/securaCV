// CAN 2.0 (ISO 11898-1) frame model — pure, host-testable core (no Arduino).
//
// The TWAI controller on the ESP32-S3 does the bit-level work (framing, stuffing,
// CRC, ACK), so this core is the honesty layer *above* the wire: the frame
// struct, the spec's identifier/DLC validity rules, SocketCAN-style acceptance
// filtering, and a stable ASCII formatter for logs. All of it is exhaustively
// host-tested (tests_host/test_can_frame.cpp) without a board; the thin TWAI
// transport that actually moves frames lives in canary/io/can_bus.* behind
// FEATURE_CAN.
//
// Spec anchors (ISO 11898-1 / CAN 2.0):
//   * base frame  (2.0A): 11-bit identifier, 0x000..0x7FF
//   * extended    (2.0B): 29-bit identifier, 0x0000_0000..0x1FFF_FFFF
//   * DLC: 0..8 data bytes (classic CAN)
//   * RTR: remote-transmission-request frames carry a DLC but no data bytes

#ifndef CANARY_IO_CAN_FRAME_H
#define CANARY_IO_CAN_FRAME_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace canary {
namespace io {
namespace can {

// Identifier ceilings and payload cap, straight from the spec.
constexpr uint32_t STD_ID_MAX = 0x7FFu;         // 11-bit base identifier
constexpr uint32_t EXT_ID_MAX = 0x1FFFFFFFu;    // 29-bit extended identifier
constexpr uint8_t MAX_DLC = 8;                  // classic CAN payload bytes

// The standard ISO 11898 bit rates the TWAI timing presets cover. can_begin()
// accepts one of these (bit/s); anything else is refused rather than guessed.
constexpr uint32_t BITRATE_125K = 125000u;
constexpr uint32_t BITRATE_250K = 250000u;
constexpr uint32_t BITRATE_500K = 500000u;
constexpr uint32_t BITRATE_1M = 1000000u;

// A classic-CAN frame in host-neutral form (what the driver converts to/from a
// twai_message_t). `data` beyond `dlc` is not meaningful; RTR frames carry none.
struct Frame {
  uint32_t id;        // 11- or 29-bit identifier (right-justified)
  bool extended;      // true = 29-bit (2.0B), false = 11-bit (2.0A)
  bool rtr;           // remote-transmission-request (no data bytes on the wire)
  uint8_t dlc;        // 0..8
  uint8_t data[MAX_DLC];
};

// An identifier is valid only if it fits the frame's addressing mode.
inline bool valid_id(uint32_t id, bool extended) {
  return id <= (extended ? EXT_ID_MAX : STD_ID_MAX);
}

inline bool valid_dlc(uint8_t dlc) { return dlc <= MAX_DLC; }

inline bool valid_frame(const Frame& f) {
  return valid_id(f.id, f.extended) && valid_dlc(f.dlc);
}

inline bool valid_bitrate(uint32_t bps) {
  return bps == BITRATE_125K || bps == BITRATE_250K || bps == BITRATE_500K ||
         bps == BITRATE_1M;
}

// SocketCAN-style acceptance test: a frame is accepted when the masked
// identifier matches the masked filter — (rx & mask) == (want & mask). A mask
// bit of 1 means "this bit must match"; 0 means "don't care". mask == 0 accepts
// everything; mask == the id-width ceiling matches one exact id.
inline bool filter_accepts(uint32_t rx_id, uint32_t want_id, uint32_t mask) {
  return (rx_id & mask) == (want_id & mask);
}

// Render a frame as a stable, ASCII-only line for the bench log, e.g.
//   "id=0x0AB ext=0 rtr=0 dlc=3 data=DE AD BE"
//   "id=0x18FF50E5 ext=1 rtr=1 dlc=8 remote"
// Returns the length written (excluding the NUL), or 0 if `cap` is too small.
// Never writes past `cap`; std ids print 3 hex digits, extended ids 8.
inline size_t format_frame(const Frame& f, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  int n = f.extended
              ? std::snprintf(out, cap, "id=0x%08lX ext=1 rtr=%d dlc=%u",
                              (unsigned long)(f.id & EXT_ID_MAX), f.rtr ? 1 : 0,
                              (unsigned)f.dlc)
              : std::snprintf(out, cap, "id=0x%03lX ext=0 rtr=%d dlc=%u",
                              (unsigned long)(f.id & STD_ID_MAX), f.rtr ? 1 : 0,
                              (unsigned)f.dlc);
  if (n < 0 || (size_t)n >= cap) return 0;
  size_t len = (size_t)n;

  if (f.rtr) {
    int m = std::snprintf(out + len, cap - len, " remote");
    if (m < 0 || (size_t)m >= cap - len) return 0;
    return len + (size_t)m;
  }

  const uint8_t bytes = f.dlc > MAX_DLC ? MAX_DLC : f.dlc;
  int m = std::snprintf(out + len, cap - len, " data=");
  if (m < 0 || (size_t)m >= cap - len) return 0;
  len += (size_t)m;
  for (uint8_t i = 0; i < bytes; i++) {
    m = std::snprintf(out + len, cap - len, i == 0 ? "%02X" : " %02X", f.data[i]);
    if (m < 0 || (size_t)m >= cap - len) return 0;
    len += (size_t)m;
  }
  return len;
}

}  // namespace can
}  // namespace io
}  // namespace canary

#endif  // CANARY_IO_CAN_FRAME_H
