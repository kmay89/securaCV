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
#endif
