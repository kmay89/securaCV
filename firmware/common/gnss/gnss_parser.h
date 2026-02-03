/**
 * @file gnss_parser.h
 * @brief NMEA GNSS/GPS parser
 *
 * Parses standard NMEA sentences from GPS/GNSS receivers.
 * Supports GGA, RMC, GSA, GSV, and VTG sentences.
 */

#pragma once

#include "../core/types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CONSTANTS
// ============================================================================

#define GNSS_NMEA_MAX_LEN       128     // Max NMEA sentence length
#define GNSS_FIELD_MAX_LEN      16      // Max field length

// ============================================================================
// PARSER STATE
// ============================================================================

/**
 * @brief GNSS parser state
 */
typedef struct {
    // Accumulated data
    gnss_fix_t fix;
    gnss_time_t time;

    // Parser state
    char sentence_buf[GNSS_NMEA_MAX_LEN];
    size_t sentence_len;
    bool in_sentence;

    // Statistics
    uint32_t gga_count;
    uint32_t rmc_count;
    uint32_t gsa_count;
    uint32_t gsv_count;
    uint32_t vtg_count;
    uint32_t checksum_errors;
    uint32_t parse_errors;

    // Callbacks
    void (*on_fix_update)(const gnss_fix_t* fix, void* user_data);
    void (*on_time_update)(const gnss_time_t* time, void* user_data);
    void* user_data;
} gnss_parser_t;

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize GNSS parser
 * @param parser Parser state
 */
void gnss_parser_init(gnss_parser_t* parser);

/**
 * @brief Reset parser state
 * @param parser Parser state
 */
void gnss_parser_reset(gnss_parser_t* parser);

/**
 * @brief Set fix update callback
 * @param parser Parser state
 * @param callback Callback function
 * @param user_data User context
 */
void gnss_parser_set_fix_callback(
    gnss_parser_t* parser,
    void (*callback)(const gnss_fix_t* fix, void* user_data),
    void* user_data
);

/**
 * @brief Set time update callback
 * @param parser Parser state
 * @param callback Callback function
 * @param user_data User context
 */
void gnss_parser_set_time_callback(
    gnss_parser_t* parser,
    void (*callback)(const gnss_time_t* time, void* user_data),
    void* user_data
);

// ============================================================================
// PARSING
// ============================================================================

/**
 * @brief Process incoming byte
 *
 * Call this for each byte received from the GNSS module.
 *
 * @param parser Parser state
 * @param byte Incoming byte
 * @return true if a complete sentence was parsed
 */
bool gnss_parser_process_byte(gnss_parser_t* parser, uint8_t byte);

/**
 * @brief Process multiple bytes
 *
 * @param parser Parser state
 * @param data Data buffer
 * @param len Data length
 * @return Number of sentences parsed
 */
int gnss_parser_process(gnss_parser_t* parser, const uint8_t* data, size_t len);

// ============================================================================
// DATA ACCESS
// ============================================================================

/**
 * @brief Get current fix data
 * @param parser Parser state
 * @return Pointer to fix data
 */
const gnss_fix_t* gnss_parser_get_fix(const gnss_parser_t* parser);

/**
 * @brief Get current time data
 * @param parser Parser state
 * @return Pointer to time data
 */
const gnss_time_t* gnss_parser_get_time(const gnss_parser_t* parser);

/**
 * @brief Check if fix is valid
 * @param parser Parser state
 * @return true if valid fix
 */
bool gnss_parser_has_fix(const gnss_parser_t* parser);

/**
 * @brief Get fix age in milliseconds
 * @param parser Parser state
 * @return Milliseconds since last valid fix update
 */
uint32_t gnss_parser_fix_age_ms(const gnss_parser_t* parser);

// ============================================================================
// STATISTICS
// ============================================================================

/**
 * @brief Get parser statistics
 */
typedef struct {
    uint32_t gga_count;
    uint32_t rmc_count;
    uint32_t gsa_count;
    uint32_t gsv_count;
    uint32_t vtg_count;
    uint32_t checksum_errors;
    uint32_t parse_errors;
} gnss_stats_t;

/**
 * @brief Get parser statistics
 * @param parser Parser state
 * @param stats Output statistics
 */
void gnss_parser_get_stats(const gnss_parser_t* parser, gnss_stats_t* stats);

// ============================================================================
// UTILITIES
// ============================================================================

/**
 * @brief Calculate distance between two points (Haversine)
 * @param lat1 Latitude 1 (degrees)
 * @param lon1 Longitude 1 (degrees)
 * @param lat2 Latitude 2 (degrees)
 * @param lon2 Longitude 2 (degrees)
 * @return Distance in meters
 */
double gnss_distance_m(double lat1, double lon1, double lat2, double lon2);

/**
 * @brief Calculate bearing between two points
 * @param lat1 Latitude 1 (degrees)
 * @param lon1 Longitude 1 (degrees)
 * @param lat2 Latitude 2 (degrees)
 * @param lon2 Longitude 2 (degrees)
 * @return Bearing in degrees (0-360)
 */
double gnss_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/**
 * @brief Convert knots to km/h
 */
static inline double gnss_knots_to_kmh(double knots) {
    return knots * 1.852;
}

/**
 * @brief Convert knots to m/s
 */
static inline double gnss_knots_to_mps(double knots) {
    return knots * 0.514444;
}

#ifdef __cplusplus
}
#endif
