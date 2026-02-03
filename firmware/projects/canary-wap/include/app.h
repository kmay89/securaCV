/**
 * @file app.h
 * @brief Application-level declarations for Canary WAP
 */

#pragma once

#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// APPLICATION API
// ============================================================================

/**
 * @brief Get device identity
 * @return Pointer to device identity structure
 */
const device_identity_t* app_get_identity(void);

/**
 * @brief Get system health metrics
 * @return Pointer to health structure
 */
const system_health_t* app_get_health(void);

/**
 * @brief Get GNSS fix data
 * @return Pointer to GNSS fix structure
 */
const gnss_fix_t* app_get_gnss_fix(void);

/**
 * @brief Get WiFi status
 * @return Pointer to WiFi status structure
 */
const wifi_status_info_t* app_get_wifi_status(void);

/**
 * @brief Request system restart
 * @param delay_ms Delay before restart (0 = immediate)
 */
void app_request_restart(uint32_t delay_ms);

/**
 * @brief Enter pairing mode
 * @param code Output pairing code (7 chars: 6 digits + null)
 * @return RESULT_OK on success
 */
result_t app_enter_pairing_mode(char code[7]);

/**
 * @brief Exit pairing mode
 * @return RESULT_OK on success
 */
result_t app_exit_pairing_mode(void);

#ifdef __cplusplus
}
#endif
