/* Host stand-in for <esp_timer.h> — see README.md. Defined by the test. */
#ifndef SECURACV_CSI_HOST_STUB_ESP_TIMER_H
#define SECURACV_CSI_HOST_STUB_ESP_TIMER_H

#include <stdint.h>

int64_t esp_timer_get_time(void);   /* microseconds */

#endif /* SECURACV_CSI_HOST_STUB_ESP_TIMER_H */
