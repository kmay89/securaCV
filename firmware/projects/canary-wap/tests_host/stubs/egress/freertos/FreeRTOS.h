/* FreeRTOS base types for the events-egress host build. */
#ifndef STUB_EGRESS_FREERTOS_H
#define STUB_EGRESS_FREERTOS_H
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;
#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#endif
