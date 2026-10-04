/* Host stand-in for <freertos/task.h> (the Bluetooth channel host harness).
 * vTaskDelay is where a task waiting for the loop task gives up the CPU
 * (bluetooth_channel::submit), so it calls host_sim::on_task_delay: by
 * default it moves the clock by the delay (1 tick = 1 ms); a test that plays
 * the loop task getting its turn sets it to run update(). */
#ifndef STUB_BT_FREERTOS_TASK_H
#define STUB_BT_FREERTOS_TASK_H

#include <functional>
#include <thread>

#include "../Arduino.h"
#include "FreeRTOS.h"

namespace host_sim {
inline std::function<void(uint32_t ms)> on_task_delay;
}  // namespace host_sim

inline void vTaskDelay(TickType_t ticks) {
  if (host_sim::on_task_delay) {
    host_sim::on_task_delay((uint32_t)ticks);
  } else {
    host_sim::now_ms += (uint32_t)ticks;
    std::this_thread::yield();   // the threaded test: let the loop thread run
  }
}

#endif
