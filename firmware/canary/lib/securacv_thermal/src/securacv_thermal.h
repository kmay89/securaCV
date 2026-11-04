/*
 * SecuraCV Canary — Shared die-temperature provider
 *
 * Single owner of the ESP32-S3 internal temperature-sensor driver.
 *
 * WHY THIS EXISTS: ESP-IDF 5.x allows exactly ONE installed instance of
 * the temperature-sensor driver. Three consumers need the die temp —
 * the envsens tamper-drift detector, the camera thermal throttle, and
 * the diagnostics self-test — and each previously installed its own
 * handle. Whichever initialized first won; the others failed silently
 * (camera thermal protection dead, temp self-test failing as NAN).
 * Every consumer must read through thermal_read_die_c() instead.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_THERMAL_H
#define SECURACV_THERMAL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the chip die temperature in °C. Installs/enables the sensor
 * driver lazily on first call (range 20–100 °C, ±2 °C — the predefined
 * band that covers normal operation through the camera's 80 °C pause
 * threshold; see Espressif temp_sensor docs). Thread-safe: callable
 * from the main loop and the httpd task concurrently.
 *
 * Returns false (and leaves *out_c untouched) when the sensor is
 * unavailable on this target or the read fails. Note the sensor
 * measures SILICON temperature, not ambient — Espressif documents it
 * as good for tracking changes, not precise absolute values. */
bool thermal_read_die_c(float* out_c);

/* True when this target has a die-temperature sensor the provider drives
 * (ESP32-S3, -S2 and -C3), false where it has none (the classic ESP32 of
 * env:esp32cam and env:esp32-wroom, and the host build), where
 * thermal_read_die_c() always answers false. A caller that counts misses
 * asks this first, so a chip with no sensor is not read as one whose sensor
 * fails (sweep F432). True does not promise a read: the driver can still
 * fail to install or to read, and that is a miss. */
bool thermal_sensor_present(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_THERMAL_H */
