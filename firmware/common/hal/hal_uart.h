/**
 * @file hal_uart.h
 * @brief HAL UART Interface
 *
 * Provides a hardware-independent UART interface for serial communication.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// UART TYPES
// ============================================================================

typedef int uart_port_t;

typedef enum {
    UART_PARITY_NONE = 0,
    UART_PARITY_ODD,
    UART_PARITY_EVEN,
} uart_parity_t;

typedef enum {
    UART_STOP_1 = 1,
    UART_STOP_2 = 2,
} uart_stop_t;

typedef struct {
    uint32_t baud;
    uint8_t data_bits;          // 5, 6, 7, or 8
    uart_parity_t parity;
    uart_stop_t stop_bits;
    int tx_pin;                 // -1 to use default
    int rx_pin;                 // -1 to use default
    size_t rx_buffer_size;      // 0 for default
    size_t tx_buffer_size;      // 0 for default
} uart_config_t;

// Default configuration
#define UART_CONFIG_DEFAULT { \
    .baud = 115200, \
    .data_bits = 8, \
    .parity = UART_PARITY_NONE, \
    .stop_bits = UART_STOP_1, \
    .tx_pin = -1, \
    .rx_pin = -1, \
    .rx_buffer_size = 256, \
    .tx_buffer_size = 256, \
}

// ============================================================================
// UART FUNCTIONS
// ============================================================================

/**
 * @brief Initialize UART port
 * @param port UART port number (0, 1, 2, etc.)
 * @param config Configuration structure
 * @return 0 on success, negative on error
 */
int hal_uart_init(uart_port_t port, const uart_config_t* config);

/**
 * @brief Deinitialize UART port
 * @param port UART port number
 * @return 0 on success, negative on error
 */
int hal_uart_deinit(uart_port_t port);

/**
 * @brief Write data to UART
 * @param port UART port number
 * @param data Data buffer
 * @param len Number of bytes to write
 * @return Number of bytes written, negative on error
 */
int hal_uart_write(uart_port_t port, const uint8_t* data, size_t len);

/**
 * @brief Read data from UART
 * @param port UART port number
 * @param data Buffer to store data
 * @param len Maximum bytes to read
 * @param timeout_ms Timeout in milliseconds (0 = non-blocking)
 * @return Number of bytes read, negative on error
 */
int hal_uart_read(uart_port_t port, uint8_t* data, size_t len, uint32_t timeout_ms);

/**
 * @brief Get number of bytes available in RX buffer
 * @param port UART port number
 * @return Number of bytes available, negative on error
 */
int hal_uart_available(uart_port_t port);

/**
 * @brief Flush TX buffer (wait for transmission to complete)
 * @param port UART port number
 * @return 0 on success, negative on error
 */
int hal_uart_flush(uart_port_t port);

/**
 * @brief Clear RX buffer
 * @param port UART port number
 * @return 0 on success, negative on error
 */
int hal_uart_clear_rx(uart_port_t port);

#ifdef __cplusplus
}
#endif
