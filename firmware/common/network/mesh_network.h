/**
 * @file mesh_network.h
 * @brief Opera Protocol secure mesh network
 *
 * Implements peer-to-peer secure mesh networking for witness devices.
 * Uses Ed25519 for device authentication and ChaCha20-Poly1305 for
 * message encryption.
 *
 * SECURITY PROPERTIES:
 * - Ed25519 device key authentication
 * - ChaCha20-Poly1305 encrypted messages
 * - Opera isolation (prevents neighbor interference)
 * - Visual pairing confirmation codes
 * - Replay prevention with monotonic counters
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

#define MESH_MAX_PEERS          8       // Maximum connected peers
#define MESH_MAX_MSG_SIZE       512     // Maximum message payload
#define MESH_DEVICE_ID_LEN      32      // Device ID length
#define MESH_PUBKEY_LEN         32      // Ed25519 public key
#define MESH_SIG_LEN            64      // Ed25519 signature
#define MESH_PAIRING_CODE_LEN   6       // Visual pairing code

// ============================================================================
// TYPES
// ============================================================================

/**
 * @brief Mesh network state
 */
typedef enum {
    MESH_STATE_DISABLED = 0,
    MESH_STATE_INITIALIZING,
    MESH_STATE_READY,
    MESH_STATE_PAIRING,
    MESH_STATE_CONNECTED,
    MESH_STATE_ERROR,
} mesh_state_t;

/**
 * @brief Peer connection state
 */
typedef enum {
    PEER_STATE_UNKNOWN = 0,
    PEER_STATE_DISCOVERED,
    PEER_STATE_PAIRING,
    PEER_STATE_PAIRED,
    PEER_STATE_CONNECTED,
    PEER_STATE_DISCONNECTED,
} peer_state_t;

/**
 * @brief Mesh message type
 */
typedef enum {
    MESH_MSG_DISCOVERY = 0,
    MESH_MSG_PAIRING_REQUEST,
    MESH_MSG_PAIRING_RESPONSE,
    MESH_MSG_PAIRING_CONFIRM,
    MESH_MSG_HEARTBEAT,
    MESH_MSG_WITNESS_RECORD,
    MESH_MSG_ATTESTATION_REQUEST,
    MESH_MSG_ATTESTATION_RESPONSE,
    MESH_MSG_ALERT,
} mesh_msg_type_t;

/**
 * @brief Peer information
 */
typedef struct {
    char device_id[MESH_DEVICE_ID_LEN];
    uint8_t public_key[MESH_PUBKEY_LEN];
    peer_state_t state;
    int8_t rssi;
    uint32_t last_seen_ms;
    uint32_t messages_sent;
    uint32_t messages_recv;
    uint8_t channel;
    bool trusted;
} mesh_peer_t;

/**
 * @brief Mesh network status
 */
typedef struct {
    mesh_state_t state;
    uint8_t peer_count;
    uint8_t connected_count;
    uint8_t channel;
    bool discoverable;
    uint32_t messages_sent;
    uint32_t messages_recv;
    uint32_t uptime_ms;
} mesh_status_t;

/**
 * @brief Mesh message callback
 */
typedef void (*mesh_msg_callback_t)(
    mesh_msg_type_t type,
    const mesh_peer_t* peer,
    const uint8_t* payload,
    size_t payload_len,
    void* user_data
);

/**
 * @brief Peer event callback
 */
typedef void (*mesh_peer_callback_t)(
    const mesh_peer_t* peer,
    peer_state_t old_state,
    peer_state_t new_state,
    void* user_data
);

// ============================================================================
// CONFIGURATION
// ============================================================================

/**
 * @brief Mesh network configuration
 */
typedef struct {
    const char* device_id;              // Device identifier
    const uint8_t* private_key;         // Ed25519 private key (32 bytes)
    const uint8_t* public_key;          // Ed25519 public key (32 bytes)
    const char* opera_id;               // Opera network identifier
    uint8_t channel;                    // Radio channel (0 = auto)
    bool auto_connect;                  // Auto-connect to known peers
    bool discoverable;                  // Allow discovery
    uint32_t heartbeat_interval_ms;     // Heartbeat interval
    uint32_t discovery_interval_ms;     // Discovery beacon interval
    mesh_msg_callback_t msg_callback;   // Message callback
    mesh_peer_callback_t peer_callback; // Peer event callback
    void* user_data;                    // Callback user data
} mesh_config_t;

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * @brief Initialize mesh network
 * @param config Configuration
 * @return RESULT_OK on success
 */
result_t mesh_init(const mesh_config_t* config);

/**
 * @brief Deinitialize mesh network
 * @return RESULT_OK on success
 */
result_t mesh_deinit(void);

/**
 * @brief Start mesh network
 * @return RESULT_OK on success
 */
result_t mesh_start(void);

/**
 * @brief Stop mesh network
 * @return RESULT_OK on success
 */
result_t mesh_stop(void);

/**
 * @brief Process mesh network (call from main loop)
 *
 * Handles message processing, heartbeats, and peer management.
 */
void mesh_process(void);

// ============================================================================
// STATUS
// ============================================================================

/**
 * @brief Get mesh network status
 * @param status Output status
 * @return RESULT_OK on success
 */
result_t mesh_get_status(mesh_status_t* status);

/**
 * @brief Get peer list
 * @param peers Output peer array
 * @param max_peers Maximum peers to return
 * @return Number of peers
 */
int mesh_get_peers(mesh_peer_t* peers, size_t max_peers);

/**
 * @brief Get peer by device ID
 * @param device_id Device identifier
 * @param peer Output peer info
 * @return RESULT_OK if found
 */
result_t mesh_get_peer(const char* device_id, mesh_peer_t* peer);

// ============================================================================
// PAIRING
// ============================================================================

/**
 * @brief Start pairing mode
 *
 * Enables discovery and generates pairing code.
 *
 * @param code Output pairing code (6 chars + null)
 * @return RESULT_OK on success
 */
result_t mesh_start_pairing(char code[MESH_PAIRING_CODE_LEN + 1]);

/**
 * @brief Stop pairing mode
 * @return RESULT_OK on success
 */
result_t mesh_stop_pairing(void);

/**
 * @brief Confirm pairing with code
 * @param device_id Peer device ID
 * @param code Pairing code to verify
 * @return RESULT_OK if code matches
 */
result_t mesh_confirm_pairing(const char* device_id, const char* code);

/**
 * @brief Unpair from device
 * @param device_id Device to unpair
 * @return RESULT_OK on success
 */
result_t mesh_unpair(const char* device_id);

// ============================================================================
// MESSAGING
// ============================================================================

/**
 * @brief Send message to peer
 * @param device_id Target device
 * @param type Message type
 * @param payload Message payload
 * @param payload_len Payload length
 * @return RESULT_OK on success
 */
result_t mesh_send(
    const char* device_id,
    mesh_msg_type_t type,
    const uint8_t* payload,
    size_t payload_len
);

/**
 * @brief Broadcast message to all connected peers
 * @param type Message type
 * @param payload Message payload
 * @param payload_len Payload length
 * @return Number of peers message was sent to
 */
int mesh_broadcast(
    mesh_msg_type_t type,
    const uint8_t* payload,
    size_t payload_len
);

/**
 * @brief Send witness record to all peers
 * @param record Witness record
 * @return Number of peers sent to
 */
int mesh_broadcast_witness(const witness_record_t* record);

// ============================================================================
// DISCOVERY
// ============================================================================

/**
 * @brief Enable/disable discovery
 * @param enable true to enable
 * @return RESULT_OK on success
 */
result_t mesh_set_discoverable(bool enable);

/**
 * @brief Trigger discovery scan
 * @return RESULT_OK on success
 */
result_t mesh_scan(void);

#ifdef __cplusplus
}
#endif
