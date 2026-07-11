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
#include "build_config.h"
#if FEATURE_MESH_NETWORK
#include <esp_now.h>
#endif
#include "log_level.h"
#include "mesh_beacon.h"        // BEACON_EVENT wire format (PR canary-wap parity)
#include "mesh_channel_hop.h"   // CHANNEL_LOCK wire format + HopTracker (PR 4b)
#include "mesh_hub_election.h"  // HUB_ELECTION wire format + HubMonitor (PR 4c)

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
//
// NOTE: ESP-NOW shares the WiFi radio. On ESP32 the radio is single-channel,
// so ESP-NOW can only operate on whichever channel the WiFi MAC is currently
// tuned to. Earlier revisions pinned this to channel 1, which fought the
// home WiFi whenever STA was associated on a different channel. The mesh
// channel is now chosen dynamically by mesh_channel_policy::current(); peer
// entries are registered with peer.channel = 0 so ESP-NOW follows the
// current radio. See mesh_channel_policy.h and docs/network_coexistence.md.
//
// ESPNOW_CHANNEL is retained as a build-time fallback for tests / pre-radio
// init paths only. Production code MUST go through mesh_channel_policy.
static const uint8_t ESPNOW_CHANNEL = 0;  // 0 = follow current radio channel
extern const uint8_t BROADCAST_ADDR[6];

// ════════════════════════════════════════════════════════════════════════════
// ENUMS
// ════════════════════════════════════════════════════════════════════════════

// Mesh network state
enum MeshState : uint8_t {
  MESH_DISABLED = 0,       // Feature disabled
  MESH_INITIALIZING,       // Loading config, starting transports
  MESH_NO_OPERA,           // No opera configured, awaiting pairing
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
  MSG_ENCRYPTED,           // Encrypted payload wrapper
  // v0.3 (audit O3 closure): transactional opera_secret rotation. Initiated
  // when remove_peer() is called; old surviving members each ACK the new
  // secret before it's committed to NVS.
  MSG_OPERA_REKEY,         // Initiator → each surviving member; encrypted new secret
  MSG_OPERA_REKEY_ACK,     // Surviving member → initiator; confirms receipt
  // PR (canary-wap parity): BLE Scout's arrived/departed transitions
  // broadcast to peer Canaries as a signed envelope carrying the
  // 25-byte mesh_beacon::Payload (state + label). Receivers decode
  // and forward to mesh_network::set_beacon_event_handler.
  MSG_BEACON_EVENT,
  // PR 4b: Hub → peers coordinated channel-hop proposal.
  // 2-byte mesh_channel_hop::Payload (channel + reason). Receivers
  // apply csi_hal::set_channel_lock() and forward to
  // mesh_network::set_channel_lock_handler.
  // Note: this numbering is for canary-wap's outer frame type byte,
  // separate from mesh_envelope::MsgType (PIO's inner signed header).
  // The two don't interop on the same mesh so the values need not match.
  MSG_CHANNEL_LOCK = 20,
  // PR 4c: Hub failover election broadcast.
  MSG_HUB_ELECTION = 21
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

// Opera rekey payload (v0.3, audit O3 closure).
// Sent from the device that triggered remove_peer() to each surviving member,
// encrypted under that member's existing session key. The receiver decrypts,
// installs the new secret + opera_id, and replies with MSG_OPERA_REKEY_ACK.
//
// Format: 12-byte nonce + (32-byte secret encrypted with ChaCha20-Poly1305) +
// 16-byte tag = 60 bytes.
struct OperaRekeyPayload {
  uint8_t nonce[NONCE_SIZE];
  uint8_t encrypted_secret[OPERA_SECRET_SIZE];
  uint8_t tag[16];
  uint32_t rekey_id;                        // monotonically increasing; latest wins
};

struct OperaRekeyAckPayload {
  uint32_t rekey_id;                        // echoes the rekey this ACK matches
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

// Get this device's own fingerprint (first 8 bytes of SHA-256 of pubkey).
// Returns false if mesh is not initialized.
bool get_self_fingerprint(uint8_t out[FINGERPRINT_SIZE]);

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

// ════════════════════════════════════════════════════════════════════════════
// REPLAY COUNTER PERSISTENCE
//
// Save/load per-peer msg_counter_rx to NVS so replay defense survives
// reboots. Called from canary_wap.ino at boot (load) and periodically
// from loop (save every 5 min) + on clean shutdown.
// ════════════════════════════════════════════════════════════════════════════

bool save_replay_counters();
bool load_replay_counters();

// ════════════════════════════════════════════════════════════════════════════
// BEACON EVENT (BLE Scout → mesh broadcast)
//
// canary-wap parity for the PIO mesh_session::send_beacon_event /
// on_opera_frame chain. Wire format lives in mesh_beacon.h; this
// API just calls send_to_peer for every paired peer in g_peers
// with the encoded payload + MSG_BEACON_EVENT type byte. Receivers
// decode and dispatch through the handler installed via
// set_beacon_event_handler.
//
// Threading: send_beacon_event MUST be called from the main loop
// (same task as update()). Outbound counter mutation lives inside
// each OperaPeer struct — sequential per-peer access only. If the
// caller's source of beacon events runs cross-task (e.g. ble_scout's
// broadcast callback from the NimBLE host task) the integration
// layer marshals through a queue before calling here.
// ════════════════════════════════════════════════════════════════════════════

// Broadcast a signed BEACON_EVENT to every connected peer. Returns
// the number of peers that accepted the send (mesh_transport-style
// semantics). Returns 0 if no opera_secret is loaded, no peers are
// connected, or label is too long for the wire format.
size_t send_beacon_event(mesh_beacon::BeaconState state,
                         const char*              label);

// Receive callback: invoked from the main loop when a verified
// MSG_BEACON_EVENT is decoded. `sender_fp` is the 8-byte peer
// fingerprint (already validated against g_peers); `label` is
// printable ASCII per the chokepoint contract.
typedef void (*beacon_event_handler_fn)(
    const uint8_t              sender_fp[FINGERPRINT_SIZE],
    mesh_beacon::BeaconState   state,
    const char*                label);

void set_beacon_event_handler(beacon_event_handler_fn fn);

// ════════════════════════════════════════════════════════════════════════════
// CHANNEL LOCK — coordinated channel-hop (PR 4b)
//
// The Hub broadcasts a CHANNEL_LOCK to all peers when channel utilization
// exceeds the threshold. Receivers should call csi_hal::set_channel_lock().
//
// Threading: send_channel_lock MUST be called from the main loop.
// ════════════════════════════════════════════════════════════════════════════

size_t send_channel_lock(uint8_t channel, mesh_channel_hop::Reason reason);

typedef void (*channel_lock_handler_fn)(
    const uint8_t              sender_fp[FINGERPRINT_SIZE],
    uint8_t                    channel,
    mesh_channel_hop::Reason   reason);

void set_channel_lock_handler(channel_lock_handler_fn fn);

// ════════════════════════════════════════════════════════════════════════════
// HUB ELECTION — failover broadcast (PR 4c)
//
// When the Hub's heartbeat is absent for 60s, sensors self-elect by
// lowest fingerprint and broadcast HUB_ELECTION. Peers verify the
// election is deterministic.
// ════════════════════════════════════════════════════════════════════════════

size_t send_hub_election(mesh_hub_election::Event event,
                         const uint8_t fingerprint[FINGERPRINT_SIZE]);

typedef void (*hub_election_handler_fn)(
    const uint8_t              sender_fp[FINGERPRINT_SIZE],
    mesh_hub_election::Event   event,
    const uint8_t              elected_fp[FINGERPRINT_SIZE]);

void set_hub_election_handler(hub_election_handler_fn fn);

} // namespace mesh_network

// ════════════════════════════════════════════════════════════════════════════
// CHIRP CHANNEL — Anonymous Community Witness Network
// ════════════════════════════════════════════════════════════════════════════
//
// Chirp Channel is an OPTIONAL anonymous mesh for community alerts.
// Key differences from Opera:
//   - Ephemeral identity (new each session, no tracking)
//   - Human-in-the-loop (no automated broadcasts)
//   - Soft witness alerts (calm, not alarming)
//   - No history retained (privacy-first)
//   - 3-hop max range (neighborhood, not city)
//
// Philosophy: "Safety in numbers, not surveillance"
// See spec/chirp_channel_v0.md for full specification.
// ════════════════════════════════════════════════════════════════════════════

namespace chirp_channel {

// Protocol constants.
// v0.2 (PROTOCOL_VERSION = 1) is the security-hardened wire format introduced
// alongside the audit findings in docs/audit/mesh_and_chirp_audit_v1.md.
// v0.1 frames (version byte = 0) are rejected by v0.2 receivers; the change
// is intentional and not backwards-compatible because v0.1 carried a
// decorative-only signature field that was never verified (audit C1).
static const uint8_t PROTOCOL_VERSION = 1;
static const uint8_t CHIRP_MAGIC = 0xC4;           // Message identifier
// CHIRP_CHANNEL: previously pinned to 6 to keep chirp off Opera's channel 1.
// With the shared single-radio reality (see mesh_channel_policy.h) chirp now
// rides the same radio as Opera; the broadcast peer is registered with
// channel = 0 so it follows the current radio channel. The constant is kept
// at its historical value purely as the host-build fallback for tests.
static const uint8_t CHIRP_CHANNEL = 0;            // 0 = follow current radio channel
static const size_t MAX_MESSAGE_LEN = 64;          // Max chirp message length (legacy field; v0.2 uses structured fields)
static const size_t MAX_RECENT_CHIRPS = 16;        // Stored chirps (priority heap by urgency)
// Deduplication: v0.3 switched to a 4 KB / 4-hash Bloom filter (audit C9
// closure). The historical fixed-array constant is retained as a hint to
// any external tool that snapshots header constants.
static const size_t MAX_NONCE_CACHE = 1024;        // Deprecated since v0.3 — Bloom filter used instead
static const size_t MAX_NEARBY_CACHE = 32;         // Nearby device cache
static const size_t SESSION_ID_SIZE = 8;           // Ephemeral session ID
static const size_t SESSION_PUBKEY_SIZE = 32;      // Ed25519 session pubkey carried in v0.2 frames
static const size_t EMOJI_DISPLAY_SIZE = 31;       // 5 emojis (up to 6 bytes each) + null (v0.2: lifted from 3)
static const size_t MAX_CONFIRMERS_PER_CHIRP = 8;  // Unique pubkey set tracked per pending chirp (v0.2 Sybil resistance)
static const uint32_t MIN_UNIX_TIME = 1700000000;  // Threshold below which time(nullptr) is treated as unsynced (v0.2)
static const uint8_t MAX_WITNESSES_PER_PUBKEY_PER_HOUR = 4;  // v0.2 per-pubkey receive-side rate limit (C14)
static const size_t MAX_PUBKEY_RATE_TRACK = 32;    // LRU of recent originating pubkeys for rate-limit tracking

// Timing (milliseconds)
static const uint32_t PRESENCE_INTERVAL_MS = 60000;    // Send presence every 60s
static const uint32_t CHIRP_TTL_MS = 300000;           // Messages valid for 5 min
static const uint32_t NEARBY_TIMEOUT_MS = 180000;      // Nearby stale after 3 min
static const uint32_t DEFAULT_DISPLAY_MS = 1800000;    // Display chirps for 30 min
static const uint32_t PRESENCE_REQUIRED_MS = 600000;   // 10 min presence before can send

// Escalating cooldowns (abuse prevention)
static const uint32_t COOLDOWN_TIER_1_MS = 300000;     // 5 min after 1st chirp
static const uint32_t COOLDOWN_TIER_2_MS = 900000;     // 15 min after 2nd chirp
static const uint32_t COOLDOWN_TIER_3_MS = 3600000;    // 1 hour after 3rd chirp
static const uint32_t COOLDOWN_TIER_4_MS = 14400000;   // 4 hours after 4th+ chirp
static const uint32_t COOLDOWN_RESET_MS = 86400000;    // Reset tiers after 24h no chirps

// Rate limits
static const uint8_t MAX_RELAYS_PER_MINUTE = 10;
static const uint8_t MAX_HOP_COUNT = 3;
static const uint8_t CONFIRMATIONS_REQUIRED = 2;       // Need 2 witnesses before relay
static const uint8_t CONFIRMATIONS_SAFETY = 1;         // Safety templates need only 1

// Night mode (restricted hours)
static const uint8_t NIGHT_START_HOUR = 22;            // 10 PM
static const uint8_t NIGHT_END_HOUR = 6;               // 6 AM

// Community mute threshold
static const uint8_t SUPPRESS_THRESHOLD_PERCENT = 50;  // >50% dismiss = suppress
static const uint32_t SUPPRESS_WINDOW_MS = 120000;     // Within 2 minutes

// ────────────────────────────────────────────────────────────────────────────
// ENUMS
// ────────────────────────────────────────────────────────────────────────────

// Chirp channel state
enum ChirpState : uint8_t {
  CHIRP_DISABLED = 0,      // Feature disabled (default)
  CHIRP_INITIALIZING,      // Generating session identity
  CHIRP_LISTENING,         // Receiving chirps, passive mode
  CHIRP_ACTIVE,            // Full participation
  CHIRP_MUTED,             // Temporarily ignoring chirps
  CHIRP_COOLDOWN           // Rate limited after sending
};

// Chirp message types
enum ChirpMsgType : uint8_t {
  CHIRP_MSG_PRESENCE = 0,  // Discovery beacon
  CHIRP_MSG_WITNESS,       // Soft alert (main message type)
  CHIRP_MSG_ACK,           // Acknowledgment (signed in v0.2)
  CHIRP_MSG_MUTE,          // Temporary opt-out broadcast
  CHIRP_MSG_SUPPRESS_VOTE, // v0.2: signed community suppress vote (C7)
  CHIRP_MSG_SELFTEST_OK    // v0.2: NFPA-72-style daily health beacon (trouble surface)
};

// Chirp categories (what's happening) — Emergency-focused
enum ChirpCategory : uint8_t {
  CHIRP_CAT_AUTHORITY = 0, // Law enforcement / government presence
  CHIRP_CAT_INFRA,         // Infrastructure failures
  CHIRP_CAT_EMERGENCY,     // Fire, medical, immediate danger
  CHIRP_CAT_WEATHER,       // Environmental threats
  CHIRP_CAT_MUTUAL_AID,    // Community helping community
  CHIRP_CAT_ALL_CLEAR      // De-escalation, situation resolved
};

// ════════════════════════════════════════════════════════════════════════════
// MESSAGE TEMPLATES — Emergency-focused, NOT surveillance
//
// Philosophy: "Witness authority, not neighbors"
// - NO "suspicious person" templates (racial profiling vector)
// - NO "unfamiliar vehicle" templates (harassment enabler)
// - NO descriptions of individuals EVER
// - ONLY emergencies, authority presence, infrastructure, mutual aid
// ════════════════════════════════════════════════════════════════════════════

enum ChirpTemplate : uint8_t {
  // Authority Presence (0x00-0x0F) — When power shows up
  TPL_AUTH_POLICE_ACTIVITY     = 0x00,  // "police activity in area"
  TPL_AUTH_HEAVY_RESPONSE      = 0x01,  // "heavy law enforcement response"
  TPL_AUTH_ROAD_BLOCKED_LE     = 0x02,  // "road blocked by law enforcement"
  TPL_AUTH_HELICOPTER          = 0x03,  // "helicopter circling area"
  // 0x04 (former TPL_AUTH_FEDERAL_PRESENCE) intentionally reserved — removed
  // in v0.2 per audit finding C17. If a deployment genuinely needs to signal
  // federal/agency presence, it must originate through the higher-trust
  // Beacon channel (spec/beacon_channel_v0.md) which requires two-pubkey
  // cryptographic co-signing. Do NOT reassign this template ID.

  // Infrastructure (0x10-0x1F) — Systems failing
  TPL_INFRA_POWER_OUT          = 0x10,  // "power outage"
  TPL_INFRA_WATER_ISSUE        = 0x11,  // "water service disruption"
  TPL_INFRA_GAS_SMELL          = 0x12,  // "gas smell - evacuate?"
  TPL_INFRA_INTERNET_DOWN      = 0x13,  // "internet outage in area"
  TPL_INFRA_ROAD_CLOSED        = 0x14,  // "road closed or blocked"

  // Emergency (0x20-0x2F) — Immediate danger
  TPL_EMERG_FIRE_VISIBLE       = 0x20,  // "fire or smoke visible"
  TPL_EMERG_MEDICAL_SCENE      = 0x21,  // "medical emergency scene"
  TPL_EMERG_MULTIPLE_AMBULANCE = 0x22,  // "multiple ambulances responding"
  TPL_EMERG_EVACUATION         = 0x23,  // "evacuation in progress"
  TPL_EMERG_SHELTER_IN_PLACE   = 0x24,  // "shelter in place advisory"

  // Weather (0x30-0x3F) — Environmental threats
  TPL_WX_SEVERE_WARNING        = 0x30,  // "severe weather warning"
  TPL_WX_TORNADO               = 0x31,  // "tornado warning"
  TPL_WX_FLOOD                 = 0x32,  // "flooding reported"
  TPL_WX_LIGHTNING_CLOSE       = 0x33,  // "dangerous lightning nearby"

  // Mutual Aid (0x40-0x4F) — Community support
  TPL_AID_WELFARE_CHECK        = 0x40,  // "neighbor may need help"
  TPL_AID_SUPPLIES_NEEDED      = 0x41,  // "supplies needed in area"
  TPL_AID_OFFERING_HELP        = 0x42,  // "offering assistance"

  // All Clear (0x80-0x8F) — De-escalation
  TPL_CLR_RESOLVED             = 0x80,  // "situation resolved"
  TPL_CLR_SAFE                 = 0x81,  // "area appears safe now"
  TPL_CLR_FALSE_ALARM          = 0x82,  // "false alarm"

  TPL_INVALID                  = 0xFF
};

// Detail slots — Minimal, factual, NO people descriptions
enum ChirpDetailSlot : uint8_t {
  DETAIL_NONE = 0,

  // Scale indicators (for AUTH_*)
  DETAIL_SCALE_FEW = 1,           // "few vehicles"
  DETAIL_SCALE_MANY = 2,          // "many vehicles"
  DETAIL_SCALE_MASSIVE = 3,       // "massive response"

  // Status indicators (for EMERG_*)
  DETAIL_STATUS_ONGOING = 10,     // "ongoing"
  DETAIL_STATUS_CONTAINED = 11,   // "contained"
  DETAIL_STATUS_SPREADING = 12,   // "spreading"

  // Direction (general)
  DETAIL_DIR_NORTH = 20,
  DETAIL_DIR_SOUTH = 21,
  DETAIL_DIR_EAST = 22,
  DETAIL_DIR_WEST = 23
};

// Chirp urgency (how important)
enum ChirpUrgency : uint8_t {
  CHIRP_URG_INFO = 0,      // FYI, no action needed (blue)
  CHIRP_URG_CAUTION,       // Heads up, be aware (yellow)
  CHIRP_URG_URGENT         // Important, pay attention (orange, NOT red)
};

// Acknowledgment types
enum ChirpAckType : uint8_t {
  CHIRP_ACK_SEEN = 0,      // Device received the chirp
  CHIRP_ACK_CONFIRMED,     // Human confirms they also see this
  CHIRP_ACK_RESOLVED       // Situation is resolved
};

// ────────────────────────────────────────────────────────────────────────────
// TYPES
// ────────────────────────────────────────────────────────────────────────────

// Ephemeral session identity (regenerated each enable/reboot)
struct ChirpSession {
  uint8_t session_id[SESSION_ID_SIZE];
  uint8_t session_pubkey[32];
  uint8_t session_privkey[32];
  char emoji_display[EMOJI_DISPLAY_SIZE];       // "🐦🌳⭐"
  uint32_t created_ms;
  bool valid;
};

// Chirp channel status
struct ChirpStatus {
  ChirpState state;
  char session_emoji[EMOJI_DISPLAY_SIZE];
  uint8_t nearby_count;                         // Anonymous count only
  uint8_t recent_chirp_count;
  uint32_t last_chirp_sent_ms;                  // 0 if never
  uint32_t cooldown_remaining_ms;
  bool relay_enabled;
  bool muted;
  uint32_t mute_remaining_ms;
};

// Nearby device (anonymous, just presence)
struct NearbyDevice {
  uint8_t session_id[SESSION_ID_SIZE];
  char emoji[EMOJI_DISPLAY_SIZE];
  uint32_t last_seen_ms;
  int8_t rssi;
  bool listening;                               // Is accepting chirps
};

// Received chirp — v0.2.
// The confirm/dismiss counters are now derived from local pubkey-keyed sets,
// never from wire values (audit C2, C5, C7).
struct ReceivedChirp {
  uint8_t sender_session[SESSION_ID_SIZE];
  uint8_t sender_pubkey[SESSION_PUBKEY_SIZE];   // v0.2: full pubkey for verification + identity-firewall checks
  char sender_emoji[EMOJI_DISPLAY_SIZE];
  ChirpTemplate template_id;
  ChirpDetailSlot detail;
  ChirpUrgency urgency;
  uint8_t hop_count;
  uint32_t received_ms;
  uint32_t timestamp;                           // Wall-clock send time (UNIX seconds, v0.2)
  uint8_t nonce[8];
  // Confirmation set: distinct session_pubkeys that have signed a CHIRP_ACK
  // with ack_type = CHIRP_ACK_CONFIRMED for this nonce. Bounded.
  uint8_t  confirmed_by[MAX_CONFIRMERS_PER_CHIRP][SESSION_PUBKEY_SIZE];
  uint8_t  confirm_count;                       // = number of valid entries in confirmed_by[]
  // Suppress vote set: distinct session_pubkeys that have signed a
  // CHIRP_MSG_SUPPRESS_VOTE for this nonce.
  uint8_t  suppressed_by[MAX_CONFIRMERS_PER_CHIRP][SESSION_PUBKEY_SIZE];
  uint8_t  suppress_count;
  bool     validated;                           // ≥ threshold confirmations (from pubkeys ≠ originator) received
  bool     suppressed;                          // Community voted to suppress
  bool     relayed;                             // Did we relay this
  bool     dismissed;                           // User dismissed locally
  bool     unverifiable_timestamp;              // Time was unsynced on receipt; display with warning badge
  // v0.3 (audit C4 closure): original signature is preserved so that when we
  // relay we can include the origin pubkey + origin signature in the
  // signed_origin envelope, and downstream receivers can verify the chain
  // end-to-end rather than trusting the relayer's attestation alone.
  uint8_t  origin_signature[64];
  bool     has_origin_signature;
};

// Outgoing chirp (for send queue)
struct OutgoingChirp {
  ChirpTemplate template_id;                    // What happened (structured)
  ChirpDetailSlot detail;                       // Optional detail
  ChirpUrgency urgency;
  uint8_t ttl_minutes;
};

// Cooldown tracking (escalating)
struct CooldownState {
  uint8_t chirps_sent_today;                    // Resets after 24h
  uint32_t last_chirp_ms;                       // For tier calculation
  uint32_t first_chirp_today_ms;                // For 24h reset
};

// ────────────────────────────────────────────────────────────────────────────
// MESSAGE STRUCTURES (Wire Format)
// ────────────────────────────────────────────────────────────────────────────

// Common header for all chirp messages
struct ChirpHeader {
  uint8_t magic;                                // CHIRP_MAGIC (0xC4)
  uint8_t version;                              // Protocol version
  uint8_t msg_type;                             // ChirpMsgType
  uint8_t session_id[SESSION_ID_SIZE];
  uint8_t hop_count;
  uint32_t timestamp;                           // Unix timestamp (seconds)
  uint8_t nonce[8];                             // Random for dedup
};

// Presence beacon payload
struct ChirpPresencePayload {
  char emoji[EMOJI_DISPLAY_SIZE];
  uint8_t listening;                            // bool
  uint8_t last_chirp_age_min;                   // 255 = never
};

// Witness payload (the main alert) — v0.2 wire format.
// Changes from v0.1:
//  - Removed `confirm_count` (audit C2): receivers track confirmations locally
//    via signed CHIRP_MSG_ACK + unique session_pubkey set, never trust wire-level
//    counts.
//  - Removed `category` (now derived from template_id high nibble).
//  - Renamed `msg_len` → `template_id` and added explicit `detail_slot`;
//    the legacy `message[]` reservation is removed.
//  - Added `session_pubkey` (32 B) so receivers can verify the signature
//    cryptographically (audit C1, C6).
//  - Added `signed_origin_pubkey` + `signed_origin_signature` for relays
//    so the original signer's attestation survives re-signing on relay
//    (audit C4). Zeroed on hop_count==0 frames.
//  - Top-level `signature` is now over a canonical string defined in
//    `chirp_channel.cpp::build_witness_canonical()` and must be verified
//    against `session_pubkey` (which itself must match the header
//    `session_id` via SHA-256("securacv:chirp:session:v0" || pubkey)[0:8]).
struct ChirpWitnessPayload {
  uint8_t template_id;                              // ChirpTemplate (v0.2)
  uint8_t detail_slot;                              // ChirpDetailSlot
  uint8_t urgency;                                  // ChirpUrgency
  uint8_t ttl_minutes;
  uint8_t reserved[4];                              // pad to 8 bytes; must be zero
  uint8_t session_pubkey[SESSION_PUBKEY_SIZE];      // 32 B Ed25519 pubkey of originator (top-level signer on relay)
  uint8_t signature[64];                            // Ed25519 over canonical (top-level: originator on hop 0, relayer on hop >0)
  uint8_t signed_origin_pubkey[SESSION_PUBKEY_SIZE]; // Original signer's pubkey (zero on hop 0)
  uint8_t signed_origin_signature[64];              // Original signer's signature (zero on hop 0)
};

// Acknowledgment payload — v0.2 wire format.
// Now carries the confirmer's session_pubkey + signature so receivers can
// verify the ACK and dedup by pubkey (audit C5). Unsigned ACKs are rejected.
struct ChirpAckPayload {
  uint8_t original_nonce[8];
  uint8_t ack_type;                                 // ChirpAckType
  uint8_t reserved[3];                              // pad; must be zero
  uint8_t confirmer_session_pubkey[SESSION_PUBKEY_SIZE];
  uint8_t signature[64];                            // Ed25519 over canonical
};

// Mute broadcast payload
struct ChirpMutePayload {
  uint8_t duration_minutes;                         // 15, 30, 60, or 120
  uint8_t reason;                                   // 0=busy, 1=sleeping, 2=away, 255=none
};

// Suppress vote payload — v0.2 (audit C7).
// Signed community vote to suppress a noise/spam chirp.
struct ChirpSuppressVotePayload {
  uint8_t original_nonce[8];                        // Chirp being suppressed
  uint8_t voter_session_pubkey[SESSION_PUBKEY_SIZE];
  uint8_t signature[64];                            // Ed25519 over canonical
};

// Self-test heartbeat payload — v0.2 (NFPA-72-style trouble surface).
// Daily-cadence proof-of-health from a Chirp-active device. Receivers
// track last_selftest_seen[session_pubkey] and surface Trouble if absent.
struct ChirpSelfTestPayload {
  uint32_t uptime_sec;
  uint16_t free_heap_kb;
  uint8_t  key_self_test_ok;                        // 1 if Ed25519 sign/verify round-trip OK
  uint8_t  reserved;
  uint8_t  session_pubkey[SESSION_PUBKEY_SIZE];
  uint8_t  signature[64];
};

// ────────────────────────────────────────────────────────────────────────────
// CALLBACKS
// ────────────────────────────────────────────────────────────────────────────

// Callback when chirp is received
typedef void (*ChirpReceivedCallback)(const ReceivedChirp* chirp);

// Callback when nearby count changes
typedef void (*NearbyChangedCallback)(uint8_t nearby_count);

// Callback when state changes
typedef void (*ChirpStateCallback)(ChirpState old_state, ChirpState new_state);

// ────────────────────────────────────────────────────────────────────────────
// FUNCTIONS
// ────────────────────────────────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────────────────
// Initialization
// ──────────────────────────────────────────────────────────────────────────

// Initialize chirp channel (call once at boot, does NOT enable)
bool init();

// Shutdown chirp channel
void deinit();

// Enable chirp channel (generates new session identity)
bool enable();

// Disable chirp channel (discards session identity)
void disable();

// Check if enabled
bool is_enabled();

// ──────────────────────────────────────────────────────────────────────────
// Main loop
// ──────────────────────────────────────────────────────────────────────────

// Call from main loop to process messages
void update();

// ──────────────────────────────────────────────────────────────────────────
// Status
// ──────────────────────────────────────────────────────────────────────────

// Get current chirp channel status
ChirpStatus get_status();

// Get state name as string
const char* state_name(ChirpState state);

// Get category name as string
const char* category_name(ChirpCategory category);

// Get urgency name as string
const char* urgency_name(ChirpUrgency urgency);

// Check if active and can receive chirps
bool is_active();

// Check if can send chirp (not in cooldown)
bool can_send_chirp();

// ──────────────────────────────────────────────────────────────────────────
// Sending chirps (HUMAN-IN-THE-LOOP)
// ──────────────────────────────────────────────────────────────────────────

// Send a chirp to the community using structured templates (NO FREE TEXT)
// IMPORTANT: This should only be called after human confirmation!
// Returns false if rate-limited, disabled, or presence requirement not met
bool send_chirp(ChirpTemplate template_id, ChirpUrgency urgency,
                ChirpDetailSlot detail = DETAIL_NONE, uint8_t ttl_minutes = 15);

// Send an all-clear (de-escalation)
bool send_all_clear(ChirpTemplate clear_type = TPL_CLR_RESOLVED);

// Check if presence requirement is met (10 min)
bool has_presence_requirement();

// Get current cooldown tier (1-4)
uint8_t get_cooldown_tier();

// Get cooldown remaining for current tier
uint32_t get_cooldown_remaining_ms();

// Get template display text (for UI)
const char* get_template_text(ChirpTemplate template_id);

// Get detail display text (for UI)
const char* get_detail_text(ChirpDetailSlot detail);

// Validate template ID
bool is_valid_template(ChirpTemplate template_id);

// Check if currently in night mode (restricted)
bool is_night_mode();

// ──────────────────────────────────────────────────────────────────────────
// Receiving chirps
// ──────────────────────────────────────────────────────────────────────────

// Get recent chirps (returns count)
const ReceivedChirp* get_recent_chirps(size_t* count);

// Get pending chirps (unvalidated, awaiting confirmation)
const ReceivedChirp* get_pending_chirps(size_t* count);

// Confirm a chirp ("I see this too") - adds witness count
// If enough confirmations, chirp becomes validated and relays
bool confirm_chirp(const uint8_t* nonce);

// Dismiss a chirp from display (contributes to suppress voting)
bool dismiss_chirp(const uint8_t* nonce);

// Clear all recent chirps
void clear_chirps();

// Get validation status text
const char* get_validation_status(const ReceivedChirp* chirp);

// ──────────────────────────────────────────────────────────────────────────
// Nearby devices (anonymous)
// ──────────────────────────────────────────────────────────────────────────

// Get count of nearby chirp-enabled devices
uint8_t get_nearby_count();

// Get nearby devices (anonymous info only)
const NearbyDevice* get_nearby_devices(size_t* count);

// ──────────────────────────────────────────────────────────────────────────
// Mute control
// ──────────────────────────────────────────────────────────────────────────

// Mute chirps for duration (15, 30, 60, or 120 minutes)
bool mute(uint8_t duration_minutes);

// Unmute chirps
void unmute();

// Check if muted
bool is_muted();

// ──────────────────────────────────────────────────────────────────────────
// Settings
// ──────────────────────────────────────────────────────────────────────────

// Enable/disable relaying other chirps
void set_relay_enabled(bool enabled);
bool is_relay_enabled();

// Set minimum urgency to display (filters lower urgency)
void set_urgency_filter(ChirpUrgency min_urgency);
ChirpUrgency get_urgency_filter();

// ──────────────────────────────────────────────────────────────────────────
// Callbacks
// ──────────────────────────────────────────────────────────────────────────

// Set callback for received chirps
void set_chirp_callback(ChirpReceivedCallback callback);

// Set callback for nearby count changes
void set_nearby_callback(NearbyChangedCallback callback);

// Set callback for state changes
void set_state_callback(ChirpStateCallback callback);

// ──────────────────────────────────────────────────────────────────────────
// Session info (for display)
// ──────────────────────────────────────────────────────────────────────────

// Get current session emoji (e.g., "🐦🌳⭐")
const char* get_session_emoji();

// Get session ID (for debugging only)
const uint8_t* get_session_id();

// Dispatch an ESP-NOW frame received by the mesh callback into chirp_channel.
// `rssi_dbm` is the raw RSSI value from `esp_now_recv_info_t::rx_ctrl->rssi`
// (typical range: -30 for very close, -95 near noise floor).
void dispatch_espnow_message(const uint8_t* mac, const uint8_t* data,
                             int len, int8_t rssi_dbm);

} // namespace chirp_channel

#endif // SECURACV_MESH_NETWORK_H
