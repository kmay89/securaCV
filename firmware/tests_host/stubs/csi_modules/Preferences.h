/* Fake Preferences for test_csi_modules_integration.cpp: the canary's CSI
 * module bridge (canary/src/csi_modules_integration.cpp) reads its module
 * settings through read-only Preferences handles. This NVS holds nothing, so
 * every open fails and each setting reads as the module's own default, as on
 * a device that was never configured. */
#ifndef STUB_CSI_MODULES_PREFERENCES_H
#define STUB_CSI_MODULES_PREFERENCES_H

#include <stdint.h>

class Preferences {
 public:
  bool begin(const char*, bool = false) { return false; }
  void end() {}
  int32_t getInt(const char*, int32_t d = 0) { return d; }
  bool getBool(const char*, bool d = false) { return d; }
  float getFloat(const char*, float d = 0.0f) { return d; }
};

#endif
