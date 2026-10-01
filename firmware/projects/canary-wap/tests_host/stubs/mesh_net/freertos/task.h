/* Host stand-in for <freertos/task.h> (canary-wap mesh_network host
 * harness). vTaskDelay is where a task waiting for the loop task gives up
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

inline void vTaskDelay(TickType_t ticks) {
  ++host_sim::task_delays;
  if (host_sim::on_task_delay) {
    host_sim::on_task_delay((uint32_t)ticks);
  } else {
    host_sim::now_ms += (uint32_t)ticks;
  }
}

#endif
