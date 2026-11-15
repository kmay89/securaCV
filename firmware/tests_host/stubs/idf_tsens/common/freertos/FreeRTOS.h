/* FreeRTOS types for the shared die-temperature provider's host build
 * (test_camera_thermal.cpp). The fake tick is 1 ms, so a wait in ticks reads
 * as the milliseconds asked for. */
#ifndef STUB_IDF_TSENS_FREERTOS_H
#define STUB_IDF_TSENS_FREERTOS_H

#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif
