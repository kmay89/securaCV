/**
 * @file log.h
 * @brief Logging infrastructure for SecuraCV firmware
 *
 * Provides a unified logging interface with multiple log levels,
 * compile-time filtering, and configurable output backends.
 */

#pragma once

#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// LOG LEVELS
// ============================================================================

typedef enum {
    LOG_LEVEL_NONE = 0,     // No logging
    LOG_LEVEL_ERROR = 1,    // Errors only
    LOG_LEVEL_WARN = 2,     // Warnings and errors
    LOG_LEVEL_INFO = 3,     // Informational messages
    LOG_LEVEL_DEBUG = 4,    // Debug messages
    LOG_LEVEL_VERBOSE = 5,  // Verbose debug
} log_level_t;

// Default log level if not defined
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

// ============================================================================
// LOG OUTPUT CONFIGURATION
// ============================================================================

/**
 * @brief Log output callback type
 * @param level Log level
 * @param tag Module tag
 * @param msg Formatted message
 */
typedef void (*log_output_fn)(log_level_t level, const char* tag, const char* msg);

/**
 * @brief Set log output function
 * @param fn Output function (NULL for default serial output)
 */
void log_set_output(log_output_fn fn);

/**
 * @brief Set runtime log level
 * @param level Minimum level to output
 */
void log_set_level(log_level_t level);

/**
 * @brief Get current log level
 * @return Current log level
 */
log_level_t log_get_level(void);

// ============================================================================
// LOG FUNCTIONS
// ============================================================================

/**
 * @brief Log a message at specified level
 * @param level Log level
 * @param tag Module tag
 * @param fmt Printf-style format string
 */
void log_write(log_level_t level, const char* tag, const char* fmt, ...);

/**
 * @brief Log a message with va_list
 */
void log_writev(log_level_t level, const char* tag, const char* fmt, va_list args);

// ============================================================================
// LOG MACROS
// ============================================================================

// Define the log tag for the current file
#ifndef LOG_TAG
#define LOG_TAG "APP"
#endif

// Compile-time filtered log macros
#if LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOG_E(fmt, ...) log_write(LOG_LEVEL_ERROR, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_E(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_WARN
#define LOG_W(fmt, ...) log_write(LOG_LEVEL_WARN, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_W(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_I(fmt, ...) log_write(LOG_LEVEL_INFO, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_I(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_D(fmt, ...) log_write(LOG_LEVEL_DEBUG, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_D(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_VERBOSE
#define LOG_V(fmt, ...) log_write(LOG_LEVEL_VERBOSE, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_V(fmt, ...) ((void)0)
#endif

// ============================================================================
// ASSERTION AND PANIC
// ============================================================================

/**
 * @brief Panic handler - called on unrecoverable errors
 * @param file Source file
 * @param line Line number
 * @param msg Error message
 */
void log_panic(const char* file, int line, const char* msg);

/**
 * @brief Assert macro - panics if condition is false
 */
#define LOG_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            log_panic(__FILE__, __LINE__, msg); \
        } \
    } while (0)

// ============================================================================
// HEX DUMP
// ============================================================================

/**
 * @brief Dump data as hex for debugging
 * @param tag Module tag
 * @param data Data to dump
 * @param len Data length
 */
void log_hexdump(const char* tag, const void* data, size_t len);

#ifdef __cplusplus
}
#endif
