/* Host stand-in for <freertos/FreeRTOS.h> (the MQTT bridge host build,
 * test_mqtt_reinit.cpp): the types <freertos/task.h> below needs. */
#ifndef STUB_MQTT_FREERTOS_H
#define STUB_MQTT_FREERTOS_H

#include <stdint.h>

typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;
#define pdPASS  ((BaseType_t)1)
#define pdFAIL  ((BaseType_t)0)

#endif
