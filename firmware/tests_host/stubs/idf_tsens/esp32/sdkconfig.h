/* The chip target as sdkconfig.h names it, for the classic ESP32 of
 * env:esp32cam and env:esp32-wroom (test_camera_thermal.cpp, sweep F432).
 * IDF defines CONFIG_IDF_TARGET_* in this header and nowhere on the
 * compiler's command line, so a source that tests it before including
 * sdkconfig.h sees no target on any chip. The three lines are the ones the
 * Arduino core's prebuilt sdkconfig.h carries for the target. */
#ifndef STUB_IDF_TSENS_SDKCONFIG_ESP32_H
#define STUB_IDF_TSENS_SDKCONFIG_ESP32_H

#define CONFIG_IDF_TARGET_ARCH_XTENSA 1
#define CONFIG_IDF_TARGET "esp32"
#define CONFIG_IDF_TARGET_ESP32 1

#endif
