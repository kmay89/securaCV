/* A fake FreeRTOS recursive mutex for test_nvs_manager_lock.cpp — the only
 * semaphore calls NvsManager makes, so a non-recursive create or take does
 * not compile here. The mutex has an owner: a take by the owner nests, a take
 * by any other task while it is held fails at once (the host cannot block,
 * so every contended wait is one that ran out), and a give by a task that
 * does not hold it is counted as a fault. g_host_task names the task making
 * the next call. A call on a null handle is counted, never dereferenced. */
#ifndef STUB_NVS_MANAGER_SEMPHR_H
#define STUB_NVS_MANAGER_SEMPHR_H

#include "freertos/FreeRTOS.h"

struct HostRecursiveMutex {
  int owner = -1;
  int count = 0;
  int takes = 0;
  int gives = 0;
  int bad_gives = 0;         // a give by a task that does not hold it
  TickType_t last_wait = 0;  // the wait the last take asked for
};
typedef HostRecursiveMutex* SemaphoreHandle_t;

inline int g_host_task = 0;
inline bool g_host_mutex_create_fails = false;  // creation fails (no heap)
inline int g_host_mutexes_created = 0;
inline int g_host_null_handle_calls = 0;
inline HostRecursiveMutex g_host_mutexes[4];

inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() {
  if (g_host_mutex_create_fails || g_host_mutexes_created >= 4) return nullptr;
  return &g_host_mutexes[g_host_mutexes_created++];
}

inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t m, TickType_t wait) {
  if (m == nullptr) { g_host_null_handle_calls++; return pdFALSE; }
  m->takes++;
  m->last_wait = wait;
  if (m->count == 0) { m->owner = g_host_task; m->count = 1; return pdTRUE; }
  if (m->owner == g_host_task) { m->count++; return pdTRUE; }
  return pdFALSE;
}

inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t m) {
  if (m == nullptr) { g_host_null_handle_calls++; return pdFALSE; }
  m->gives++;
  if (m->count == 0 || m->owner != g_host_task) { m->bad_gives++; return pdFALSE; }
  if (--m->count == 0) m->owner = -1;
  return pdTRUE;
}

#endif
