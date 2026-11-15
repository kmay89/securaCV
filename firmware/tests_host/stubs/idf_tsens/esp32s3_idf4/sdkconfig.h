/* The chip target as sdkconfig.h names it, for the ESP32-S3 of the stock
 * units (Arduino 2.0.17 / ESP-IDF 4.4.7; test_camera_thermal.cpp, sweep
 * F432). IDF defines CONFIG_IDF_TARGET_* in this header and nowhere on the
 * compiler's command line. The three lines are the ones the core's prebuilt
 * tools/sdk/esp32s3/qio_opi/include/sdkconfig.h carries for the target. */
#ifndef STUB_IDF_TSENS_SDKCONFIG_ESP32S3_IDF4_H
#define STUB_IDF_TSENS_SDKCONFIG_ESP32S3_IDF4_H

#define CONFIG_IDF_TARGET_ARCH_XTENSA 1
#define CONFIG_IDF_TARGET "esp32s3"
#define CONFIG_IDF_TARGET_ESP32S3 1

#endif
