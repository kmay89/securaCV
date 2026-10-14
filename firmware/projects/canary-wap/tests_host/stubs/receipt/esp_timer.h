/* esp_timer.h for the receipt-session suite (sweep F289's review): the
 * session and pair-token clock, microseconds since boot in 64 bits, off the
 * clock stubs/receipt/Arduino.h's millis() is the low 32 bits of. */
#ifndef STUB_RECEIPT_ESP_TIMER_H
#define STUB_RECEIPT_ESP_TIMER_H

#include <stdint.h>

#include "Arduino.h"

inline int64_t esp_timer_get_time() { return (int64_t)(stub_receipt::now_ms * 1000ULL); }

#endif
