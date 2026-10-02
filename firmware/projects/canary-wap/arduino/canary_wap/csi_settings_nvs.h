/**
 * @file csi_settings_nvs.h
 * @brief The canary-wap's CSI modules' boot init (sweep F93).
 */

#ifndef SECURACV_WAP_CSI_SETTINGS_NVS_H
#define SECURACV_WAP_CSI_SETTINGS_NVS_H

#include <stddef.h>

/**
 * Run every registered module's init() once (csi_module_init_all), reading
 * its stored settings through one read-only NVS handle for the whole boot
 * (csi_module_settings_nvs.h's rule, the canary's too). Returns how many
 * init() calls it made. csi_integration::init() calls it once, right after
 * register_v1_modules() and before the HAL installs the features callback;
 * check_wap_event_egress.py's rule 3 holds that. Loop task, at boot.
 */
size_t csi_settings_nvs_init_modules(void);

#endif /* SECURACV_WAP_CSI_SETTINGS_NVS_H */
