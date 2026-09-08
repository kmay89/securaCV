/* csi_hal.cpp timestamps frames from esp_timer_get_time() (us); the test
 * derives it from the same fake millis() clock so the rate limiter and the
 * watchdog see one time base. C linkage, as in the real header. */
#ifndef STUB_CSI_HAL_ESP_TIMER_H
#define STUB_CSI_HAL_ESP_TIMER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
#endif
