/**
 * @file hal_timer.h
 * @brief HAL Timer Interface
 *
 * Provides hardware-independent timer functionality.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// TIMER TYPES
// ============================================================================

typedef int timer_id_t;

/**
 * @brief Timer callback function type
 * @param timer_id Timer that triggered
 * @param user_data User-provided context
 */
typedef void (*timer_callback_t)(timer_id_t timer_id, void* user_data);

typedef struct {
    uint32_t period_us;         // Period in microseconds
    bool auto_reload;           // Repeat or one-shot
    timer_callback_t callback;
    void* user_data;
} timer_config_t;

// ============================================================================
// TIMER FUNCTIONS
// ============================================================================

/**
 * @brief Create a timer
 * @param config Timer configuration
 * @return Timer ID (>= 0) or negative error code
 */
timer_id_t hal_timer_create(const timer_config_t* config);

/**
 * @brief Start a timer
 * @param timer_id Timer ID
 * @return 0 on success, negative on error
 */
int hal_timer_start(timer_id_t timer_id);

/**
 * @brief Stop a timer
 * @param timer_id Timer ID
 * @return 0 on success, negative on error
 */
int hal_timer_stop(timer_id_t timer_id);

/**
 * @brief Delete a timer
 * @param timer_id Timer ID
 * @return 0 on success, negative on error
 */
int hal_timer_delete(timer_id_t timer_id);

/**
 * @brief Check if timer is running
 * @param timer_id Timer ID
 * @return true if running, false otherwise
 */
bool hal_timer_is_running(timer_id_t timer_id);

/**
 * @brief Change timer period
 * @param timer_id Timer ID
 * @param period_us New period in microseconds
 * @return 0 on success, negative on error
 */
int hal_timer_set_period(timer_id_t timer_id, uint32_t period_us);

// ============================================================================
// SIMPLE DELAY TIMERS (non-blocking checks)
// ============================================================================

/**
 * @brief Simple elapsed time tracker
 */
typedef struct {
    uint32_t start_ms;
    uint32_t duration_ms;
} hal_deadline_t;

/**
 * @brief Start a deadline timer
 * @param deadline Deadline structure
 * @param duration_ms Duration in milliseconds
 */
void hal_deadline_start(hal_deadline_t* deadline, uint32_t duration_ms);

/**
 * @brief Check if deadline has expired
 * @param deadline Deadline structure
 * @return true if expired
 */
bool hal_deadline_expired(const hal_deadline_t* deadline);

/**
 * @brief Get remaining time until deadline
 * @param deadline Deadline structure
 * @return Milliseconds remaining (0 if expired)
 */
uint32_t hal_deadline_remaining(const hal_deadline_t* deadline);

#ifdef __cplusplus
}
#endif
