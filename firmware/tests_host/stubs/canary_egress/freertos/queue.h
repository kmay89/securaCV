/* A FreeRTOS queue for the canary events-egress host build: fixed depth and item
 * size, copy in and copy out, never blocks (the host build is one thread;
 * a send to a full queue fails at once whatever the timeout, as a zero
 * timeout does on the device). stub_queue_sends_while_full() counts the
 * refusals so a test can see the producer was not blocked. */
#ifndef STUB_CANARY_EGRESS_FREERTOS_QUEUE_H
#define STUB_CANARY_EGRESS_FREERTOS_QUEUE_H

#include <string.h>

#include <deque>
#include <string>

#include "FreeRTOS.h"

struct StubQueue {
  UBaseType_t depth;
  UBaseType_t item;
  std::deque<std::string> items;
};
typedef StubQueue* QueueHandle_t;

inline unsigned& stub_queue_sends_while_full() {
  static unsigned n = 0;
  return n;
}

inline QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t item) {
  return new StubQueue{depth, item, {}};
}
inline BaseType_t xQueueSend(QueueHandle_t q, const void* p, TickType_t) {
  if (!q) return pdFALSE;
  if (q->items.size() >= q->depth) {
    stub_queue_sends_while_full()++;
    return pdFALSE;
  }
  q->items.emplace_back(static_cast<const char*>(p), q->item);
  return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t q, void* p, TickType_t) {
  if (!q || q->items.empty()) return pdFALSE;
  memcpy(p, q->items.front().data(), q->item);
  q->items.pop_front();
  return pdTRUE;
}
inline void vQueueDelete(QueueHandle_t q) { delete q; }
inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
  return q ? (UBaseType_t)q->items.size() : 0;
}

#endif
