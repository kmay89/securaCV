/* A fake FreeRTOS mutex for the shared die-temperature provider's host build
 * (test_camera_thermal.cpp): the plain (non-recursive) create, take and give
 * securacv_thermal.cpp makes, so a recursive call does not compile here. The
 * host cannot block, so a take while it is held is a wait that ran out. A
 * call on a null handle is counted, never dereferenced. */
#ifndef STUB_IDF_TSENS_SEMPHR_H
#define STUB_IDF_TSENS_SEMPHR_H

#include "freertos/FreeRTOS.h"

struct HostTsensMutex {
  int count = 0;             // 1 while held
  int takes = 0;
  int gives = 0;
  int failed_takes = 0;      // a take while held
  int bad_gives = 0;         // a give while not held
  TickType_t last_wait = 0;  // the wait the last take asked for
};
typedef HostTsensMutex* SemaphoreHandle_t;

inline bool g_host_tsens_mutex_create_fails = false;  // creation fails (no heap)
inline int g_host_tsens_mutexes_created = 0;
inline int g_host_tsens_null_handle_calls = 0;
inline HostTsensMutex g_host_tsens_mutexes[2];

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  if (g_host_tsens_mutex_create_fails || g_host_tsens_mutexes_created >= 2) return nullptr;
  return &g_host_tsens_mutexes[g_host_tsens_mutexes_created++];
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
  if (m == nullptr) { g_host_tsens_null_handle_calls++; return pdFALSE; }
  m->takes++;
  m->last_wait = wait;
  if (m->count != 0) { m->failed_takes++; return pdFALSE; }
  m->count = 1;
  return pdTRUE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {
  if (m == nullptr) { g_host_tsens_null_handle_calls++; return pdFALSE; }
  m->gives++;
  if (m->count == 0) { m->bad_gives++; return pdFALSE; }
  m->count = 0;
  return pdTRUE;
}

#endif
