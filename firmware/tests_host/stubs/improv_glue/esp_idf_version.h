#pragma once
// esp_idf_version.h stand-in: the IDF under each core the families build the
// glue on (firmware/envs/platformio/platforms.ini): Arduino-ESP32 3.3.8 on
// ESP-IDF 5.5.4 (pioarduino 55.03.38-1, Sense) for ESP_ARDUINO_VERSION_MAJOR
// 3, and 2.0.17 on IDF 4.4.7 (espressif32 6.9.0, Vision) for 2. The glue's
// one IDF gate (the USB / JTAG / glitch reset reasons, >= 5.1.0) is the only
// reader, so it is compiled both ways.
#define ESP_IDF_VERSION_VAL(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5, 5, 4)
#else
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(4, 4, 7)
#endif
