/*
 * SecuraCV Canary WAP — pure camera-peek stream decisions (host-testable)
 *
 * Arduino-free: stdint only. The MJPEG worker task and the /api/peek
 * handlers live in canary_wap.ino; the value math they report lives here so
 * a host g++ run (test_peek_stream_logic.cpp) can pin it.
 *
 * Why this exists: /api/peek/status used to report stream_uptime_ms = 0 the
 * moment the stream stopped, which zeroed the derived avg throughput too —
 * the dashboard showed "THROUGHPUT 0 kbps / STREAM UPTIME —" for a stream
 * that had just delivered megabytes. The uptime clock now freezes at the
 * recorded stream end instead of collapsing.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_PEEK_STREAM_LOGIC_H
#define SECURACV_PEEK_STREAM_LOGIC_H

#include <stdint.h>

namespace peek_stream_logic {

// Stream uptime as reported by /api/peek/status.
// - Never streamed (start == 0): 0.
// - Live: elapsed since start (unsigned subtraction is millis()-wrap-safe).
// - Finished: frozen at the recorded end so "LAST STREAM" stats stay
//   truthful; 0 only if no end was recorded (metrics were reset).
inline uint32_t stream_uptime_ms(bool active, uint32_t start_ms,
                                 uint32_t end_ms, uint32_t now_ms) {
  if (start_ms == 0) return 0;
  if (active) return (uint32_t)(now_ms - start_ms);
  if (end_ms == 0) return 0;
  return (uint32_t)(end_ms - start_ms);
}

// Average stream throughput in kbps from real byte totals. bytes*8/ms is
// kbits/s directly (the /1000 for ms cancels the *1000 for kilo). Returns 0
// when uptime is 0 — never a fabricated rate.
inline uint32_t avg_kbps(uint64_t total_bytes, uint32_t uptime_ms) {
  if (uptime_ms == 0) return 0;
  return (uint32_t)((total_bytes * 8ULL) / (uint64_t)uptime_ms);
}

// Clamp the operator-tunable frame pacing. The 20 ms floor is load-bearing:
// the stream worker runs at priority 3 and this vTaskDelay is what keeps the
// watchdog-subscribed IDLE tasks fed, so it must never clamp to zero.
inline uint32_t pace_clamp_ms(uint32_t requested_ms) {
  if (requested_ms < 20)  return 20;
  if (requested_ms > 500) return 500;
  return requested_ms;
}

}  // namespace peek_stream_logic

#endif  // SECURACV_PEEK_STREAM_LOGIC_H
