/**
 * @file csi_settings_nvs.cpp
 * @brief The canary-wap's CSI module settings readers (sweep F93).
 *
 * The library declares csi_module_settings_int / _bool / _float weak, each
 * returning the caller's default (csi_module.cpp). These strong definitions
 * read NVS by csi_module_settings_nvs.h's rule (staged copy), the one the
 * canary's bridge (canary/src/csi_modules_integration.cpp) reads by too:
 * namespace "csi", the shared short keys, typed reads, and the caller's
 * default for an unmapped key, a row that is absent or a namespace that
 * will not open.
 *
 * A module reads them in its init(): once at boot, from
 * csi_integration::init()'s csi_module_init_all(), and again through
 * reinit_module() after /api/settings, /api/csi/calibrate/apply or a Tuning
 * Lab change writes a row. They lived in csi_integration.cpp, which no host
 * suite compiles; here tests_host/test_wap_module_boot.cpp builds them over
 * a fake Preferences, with the staged library and modules.
 */

#include <Preferences.h>

#include "csi_module.h"
#include "csi_module_settings_nvs.h"

extern "C" int32_t csi_module_settings_int(const csi_module_settings_t*,
                                           const char* key,
                                           int32_t default_value) {
  return csi_module_settings_nvs::read_int<Preferences>(key, default_value);
}

extern "C" bool csi_module_settings_bool(const csi_module_settings_t*,
                                         const char* key,
                                         bool default_value) {
  return csi_module_settings_nvs::read_bool<Preferences>(key, default_value);
}

extern "C" float csi_module_settings_float(const csi_module_settings_t*,
                                           const char* key,
                                           float default_value) {
  return csi_module_settings_nvs::read_float<Preferences>(key, default_value);
}
