/**
 * @file hal_i2c.h
 * @brief HAL I2C Interface
 *
 * Provides a hardware-independent I2C interface.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// I2C TYPES
// ============================================================================

typedef int i2c_bus_t;

typedef struct {
    int sda_pin;            // -1 for default
    int scl_pin;            // -1 for default
    uint32_t freq_hz;       // Clock frequency (100000 or 400000 typical)
} i2c_config_t;

#define I2C_CONFIG_DEFAULT { \
    .sda_pin = -1, \
    .scl_pin = -1, \
    .freq_hz = 100000, \
}

// ============================================================================
// I2C FUNCTIONS
// ============================================================================

/**
 * @brief Initialize I2C bus
 * @param bus I2C bus number
 * @param config Configuration structure
 * @return 0 on success, negative on error
 */
int hal_i2c_init(i2c_bus_t bus, const i2c_config_t* config);

/**
 * @brief Deinitialize I2C bus
 * @param bus I2C bus number
 * @return 0 on success, negative on error
 */
int hal_i2c_deinit(i2c_bus_t bus);

/**
 * @brief Write data to I2C device
 * @param bus I2C bus number
 * @param addr 7-bit device address
 * @param data Data to write
 * @param len Number of bytes
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes written, negative on error
 */
int hal_i2c_write(i2c_bus_t bus, uint8_t addr, const uint8_t* data, size_t len, uint32_t timeout_ms);

/**
 * @brief Read data from I2C device
 * @param bus I2C bus number
 * @param addr 7-bit device address
 * @param data Buffer for received data
 * @param len Number of bytes to read
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes read, negative on error
 */
int hal_i2c_read(i2c_bus_t bus, uint8_t addr, uint8_t* data, size_t len, uint32_t timeout_ms);

/**
 * @brief Write then read (combined transaction)
 * @param bus I2C bus number
 * @param addr 7-bit device address
 * @param write_data Data to write first
 * @param write_len Number of bytes to write
 * @param read_data Buffer for received data
 * @param read_len Number of bytes to read
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes read, negative on error
 */
int hal_i2c_write_read(i2c_bus_t bus, uint8_t addr,
                       const uint8_t* write_data, size_t write_len,
                       uint8_t* read_data, size_t read_len,
                       uint32_t timeout_ms);

/**
 * @brief Check if device is present on bus
 * @param bus I2C bus number
 * @param addr 7-bit device address
 * @return true if device responds, false otherwise
 */
bool hal_i2c_probe(i2c_bus_t bus, uint8_t addr);

/**
 * @brief Scan bus for devices
 * @param bus I2C bus number
 * @param addrs Buffer to store found addresses
 * @param max_addrs Maximum addresses to return
 * @return Number of devices found, negative on error
 */
int hal_i2c_scan(i2c_bus_t bus, uint8_t* addrs, size_t max_addrs);

#ifdef __cplusplus
}
#endif
