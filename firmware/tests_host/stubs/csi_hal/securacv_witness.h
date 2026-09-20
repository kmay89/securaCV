/* Stand-in for the canary product's securacv_witness.h: the adapter and the
 * canary/include/health_log.h bridge call log_health() and nothing else from
 * it. The real header pulls in Arduino and canary_config.h; the test defines
 * log_health() and counts what reaches it. Same signature as the product's. */
#ifndef STUB_CSI_HAL_SECURACV_WITNESS_H
#define STUB_CSI_HAL_SECURACV_WITNESS_H
#include "log_level.h"
void log_health(LogLevel level, LogCategory category, const char* message,
                const char* detail = nullptr);
#endif
