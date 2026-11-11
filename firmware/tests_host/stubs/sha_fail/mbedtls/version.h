/* Host stand-in for mbedTLS's version.h, for test_sha_fail_flagship.cpp
 * (sweep F455). The suite is built once per API spelling, and the Makefile
 * names the version: SHA_STUB_MBEDTLS_VERSION=0x021C0700 is mbedTLS 2.28.7
 * (ESP-IDF 4.4, the Arduino 2.0.17 core: every flagship env but env:full,
 * canary-vision and canary-sentinel-lite), 0x03060300 is 3.6.3 (ESP-IDF
 * 5.5, the Arduino 3.x core: env:full, canary-sense, canary-sentinel). */
#pragma once

#ifndef SHA_STUB_MBEDTLS_VERSION
#error "build with -DSHA_STUB_MBEDTLS_VERSION=0x021C0700 (mbedTLS 2.28) or 0x03060300 (mbedTLS 3.6)"
#endif

#define MBEDTLS_VERSION_NUMBER SHA_STUB_MBEDTLS_VERSION
