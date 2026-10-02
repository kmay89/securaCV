/* Host stand-in for <freertos/task.h> (the MQTT bridge host build,
 * test_mqtt_reinit.cpp). The test is one thread, so a task is a role:
 * xTaskCreate() only records the task, and the test runs it when it wants
 * that task to have its turn (stub_mqtt::run_tasks), which is how a worker
 * that has not finished yet is played. stub_mqtt::fail_task_create makes
 * the next creates fail, as a device out of memory does. */
#ifndef STUB_MQTT_FREERTOS_TASK_H
#define STUB_MQTT_FREERTOS_TASK_H

#include <stdint.h>

#include <string>
#include <vector>

#include "FreeRTOS.h"

typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

namespace stub_mqtt {
struct Task {
  TaskFunction_t fn;
  void* arg;
  std::string name;
};
inline std::vector<Task> tasks;        // created, not yet run
inline unsigned tasks_created = 0;
inline unsigned tasks_deleted = 0;
inline unsigned fail_task_create = 0;  // this many creates fail next
}  // namespace stub_mqtt

inline BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t /*stack_bytes*/,
                              void* arg, UBaseType_t /*priority*/, TaskHandle_t* out) {
  if (stub_mqtt::fail_task_create > 0) {
    --stub_mqtt::fail_task_create;
    return pdFAIL;
  }
  stub_mqtt::tasks.push_back({fn, arg, name ? name : ""});
  ++stub_mqtt::tasks_created;
  if (out) *out = reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(stub_mqtt::tasks_created));
  return pdPASS;
}

/* A task ending itself (vTaskDelete(nullptr)): its function returns next. */
inline void vTaskDelete(TaskHandle_t) { ++stub_mqtt::tasks_deleted; }

/* The task the test is playing, as a handle: test_mqtt_reinit.cpp gives each
 * of its roles its own (stub_mqtt::current_task). Never nullptr, as on a
 * device once the scheduler runs. */
namespace stub_mqtt {
inline TaskHandle_t (*current_task)() = nullptr;
}  // namespace stub_mqtt
inline TaskHandle_t xTaskGetCurrentTaskHandle() {
  return stub_mqtt::current_task != nullptr ? stub_mqtt::current_task()
                                            : reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(1));
}

#endif
