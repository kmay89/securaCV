/* FreeRTOS base types for the events-egress host build, and the portMUX
 * spinlock loop_command_ring::PortMuxLock takes (the egress publishes its
 * counters through loop_snapshot.h for other tasks, sweep F149). The
 * harness is one thread, so the critical section only counts its entries
 * and exits (host_sim::mux_depth), which a test can read to see every one
 * closed. */
#ifndef STUB_EGRESS_FREERTOS_H
#define STUB_EGRESS_FREERTOS_H
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;
#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)

namespace host_sim {
inline int mux_depth = 0;
}  // namespace host_sim

struct StubPortMux {
  int unused;
};
#define portMUX_TYPE StubPortMux
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(m) ((void)(m), ++host_sim::mux_depth)
#define portEXIT_CRITICAL(m) ((void)(m), --host_sim::mux_depth)
#endif
