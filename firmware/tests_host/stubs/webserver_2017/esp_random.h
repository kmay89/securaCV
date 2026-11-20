/* Host fake of esp_random() for test_webserver_boundary.cpp (digest-auth nonces only). */
#ifndef STUB_WEBSERVER_2017_ESP_RANDOM_H
#define STUB_WEBSERVER_2017_ESP_RANDOM_H
#include <stdint.h>
inline uint32_t esp_random() { return 0x5ec0acf1u; }
#endif
