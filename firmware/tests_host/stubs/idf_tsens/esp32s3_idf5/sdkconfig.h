/* The chip target as sdkconfig.h names it, for the ESP32-S3 on the core-3
 * line ([env:full]: pioarduino, Arduino-ESP32 3.3.8 / ESP-IDF 5.5.4;
 * test_camera_thermal.cpp, sweep F432). IDF defines CONFIG_IDF_TARGET_* in
 * this header and nowhere on the compiler's command line. */
#ifndef STUB_IDF_TSENS_SDKCONFIG_ESP32S3_IDF5_H
#define STUB_IDF_TSENS_SDKCONFIG_ESP32S3_IDF5_H

#define CONFIG_IDF_TARGET_ARCH_XTENSA 1
#define CONFIG_IDF_TARGET "esp32s3"
#define CONFIG_IDF_TARGET_ESP32S3 1

#endif
