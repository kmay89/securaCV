/* Host stand-in for <freertos/semphr.h> (the MQTT bridge host build,
 * test_mqtt_reinit.cpp). The test is one thread, so a mutex is a flag: a
 * take while it is held is what a second writer would block on, and the
 * stub counts it (stub_mqtt::mutex_contended) and answers pdFALSE instead
 * of blocking, so a test can play a save inside another save's write and
 * hold that it wrote nothing (csi_mqtt.cpp's SaveScope refuses on pdFALSE).
 * One mutex is modeled: the broker row's writer lock. */
#ifndef STUB_MQTT_FREERTOS_SEMPHR_H
#define STUB_MQTT_FREERTOS_SEMPHR_H
#include <stdint.h>
#include <cstdlib>
#include "FreeRTOS.h"
typedef void* SemaphoreHandle_t;
namespace stub_mqtt {
inline bool     mutex_held = false;
inline unsigned mutexes_created = 0;
inline unsigned mutex_takes = 0;        // takes that got the mutex
inline unsigned mutex_contended = 0;    // takes while held: FreeRTOS would have blocked
inline unsigned fail_mutex_create = 0;  // this many creates fail next
}  // namespace stub_mqtt
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  if (stub_mqtt::fail_mutex_create > 0) {
    --stub_mqtt::fail_mutex_create;
    return nullptr;
  }
  ++stub_mqtt::mutexes_created;
  return reinterpret_cast<SemaphoreHandle_t>(static_cast<uintptr_t>(stub_mqtt::mutexes_created));
}
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t /*ticks*/) {
  if (h == nullptr) std::abort();                    /* FreeRTOS asserts on a null handle */
  if (stub_mqtt::mutex_held) {
    ++stub_mqtt::mutex_contended;
    return pdFALSE;
  }
  stub_mqtt::mutex_held = true;
  ++stub_mqtt::mutex_takes;
  return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
  if (h == nullptr || !stub_mqtt::mutex_held) std::abort();   /* a give without a take */
  stub_mqtt::mutex_held = false;
  return pdTRUE;
}
#endif  // STUB_MQTT_FREERTOS_SEMPHR_H
