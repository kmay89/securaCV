/**
 * @file hal_gpio.h
 * @brief HAL GPIO Interface
 *
 * Provides a hardware-independent GPIO interface. Implementations
 * are provided by board-specific code.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// GPIO TYPES
// ============================================================================

typedef int gpio_pin_t;

typedef enum {
    GPIO_MODE_INPUT = 0,
    GPIO_MODE_OUTPUT,
    GPIO_MODE_INPUT_PULLUP,
    GPIO_MODE_INPUT_PULLDOWN,
    GPIO_MODE_OUTPUT_OD,        // Open drain
    GPIO_MODE_ANALOG,
} gpio_mode_t;

typedef enum {
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_RISING,
    GPIO_INTR_FALLING,
    GPIO_INTR_BOTH,
    GPIO_INTR_LOW,
    GPIO_INTR_HIGH,
} gpio_intr_t;

/**
 * @brief GPIO interrupt callback function type
 * @param pin Pin that triggered the interrupt
 * @param user_data User-provided context
 */
typedef void (*gpio_isr_callback_t)(gpio_pin_t pin, void* user_data);

// ============================================================================
// GPIO FUNCTIONS
// ============================================================================

/**
 * @brief Configure a GPIO pin
 * @param pin Pin number
 * @param mode Pin mode
 * @return 0 on success, negative on error
 */
int hal_gpio_config(gpio_pin_t pin, gpio_mode_t mode);

/**
 * @brief Write digital value to GPIO
 * @param pin Pin number
 * @param value 0 for LOW, non-zero for HIGH
 */
void hal_gpio_write(gpio_pin_t pin, int value);

/**
 * @brief Read digital value from GPIO
 * @param pin Pin number
 * @return 0 for LOW, 1 for HIGH
 */
int hal_gpio_read(gpio_pin_t pin);

/**
 * @brief Toggle GPIO output
 * @param pin Pin number
 */
void hal_gpio_toggle(gpio_pin_t pin);

/**
 * @brief Attach interrupt to GPIO
 * @param pin Pin number
 * @param mode Interrupt trigger mode
 * @param callback Callback function
 * @param user_data User context passed to callback
 * @return 0 on success, negative on error
 */
int hal_gpio_attach_interrupt(gpio_pin_t pin, gpio_intr_t mode,
                              gpio_isr_callback_t callback, void* user_data);

/**
 * @brief Detach interrupt from GPIO
 * @param pin Pin number
 * @return 0 on success, negative on error
 */
int hal_gpio_detach_interrupt(gpio_pin_t pin);

/**
 * @brief Read analog value from GPIO (if ADC capable)
 * @param pin Pin number
 * @return ADC value (12-bit: 0-4095), negative on error
 */
int hal_gpio_analog_read(gpio_pin_t pin);

#ifdef __cplusplus
}
#endif
