/* Host stand-in for <freertos/task.h> (canary-wap mesh_network host
 * harness). xTaskGetCurrentTaskHandle names the task the test plays.
 * vTaskDelay is where a task waiting for the loop task gives up
 * the CPU (mesh_network::submit), so it calls host_sim::on_task_delay: by
 * default it moves the clock by the delay (1 tick = 1 ms); a test that
 * plays the loop task getting its turn sets it to run update(). */
#ifndef STUB_MESH_NET_FREERTOS_TASK_H
#define STUB_MESH_NET_FREERTOS_TASK_H

#include <functional>

#include "Arduino.h"
#include "FreeRTOS.h"

namespace host_sim {
inline unsigned task_delays = 0;
inline std::function<void(uint32_t ms)> on_task_delay;
}  // namespace host_sim

// The task a call is made on: the HTTP server's while a test plays it
// (host_sim::on_httpd_task, stubs/mesh_net/Arduino.h), else the loop task.
typedef void* TaskHandle_t;
inline TaskHandle_t xTaskGetCurrentTaskHandle() {
  static int loop_task = 0;
  static int httpd_task = 0;
  return host_sim::on_httpd_task ? static_cast<TaskHandle_t>(&httpd_task)
                                 : static_cast<TaskHandle_t>(&loop_task);
}

inline void vTaskDelay(TickType_t ticks) {
  ++host_sim::task_delays;
  if (host_sim::on_task_delay) {
    host_sim::on_task_delay((uint32_t)ticks);
  } else {
    host_sim::now_ms += (uint32_t)ticks;
  }
}

#endif
