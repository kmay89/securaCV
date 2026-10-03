/* FreeRTOS base types for the canary events-egress host build
 * (test_canary_event_egress.cpp; the canary-wap's tests_host has the same). */
#ifndef STUB_CANARY_EGRESS_FREERTOS_H
#define STUB_CANARY_EGRESS_FREERTOS_H
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;
#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)

/* The port's spinlock (portmacro.h on the device), for the egress's stats
 * snapshot (sweep F179). The host build is one thread, so a critical
 * section only counts: stub_critical_sections() is each one entered, so a
 * test can see that publishing an unchanged copy takes none. */
typedef struct {
  int held;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
inline unsigned& stub_critical_sections() {
  static unsigned n = 0;
  return n;
}
#define portENTER_CRITICAL(mux) \
  do {                          \
    ++(mux)->held;              \
    ++stub_critical_sections(); \
  } while (0)
#define portEXIT_CRITICAL(mux) \
  do {                         \
    --(mux)->held;             \
  } while (0)
#endif
