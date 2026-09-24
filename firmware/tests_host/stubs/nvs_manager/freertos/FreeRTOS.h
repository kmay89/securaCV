/* FreeRTOS types for test_nvs_manager_lock.cpp. The fake tick is 1 ms, so a
 * wait in ticks reads as the milliseconds asked for. */
#ifndef STUB_NVS_MANAGER_FREERTOS_H
#define STUB_NVS_MANAGER_FREERTOS_H

#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif
