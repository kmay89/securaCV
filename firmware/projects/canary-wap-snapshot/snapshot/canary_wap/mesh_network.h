/*
 * SecuraCV Canary — Mesh Network (Opera Protocol)
 * Version 0.1.0
 *
 * Secure peer-to-peer mesh network for canary devices.
 * Enables mutual protection: devices alert each other of tamper/power events.
 *
 * Security Properties:
 * - Ed25519 device key authentication
 * - ChaCha20-Poly1305 encrypted messages
 * - Opera isolation (prevents neighbor interference)
 * - Visual pairing confirmation codes
 * - Replay prevention with monotonic counters
 *
 * See spec/canary_mesh_network_v0.md for full protocol specification.
 */

#ifndef SECURACV_MESH_NETWORK_H
#define SECURACV_MESH_NETWORK_H

#include <Arduino.h>
#include <esp_now.h>
#include "log_level.h"

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

namespace mesh_network {

// Protocol version
static const uint8_t PROTOCOL_VERSION = 0;

// Network limits
static const size_t MAX_OPERA_SIZE = 16;           // Maximum peers in an opera
static const size_t MAX_PEER_NAME_LEN = 24;        // Max device name length
static const size_t MAX_OPERA_NAME_LEN = 32;       // Max opera name length
static const size_t MAX_MESSAGE_SIZE = 250;        // ESP-NOW limit
static const size_t MAX_ALERT_HISTORY = 32;        // Stored alerts

// Timing (milliseconds)
static const uint32_t HEARTBEAT_INTERVAL_MS = 30000;   // Send heartbeat every 30s
static const uint32_t PEER_STALE_MS = 90000;           // Peer stale after 90s
static const uint32_t PEER_OFFLINE_MS = 300000;        // Peer offline after 5min
static const uint32_t AUTH_TIMEOUT_MS = 10000;         // Authentication timeout
static const uint32_t PAIRING_TIMEOUT_MS = 120000;     // Pairing timeout (2min)
static const uint32_t RECONNECT_INTERVAL_MS = 5000;    // Reconnect attempt interval
static const uint32_t MESSAGE_TTL_MS = 300000;         // Message validity (5min)

// Crypto sizes
static const size_t OPERA_ID_SIZE = 16;
static const size_t OPERA_SECRET_SIZE = 32;
static const size_t PUBKEY_SIZE = 32;
static const size_t PRIVKEY_SIZE = 32;
static const size_t SIGNATURE_SIZE = 64;
static const size_t FINGERPRINT_SIZE = 8;
static const size_t NONCE_SIZE = 12;
static const size_t SESSION_KEY_SIZE = 32;
static const size_t AUTH_CHALLENGE_SIZE = 32;

// ESP-NOW configuration
static const uint8_t ESPNOW_CHANNEL = 1;
extern const uint8_t BROADCAST_ADDR[6];

// ════════════════════════════════════════════════════════════════════════════
// ENUMS
// ════════════════════════════════════════════════════════════════════════════

// Mesh network state
enum MeshState : uint8_t {
  MESH_DISABLED = 0,       // Feature disabled
  MESH_INITIALIZING,       // Loading config, starting transports
  MESH_NO_FLOCK,           // No opera configured, awaiting pairing
  MESH_CONNECTING,         // Attempting to reach opera members
  MESH_ACTIVE,             // Connected to one or more peers
  MESH_PAIRING_INIT,       // Pairing mode - initiator
  MESH_PAIRING_JOIN,       // Pairing mode - joiner
  MESH_PAIRING_CONFIRM,    // Awaiting user confirmation
  MESH_ERROR               // Fatal error
};

// Individual peer state
enum PeerState : uint8_t {
  PEER_UNKNOWN = 0,        // Never contacted
  PEER_DISCOVERED,         // Found via broadcast
  PEER_AUTHENTICATING,     // Auth handshake in progress
  PEER_CONNECTED,          // Authenticated and active
  PEER_STALE,              // No heartbeat for 90s
  PEER_OFFLINE,            // No heartbeat for 5min
  PEER_ALERT,              // Received alert from this peer
  PEER_REMOVED             // Removed from opera
};

// Message types
enum MessageType : uint8_t {
  MSG_HEARTBEAT = 0,
  MSG_AUTH_CHALLENGE,
  MSG_AUTH_RESPONSE,
  MSG_AUTH_COMPLETE,
  MSG_TAMPER_ALERT,
  MSG_POWER_ALERT,
  MSG_OFFLINE_IMMINENT,
  MSG_PEER_LIST,
  MSG_PAIR_DISCOVER,
  MSG_PAIR_OFFER,
  MSG_PAIR_ACCEPT,
  MSG_PAIR_CONFIRM,
  MSG_PAIR_COMPLETE,
  MSG_LEAVE_OPERA,
  MSG_ENCRYPTED            // Encrypted payload wrapper
};

// Alert types
enum AlertType : uint8_t {
  ALERT_TAMPER = 0,        // Physical tamper detected
  ALERT_MOTION,            // Unexpected motion
  ALERT_BREACH,            // Enclosure breach
  ALERT_POWER_LOSS,        // Main power lost
  ALERT_LOW_VOLTAGE,       // Voltage dropping
  ALERT_BATTERY_CRITICAL,  // Battery nearly depleted
  ALERT_OFFLINE_SHUTDOWN,  // Graceful shutdown
  ALERT_OFFLINE_TAMPER,    // Forced offline by tamper
  ALERT_OFFLINE_POWER,     // Forced offline by power loss
  ALERT_OFFLINE_REBOOT     // Voluntary reboot
};

// Pairing role
enum PairingRole : uint8_t {
  PAIR_ROLE_NONE = 0,
  PAIR_ROLE_INITIATOR,     // Existing opera member
  PAIR_ROLE_JOINER         // New device joining
};

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

// Peer information
struct OperaPeer {
  uint8_t pubkey[PUBKEY_SIZE];              // Device public key
  uint8_t fingerprint[FINGERPRINT_SIZE];    // Key fingerprint
  uint8_t mac_addr[6];                      // ESP-NOW MAC address
  uint8_t session_key[SESSION_KEY_SIZE];    // Derived session key
  char name[MAX_PEER_NAME_LEN + 1];         // User-friendly name
  PeerState state;
  uint64_t msg_counter_tx;                  // Outgoing message counter
  uint64_t msg_counter_rx;                  // Last received counter
  uint32_t last_seen_ms;                    // Last heartbeat received
  uint32_t last_tx_ms;                      // Last message sent
  int8_t rssi;                              // Signal strength
  uint8_t alerts_received;                  // Alert count from this peer
  bool session_established;                 // Session key derived
};

// Opera configuration (persisted to NVS)
struct OperaConfig {
  bool enabled;
  bool configured;                          // Has opera been set up
  uint8_t opera_id[OPERA_ID_SIZE];
  uint8_t opera_secret[OPERA_SECRET_SIZE];
  char opera_name[MAX_OPERA_NAME_LEN + 1];
  uint8_t peer_count;
  // Peer pubkeys stored separately
};

// Mesh network status
struct MeshStatus {
  MeshState state;
  bool espnow_active;
  uint8_t peers_total;
  uint8_t peers_online;
  uint8_t peers_offline;
  uint8_t peers_stale;
  uint32_t messages_sent;
  uint32_t messages_received;
  uint32_t alerts_sent;
  uint32_t alerts_received;
  uint32_t auth_failures;
  uint32_t uptime_ms;
  uint32_t last_heartbeat_ms;
  char opera_id_hex[OPERA_ID_SIZE * 2 + 1];
};

// Pairing session state
struct PairingSession {
  PairingRole role;
  uint8_t peer_pubkey[PUBKEY_SIZE];
  uint8_t peer_mac[6];
  uint8_t ephemeral_pubkey[PUBKEY_SIZE];
  uint8_t ephemeral_privkey[PRIVKEY_SIZE];
  uint8_t session_key[SESSION_KEY_SIZE];
  uint32_t confirmation_code;               // 6-digit code
  uint32_t started_ms;
  bool code_displayed;
  bool code_confirmed;
};

// Alert record
struct MeshAlert {
  uint32_t timestamp_ms;
  AlertType type;
  LogLevel severity;
  uint8_t sender_fp[FINGERPRINT_SIZE];
  char sender_name[MAX_PEER_NAME_LEN + 1];
  uint32_t witness_seq;                     // Related witness record
  char detail[48];
};

// ════════════════════════════════════════════════════════════════════════════
// MESSAGE STRUCTURES
// ════════════════════════════════════════════════════════════════════════════

// Common message header (unencrypted)
struct MessageHeader {
  uint8_t version;
  uint8_t msg_type;
  uint8_t opera_id[OPERA_ID_SIZE];
  uint8_t sender_fp[FINGERPRINT_SIZE];
  uint64_t counter;
  uint32_t timestamp;
};

// Heartbeat payload
struct HeartbeatPayload {
  uint8_t status;                           // 0=online, 1=low_battery, 2=warning
  uint32_t uptime_sec;
  uint8_t peer_count;
  uint8_t battery_pct;                      // 0-100 or 255 if unknown
};

// Authentication challenge
struct AuthChallengePayload {
  uint8_t nonce[AUTH_CHALLENGE_SIZE];
  uint8_t pubkey[PUBKEY_SIZE];
};

// Authentication response
struct AuthResponsePayload {
  uint8_t challenge_sig[SIGNATURE_SIZE];
  uint8_t pubkey[PUBKEY_SIZE];
  uint8_t opera_proof[SIGNATURE_SIZE];      // Signs opera_id
};

// Tamper alert payload
struct TamperAlertPayload {
  uint8_t alert_type;
  uint8_t severity;
  uint32_t witness_seq;
  char detail[48];
};

// Power alert payload
struct PowerAlertPayload {
  uint8_t alert_type;
  uint16_t voltage_mv;
  uint16_t estimated_runtime_sec;
};

// Offline imminent payload
struct OfflineImminentPayload {
  uint8_t reason;                           // AlertType for offline reason
  uint32_t final_seq;
  uint8_t final_chain_hash[8];
};

// Pairing discovery broadcast
struct PairDiscoverPayload {
  uint8_t pubkey[PUBKEY_SIZE];
  char device_name[MAX_PEER_NAME_LEN + 1];
  uint8_t role;                             // PairingRole
};

// Pairing offer (initiator -> joiner)
struct PairOfferPayload {
  uint8_t ephemeral_pubkey[PUBKEY_SIZE];
  uint8_t device_pubkey[PUBKEY_SIZE];
  char opera_name[MAX_OPERA_NAME_LEN + 1];
  uint8_t opera_member_count;
};

// Pairing confirmation (after code verified)
struct PairConfirmPayload {
  uint8_t confirmation_hash[32];            // Proves both saw same code
};

// Pairing complete (sends encrypted opera secret)
struct PairCompletePayload {
  uint8_t encrypted_secret[OPERA_SECRET_SIZE + 16];  // ChaCha20-Poly1305
  uint8_t nonce[NONCE_SIZE];
};

// ════════════════════════════════════════════════════════════════════════════
// CALLBACKS
// ════════════════════════════════════════════════════════════════════════════

// Callback when alert is received from peer
typedef void (*AlertCallback)(const MeshAlert* alert);

// Callback when peer state changes
typedef void (*PeerStateCallback)(const OperaPeer* peer, PeerState old_state, PeerState new_state);

// Callback when pairing state changes
typedef void (*PairingCallback)(PairingRole role, uint32_t confirmation_code, bool success);

// ════════════════════════════════════════════════════════════════════════════
// FUNCTION DECLARATIONS
// ════════════════════════════════════════════════════════════════════════════

// ──────────────────────────────────────────────────────────────────────────
// Initialization
// ──────────────────────────────────────────────────────────────────────────

// Initialize mesh network (call once at boot)
// device_privkey: 32-byte Ed25519 private key
// device_pubkey: 32-byte Ed25519 public key
// device_name: User-friendly device name
bool init(const uint8_t* device_privkey, const uint8_t* device_pubkey, const char* device_name);

// Shutdown mesh network
void deinit();

// Enable or disable mesh networking
void set_enabled(bool enabled);
bool is_enabled();

// ──────────────────────────────────────────────────────────────────────────
// Main loop
// ──────────────────────────────────────────────────────────────────────────

// Call from main loop to process messages and maintain connections
void update();

// ──────────────────────────────────────────────────────────────────────────
// Status
// ──────────────────────────────────────────────────────────────────────────

// Get current mesh status
MeshStatus get_status();

// Get mesh state as string
const char* state_name(MeshState state);

// Get peer state as string
const char* peer_state_name(PeerState state);

// Get alert type as string
const char* alert_type_name(AlertType type);

// Check if mesh is active and connected
bool is_active();

// Check if we're in a opera
bool has_opera();

// ──────────────────────────────────────────────────────────────────────────
// Peer management
// ──────────────────────────────────────────────────────────────────────────

// Get peer count
uint8_t get_peer_count();

// Get peer by index (0 to peer_count-1)
const OperaPeer* get_peer(uint8_t index);

// Get peer by fingerprint
const OperaPeer* get_peer_by_fingerprint(const uint8_t* fingerprint);

// Get online peer count
uint8_t get_online_peer_count();

// Remove peer from opera (requires re-keying)
bool remove_peer(const uint8_t* fingerprint);

// ──────────────────────────────────────────────────────────────────────────
// Opera management
// ──────────────────────────────────────────────────────────────────────────

// Get opera configuration
const OperaConfig* get_opera_config();

// Set opera name
bool set_opera_name(const char* name);

// Leave current opera
bool leave_opera();

// ──────────────────────────────────────────────────────────────────────────
// Pairing
// ──────────────────────────────────────────────────────────────────────────

// Start pairing as initiator (existing opera member or creating new opera)
bool start_pairing_initiator(const char* opera_name = nullptr);

// Start pairing as joiner (joining existing opera)
bool start_pairing_joiner();

// Cancel ongoing pairing
void cancel_pairing();

// Confirm pairing code matches
bool confirm_pairing();

// Get pairing session state
const PairingSession* get_pairing_session();

// Check if in pairing mode
bool is_pairing();

// ──────────────────────────────────────────────────────────────────────────
// Alerts
// ──────────────────────────────────────────────────────────────────────────

// Broadcast tamper alert to all peers
bool broadcast_tamper_alert(AlertType type, LogLevel severity, uint32_t witness_seq, const char* detail);

// Broadcast power alert to all peers
bool broadcast_power_alert(AlertType type, uint16_t voltage_mv, uint16_t estimated_runtime_sec);

// Broadcast offline imminent to all peers (call just before shutdown)
bool broadcast_offline_imminent(AlertType reason, uint32_t final_seq, const uint8_t* final_chain_hash);

// Get recent alerts
const MeshAlert* get_alerts(size_t* count);

// Clear alert history
void clear_alerts();

// ──────────────────────────────────────────────────────────────────────────
// Callbacks
// ──────────────────────────────────────────────────────────────────────────

// Set callback for received alerts
void set_alert_callback(AlertCallback callback);

// Set callback for peer state changes
void set_peer_state_callback(PeerStateCallback callback);

// Set callback for pairing events
void set_pairing_callback(PairingCallback callback);

// ──────────────────────────────────────────────────────────────────────────
// Low-level (for testing/debugging)
// ──────────────────────────────────────────────────────────────────────────

// Force heartbeat send
void send_heartbeat();

// Get message statistics
void get_message_stats(uint32_t* sent, uint32_t* received, uint32_t* errors);

} // namespace mesh_network

#endif // SECURACV_MESH_NETWORK_H
