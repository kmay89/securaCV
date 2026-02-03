/**
 * @file chirp_channel.h
 * @brief Privacy-first Community Witness Network
 *
 * Anonymous community alert system with ephemeral identities.
 * "Safety in numbers, not surveillance"
 * "Witness authority, not neighbors"
 *
 * Key Properties:
 * - Ephemeral session identity (new each enable/reboot)
 * - Human-in-the-loop (no automated broadcasts)
 * - STRUCTURED TEMPLATES ONLY (no free text - abuse prevention)
 * - 3-hop max range (neighborhood only)
 * - No persistent history
 * - Escalating cooldowns (prevents spam/hysteria)
 * - Witness confirmation requirement (2 needed before relay)
 * - Community suppress voting (50% dismiss = suppress)
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

#define CHIRP_SESSION_ID_SIZE       8       // Session identifier size
#define CHIRP_EMOJI_DISPLAY_SIZE    20      // Max emoji string (3 x 6 bytes + null)
#define CHIRP_NONCE_SIZE            8       // Message nonce size
#define CHIRP_MAX_RECENT            32      // Recent chirps to track
#define CHIRP_MAX_NEARBY            16      // Nearby devices to track
#define CHIRP_MAX_NONCE_CACHE       64      // Nonces to deduplicate
#define CHIRP_MAX_HOPS              3       // Maximum relay hops

// Timing constants (milliseconds)
#define CHIRP_PRESENCE_INTERVAL_MS  15000   // Presence beacon interval
#define CHIRP_PRESENCE_TIMEOUT_MS   45000   // Nearby device timeout
#define CHIRP_RECENT_TTL_MS         1800000 // 30 min: chirp visibility
#define CHIRP_RELAY_DELAY_MIN_MS    2000    // Min delay before relay
#define CHIRP_RELAY_DELAY_MAX_MS    8000    // Max delay before relay
#define CHIRP_PRESENCE_REQ_MS       600000  // 10 min: required before sending

// Cooldown tiers (escalating)
#define CHIRP_COOLDOWN_TIER1_MS     30000   // 30 sec: first chirp
#define CHIRP_COOLDOWN_TIER2_MS     300000  // 5 min: 2nd chirp
#define CHIRP_COOLDOWN_TIER3_MS     1800000 // 30 min: 3rd chirp
#define CHIRP_COOLDOWN_TIER4_MS     3600000 // 60 min: 4th+ chirp
#define CHIRP_COOLDOWN_RESET_MS     86400000 // 24 hr: reset tiers

// ============================================================================
// ENUMS
// ============================================================================

/**
 * @brief Chirp channel state
 */
typedef enum {
    CHIRP_STATE_DISABLED = 0,   // Feature disabled
    CHIRP_STATE_INITIALIZING,   // Loading settings
    CHIRP_STATE_READY,          // Active, can send/receive
    CHIRP_STATE_COOLDOWN,       // Active but in send cooldown
    CHIRP_STATE_MUTED,          // Temporarily muted
    CHIRP_STATE_ERROR,          // Error state
} chirp_state_t;

/**
 * @brief Chirp message category
 */
typedef enum {
    CHIRP_CAT_AUTHORITY = 0,    // Law enforcement presence
    CHIRP_CAT_INFRA,            // Infrastructure issues
    CHIRP_CAT_EMERGENCY,        // Immediate danger
    CHIRP_CAT_WEATHER,          // Environmental threats
    CHIRP_CAT_MUTUAL_AID,       // Community support
    CHIRP_CAT_ALL_CLEAR,        // De-escalation
} chirp_category_t;

/**
 * @brief Chirp urgency level
 */
typedef enum {
    CHIRP_URG_INFO = 0,         // Informational
    CHIRP_URG_CAUTION,          // Attention advised
    CHIRP_URG_URGENT,           // Immediate attention
} chirp_urgency_t;

/**
 * @brief Chirp template IDs
 *
 * NO FREE TEXT ALLOWED - all messages use predefined templates.
 */
typedef enum {
    // Authority presence (0x00-0x0F)
    CHIRP_TPL_AUTH_POLICE_ACTIVITY = 0x00,
    CHIRP_TPL_AUTH_HEAVY_RESPONSE = 0x01,
    CHIRP_TPL_AUTH_ROAD_BLOCKED = 0x02,
    CHIRP_TPL_AUTH_HELICOPTER = 0x03,
    CHIRP_TPL_AUTH_FEDERAL = 0x04,

    // Infrastructure (0x10-0x1F)
    CHIRP_TPL_INFRA_POWER_OUT = 0x10,
    CHIRP_TPL_INFRA_WATER_ISSUE = 0x11,
    CHIRP_TPL_INFRA_GAS_SMELL = 0x12,
    CHIRP_TPL_INFRA_INTERNET_DOWN = 0x13,
    CHIRP_TPL_INFRA_ROAD_CLOSED = 0x14,

    // Emergency (0x20-0x2F)
    CHIRP_TPL_EMERG_FIRE = 0x20,
    CHIRP_TPL_EMERG_MEDICAL = 0x21,
    CHIRP_TPL_EMERG_MULTI_AMBULANCE = 0x22,
    CHIRP_TPL_EMERG_EVACUATION = 0x23,
    CHIRP_TPL_EMERG_SHELTER = 0x24,

    // Weather (0x30-0x3F)
    CHIRP_TPL_WX_SEVERE = 0x30,
    CHIRP_TPL_WX_TORNADO = 0x31,
    CHIRP_TPL_WX_FLOOD = 0x32,
    CHIRP_TPL_WX_LIGHTNING = 0x33,

    // Mutual aid (0x40-0x4F)
    CHIRP_TPL_AID_WELFARE = 0x40,
    CHIRP_TPL_AID_SUPPLIES = 0x41,
    CHIRP_TPL_AID_OFFERING = 0x42,

    // All clear (0x80-0x8F)
    CHIRP_TPL_CLR_RESOLVED = 0x80,
    CHIRP_TPL_CLR_SAFE = 0x81,
    CHIRP_TPL_CLR_FALSE_ALARM = 0x82,
} chirp_template_t;

/**
 * @brief Chirp detail modifier
 */
typedef enum {
    CHIRP_DETAIL_NONE = 0,
    CHIRP_DETAIL_SCALE_FEW = 1,
    CHIRP_DETAIL_SCALE_MANY = 2,
    CHIRP_DETAIL_SCALE_MASSIVE = 3,
    CHIRP_DETAIL_STATUS_ONGOING = 10,
    CHIRP_DETAIL_STATUS_CONTAINED = 11,
    CHIRP_DETAIL_STATUS_SPREADING = 12,
    CHIRP_DETAIL_DIR_NORTH = 20,
    CHIRP_DETAIL_DIR_SOUTH = 21,
    CHIRP_DETAIL_DIR_EAST = 22,
    CHIRP_DETAIL_DIR_WEST = 23,
} chirp_detail_t;

/**
 * @brief Acknowledgment type
 */
typedef enum {
    CHIRP_ACK_SEEN = 0,         // Saw the chirp
    CHIRP_ACK_CONFIRMED,        // Confirming I also witness this
    CHIRP_ACK_RESOLVED,         // Situation resolved
} chirp_ack_type_t;

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief Nearby device information
 */
typedef struct {
    uint8_t session_id[CHIRP_SESSION_ID_SIZE];
    char emoji[CHIRP_EMOJI_DISPLAY_SIZE];
    int8_t rssi;
    uint32_t last_seen_ms;
    bool listening;
} chirp_nearby_t;

/**
 * @brief Received chirp information
 */
typedef struct {
    uint8_t nonce[CHIRP_NONCE_SIZE];
    uint8_t sender_id[CHIRP_SESSION_ID_SIZE];
    char sender_emoji[CHIRP_EMOJI_DISPLAY_SIZE];
    chirp_template_t template_id;
    chirp_detail_t detail;
    chirp_urgency_t urgency;
    uint8_t hop_count;
    uint8_t ttl_minutes;
    uint32_t received_ms;
    uint8_t confirm_count;
    bool validated;             // Has 2+ confirmations
    bool relayed;               // Already relayed
    bool suppressed;            // Community suppressed
    bool dismissed;             // User dismissed
} chirp_received_t;

/**
 * @brief Chirp channel status
 */
typedef struct {
    chirp_state_t state;
    char session_emoji[CHIRP_EMOJI_DISPLAY_SIZE];
    uint8_t nearby_count;
    uint8_t recent_chirp_count;
    uint32_t last_chirp_sent_ms;
    uint32_t cooldown_remaining_ms;
    uint8_t cooldown_tier;
    bool relay_enabled;
    bool muted;
    uint32_t mute_remaining_ms;
    bool presence_met;          // 10 min presence requirement
} chirp_status_t;

// ============================================================================
// CALLBACKS
// ============================================================================

/**
 * @brief Chirp received callback
 */
typedef void (*chirp_received_cb_t)(
    const chirp_received_t* chirp,
    void* user_data
);

/**
 * @brief Nearby devices changed callback
 */
typedef void (*chirp_nearby_cb_t)(
    uint8_t nearby_count,
    void* user_data
);

/**
 * @brief State change callback
 */
typedef void (*chirp_state_cb_t)(
    chirp_state_t old_state,
    chirp_state_t new_state,
    void* user_data
);

// ============================================================================
// CONFIGURATION
// ============================================================================

/**
 * @brief Chirp channel configuration
 */
typedef struct {
    bool auto_relay;                    // Auto-relay validated chirps
    chirp_urgency_t min_urgency;        // Minimum urgency to display
    chirp_received_cb_t chirp_callback;
    chirp_nearby_cb_t nearby_callback;
    chirp_state_cb_t state_callback;
    void* user_data;
} chirp_config_t;

// Default configuration
#define CHIRP_CONFIG_DEFAULT { \
    .auto_relay = true, \
    .min_urgency = CHIRP_URG_INFO, \
    .chirp_callback = NULL, \
    .nearby_callback = NULL, \
    .state_callback = NULL, \
    .user_data = NULL, \
}

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize chirp channel
 * @param config Configuration
 * @return RESULT_OK on success
 */
result_t chirp_init(const chirp_config_t* config);

/**
 * @brief Deinitialize chirp channel
 * @return RESULT_OK on success
 */
result_t chirp_deinit(void);

/**
 * @brief Enable chirp channel
 *
 * Generates new ephemeral session identity.
 *
 * @return RESULT_OK on success
 */
result_t chirp_enable(void);

/**
 * @brief Disable chirp channel
 *
 * Destroys session identity.
 *
 * @return RESULT_OK on success
 */
result_t chirp_disable(void);

/**
 * @brief Process chirp channel (call from main loop)
 *
 * Handles presence beacons, relay decisions, cleanup.
 */
void chirp_process(void);

// ============================================================================
// STATUS
// ============================================================================

/**
 * @brief Check if chirp channel is enabled
 * @return true if enabled
 */
bool chirp_is_enabled(void);

/**
 * @brief Get chirp channel status
 * @param status Output status
 * @return RESULT_OK on success
 */
result_t chirp_get_status(chirp_status_t* status);

/**
 * @brief Get session emoji string
 * @return Session emoji (or empty if disabled)
 */
const char* chirp_get_session_emoji(void);

/**
 * @brief Check if presence requirement is met
 *
 * User must be active for 10 minutes before sending.
 *
 * @return true if can send
 */
bool chirp_has_presence(void);

/**
 * @brief Check if can send a chirp now
 * @return true if not in cooldown
 */
bool chirp_can_send(void);

/**
 * @brief Get current cooldown tier (1-4)
 * @return Cooldown tier
 */
uint8_t chirp_get_cooldown_tier(void);

/**
 * @brief Get remaining cooldown time
 * @return Milliseconds remaining in cooldown
 */
uint32_t chirp_get_cooldown_remaining(void);

// ============================================================================
// SENDING
// ============================================================================

/**
 * @brief Send a chirp using template
 *
 * HUMAN CONFIRMATION REQUIRED - no automated broadcasts.
 *
 * @param template_id Template to use (no free text)
 * @param urgency Urgency level
 * @param detail Optional detail modifier
 * @param ttl_minutes Time-to-live in minutes
 * @return RESULT_OK on success
 */
result_t chirp_send(
    chirp_template_t template_id,
    chirp_urgency_t urgency,
    chirp_detail_t detail,
    uint8_t ttl_minutes
);

/**
 * @brief Send all-clear for a situation
 * @param clear_type Type of all-clear
 * @return RESULT_OK on success
 */
result_t chirp_send_all_clear(chirp_template_t clear_type);

// ============================================================================
// RECEIVING
// ============================================================================

/**
 * @brief Get recent chirps
 * @param chirps Output array (must be at least CHIRP_MAX_RECENT)
 * @param max_chirps Maximum to return
 * @return Number of chirps returned
 */
int chirp_get_recent(chirp_received_t* chirps, size_t max_chirps);

/**
 * @brief Get nearby devices
 * @param nearby Output array (must be at least CHIRP_MAX_NEARBY)
 * @param max_nearby Maximum to return
 * @return Number of devices returned
 */
int chirp_get_nearby(chirp_nearby_t* nearby, size_t max_nearby);

/**
 * @brief Confirm a chirp (I also witness this)
 * @param nonce Chirp nonce
 * @return RESULT_OK on success
 */
result_t chirp_confirm(const uint8_t nonce[CHIRP_NONCE_SIZE]);

/**
 * @brief Dismiss a chirp from display
 * @param nonce Chirp nonce
 * @return RESULT_OK on success
 */
result_t chirp_dismiss(const uint8_t nonce[CHIRP_NONCE_SIZE]);

/**
 * @brief Clear all received chirps
 */
void chirp_clear_all(void);

// ============================================================================
// MUTING
// ============================================================================

/**
 * @brief Mute chirps for duration
 * @param duration_minutes Duration (15, 30, 60, or 120)
 * @return RESULT_OK on success
 */
result_t chirp_mute(uint8_t duration_minutes);

/**
 * @brief Unmute chirps
 * @return RESULT_OK on success
 */
result_t chirp_unmute(void);

/**
 * @brief Check if muted
 * @return true if muted
 */
bool chirp_is_muted(void);

// ============================================================================
// SETTINGS
// ============================================================================

/**
 * @brief Set relay enabled
 * @param enabled true to enable relay
 */
void chirp_set_relay_enabled(bool enabled);

/**
 * @brief Check if relay is enabled
 * @return true if relay enabled
 */
bool chirp_is_relay_enabled(void);

/**
 * @brief Set urgency filter
 * @param min_urgency Minimum urgency to display
 */
void chirp_set_urgency_filter(chirp_urgency_t min_urgency);

/**
 * @brief Get current urgency filter
 * @return Minimum urgency setting
 */
chirp_urgency_t chirp_get_urgency_filter(void);

// ============================================================================
// TEMPLATE HELPERS
// ============================================================================

/**
 * @brief Check if template ID is valid
 * @param template_id Template to check
 * @return true if valid
 */
bool chirp_is_valid_template(chirp_template_t template_id);

/**
 * @brief Get template text
 * @param template_id Template ID
 * @return Human-readable text
 */
const char* chirp_get_template_text(chirp_template_t template_id);

/**
 * @brief Get detail text
 * @param detail Detail modifier
 * @return Human-readable text
 */
const char* chirp_get_detail_text(chirp_detail_t detail);

/**
 * @brief Get state name
 * @param state State value
 * @return State name string
 */
const char* chirp_state_name(chirp_state_t state);

/**
 * @brief Get category name
 * @param category Category value
 * @return Category name string
 */
const char* chirp_category_name(chirp_category_t category);

/**
 * @brief Get urgency name
 * @param urgency Urgency value
 * @return Urgency name string
 */
const char* chirp_urgency_name(chirp_urgency_t urgency);

/**
 * @brief Check if template is allowed at night
 * @param template_id Template to check
 * @return true if allowed during night hours
 */
bool chirp_template_night_allowed(chirp_template_t template_id);

/**
 * @brief Get category for template
 * @param template_id Template ID
 * @return Category
 */
chirp_category_t chirp_template_category(chirp_template_t template_id);

// ============================================================================
// INTERNAL (for platform implementations)
// ============================================================================

/**
 * @brief Handle incoming raw message
 *
 * Called by platform-specific transport (ESP-NOW, etc.)
 *
 * @param src_mac Source MAC address (6 bytes)
 * @param data Message data
 * @param len Message length
 * @param rssi Signal strength
 */
void chirp_handle_message(
    const uint8_t src_mac[6],
    const uint8_t* data,
    size_t len,
    int8_t rssi
);

#ifdef __cplusplus
}
#endif
