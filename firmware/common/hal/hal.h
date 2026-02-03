/**
 * @file hal.h
 * @brief Hardware Abstraction Layer - Main Header
 *
 * This is the main entry point for the HAL. Include this file to get access
 * to all HAL interfaces. The actual implementations are provided by board-specific
 * code that is linked at build time.
 *
 * @note HAL interfaces define WHAT operations are available, not HOW they work.
 * @note Board-specific implementations provide the HOW.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Include all HAL interface headers
#include "hal_gpio.h"
#include "hal_uart.h"
#include "hal_spi.h"
#include "hal_i2c.h"
#include "hal_timer.h"
#include "hal_storage.h"
#include "hal_crypto.h"
#include "hal_wifi.h"
#include "hal_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// HAL INITIALIZATION
// ============================================================================

/**
 * @brief HAL error codes
 */
typedef enum {
    HAL_OK = 0,
    HAL_ERROR = -1,
    HAL_ERROR_INVALID_PARAM = -2,
    HAL_ERROR_NOT_INITIALIZED = -3,
    HAL_ERROR_ALREADY_INITIALIZED = -4,
    HAL_ERROR_TIMEOUT = -5,
    HAL_ERROR_BUSY = -6,
    HAL_ERROR_NO_MEMORY = -7,
    HAL_ERROR_NOT_SUPPORTED = -8,
    HAL_ERROR_IO = -9,
    HAL_ERROR_HARDWARE = -10,
} hal_err_t;

/**
 * @brief Initialize the HAL layer
 *
 * Must be called once at startup before using any HAL functions.
 * Initializes all subsystems based on board capabilities.
 *
 * @return HAL_OK on success, error code otherwise
 */
hal_err_t hal_init(void);

/**
 * @brief Deinitialize the HAL layer
 *
 * Releases all resources and puts hardware in safe state.
 *
 * @return HAL_OK on success, error code otherwise
 */
hal_err_t hal_deinit(void);

/**
 * @brief Get HAL version string
 * @return Version string (e.g., "1.0.0")
 */
const char* hal_version(void);

/**
 * @brief Get board identifier
 * @return Board ID string (e.g., "xiao-esp32s3-sense")
 */
const char* hal_board_id(void);

/**
 * @brief Get MCU identifier
 * @return MCU ID string (e.g., "ESP32-S3")
 */
const char* hal_mcu_id(void);

// ============================================================================
// SYSTEM UTILITIES
// ============================================================================

/**
 * @brief Get current timestamp in milliseconds
 * @return Milliseconds since boot
 */
uint32_t hal_millis(void);

/**
 * @brief Get current timestamp in microseconds
 * @return Microseconds since boot
 */
uint64_t hal_micros(void);

/**
 * @brief Delay for specified milliseconds
 * @param ms Milliseconds to delay
 */
void hal_delay_ms(uint32_t ms);

/**
 * @brief Delay for specified microseconds
 * @param us Microseconds to delay
 */
void hal_delay_us(uint32_t us);

/**
 * @brief Perform a software reset
 */
void hal_reset(void);

/**
 * @brief Get unique hardware ID
 * @param id_out Buffer to store ID (must be at least 16 bytes)
 * @param len Length of buffer
 * @return Number of bytes written, or negative error code
 */
int hal_get_unique_id(uint8_t* id_out, size_t len);

/**
 * @brief Get free heap memory in bytes
 * @return Free heap bytes
 */
uint32_t hal_free_heap(void);

/**
 * @brief Get minimum free heap since boot
 * @return Minimum free heap bytes
 */
uint32_t hal_min_free_heap(void);

// ============================================================================
// WATCHDOG
// ============================================================================

/**
 * @brief Initialize hardware watchdog
 * @param timeout_sec Timeout in seconds
 * @return HAL_OK on success
 */
hal_err_t hal_watchdog_init(uint32_t timeout_sec);

/**
 * @brief Feed the watchdog (reset timeout)
 */
void hal_watchdog_feed(void);

/**
 * @brief Disable the watchdog
 * @return HAL_OK on success
 */
hal_err_t hal_watchdog_disable(void);

// ============================================================================
// RANDOM NUMBER GENERATION
// ============================================================================

/**
 * @brief Get hardware random number
 * @return Random 32-bit value
 */
uint32_t hal_random(void);

/**
 * @brief Fill buffer with hardware random bytes
 * @param buf Buffer to fill
 * @param len Number of bytes to generate
 * @return HAL_OK on success
 */
hal_err_t hal_random_bytes(uint8_t* buf, size_t len);

#ifdef __cplusplus
}
#endif
