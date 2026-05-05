/*
 * SecuraCV Canary — Low-Power HAL
 *
 * A thin abstraction over the ESP32-S3's native deep-sleep machinery.
 * Application code never calls esp_sleep_* directly — everything goes
 * through this header so a future port to a non-S3 chip (or a different
 * sleep regime, e.g. light-sleep with WiFi association preserved) only
 * touches one file.
 *
 * Wake sources supported (all natively wired to the ESP32-S3 RTC):
 *
 *   • TIMER     — esp_sleep_enable_timer_wakeup()
 *                 Periodic wakeup on a microsecond budget.
 *
 *   • TOUCH     — esp_sleep_enable_touchpad_wakeup()
 *                 Any touch pad configured with touch_pad_config() can
 *                 wake the chip; the RTC touch peripheral runs while the
 *                 main cores are off. On wake, the firing pad is read
 *                 from esp_sleep_get_touchpad_wakeup_status().
 *
 *   • EXT0      — esp_sleep_enable_ext0_wakeup(gpio, level)
 *                 One RTC-domain GPIO; cheap but only one pin.
 *
 *   • EXT1      — esp_sleep_enable_ext1_wakeup(mask, mode)
 *                 Multiple RTC-domain GPIOs at once.
 *
 *   • ULP       — esp_sleep_enable_ulp_wakeup()
 *                 The ULP-RISC-V coprocessor wakes the chip when its
 *                 program triggers a wake. The actual ULP program is a
 *                 follow-up; this HAL only enables the source.
 *
 * IMPORTANT: arming a wake source does NOT enter deep sleep. The
 * application calls lowpower_enter_deep_sleep() explicitly when ready.
 * Without that call, the chip stays awake. This separation lets us
 * register wake sources at boot (so we know the wake reason on resume)
 * while leaving the actual sleep decision to the application's policy
 * layer (FEATURE_DEEP_SLEEP gates the call site in main.cpp).
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_LOWPOWER_H
#define SECURACV_LOWPOWER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Plain-English wake-reason enum returned by lowpower_get_wake_reason().
 * Numeric values are stable; new sources append. */
typedef enum {
  LOWPOWER_WAKE_UNDEFINED = 0,   /* normal cold boot */
  LOWPOWER_WAKE_TIMER     = 1,
  LOWPOWER_WAKE_TOUCH     = 2,
  LOWPOWER_WAKE_EXT0      = 3,
  LOWPOWER_WAKE_EXT1      = 4,
  LOWPOWER_WAKE_ULP       = 5,
  LOWPOWER_WAKE_GPIO      = 6,
  LOWPOWER_WAKE_OTHER     = 7,
} lowpower_wake_reason_t;

/* Capability bitmask returned by lowpower_get_caps(). */
typedef enum {
  LOWPOWER_CAP_TIMER       = 1 << 0,
  LOWPOWER_CAP_TOUCH       = 1 << 1,
  LOWPOWER_CAP_EXT0        = 1 << 2,
  LOWPOWER_CAP_EXT1        = 1 << 3,
  LOWPOWER_CAP_ULP_RISCV   = 1 << 4,  /* ESP32-S3 / S2 */
  LOWPOWER_CAP_ULP_FSM     = 1 << 5,  /* original ESP32 only */
} lowpower_cap_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the HAL. Idempotent. Always succeeds — wake-source arming
 * is independent of init success/failure. */
bool lowpower_init(void);

/* Bitmask of LOWPOWER_CAP_* values available on this target at runtime. */
uint32_t lowpower_get_caps(void);

/* Capture the wake reason from the previous deep-sleep cycle. Should
 * be called early in setup() so the cause is logged before any
 * subsystem brings up wake-disturbing hardware. Returns
 * LOWPOWER_WAKE_UNDEFINED on a normal cold boot.
 *
 * Thin wrapper around esp_sleep_get_wakeup_cause() that maps the
 * IDF enum to our LOWPOWER_WAKE_* values. */
uint8_t lowpower_get_wake_reason(void);

/* Plain-English label for a lowpower_wake_reason_t. */
const char* lowpower_wake_reason_name(uint8_t reason);

/* If the wake reason was LOWPOWER_WAKE_TOUCH, return the touch_pad_t
 * channel that fired. Otherwise returns -1.
 *
 * Wraps esp_sleep_get_touchpad_wakeup_status(). */
int lowpower_get_wake_touch_pad(void);

/* Arm wake sources. Each call adds a source to the set the next
 * lowpower_enter_deep_sleep() will respect. Calling arm_*() on an
 * already-armed source replaces the parameters. */
bool lowpower_arm_wake_timer(uint64_t microseconds);
bool lowpower_arm_wake_touch(void);
bool lowpower_arm_wake_ext0(int gpio_num, int level);
bool lowpower_arm_wake_ext1(uint64_t gpio_mask, int any_or_all_mode);
bool lowpower_arm_wake_ulp(void);

/* Disable all armed wake sources. Useful before a clean reset. */
void lowpower_disarm_all(void);

/* Enter deep sleep. Does not return — execution resumes from the boot
 * vector when a wake source fires. Wraps esp_deep_sleep_start().
 *
 * IMPORTANT: callers should flush SD/NVS, drop network connections, and
 * stop sensing libraries before invoking this — there is no graceful
 * shutdown path inside the call.  */
void lowpower_enter_deep_sleep(void);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_LOWPOWER_H */
