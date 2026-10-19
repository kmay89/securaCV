/* Host stand-in for <freertos/FreeRTOS.h> (the MQTT bridge host build,
 * test_mqtt_reinit.cpp): the types <freertos/task.h> below needs. */
#ifndef STUB_MQTT_FREERTOS_H
#define STUB_MQTT_FREERTOS_H

#include <stdint.h>

typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;
#define pdPASS  ((BaseType_t)1)
#define pdFAIL  ((BaseType_t)0)
#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
typedef uint32_t TickType_t;
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)

#endif
