/* Host stand-in for <freertos/FreeRTOS.h> (canary-wap mesh_network host
 * harness): the tick type, pdMS_TO_TICKS at the device's 1 kHz tick, and
 * the portMUX spinlock loop_command_ring::PortMuxLock takes. The harness
 * is one thread, so the critical section only counts its entries and
 * exits (host_sim::mux_depth), which a test can read to see every one
 * closed. */
#ifndef STUB_MESH_NET_FREERTOS_H
#define STUB_MESH_NET_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;
#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

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
