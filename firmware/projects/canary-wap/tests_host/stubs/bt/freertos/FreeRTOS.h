/* Host stand-in for <freertos/FreeRTOS.h> (the Bluetooth channel host
 * harness): the tick type, pdMS_TO_TICKS at the device's 1 kHz tick, and
 * the portMUX spinlock loop_command_ring::PortMuxLock takes (and
 * loop_event_queue.h's queue, through it). A real spinlock, since the
 * threaded test (sweep F143) takes it from two threads, as the NimBLE host
 * task and the loop task take it on the device; each thread counts its own
 * entries and exits (host_sim::mux_depth). */
#ifndef STUB_BT_FREERTOS_H
#define STUB_BT_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;
#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

namespace host_sim {
inline thread_local int mux_depth = 0;
}  // namespace host_sim

struct StubPortMux {
  int locked;
};
inline void stub_mux_enter(StubPortMux* m) {
  while (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE) != 0) {
  }
  ++host_sim::mux_depth;
}
inline void stub_mux_exit(StubPortMux* m) {
  --host_sim::mux_depth;
  __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
}
#define portMUX_TYPE StubPortMux
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(m) stub_mux_enter(m)
#define portEXIT_CRITICAL(m) stub_mux_exit(m)

#endif
