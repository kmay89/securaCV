/**
 * @file hal_spi.h
 * @brief HAL SPI Interface
 *
 * Provides a hardware-independent SPI interface.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// SPI TYPES
// ============================================================================

typedef int spi_bus_t;

typedef enum {
    SPI_MODE_0 = 0,     // CPOL=0, CPHA=0
    SPI_MODE_1,         // CPOL=0, CPHA=1
    SPI_MODE_2,         // CPOL=1, CPHA=0
    SPI_MODE_3,         // CPOL=1, CPHA=1
} spi_mode_t;

typedef enum {
    SPI_BIT_ORDER_MSB_FIRST = 0,
    SPI_BIT_ORDER_LSB_FIRST,
} spi_bit_order_t;

typedef struct {
    int sck_pin;            // -1 for default
    int miso_pin;           // -1 for default
    int mosi_pin;           // -1 for default
    uint32_t freq_hz;       // Clock frequency
    spi_mode_t mode;
    spi_bit_order_t bit_order;
} spi_config_t;

#define SPI_CONFIG_DEFAULT { \
    .sck_pin = -1, \
    .miso_pin = -1, \
    .mosi_pin = -1, \
    .freq_hz = 1000000, \
    .mode = SPI_MODE_0, \
    .bit_order = SPI_BIT_ORDER_MSB_FIRST, \
}

// ============================================================================
// SPI FUNCTIONS
// ============================================================================

/**
 * @brief Initialize SPI bus
 * @param bus SPI bus number
 * @param config Configuration structure
 * @return 0 on success, negative on error
 */
int hal_spi_init(spi_bus_t bus, const spi_config_t* config);

/**
 * @brief Deinitialize SPI bus
 * @param bus SPI bus number
 * @return 0 on success, negative on error
 */
int hal_spi_deinit(spi_bus_t bus);

/**
 * @brief Begin SPI transaction (assert CS)
 * @param bus SPI bus number
 * @param cs_pin Chip select pin
 * @return 0 on success, negative on error
 */
int hal_spi_begin(spi_bus_t bus, int cs_pin);

/**
 * @brief End SPI transaction (deassert CS)
 * @param bus SPI bus number
 * @param cs_pin Chip select pin
 * @return 0 on success, negative on error
 */
int hal_spi_end(spi_bus_t bus, int cs_pin);

/**
 * @brief Transfer data (simultaneous read/write)
 * @param bus SPI bus number
 * @param tx_data Data to transmit (NULL for read-only)
 * @param rx_data Buffer for received data (NULL for write-only)
 * @param len Number of bytes
 * @return Number of bytes transferred, negative on error
 */
int hal_spi_transfer(spi_bus_t bus, const uint8_t* tx_data, uint8_t* rx_data, size_t len);

/**
 * @brief Write single byte
 * @param bus SPI bus number
 * @param data Byte to write
 * @return Byte received, negative on error
 */
int hal_spi_write_byte(spi_bus_t bus, uint8_t data);

/**
 * @brief Change SPI clock frequency
 * @param bus SPI bus number
 * @param freq_hz New frequency in Hz
 * @return 0 on success, negative on error
 */
int hal_spi_set_freq(spi_bus_t bus, uint32_t freq_hz);

#ifdef __cplusplus
}
#endif
