/* Host tests for peek_stream_logic.h — the value math behind
 * /api/peek/status and the MJPEG worker's pacing. Build & run (CI:
 * firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_peek_stream_logic.cpp \
 *       -o /tmp/test_peek_stream_logic && /tmp/test_peek_stream_logic
 */

#include <cstdio>

#include "peek_stream_logic.h"

using namespace peek_stream_logic;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_stream_uptime() {
  // Args: (active, start_ms, end_ms, last_frame_ms, now_ms)

  // Never streamed: no uptime, regardless of the other inputs.
  CHECK(stream_uptime_ms(false, 0, 0, 0, 123456) == 0);
  CHECK(stream_uptime_ms(true,  0, 0, 0, 123456) == 0);

  // Live stream: counts from start to now.
  CHECK(stream_uptime_ms(true, 10000, 0, 0, 10000) == 0);
  CHECK(stream_uptime_ms(true, 10000, 0, 45000, 73000) == 63000);

  // THE BUG THIS PINS: a finished stream used to report 0 (the old code was
  // `(start && active) ? now - start : 0`), which zeroed the derived avg
  // throughput too — "THROUGHPUT 0 kbps / STREAM UPTIME —" for a stream that
  // had just run for a minute. Finished streams report their frozen duration.
  CHECK(stream_uptime_ms(false, 10000, 73000, 72960, 999999) == 63000);
  // ...and the frozen value does not drift as now_ms advances.
  CHECK(stream_uptime_ms(false, 10000, 73000, 72960, 5000000) == 63000);

  // THE STOP RACE (Codex P2 on #822): a stop request clears the active flag
  // while the worker is still inside its frame delay — a status poll landing
  // in that window sees active=false with NO recorded end. Freeze
  // provisionally at the last delivered frame instead of collapsing to 0.
  CHECK(stream_uptime_ms(false, 10000, 0, 72960, 73050) == 62960);
  // Once the worker records the true end, it wins over the provisional value.
  CHECK(stream_uptime_ms(false, 10000, 73000, 72960, 73500) == 63000);

  // Stopped, no end recorded, and no frame was ever delivered (stream that
  // failed before its first frame): report 0, never a fabricated duration.
  CHECK(stream_uptime_ms(false, 10000, 0, 0, 99999) == 0);

  // millis() wrap: live stream spanning the 32-bit wrap still reports the
  // true elapsed time (unsigned subtraction).
  uint32_t near_wrap = 0xFFFFF000u;
  CHECK(stream_uptime_ms(true, near_wrap, 0, 0, near_wrap + 30000u /* wraps */) == 30000);
  // Frozen duration across the wrap too.
  CHECK(stream_uptime_ms(false, near_wrap, near_wrap + 30000u, near_wrap + 29900u, 0) == 30000);
  // Provisional (last-frame) freeze across the wrap.
  CHECK(stream_uptime_ms(false, near_wrap, 0, near_wrap + 29900u, 0) == 29900);
}

static void test_avg_kbps() {
  // No elapsed time: no rate — never divide by zero, never fabricate.
  CHECK(avg_kbps(0, 0) == 0);
  CHECK(avg_kbps(1000000, 0) == 0);

  // 1,000,000 bytes over 10 s = 8,000,000 bits / 10,000 ms = 800 kbps.
  CHECK(avg_kbps(1000000ULL, 10000) == 800);
  // 125 bytes over 1 ms = 1000 bits/ms = 1000 kbps.
  CHECK(avg_kbps(125ULL, 1) == 1000);
  // 64-bit totals: 8 GB over an hour doesn't overflow the intermediate.
  CHECK(avg_kbps(8ULL * 1024 * 1024 * 1024, 3600000) == 19088);
}

static void test_pace_clamp() {
  // The 20 ms floor keeps the priority-3 stream worker yielding every
  // iteration so the WDT-subscribed IDLE tasks always run; 0 must clamp UP.
  CHECK(pace_clamp_ms(0) == 20);
  CHECK(pace_clamp_ms(19) == 20);
  CHECK(pace_clamp_ms(20) == 20);
  CHECK(pace_clamp_ms(40) == 40);
  CHECK(pace_clamp_ms(500) == 500);
  CHECK(pace_clamp_ms(501) == 500);
  CHECK(pace_clamp_ms(0xFFFFFFFFu) == 500);
}

static void test_capture_abort() {
  // A dead camera must not spin the stream worker forever: the failure path
  // never touches the socket, so without a cap a vanished client is never
  // noticed and the single-stream busy flag blocks new streams until reboot.
  CHECK(!capture_should_abort(0));
  CHECK(!capture_should_abort(1));
  CHECK(!capture_should_abort(MAX_CONSECUTIVE_CAPTURE_FAILURES - 1));
  CHECK(capture_should_abort(MAX_CONSECUTIVE_CAPTURE_FAILURES));
  CHECK(capture_should_abort(MAX_CONSECUTIVE_CAPTURE_FAILURES + 1));
  // The cap is finite and small (~1 s at the 100 ms retry pace).
  CHECK(MAX_CONSECUTIVE_CAPTURE_FAILURES >= 3 &&
        MAX_CONSECUTIVE_CAPTURE_FAILURES <= 50);
}

int main() {
  test_stream_uptime();
  test_avg_kbps();
  test_pace_clamp();
  test_capture_abort();

  if (g_failures != 0) {
    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL peek_stream_logic TESTS PASSED\n");
  return 0;
}
