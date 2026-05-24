/*
 * SecuraCV Canary — Mesh Network Implementation
 * Version 0.1.0
 *
 * Implements the Opera Protocol for secure peer-to-peer mesh networking.
 * Uses ESP-NOW for primary transport with Ed25519 authentication.
 */

#include "build_config.h"

#if FEATURE_MESH_NETWORK

#include "mesh_network.h"
#include "mesh_channel_policy.h"
#include "airtime_governor.h"
#include "log_level.h"
#include "health_log.h"
// v0.5: cross-namespace call into beacon_channel::on_peer_tampered from
// handle_tamper_alert. The header is FEATURE_BEACON_CHANNEL-gated
// internally so it's a no-op when the feature is off.
#include "beacon_channel.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <esp_flash_encrypt.h>
#include <Crypto.h>
#include <Ed25519.h>
#include <Curve25519.h>
#include <mbedtls/sha256.h>
#include <mbedtls/hkdf.h>
#include <ChaChaPoly.h>

namespace mesh_network {

// ════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ════════════════════════════════════════════════════════════════════════════
// DOMAIN SEPARATION STRINGS
// ════════════════════════════════════════════════════════════════════════════

static const char* DOMAIN_FLOCK_ID = "securacv:opera:id:v0";
static const char* DOMAIN_AUTH = "securacv:mesh:auth:v0";
static const char* DOMAIN_SESSION = "securacv:mesh:session:v0";
static const char* DOMAIN_MESSAGE = "securacv:mesh:message:v0";
static const char* DOMAIN_PAIR_CONFIRM = "securacv:pair:confirm:v0";

// ════════════════════════════════════════════════════════════════════════════
// NVS KEYS
// ════════════════════════════════════════════════════════════════════════════

static const char* NVS_NS = "mesh";
static const char* NVS_ENABLED = "enabled";
static const char* NVS_FLOCK_ID = "opera_id";
static const char* NVS_FLOCK_SECRET = "opera_sec";
static const char* NVS_FLOCK_NAME = "opera_name";
static const char* NVS_PEER_COUNT = "peer_cnt";
static const char* NVS_PEER_PREFIX = "peer_";   // peer_0, peer_1, etc.

// ════════════════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════════════════

static bool g_initialized = false;
static Preferences g_prefs;

// Device identity (references to main firmware keys)
static const uint8_t* g_device_privkey = nullptr;
static const uint8_t* g_device_pubkey = nullptr;
static uint8_t g_device_fingerprint[FINGERPRINT_SIZE];
static char g_device_name[MAX_PEER_NAME_LEN + 1];

// Opera state
static OperaConfig g_opera_config;
static OperaPeer g_peers[MAX_OPERA_SIZE];
static uint8_t g_peer_count = 0;

// BLE-Scout BEACON_EVENT handler (canary-wap parity for PIO
// mesh_session::set_beacon_event_handler). nullptr means inbound
// events get decoded then dropped silently — the integration
// layer wires this in csi_integration.cpp.
static beacon_event_handler_fn g_beacon_event_handler = nullptr;
static channel_lock_handler_fn g_channel_lock_handler = nullptr;

// Mesh state
static MeshState g_mesh_state = MESH_DISABLED;
static bool g_espnow_initialized = false;

// Statistics
static uint32_t g_messages_sent = 0;
static uint32_t g_messages_received = 0;
static uint32_t g_message_errors = 0;
static uint32_t g_alerts_sent = 0;
static uint32_t g_alerts_received = 0;
static uint32_t g_auth_failures = 0;
static uint32_t g_start_time_ms = 0;

// Timing
static uint32_t g_last_heartbeat_ms = 0;
static uint32_t g_last_peer_check_ms = 0;

// v0.3 (audit O3 closure): in-flight opera_secret rekey state.
//
// A single rekey is allowed at a time; concurrent calls are rejected.
// `pending_secret` and `pending_opera_id` are committed to NVS only after
// every surviving member has ACKed or the timeout expires (whichever first).
//
// Threading invariant (gemini P1 follow-up): every mutator of `g_rekey`
// lives in the main loop task:
//   - `remove_peer()` is invoked from the REST handler thread, but the
//     existing wifi_provision serializer + Bearer-gate trampoline ensure
//     it's called on the main task. (See mesh_network.cpp's other
//     loop-driven mutators: handle_received_message, update().)
//   - `maybe_finalize_rekey()` is called from `update()` (loop task).
//   - The case MSG_OPERA_REKEY_ACK branch in handle_received_message
//     also runs on the loop task, because ESP-NOW frames are queued via
//     `g_rx_pending` and dispatched only inside `update()`.
//
// No cross-task access to `pending_acks` therefore exists. If a future
// change introduces an ISR or non-loop-task mutator, switch to
// `__atomic_fetch_and(&pending_acks, ~bit, __ATOMIC_RELAXED)` for the
// clearing operation and `__atomic_load_n(&pending_acks, ...)` for the
// commit-decision read in `maybe_finalize_rekey()`.
static const uint32_t REKEY_TIMEOUT_MS = 60000;
struct RekeyState {
  bool        active;
  uint32_t    rekey_id;
  uint32_t    started_ms;
  uint8_t     pending_secret[OPERA_SECRET_SIZE];
  uint8_t     pending_opera_id[OPERA_ID_SIZE];
  // Bit-vector of peer indices that have ACKed (mirrors g_peers indices at the
  // time rekey was initiated). MAX_OPERA_SIZE is 16 so a uint16_t suffices.
  uint16_t    pending_acks;   // bit i set if peer i hasn't ACKed yet
};
static RekeyState g_rekey = {};

// Pairing
static PairingSession g_pairing;

// Alert history
static MeshAlert g_alert_history[MAX_ALERT_HISTORY];
static size_t g_alert_count = 0;
static size_t g_alert_head = 0;

// Callbacks
static AlertCallback g_alert_callback = nullptr;
static PeerStateCallback g_peer_state_callback = nullptr;
static PairingCallback g_pairing_callback = nullptr;

// Receive buffer
static uint8_t g_rx_buffer[MAX_MESSAGE_SIZE];
static size_t g_rx_len = 0;
static uint8_t g_rx_mac[6];
static int8_t g_rx_rssi = 0;
static volatile bool g_rx_pending = false;

// ════════════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════════════════

static void compute_fingerprint(const uint8_t* pubkey, uint8_t* fp_out);
static void compute_opera_id(const uint8_t* secret, uint8_t* id_out);
static bool derive_session_key(const uint8_t* local_priv, const uint8_t* peer_pub, uint8_t* key_out);
static bool encrypt_message(const uint8_t* key, const uint8_t* plaintext, size_t len,
                           uint8_t* ciphertext, uint8_t* nonce_out, uint8_t* tag_out);
static bool decrypt_message(const uint8_t* key, const uint8_t* ciphertext, size_t len,
                           const uint8_t* nonce, const uint8_t* tag, uint8_t* plaintext);
static void maybe_finalize_rekey();
static bool sign_message(const uint8_t* privkey, const uint8_t* data, size_t len, uint8_t* sig_out);
static bool verify_signature(const uint8_t* pubkey, const uint8_t* data, size_t len, const uint8_t* sig);
static void update_peer_state(OperaPeer* peer, PeerState new_state);
static OperaPeer* find_peer_by_mac(const uint8_t* mac);
static OperaPeer* find_peer_by_fingerprint(const uint8_t* fp);
static bool add_peer(const uint8_t* pubkey, const uint8_t* mac, const char* name);
static bool send_raw_message(const uint8_t* mac, const uint8_t* data, size_t len);
static bool send_to_peer(OperaPeer* peer, MessageType type, const uint8_t* payload, size_t len);
static bool broadcast_message(MessageType type, const uint8_t* payload, size_t len);
static void handle_received_message(const uint8_t* mac, const uint8_t* data, size_t len);
static void handle_heartbeat(OperaPeer* peer, const uint8_t* payload);
static void handle_auth_challenge(const uint8_t* mac, const uint8_t* payload);
static void handle_auth_response(OperaPeer* peer, const uint8_t* payload);
static void handle_tamper_alert(OperaPeer* peer, const uint8_t* payload);
static void handle_power_alert(OperaPeer* peer, const uint8_t* payload);
static void handle_offline_imminent(OperaPeer* peer, const uint8_t* payload);
static void handle_pair_discover(const uint8_t* mac, const uint8_t* payload);
static void handle_pair_offer(const uint8_t* mac, const uint8_t* payload);
static void handle_pair_accept(const uint8_t* mac, const uint8_t* payload);
static void handle_pair_confirm(const uint8_t* mac, const uint8_t* payload);
static void handle_pair_complete(const uint8_t* mac, const uint8_t* payload);
static bool persist_opera_config();
static bool load_opera_config();
static bool persist_peers();
static bool load_peers();
static void store_alert(const MeshAlert* alert);

// ════════════════════════════════════════════════════════════════════════════
// ESP-NOW CALLBACKS
// ════════════════════════════════════════════════════════════════════════════

static void espnow_send_cb(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  (void)info;  // Unused - MAC available in info->peer_addr if needed
  if (status != ESP_NOW_SEND_SUCCESS) {
    g_message_errors++;
  }
}

static void espnow_recv_cb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len <= 0 || len > MAX_MESSAGE_SIZE || g_rx_pending) {
    return;
  }
  memcpy(g_rx_mac, info->src_addr, 6);
  memcpy(g_rx_buffer, data, len);
  g_rx_len = len;
  g_rx_rssi = info->rx_ctrl->rssi;  // Store actual RSSI from ESP-NOW
  g_rx_pending = true;
}

// ════════════════════════════════════════════════════════════════════════════
// CRYPTO HELPERS
// ════════════════════════════════════════════════════════════════════════════

static void sha256_domain(const char* domain, const uint8_t* data, size_t len, uint8_t* out) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const uint8_t*)domain, strlen(domain));
  mbedtls_sha256_update(&ctx, data, len);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

static void compute_fingerprint(const uint8_t* pubkey, uint8_t* fp_out) {
  uint8_t hash[32];
  sha256_domain("securacv:pubkey:fingerprint", pubkey, PUBKEY_SIZE, hash);
  memcpy(fp_out, hash, FINGERPRINT_SIZE);
}

static void compute_opera_id(const uint8_t* secret, uint8_t* id_out) {
  uint8_t hash[32];
  sha256_domain(DOMAIN_FLOCK_ID, secret, OPERA_SECRET_SIZE, hash);
  memcpy(id_out, hash, OPERA_ID_SIZE);
}

static bool derive_session_key(const uint8_t* local_priv, const uint8_t* peer_pub, uint8_t* key_out) {
  // Perform X25519 ECDH key exchange
  // Note: Ed25519 keys must be converted to Curve25519 for X25519
  // The Crypto library's Curve25519 does this conversion internally

  uint8_t shared_secret[32];

  // Perform X25519 scalar multiplication: shared = local_priv * peer_pub
  // Curve25519::eval() computes the Diffie-Hellman shared secret
  if (!Curve25519::eval(shared_secret, local_priv, peer_pub)) {
    memset(shared_secret, 0, sizeof(shared_secret));
    return false;
  }

  // Derive session key using HKDF-SHA256 for proper key derivation
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int ret = mbedtls_hkdf(md,
                         nullptr, 0,  // No salt
                         shared_secret, sizeof(shared_secret),
                         (const uint8_t*)DOMAIN_SESSION, strlen(DOMAIN_SESSION),
                         key_out, SESSION_KEY_SIZE);

  // Clear sensitive data
  memset(shared_secret, 0, sizeof(shared_secret));

  return ret == 0;
}

static bool encrypt_message(const uint8_t* key, const uint8_t* plaintext, size_t len,
                           uint8_t* ciphertext, uint8_t* nonce_out, uint8_t* tag_out) {
  // Generate random nonce
  esp_fill_random(nonce_out, NONCE_SIZE);

  ChaChaPoly chachapoly;
  if (!chachapoly.setKey(key, 32)) {
    chachapoly.clear();
    return false;
  }
  chachapoly.setIV(nonce_out, NONCE_SIZE);
  chachapoly.encrypt(ciphertext, plaintext, len);
  chachapoly.computeTag(tag_out, 16);
  chachapoly.clear();

  return true;
}

static bool decrypt_message(const uint8_t* key, const uint8_t* ciphertext, size_t len,
                           const uint8_t* nonce, const uint8_t* tag, uint8_t* plaintext) {
  ChaChaPoly chachapoly;
  if (!chachapoly.setKey(key, 32)) {
    chachapoly.clear();
    return false;
  }
  chachapoly.setIV(nonce, NONCE_SIZE);
  chachapoly.decrypt(plaintext, ciphertext, len);

  // Verify authentication tag
  bool valid = chachapoly.checkTag(tag, 16);
  chachapoly.clear();

  return valid;
}

static bool sign_message(const uint8_t* privkey, const uint8_t* data, size_t len, uint8_t* sig_out) {
  // Use domain-separated signing
  uint8_t hash[32];
  sha256_domain(DOMAIN_MESSAGE, data, len, hash);
  Ed25519::sign(sig_out, privkey, g_device_pubkey, hash, 32);
  return true;
}

static bool verify_signature(const uint8_t* pubkey, const uint8_t* data, size_t len, const uint8_t* sig) {
  uint8_t hash[32];
  sha256_domain(DOMAIN_MESSAGE, data, len, hash);
  return Ed25519::verify(sig, pubkey, hash, 32);
}

// ════════════════════════════════════════════════════════════════════════════
// PEER MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

static void update_peer_state(OperaPeer* peer, PeerState new_state) {
  if (peer->state == new_state) return;

  PeerState old_state = peer->state;
  peer->state = new_state;

  if (g_peer_state_callback) {
    g_peer_state_callback(peer, old_state, new_state);
  }
}

static OperaPeer* find_peer_by_mac(const uint8_t* mac) {
  for (uint8_t i = 0; i < g_peer_count; i++) {
    if (memcmp(g_peers[i].mac_addr, mac, 6) == 0) {
      return &g_peers[i];
    }
  }
  return nullptr;
}

static OperaPeer* find_peer_by_fingerprint(const uint8_t* fp) {
  for (uint8_t i = 0; i < g_peer_count; i++) {
    if (memcmp(g_peers[i].fingerprint, fp, FINGERPRINT_SIZE) == 0) {
      return &g_peers[i];
    }
  }
  return nullptr;
}

static bool add_peer(const uint8_t* pubkey, const uint8_t* mac, const char* name) {
  if (g_peer_count >= MAX_OPERA_SIZE) {
    return false;
  }

  OperaPeer* peer = &g_peers[g_peer_count];
  memset(peer, 0, sizeof(OperaPeer));

  memcpy(peer->pubkey, pubkey, PUBKEY_SIZE);
  compute_fingerprint(pubkey, peer->fingerprint);
  memcpy(peer->mac_addr, mac, 6);
  strncpy(peer->name, name, MAX_PEER_NAME_LEN);
  peer->name[MAX_PEER_NAME_LEN] = '\0';
  peer->state = PEER_UNKNOWN;
  peer->msg_counter_tx = 0;
  peer->msg_counter_rx = 0;
  peer->last_seen_ms = 0;
  peer->session_established = false;

  // Register with ESP-NOW
  esp_now_peer_info_t peer_info = {};
  memcpy(peer_info.peer_addr, mac, 6);
  peer_info.channel = ESPNOW_CHANNEL;
  peer_info.encrypt = false;  // We use our own encryption

  if (esp_now_add_peer(&peer_info) != ESP_OK) {
    // Peer might already exist, try to update
    esp_now_del_peer(mac);
    if (esp_now_add_peer(&peer_info) != ESP_OK) {
      return false;
    }
  }

  g_peer_count++;
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// MESSAGE SENDING
// ════════════════════════════════════════════════════════════════════════════

static bool send_raw_message(const uint8_t* mac, const uint8_t* data, size_t len) {
  if (!g_espnow_initialized || len > MAX_MESSAGE_SIZE) {
    return false;
  }

  esp_err_t result = esp_now_send(mac, data, len);
  if (result == ESP_OK) {
    g_messages_sent++;
    return true;
  }
  g_message_errors++;
  return false;
}

static bool send_to_peer(OperaPeer* peer, MessageType type, const uint8_t* payload, size_t payload_len) {
  if (!peer || !g_opera_config.configured) {
    return false;
  }

  // Build message
  uint8_t msg[MAX_MESSAGE_SIZE];
  size_t offset = 0;

  // Header
  msg[offset++] = PROTOCOL_VERSION;
  msg[offset++] = (uint8_t)type;
  memcpy(msg + offset, g_opera_config.opera_id, OPERA_ID_SIZE);
  offset += OPERA_ID_SIZE;
  memcpy(msg + offset, g_device_fingerprint, FINGERPRINT_SIZE);
  offset += FINGERPRINT_SIZE;

  // Counter (8 bytes, little-endian)
  uint64_t counter = peer->msg_counter_tx++;
  for (int i = 0; i < 8; i++) {
    msg[offset++] = (counter >> (i * 8)) & 0xFF;
  }

  // Timestamp (4 bytes)
  uint32_t timestamp = millis() / 1000;
  memcpy(msg + offset, &timestamp, 4);
  offset += 4;

  // Payload
  if (payload && payload_len > 0) {
    if (offset + payload_len > MAX_MESSAGE_SIZE - SIGNATURE_SIZE) {
      return false;
    }
    memcpy(msg + offset, payload, payload_len);
    offset += payload_len;
  }

  // Sign the message (excluding signature space)
  uint8_t signature[SIGNATURE_SIZE];
  sign_message(g_device_privkey, msg, offset, signature);
  memcpy(msg + offset, signature, SIGNATURE_SIZE);
  offset += SIGNATURE_SIZE;

  peer->last_tx_ms = millis();
  return send_raw_message(peer->mac_addr, msg, offset);
}

static bool broadcast_message(MessageType type, const uint8_t* payload, size_t payload_len) {
  bool any_sent = false;
  for (uint8_t i = 0; i < g_peer_count; i++) {
    if (g_peers[i].state >= PEER_CONNECTED) {
      if (send_to_peer(&g_peers[i], type, payload, payload_len)) {
        any_sent = true;
      }
    }
  }
  return any_sent;
}

// ════════════════════════════════════════════════════════════════════════════
// MESSAGE HANDLING
// ════════════════════════════════════════════════════════════════════════════

static void handle_received_message(const uint8_t* mac, const uint8_t* data, size_t len) {
  // Minimum message size: header (2+16+8+8+4) + signature (64) = 102
  if (len < 102) {
    return;
  }

  size_t offset = 0;

  // Parse header
  uint8_t version = data[offset++];
  if (version != PROTOCOL_VERSION) {
    return;  // Incompatible version
  }

  MessageType msg_type = (MessageType)data[offset++];

  const uint8_t* opera_id = data + offset;
  offset += OPERA_ID_SIZE;

  const uint8_t* sender_fp = data + offset;
  offset += FINGERPRINT_SIZE;

  uint64_t counter = 0;
  for (int i = 0; i < 8; i++) {
    counter |= ((uint64_t)data[offset++]) << (i * 8);
  }

  uint32_t timestamp;
  memcpy(&timestamp, data + offset, 4);
  offset += 4;

  const uint8_t* payload = data + offset;
  size_t payload_len = len - offset - SIGNATURE_SIZE;
  const uint8_t* signature = data + len - SIGNATURE_SIZE;

  // Pairing messages don't require opera membership
  if (msg_type >= MSG_PAIR_DISCOVER && msg_type <= MSG_PAIR_COMPLETE) {
    switch (msg_type) {
      case MSG_PAIR_DISCOVER:
        handle_pair_discover(mac, payload);
        break;
      case MSG_PAIR_OFFER:
        handle_pair_offer(mac, payload);
        break;
      case MSG_PAIR_ACCEPT:
        handle_pair_accept(mac, payload);
        break;
      case MSG_PAIR_CONFIRM:
        handle_pair_confirm(mac, payload);
        break;
      case MSG_PAIR_COMPLETE:
        handle_pair_complete(mac, payload);
        break;
      default:
        break;
    }
    return;
  }

  // Verify opera membership for non-pairing messages
  if (!g_opera_config.configured) {
    return;  // No opera, ignore regular messages
  }

  if (memcmp(opera_id, g_opera_config.opera_id, OPERA_ID_SIZE) != 0) {
    return;  // Different opera, ignore (prevents neighbor interference)
  }

  // Find peer by fingerprint
  OperaPeer* peer = find_peer_by_fingerprint(sender_fp);
  if (!peer) {
    // Unknown peer claiming to be in our opera - security violation
    g_auth_failures++;
    return;
  }

  // Update MAC address if changed (device might have reconnected)
  if (memcmp(peer->mac_addr, mac, 6) != 0) {
    memcpy(peer->mac_addr, mac, 6);

    // Re-register with ESP-NOW
    esp_now_del_peer(peer->mac_addr);
    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, mac, 6);
    peer_info.channel = ESPNOW_CHANNEL;
    peer_info.encrypt = false;
    esp_now_add_peer(&peer_info);
  }

  // Verify signature
  if (!verify_signature(peer->pubkey, data, len - SIGNATURE_SIZE, signature)) {
    g_auth_failures++;
    return;
  }

  // Check for replay (counter must be greater than last seen).
  // v0.2 (audit O1): the per-peer monotonic counter is the authoritative
  // freshness mechanism. The wall-clock TTL check that previously lived
  // here was based on `millis()/1000` (uptime, not wall clock) and produced
  // asymmetric verdicts between peers with different uptimes. It has been
  // removed. The `timestamp` field is preserved on the wire for diagnostic
  // and forensic purposes only and is not security-bearing.
  if (counter <= peer->msg_counter_rx && peer->msg_counter_rx > 0) {
    return;  // Replay attack
  }
  peer->msg_counter_rx = counter;
  (void)timestamp;  // intentionally unused as of v0.2 (audit O1)

  // Update peer state
  peer->last_seen_ms = millis();
  peer->rssi = g_rx_rssi;  // Actual RSSI from ESP-NOW callback

  if (peer->state == PEER_STALE || peer->state == PEER_OFFLINE || peer->state == PEER_UNKNOWN) {
    update_peer_state(peer, PEER_CONNECTED);
  }

  g_messages_received++;

  // Handle by message type
  switch (msg_type) {
    case MSG_HEARTBEAT:
      handle_heartbeat(peer, payload);
      break;
    case MSG_AUTH_CHALLENGE:
      handle_auth_challenge(mac, payload);
      break;
    case MSG_AUTH_RESPONSE:
      handle_auth_response(peer, payload);
      break;
    case MSG_TAMPER_ALERT:
      handle_tamper_alert(peer, payload);
      break;
    case MSG_POWER_ALERT:
      handle_power_alert(peer, payload);
      break;
    case MSG_OFFLINE_IMMINENT:
      handle_offline_imminent(peer, payload);
      break;
    case MSG_OPERA_REKEY:
      // Decrypt the new secret with our existing session key, ACK BEFORE
      // switching opera_id, then install the new secret. See
      // spec/canary_mesh_network_v0.md §5.6 + codex P1 follow-up.
      //
      // Ordering matters: the initiator still accepts only the OLD
      // opera_id until maybe_finalize_rekey() commits, so the ACK must
      // be emitted under the OLD opera_id (i.e. before we mutate
      // g_opera_config.opera_id). If we switched first, every ACK would
      // be dropped at the initiator's `opera_id` membership check, the
      // all-ACKed fast path would be unreachable, and the transaction
      // would always hit the 60 s timeout — unnecessarily staling
      // healthy peers.
      {
        if (!peer->session_established) break;
        const OperaRekeyPayload* rk = (const OperaRekeyPayload*)payload;
        uint8_t new_secret[OPERA_SECRET_SIZE];
        if (!decrypt_message(peer->session_key, rk->encrypted_secret,
                             OPERA_SECRET_SIZE, rk->nonce, rk->tag, new_secret)) {
          health_log(SCV_LOG_WARNING, SCV_CAT_CRYPTO,
                     "opera: rekey decrypt failed (dropped)");
          break;
        }

        // ── 1. ACK FIRST under the still-current opera_id + session key ──
        OperaRekeyAckPayload ack;
        ack.rekey_id = rk->rekey_id;
        send_to_peer(peer, MSG_OPERA_REKEY_ACK, (uint8_t*)&ack, sizeof(ack));

        // ── 2. Now install the new secret and derive new opera_id ──
        memcpy(g_opera_config.opera_secret, new_secret, OPERA_SECRET_SIZE);
        compute_opera_id(g_opera_config.opera_secret, g_opera_config.opera_id);
        persist_opera_config();  // FE-gated; logs alert if refused

        // ── 3. Invalidate the session so both sides re-auth ──
        peer->session_established = false;
        memset(peer->session_key, 0, SESSION_KEY_SIZE);
        peer->msg_counter_tx = 0;
        peer->msg_counter_rx = 0;
        health_log(SCV_LOG_INFO, SCV_CAT_CRYPTO,
                   "opera: rekey applied (ACK sent under old opera_id); awaiting re-auth");
      }
      break;
    case MSG_OPERA_REKEY_ACK:
      // We're the initiator; record this peer's ACK.
      {
        const OperaRekeyAckPayload* ack = (const OperaRekeyAckPayload*)payload;
        if (!g_rekey.active) break;
        if (ack->rekey_id != g_rekey.rekey_id) break;
        // Find peer index in g_peers (peer pointer arithmetic).
        size_t idx = (size_t)(peer - g_peers);
        if (idx >= MAX_OPERA_SIZE) break;
        g_rekey.pending_acks &= ~(uint16_t)(1u << idx);
        health_log(SCV_LOG_INFO, SCV_CAT_CRYPTO,
                   "opera: rekey ACK received from peer");
      }
      break;
    case MSG_BEACON_EVENT:
      // canary-wap parity for PIO mesh_session::on_opera_frame's
      // BEACON_EVENT branch (PR 5c-4). Sig/opera_id/counter checks
      // already happened above (handle_received_message validates the
      // signed envelope before dispatching by msg_type). All we do
      // here is decode the 25-byte payload and forward to the
      // integration layer via the installed handler.
      if (g_beacon_event_handler != nullptr) {
        mesh_beacon::BeaconState state;
        char label[mesh_beacon::MAX_LABEL_BYTES + 1];
        if (mesh_beacon::decode(payload, payload_len,
                                &state, label, sizeof(label))) {
          g_beacon_event_handler(peer->fingerprint, state, label);
        }
      }
      break;
    case MSG_CHANNEL_LOCK:
      if (g_channel_lock_handler != nullptr) {
        uint8_t                  channel;
        mesh_channel_hop::Reason reason;
        if (mesh_channel_hop::decode(payload, payload_len,
                                     &channel, &reason)) {
          g_channel_lock_handler(peer->fingerprint, channel, reason);
        }
      }
      break;
    default:
      break;
  }
}

static void handle_heartbeat(OperaPeer* peer, const uint8_t* payload) {
  if (!peer) return;

  const HeartbeatPayload* hb = (const HeartbeatPayload*)payload;

  // Peer is alive
  if (peer->state != PEER_CONNECTED && peer->state != PEER_ALERT) {
    update_peer_state(peer, PEER_CONNECTED);
  }
}

static void handle_auth_challenge(const uint8_t* mac, const uint8_t* payload) {
  // Someone is trying to authenticate with us
  const AuthChallengePayload* challenge = (const AuthChallengePayload*)payload;

  // Verify they're in our opera
  OperaPeer* peer = nullptr;
  for (uint8_t i = 0; i < g_peer_count; i++) {
    if (memcmp(g_peers[i].pubkey, challenge->pubkey, PUBKEY_SIZE) == 0) {
      peer = &g_peers[i];
      break;
    }
  }

  if (!peer) {
    g_auth_failures++;
    return;  // Unknown peer
  }

  // Build response
  AuthResponsePayload response;

  // Sign the challenge nonce with domain separation
  uint8_t sign_input[AUTH_CHALLENGE_SIZE + OPERA_ID_SIZE];
  memcpy(sign_input, challenge->nonce, AUTH_CHALLENGE_SIZE);
  memcpy(sign_input + AUTH_CHALLENGE_SIZE, g_opera_config.opera_id, OPERA_ID_SIZE);

  uint8_t hash[32];
  sha256_domain(DOMAIN_AUTH, sign_input, sizeof(sign_input), hash);
  Ed25519::sign(response.challenge_sig, g_device_privkey, g_device_pubkey, hash, 32);

  memcpy(response.pubkey, g_device_pubkey, PUBKEY_SIZE);

  // Sign opera_id as proof of membership
  sha256_domain(DOMAIN_AUTH, g_opera_config.opera_id, OPERA_ID_SIZE, hash);
  Ed25519::sign(response.opera_proof, g_device_privkey, g_device_pubkey, hash, 32);

  // Derive session key
  derive_session_key(g_device_privkey, peer->pubkey, peer->session_key);
  peer->session_established = true;

  update_peer_state(peer, PEER_AUTHENTICATING);
  send_to_peer(peer, MSG_AUTH_RESPONSE, (uint8_t*)&response, sizeof(response));
}

static void handle_auth_response(OperaPeer* peer, const uint8_t* payload) {
  if (!peer) return;

  const AuthResponsePayload* response = (const AuthResponsePayload*)payload;

  // Verify the pubkey matches
  if (memcmp(response->pubkey, peer->pubkey, PUBKEY_SIZE) != 0) {
    g_auth_failures++;
    return;
  }

  // Verify opera proof
  uint8_t hash[32];
  sha256_domain(DOMAIN_AUTH, g_opera_config.opera_id, OPERA_ID_SIZE, hash);
  if (!Ed25519::verify(response->opera_proof, peer->pubkey, hash, 32)) {
    g_auth_failures++;
    return;
  }

  // Derive session key
  derive_session_key(g_device_privkey, peer->pubkey, peer->session_key);
  peer->session_established = true;

  update_peer_state(peer, PEER_CONNECTED);

  // Send auth complete
  send_to_peer(peer, MSG_AUTH_COMPLETE, nullptr, 0);
}

static void handle_tamper_alert(OperaPeer* peer, const uint8_t* payload) {
  if (!peer) return;

  const TamperAlertPayload* alert = (const TamperAlertPayload*)payload;

  update_peer_state(peer, PEER_ALERT);
  peer->alerts_received++;
  g_alerts_received++;

  // Store alert
  MeshAlert mesh_alert;
  mesh_alert.timestamp_ms = millis();
  mesh_alert.type = (AlertType)alert->alert_type;
  mesh_alert.severity = (LogLevel)alert->severity;
  memcpy(mesh_alert.sender_fp, peer->fingerprint, FINGERPRINT_SIZE);
  strncpy(mesh_alert.sender_name, peer->name, MAX_PEER_NAME_LEN);
  mesh_alert.witness_seq = alert->witness_seq;
  strncpy(mesh_alert.detail, alert->detail, sizeof(mesh_alert.detail) - 1);
  mesh_alert.detail[sizeof(mesh_alert.detail) - 1] = '\0';

  store_alert(&mesh_alert);

  if (g_alert_callback) {
    g_alert_callback(&mesh_alert);
  }

#if FEATURE_BEACON_CHANNEL
  // v0.5: auto-revoke this peer's Beacon trust on tamper. The pubkey is
  // shared between Opera and Beacon identities (spec §3.1), so a single
  // event covers both channels' trust surfaces. Side-effect-only; no-op
  // if the peer isn't in our Beacon set.
  beacon_channel::on_peer_tampered(peer->pubkey);
#endif
}

static void handle_power_alert(OperaPeer* peer, const uint8_t* payload) {
  if (!peer) return;

  const PowerAlertPayload* alert = (const PowerAlertPayload*)payload;

  update_peer_state(peer, PEER_ALERT);
  peer->alerts_received++;
  g_alerts_received++;

  // Store alert
  MeshAlert mesh_alert;
  mesh_alert.timestamp_ms = millis();
  mesh_alert.type = (AlertType)alert->alert_type;
  mesh_alert.severity = SCV_LOG_ALERT;
  memcpy(mesh_alert.sender_fp, peer->fingerprint, FINGERPRINT_SIZE);
  strncpy(mesh_alert.sender_name, peer->name, MAX_PEER_NAME_LEN);
  mesh_alert.witness_seq = 0;
  snprintf(mesh_alert.detail, sizeof(mesh_alert.detail), "Voltage: %umV, Runtime: %us",
           alert->voltage_mv, alert->estimated_runtime_sec);

  store_alert(&mesh_alert);

  if (g_alert_callback) {
    g_alert_callback(&mesh_alert);
  }
}

static void handle_offline_imminent(OperaPeer* peer, const uint8_t* payload) {
  if (!peer) return;

  const OfflineImminentPayload* alert = (const OfflineImminentPayload*)payload;

  update_peer_state(peer, PEER_OFFLINE);
  peer->alerts_received++;
  g_alerts_received++;

  // Store alert
  MeshAlert mesh_alert;
  mesh_alert.timestamp_ms = millis();
  mesh_alert.type = (AlertType)alert->reason;
  mesh_alert.severity = SCV_LOG_TAMPER;
  memcpy(mesh_alert.sender_fp, peer->fingerprint, FINGERPRINT_SIZE);
  strncpy(mesh_alert.sender_name, peer->name, MAX_PEER_NAME_LEN);
  mesh_alert.witness_seq = alert->final_seq;

  // Format chain hash as hex
  char hash_hex[17];
  for (int i = 0; i < 8; i++) {
    sprintf(hash_hex + i * 2, "%02x", alert->final_chain_hash[i]);
  }
  snprintf(mesh_alert.detail, sizeof(mesh_alert.detail), "Final seq: %u, hash: %s",
           alert->final_seq, hash_hex);

  store_alert(&mesh_alert);

  if (g_alert_callback) {
    g_alert_callback(&mesh_alert);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PAIRING HANDLERS
// ════════════════════════════════════════════════════════════════════════════

static void handle_pair_discover(const uint8_t* mac, const uint8_t* payload) {
  if (g_mesh_state != MESH_PAIRING_INIT && g_mesh_state != MESH_PAIRING_JOIN) {
    return;  // Not in pairing mode
  }

  const PairDiscoverPayload* discover = (const PairDiscoverPayload*)payload;

  // If we're initiator and they're joiner, send offer
  if (g_pairing.role == PAIR_ROLE_INITIATOR && discover->role == PAIR_ROLE_JOINER) {
    // Generate ephemeral keypair for this pairing session
    Ed25519::generatePrivateKey(g_pairing.ephemeral_privkey);
    Ed25519::derivePublicKey(g_pairing.ephemeral_pubkey, g_pairing.ephemeral_privkey);

    memcpy(g_pairing.peer_pubkey, discover->pubkey, PUBKEY_SIZE);
    memcpy(g_pairing.peer_mac, mac, 6);

    // Send pairing offer
    PairOfferPayload offer;
    memcpy(offer.ephemeral_pubkey, g_pairing.ephemeral_pubkey, PUBKEY_SIZE);
    memcpy(offer.device_pubkey, g_device_pubkey, PUBKEY_SIZE);
    strncpy(offer.opera_name, g_opera_config.opera_name, MAX_OPERA_NAME_LEN);
    offer.opera_member_count = g_peer_count;

    // Register temporary peer for sending
    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, mac, 6);
    peer_info.channel = ESPNOW_CHANNEL;
    peer_info.encrypt = false;
    esp_now_add_peer(&peer_info);

    send_raw_message(mac, (uint8_t*)&offer, sizeof(offer));
  }
}

static void handle_pair_offer(const uint8_t* mac, const uint8_t* payload) {
  if (g_pairing.role != PAIR_ROLE_JOINER) {
    return;
  }

  const PairOfferPayload* offer = (const PairOfferPayload*)payload;

  memcpy(g_pairing.peer_pubkey, offer->device_pubkey, PUBKEY_SIZE);
  memcpy(g_pairing.peer_mac, mac, 6);

  // Generate our ephemeral keypair
  Ed25519::generatePrivateKey(g_pairing.ephemeral_privkey);
  Ed25519::derivePublicKey(g_pairing.ephemeral_pubkey, g_pairing.ephemeral_privkey);

  // Derive session key from ephemeral keys
  derive_session_key(g_pairing.ephemeral_privkey, offer->ephemeral_pubkey, g_pairing.session_key);

  // Compute confirmation code
  uint8_t code_hash[32];
  sha256_domain(DOMAIN_PAIR_CONFIRM, g_pairing.session_key, SESSION_KEY_SIZE, code_hash);
  g_pairing.confirmation_code = ((uint32_t)code_hash[0] << 16 | (uint32_t)code_hash[1] << 8 | code_hash[2]) % 1000000;

  // Send accept with our ephemeral key
  PairOfferPayload accept;  // Reuse structure
  memcpy(accept.ephemeral_pubkey, g_pairing.ephemeral_pubkey, PUBKEY_SIZE);
  memcpy(accept.device_pubkey, g_device_pubkey, PUBKEY_SIZE);
  strncpy(accept.opera_name, g_device_name, MAX_OPERA_NAME_LEN);
  accept.opera_member_count = 0;

  esp_now_peer_info_t peer_info = {};
  memcpy(peer_info.peer_addr, mac, 6);
  peer_info.channel = ESPNOW_CHANNEL;
  peer_info.encrypt = false;
  esp_now_add_peer(&peer_info);

  send_raw_message(mac, (uint8_t*)&accept, sizeof(accept));

  g_mesh_state = MESH_PAIRING_CONFIRM;
  g_pairing.code_displayed = true;

  if (g_pairing_callback) {
    g_pairing_callback(g_pairing.role, g_pairing.confirmation_code, false);
  }
}

static void handle_pair_accept(const uint8_t* mac, const uint8_t* payload) {
  if (g_pairing.role != PAIR_ROLE_INITIATOR) {
    return;
  }

  const PairOfferPayload* accept = (const PairOfferPayload*)payload;

  // Derive session key from ephemeral keys
  derive_session_key(g_pairing.ephemeral_privkey, accept->ephemeral_pubkey, g_pairing.session_key);

  // Compute confirmation code (should match joiner's)
  uint8_t code_hash[32];
  sha256_domain(DOMAIN_PAIR_CONFIRM, g_pairing.session_key, SESSION_KEY_SIZE, code_hash);
  g_pairing.confirmation_code = ((uint32_t)code_hash[0] << 16 | (uint32_t)code_hash[1] << 8 | code_hash[2]) % 1000000;

  g_mesh_state = MESH_PAIRING_CONFIRM;
  g_pairing.code_displayed = true;

  if (g_pairing_callback) {
    g_pairing_callback(g_pairing.role, g_pairing.confirmation_code, false);
  }
}

static void handle_pair_confirm(const uint8_t* mac, const uint8_t* payload) {
  if (g_mesh_state != MESH_PAIRING_CONFIRM || !g_pairing.code_confirmed) {
    return;
  }

  const PairConfirmPayload* confirm = (const PairConfirmPayload*)payload;

  // Verify confirmation hash
  uint8_t expected_hash[32];
  uint8_t confirm_input[SESSION_KEY_SIZE + 4];
  memcpy(confirm_input, g_pairing.session_key, SESSION_KEY_SIZE);
  memcpy(confirm_input + SESSION_KEY_SIZE, &g_pairing.confirmation_code, 4);
  sha256_domain(DOMAIN_PAIR_CONFIRM, confirm_input, sizeof(confirm_input), expected_hash);

  if (memcmp(confirm->confirmation_hash, expected_hash, 32) != 0) {
    // Confirmation failed - possible MITM
    cancel_pairing();
    return;
  }

  // If we're initiator, send the opera secret
  if (g_pairing.role == PAIR_ROLE_INITIATOR) {
    PairCompletePayload complete;

    // Encrypt opera secret with session key
    uint8_t nonce[NONCE_SIZE];
    uint8_t tag[16];
    encrypt_message(g_pairing.session_key, g_opera_config.opera_secret, OPERA_SECRET_SIZE,
                   complete.encrypted_secret, nonce, tag);
    memcpy(complete.nonce, nonce, NONCE_SIZE);
    memcpy(complete.encrypted_secret + OPERA_SECRET_SIZE, tag, 16);

    send_raw_message(g_pairing.peer_mac, (uint8_t*)&complete, sizeof(complete));

    // Add joiner to our opera
    add_peer(g_pairing.peer_pubkey, g_pairing.peer_mac, "New Device");
    persist_peers();

    g_mesh_state = MESH_ACTIVE;

    if (g_pairing_callback) {
      g_pairing_callback(g_pairing.role, g_pairing.confirmation_code, true);
    }
  }
}

static void handle_pair_complete(const uint8_t* mac, const uint8_t* payload) {
  if (g_pairing.role != PAIR_ROLE_JOINER || g_mesh_state != MESH_PAIRING_CONFIRM) {
    return;
  }

  const PairCompletePayload* complete = (const PairCompletePayload*)payload;

  // Decrypt opera secret
  uint8_t opera_secret[OPERA_SECRET_SIZE];
  const uint8_t* tag = complete->encrypted_secret + OPERA_SECRET_SIZE;

  if (!decrypt_message(g_pairing.session_key, complete->encrypted_secret, OPERA_SECRET_SIZE,
                       complete->nonce, tag, opera_secret)) {
    cancel_pairing();
    return;
  }

  // Initialize our opera config
  memcpy(g_opera_config.opera_secret, opera_secret, OPERA_SECRET_SIZE);
  compute_opera_id(opera_secret, g_opera_config.opera_id);
  g_opera_config.configured = true;
  g_opera_config.enabled = true;
  strncpy(g_opera_config.opera_name, "My Opera", MAX_OPERA_NAME_LEN);

  // Add initiator as first peer
  add_peer(g_pairing.peer_pubkey, g_pairing.peer_mac, "Opera Creator");

  // Persist
  persist_opera_config();
  persist_peers();

  // Clear sensitive pairing data
  memset(&g_pairing, 0, sizeof(g_pairing));

  g_mesh_state = MESH_ACTIVE;

  if (g_pairing_callback) {
    g_pairing_callback(PAIR_ROLE_JOINER, 0, true);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PERSISTENCE
//
// v0.2 (audit O2): the opera_secret is a symmetric secret that gates pairing
// and opera_id derivation. On an unencrypted flash, a physical-access attacker
// can extract it and become a permanent opera member from outside the
// household. Both the save and load paths now refuse to operate unless flash
// encryption is enabled (eFuse FLASH_CRYPT_CNT > 0). Devices that do not
// support flash encryption simply cannot store an opera_secret in v0.2.
// ════════════════════════════════════════════════════════════════════════════

static bool flash_encryption_enabled() {
  // `esp_flash_encryption_enabled()` from esp_flash_encrypt.h returns true
  // when the chip is operating with flash encryption active. Wraps the
  // chip-revision-specific eFuse bit-count logic. Works across ESP32 family.
  return esp_flash_encryption_enabled();
}

static bool persist_opera_config() {
  if (!flash_encryption_enabled()) {
    health_log(SCV_LOG_ALERT, SCV_CAT_CRYPTO,
               "opera: refused to persist secret — flash encryption disabled (audit O2)");
    return false;
  }
  g_prefs.begin(NVS_NS, false);
  g_prefs.putBool(NVS_ENABLED, g_opera_config.enabled);
  g_prefs.putBytes(NVS_FLOCK_ID, g_opera_config.opera_id, OPERA_ID_SIZE);
  g_prefs.putBytes(NVS_FLOCK_SECRET, g_opera_config.opera_secret, OPERA_SECRET_SIZE);
  g_prefs.putString(NVS_FLOCK_NAME, g_opera_config.opera_name);
  g_prefs.end();
  return true;
}

static bool load_opera_config() {
  if (!flash_encryption_enabled()) {
    // Refuse to load any stored secret. Wipe in-memory state and log loudly.
    memset(g_opera_config.opera_secret, 0, OPERA_SECRET_SIZE);
    memset(g_opera_config.opera_id, 0, OPERA_ID_SIZE);
    g_opera_config.configured = false;
    g_opera_config.enabled = false;
    health_log(SCV_LOG_ALERT, SCV_CAT_CRYPTO,
               "opera: refused to load — flash encryption disabled (audit O2)");
    return false;
  }

  g_prefs.begin(NVS_NS, true);
  g_opera_config.enabled = g_prefs.getBool(NVS_ENABLED, false);
  size_t id_len = g_prefs.getBytes(NVS_FLOCK_ID, g_opera_config.opera_id, OPERA_ID_SIZE);
  size_t secret_len = g_prefs.getBytes(NVS_FLOCK_SECRET, g_opera_config.opera_secret, OPERA_SECRET_SIZE);
  String name = g_prefs.getString(NVS_FLOCK_NAME, "");
  strncpy(g_opera_config.opera_name, name.c_str(), MAX_OPERA_NAME_LEN);
  g_opera_config.opera_name[MAX_OPERA_NAME_LEN] = '\0';
  g_opera_config.configured = (id_len == OPERA_ID_SIZE && secret_len == OPERA_SECRET_SIZE);
  g_prefs.end();
  return g_opera_config.configured;
}

static bool persist_peers() {
  g_prefs.begin(NVS_NS, false);
  g_prefs.putUChar(NVS_PEER_COUNT, g_peer_count);

  for (uint8_t i = 0; i < g_peer_count; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_PEER_PREFIX, i);

    // Store pubkey + mac + name
    uint8_t peer_data[PUBKEY_SIZE + 6 + MAX_PEER_NAME_LEN];
    memcpy(peer_data, g_peers[i].pubkey, PUBKEY_SIZE);
    memcpy(peer_data + PUBKEY_SIZE, g_peers[i].mac_addr, 6);
    memcpy(peer_data + PUBKEY_SIZE + 6, g_peers[i].name, MAX_PEER_NAME_LEN);

    g_prefs.putBytes(key, peer_data, sizeof(peer_data));
  }

  g_prefs.end();
  return true;
}

static bool load_peers() {
  g_prefs.begin(NVS_NS, true);
  g_peer_count = g_prefs.getUChar(NVS_PEER_COUNT, 0);

  if (g_peer_count > MAX_OPERA_SIZE) {
    g_peer_count = MAX_OPERA_SIZE;
  }

  for (uint8_t i = 0; i < g_peer_count; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_PEER_PREFIX, i);

    uint8_t peer_data[PUBKEY_SIZE + 6 + MAX_PEER_NAME_LEN];
    size_t len = g_prefs.getBytes(key, peer_data, sizeof(peer_data));

    if (len == sizeof(peer_data)) {
      memcpy(g_peers[i].pubkey, peer_data, PUBKEY_SIZE);
      memcpy(g_peers[i].mac_addr, peer_data + PUBKEY_SIZE, 6);
      memcpy(g_peers[i].name, peer_data + PUBKEY_SIZE + 6, MAX_PEER_NAME_LEN);
      g_peers[i].name[MAX_PEER_NAME_LEN] = '\0';

      compute_fingerprint(g_peers[i].pubkey, g_peers[i].fingerprint);
      g_peers[i].state = PEER_OFFLINE;
      g_peers[i].session_established = false;

      // Register with ESP-NOW
      esp_now_peer_info_t peer_info = {};
      memcpy(peer_info.peer_addr, g_peers[i].mac_addr, 6);
      peer_info.channel = ESPNOW_CHANNEL;
      peer_info.encrypt = false;
      esp_now_add_peer(&peer_info);
    }
  }

  g_prefs.end();
  return true;
}

static void store_alert(const MeshAlert* alert) {
  g_alert_history[g_alert_head] = *alert;
  g_alert_head = (g_alert_head + 1) % MAX_ALERT_HISTORY;
  if (g_alert_count < MAX_ALERT_HISTORY) {
    g_alert_count++;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

bool init(const uint8_t* device_privkey, const uint8_t* device_pubkey, const char* device_name) {
  if (g_initialized) {
    return true;
  }

  g_device_privkey = device_privkey;
  g_device_pubkey = device_pubkey;
  compute_fingerprint(device_pubkey, g_device_fingerprint);
  strncpy(g_device_name, device_name, MAX_PEER_NAME_LEN);
  g_device_name[MAX_PEER_NAME_LEN] = '\0';

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    g_mesh_state = MESH_ERROR;
    return false;
  }

  esp_now_register_send_cb(espnow_send_cb);
  esp_now_register_recv_cb(espnow_recv_cb);
  g_espnow_initialized = true;

  // Start the airtime governor (default 2% cap over a rolling 10 s window).
  // Routine traffic (heartbeats, gossip, presence) checks this before TX so
  // multi-Canary deployments don't degrade the home WiFi.
  airtime_governor::init(airtime_governor::DEFAULT_CAP_PCT);

  // Subscribe to channel changes so we can re-register the ESP-NOW broadcast
  // peer when STA reconnects on a different channel. With peer.channel = 0
  // (set in add_peer / load_peers / broadcast_message) ESP-NOW already follows
  // the radio, but on some IDF versions the peer cache caches the channel —
  // dropping and re-adding the broadcast peer guarantees a clean transition.
  mesh_channel_policy::register_listener(
      [](uint8_t /*old_ch*/, uint8_t /*new_ch*/) {
        if (esp_now_is_peer_exist(BROADCAST_ADDR)) {
          esp_now_del_peer(BROADCAST_ADDR);
        }
      });

  // Load persisted config
  load_opera_config();
  if (g_opera_config.configured) {
    load_peers();
  }

  g_start_time_ms = millis();
  g_initialized = true;

  if (g_opera_config.enabled && g_opera_config.configured) {
    g_mesh_state = MESH_CONNECTING;
  } else if (g_opera_config.enabled) {
    g_mesh_state = MESH_NO_FLOCK;
  } else {
    g_mesh_state = MESH_DISABLED;
  }

  return true;
}

void deinit() {
  if (!g_initialized) return;

  esp_now_unregister_send_cb();
  esp_now_unregister_recv_cb();
  esp_now_deinit();

  g_espnow_initialized = false;
  g_initialized = false;
  g_mesh_state = MESH_DISABLED;
}

void set_enabled(bool enabled) {
  g_opera_config.enabled = enabled;
  persist_opera_config();

  if (enabled) {
    if (g_opera_config.configured) {
      g_mesh_state = MESH_CONNECTING;
    } else {
      g_mesh_state = MESH_NO_FLOCK;
    }
  } else {
    g_mesh_state = MESH_DISABLED;
  }
}

bool is_enabled() {
  return g_opera_config.enabled;
}

void update() {
  if (!g_initialized || g_mesh_state == MESH_DISABLED) {
    return;
  }

  // Re-sample the radio channel before doing any TX. This catches the case
  // where STA reconnected on a new channel since the last loop iteration; the
  // registered listener (on_channel_changed) re-registers the broadcast peer
  // on the new channel so ESP-NOW sends land correctly.
  mesh_channel_policy::poll_radio();

  uint32_t now = millis();

  // Process received messages
  if (g_rx_pending) {
    handle_received_message(g_rx_mac, g_rx_buffer, g_rx_len);
    // Chirp shares the ESP-NOW channel with the mesh protocol; forward every
    // frame to the chirp dispatcher, which filters on CHIRP_MAGIC and ignores
    // non-chirp traffic. g_rx_rssi is the actual RSSI reported by the radio
    // in espnow_recv_cb (info->rx_ctrl->rssi).
    chirp_channel::dispatch_espnow_message(g_rx_mac, g_rx_buffer,
                                           (int)g_rx_len, g_rx_rssi);
    g_rx_pending = false;
  }

  // v0.3 (audit O3): if a rekey is in flight, finalize when all peers have
  // ACKed or the timeout expires.
  maybe_finalize_rekey();

  // Check pairing timeout
  if ((g_mesh_state == MESH_PAIRING_INIT || g_mesh_state == MESH_PAIRING_JOIN ||
       g_mesh_state == MESH_PAIRING_CONFIRM) &&
      now - g_pairing.started_ms > PAIRING_TIMEOUT_MS) {
    cancel_pairing();
  }

  // Send periodic heartbeat
  if (g_mesh_state == MESH_ACTIVE && now - g_last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
    send_heartbeat();
    g_last_heartbeat_ms = now;
  }

  // Check peer states
  if (now - g_last_peer_check_ms >= 5000) {
    g_last_peer_check_ms = now;

    bool any_online = false;
    for (uint8_t i = 0; i < g_peer_count; i++) {
      OperaPeer* peer = &g_peers[i];
      uint32_t since_seen = now - peer->last_seen_ms;

      if (peer->state == PEER_CONNECTED || peer->state == PEER_ALERT) {
        if (since_seen > PEER_OFFLINE_MS) {
          update_peer_state(peer, PEER_OFFLINE);
        } else if (since_seen > PEER_STALE_MS) {
          update_peer_state(peer, PEER_STALE);
        }
        any_online = true;
      } else if (peer->state == PEER_STALE) {
        if (since_seen > PEER_OFFLINE_MS) {
          update_peer_state(peer, PEER_OFFLINE);
        } else {
          any_online = true;
        }
      }
    }

    // Update mesh state based on peer connectivity
    if (g_mesh_state == MESH_ACTIVE && !any_online && g_peer_count > 0) {
      g_mesh_state = MESH_CONNECTING;
    } else if (g_mesh_state == MESH_CONNECTING && any_online) {
      g_mesh_state = MESH_ACTIVE;
    }
  }

  // Pairing discovery broadcasts
  if (g_mesh_state == MESH_PAIRING_INIT || g_mesh_state == MESH_PAIRING_JOIN) {
    static uint32_t last_discover = 0;
    if (now - last_discover >= 2000) {
      PairDiscoverPayload discover;
      memcpy(discover.pubkey, g_device_pubkey, PUBKEY_SIZE);
      strncpy(discover.device_name, g_device_name, MAX_PEER_NAME_LEN);
      discover.role = (uint8_t)g_pairing.role;

      send_raw_message((uint8_t*)BROADCAST_ADDR, (uint8_t*)&discover, sizeof(discover));
      last_discover = now;
    }
  }
}

MeshStatus get_status() {
  MeshStatus status;
  status.state = g_mesh_state;
  status.espnow_active = g_espnow_initialized;
  status.peers_total = g_peer_count;

  status.peers_online = 0;
  status.peers_offline = 0;
  status.peers_stale = 0;

  for (uint8_t i = 0; i < g_peer_count; i++) {
    switch (g_peers[i].state) {
      case PEER_CONNECTED:
      case PEER_ALERT:
        status.peers_online++;
        break;
      case PEER_STALE:
        status.peers_stale++;
        break;
      default:
        status.peers_offline++;
        break;
    }
  }

  status.messages_sent = g_messages_sent;
  status.messages_received = g_messages_received;
  status.alerts_sent = g_alerts_sent;
  status.alerts_received = g_alerts_received;
  status.auth_failures = g_auth_failures;
  status.uptime_ms = millis() - g_start_time_ms;
  status.last_heartbeat_ms = g_last_heartbeat_ms;

  // Format opera ID as hex
  for (size_t i = 0; i < OPERA_ID_SIZE; i++) {
    sprintf(status.opera_id_hex + i * 2, "%02x", g_opera_config.opera_id[i]);
  }
  status.opera_id_hex[OPERA_ID_SIZE * 2] = '\0';

  return status;
}

const char* state_name(MeshState state) {
  switch (state) {
    case MESH_DISABLED: return "DISABLED";
    case MESH_INITIALIZING: return "INITIALIZING";
    case MESH_NO_FLOCK: return "NO_FLOCK";
    case MESH_CONNECTING: return "CONNECTING";
    case MESH_ACTIVE: return "ACTIVE";
    case MESH_PAIRING_INIT: return "PAIRING_INIT";
    case MESH_PAIRING_JOIN: return "PAIRING_JOIN";
    case MESH_PAIRING_CONFIRM: return "PAIRING_CONFIRM";
    case MESH_ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}

const char* peer_state_name(PeerState state) {
  switch (state) {
    case PEER_UNKNOWN: return "UNKNOWN";
    case PEER_DISCOVERED: return "DISCOVERED";
    case PEER_AUTHENTICATING: return "AUTH";
    case PEER_CONNECTED: return "CONNECTED";
    case PEER_STALE: return "STALE";
    case PEER_OFFLINE: return "OFFLINE";
    case PEER_ALERT: return "ALERT";
    case PEER_REMOVED: return "REMOVED";
    default: return "???";
  }
}

const char* alert_type_name(AlertType type) {
  switch (type) {
    case ALERT_TAMPER: return "TAMPER";
    case ALERT_MOTION: return "MOTION";
    case ALERT_BREACH: return "BREACH";
    case ALERT_POWER_LOSS: return "POWER_LOSS";
    case ALERT_LOW_VOLTAGE: return "LOW_VOLTAGE";
    case ALERT_BATTERY_CRITICAL: return "BATTERY_CRIT";
    case ALERT_OFFLINE_SHUTDOWN: return "SHUTDOWN";
    case ALERT_OFFLINE_TAMPER: return "OFFLINE_TAMPER";
    case ALERT_OFFLINE_POWER: return "OFFLINE_POWER";
    case ALERT_OFFLINE_REBOOT: return "REBOOT";
    default: return "UNKNOWN";
  }
}

bool is_active() {
  return g_mesh_state == MESH_ACTIVE;
}

bool has_opera() {
  return g_opera_config.configured;
}

uint8_t get_peer_count() {
  return g_peer_count;
}

const OperaPeer* get_peer(uint8_t index) {
  if (index >= g_peer_count) return nullptr;
  return &g_peers[index];
}

const OperaPeer* get_peer_by_fingerprint(const uint8_t* fingerprint) {
  return find_peer_by_fingerprint(fingerprint);
}

uint8_t get_online_peer_count() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < g_peer_count; i++) {
    if (g_peers[i].state == PEER_CONNECTED || g_peers[i].state == PEER_ALERT) {
      count++;
    }
  }
  return count;
}

bool remove_peer(const uint8_t* fingerprint) {
  for (uint8_t i = 0; i < g_peer_count; i++) {
    if (memcmp(g_peers[i].fingerprint, fingerprint, FINGERPRINT_SIZE) == 0) {
      // Remove from ESP-NOW
      esp_now_del_peer(g_peers[i].mac_addr);

      // Shift remaining peers
      for (uint8_t j = i; j < g_peer_count - 1; j++) {
        g_peers[j] = g_peers[j + 1];
      }
      g_peer_count--;

      persist_peers();

      // ── audit O3 closure: transactional opera_secret rotation ──
      //
      // 1. Compute a candidate new secret + new opera_id (NOT yet committed).
      // 2. For each surviving member, send MSG_OPERA_REKEY encrypted under
      //    that member's current session key.
      // 3. Wait for MSG_OPERA_REKEY_ACK from each (handled in the message
      //    dispatcher above). The update() loop calls finalize_rekey() when
      //    all members ACK or the timeout expires.
      // 4. On commit, persist new secret + new opera_id to NVS. Members that
      //    didn't ACK are marked stale and will re-pair under the new
      //    opera_id.
      //
      // If a rekey is already in flight, we still rotate locally and persist
      // (the in-flight one will be superseded by the new rekey_id, and any
      // late ACKs for the old id are dropped).
      if (g_opera_config.configured && g_peer_count > 0) {
        // Stage candidate secret in g_rekey (NOT in g_opera_config yet).
        g_rekey.active = true;
        g_rekey.rekey_id = (uint32_t)millis();
        g_rekey.started_ms = millis();
        esp_fill_random(g_rekey.pending_secret, OPERA_SECRET_SIZE);
        compute_opera_id(g_rekey.pending_secret, g_rekey.pending_opera_id);

        // Encrypt + send REKEY to each surviving member; mark each as
        // pending-ACK in the bitmask.
        g_rekey.pending_acks = 0;
        for (uint8_t j = 0; j < g_peer_count; j++) {
          if (!g_peers[j].session_established) continue;
          OperaRekeyPayload rk;
          rk.rekey_id = g_rekey.rekey_id;
          // encrypt_message signature: (key, plaintext, len, ciphertext,
          //                             nonce_out, tag_out)
          if (!encrypt_message(g_peers[j].session_key,
                               g_rekey.pending_secret, OPERA_SECRET_SIZE,
                               rk.encrypted_secret, rk.nonce, rk.tag)) {
            continue;
          }
          if (send_to_peer(&g_peers[j], MSG_OPERA_REKEY,
                           (uint8_t*)&rk, sizeof(rk))) {
            g_rekey.pending_acks |= (uint16_t)(1u << j);
          }
        }

        health_log(SCV_LOG_INFO, SCV_CAT_CRYPTO,
                   "opera: rekey transaction started after peer removal (O3)");
      }

      return true;
    }
  }
  return false;
}

// Called from update() once per loop. Commits the rekey transaction when all
// peers have ACKed, or when REKEY_TIMEOUT_MS elapses (whichever first).
// Peers that didn't ACK are marked PEER_STALE; they'll re-pair under the new
// opera_id via the normal flow.
static void maybe_finalize_rekey() {
  if (!g_rekey.active) return;
  uint32_t now = millis();
  bool all_acked = (g_rekey.pending_acks == 0);
  bool timed_out = (now - g_rekey.started_ms) > REKEY_TIMEOUT_MS;
  if (!all_acked && !timed_out) return;

  // Commit candidate to live state.
  memcpy(g_opera_config.opera_secret, g_rekey.pending_secret, OPERA_SECRET_SIZE);
  memcpy(g_opera_config.opera_id, g_rekey.pending_opera_id, OPERA_ID_SIZE);

  // Invalidate sessions everywhere; mark unacked peers stale.
  for (uint8_t j = 0; j < g_peer_count; j++) {
    bool unacked = (g_rekey.pending_acks & (uint16_t)(1u << j)) != 0;
    g_peers[j].session_established = false;
    memset(g_peers[j].session_key, 0, SESSION_KEY_SIZE);
    g_peers[j].msg_counter_tx = 0;
    g_peers[j].msg_counter_rx = 0;
    g_peers[j].state = unacked ? PEER_STALE : PEER_AUTHENTICATING;
  }

  if (!persist_opera_config()) {
    health_log(SCV_LOG_ALERT, SCV_CAT_CRYPTO,
               "opera: rekey commit succeeded in memory but persist refused (O2)");
  }
  // Wipe candidate to prevent late-ACK matches.
  memset(g_rekey.pending_secret, 0, OPERA_SECRET_SIZE);
  memset(g_rekey.pending_opera_id, 0, OPERA_ID_SIZE);
  g_rekey.active = false;
  g_rekey.pending_acks = 0;
  health_log(SCV_LOG_INFO, SCV_CAT_CRYPTO,
             all_acked ? "opera: rekey committed (all ACKs received)"
                       : "opera: rekey committed (timeout; unacked peers marked stale)");
}

const OperaConfig* get_opera_config() {
  return &g_opera_config;
}

bool set_opera_name(const char* name) {
  strncpy(g_opera_config.opera_name, name, MAX_OPERA_NAME_LEN);
  g_opera_config.opera_name[MAX_OPERA_NAME_LEN] = '\0';
  return persist_opera_config();
}

bool leave_opera() {
  // Broadcast leave message to peers
  broadcast_message(MSG_LEAVE_OPERA, nullptr, 0);

  // Clear opera config
  memset(&g_opera_config, 0, sizeof(g_opera_config));

  // Remove all peers
  for (uint8_t i = 0; i < g_peer_count; i++) {
    esp_now_del_peer(g_peers[i].mac_addr);
  }
  g_peer_count = 0;

  // Persist
  persist_opera_config();
  persist_peers();

  g_mesh_state = MESH_NO_FLOCK;
  return true;
}

bool start_pairing_initiator(const char* opera_name) {
  if (g_mesh_state == MESH_PAIRING_INIT || g_mesh_state == MESH_PAIRING_JOIN) {
    return false;  // Already pairing
  }

  memset(&g_pairing, 0, sizeof(g_pairing));
  g_pairing.role = PAIR_ROLE_INITIATOR;
  g_pairing.started_ms = millis();

  // Create new opera if we don't have one
  if (!g_opera_config.configured) {
    esp_fill_random(g_opera_config.opera_secret, OPERA_SECRET_SIZE);
    compute_opera_id(g_opera_config.opera_secret, g_opera_config.opera_id);
    g_opera_config.configured = true;
    g_opera_config.enabled = true;

    if (opera_name) {
      strncpy(g_opera_config.opera_name, opera_name, MAX_OPERA_NAME_LEN);
    } else {
      strcpy(g_opera_config.opera_name, "My Canary Opera");
    }
    g_opera_config.opera_name[MAX_OPERA_NAME_LEN] = '\0';

    persist_opera_config();
  }

  g_mesh_state = MESH_PAIRING_INIT;
  return true;
}

bool start_pairing_joiner() {
  if (g_mesh_state == MESH_PAIRING_INIT || g_mesh_state == MESH_PAIRING_JOIN) {
    return false;
  }

  memset(&g_pairing, 0, sizeof(g_pairing));
  g_pairing.role = PAIR_ROLE_JOINER;
  g_pairing.started_ms = millis();

  g_mesh_state = MESH_PAIRING_JOIN;
  return true;
}

void cancel_pairing() {
  memset(&g_pairing, 0, sizeof(g_pairing));

  if (g_opera_config.configured) {
    g_mesh_state = g_peer_count > 0 ? MESH_CONNECTING : MESH_NO_FLOCK;
  } else {
    g_mesh_state = MESH_NO_FLOCK;
  }

  if (g_pairing_callback) {
    g_pairing_callback(PAIR_ROLE_NONE, 0, false);
  }
}

bool confirm_pairing() {
  if (g_mesh_state != MESH_PAIRING_CONFIRM || !g_pairing.code_displayed) {
    return false;
  }

  g_pairing.code_confirmed = true;

  // Send confirmation message
  PairConfirmPayload confirm;
  uint8_t confirm_input[SESSION_KEY_SIZE + 4];
  memcpy(confirm_input, g_pairing.session_key, SESSION_KEY_SIZE);
  memcpy(confirm_input + SESSION_KEY_SIZE, &g_pairing.confirmation_code, 4);
  sha256_domain(DOMAIN_PAIR_CONFIRM, confirm_input, sizeof(confirm_input), confirm.confirmation_hash);

  send_raw_message(g_pairing.peer_mac, (uint8_t*)&confirm, sizeof(confirm));

  return true;
}

const PairingSession* get_pairing_session() {
  return &g_pairing;
}

bool is_pairing() {
  return g_mesh_state >= MESH_PAIRING_INIT && g_mesh_state <= MESH_PAIRING_CONFIRM;
}

bool broadcast_tamper_alert(AlertType type, LogLevel severity, uint32_t witness_seq, const char* detail) {
  if (g_mesh_state != MESH_ACTIVE) {
    return false;
  }

  TamperAlertPayload payload;
  payload.alert_type = (uint8_t)type;
  payload.severity = (uint8_t)severity;
  payload.witness_seq = witness_seq;
  if (detail) {
    strncpy(payload.detail, detail, sizeof(payload.detail) - 1);
    payload.detail[sizeof(payload.detail) - 1] = '\0';
  } else {
    payload.detail[0] = '\0';
  }

  g_alerts_sent++;
  // Tamper alerts are urgent — bypass the routine airtime cap but still
  // record their cost so telemetry reflects reality.
  airtime_governor::force_reserve_urgent(millis(),
      sizeof(MessageHeader) + sizeof(payload));
  return broadcast_message(MSG_TAMPER_ALERT, (uint8_t*)&payload, sizeof(payload));
}

bool broadcast_power_alert(AlertType type, uint16_t voltage_mv, uint16_t estimated_runtime_sec) {
  if (g_mesh_state != MESH_ACTIVE) {
    return false;
  }

  PowerAlertPayload payload;
  payload.alert_type = (uint8_t)type;
  payload.voltage_mv = voltage_mv;
  payload.estimated_runtime_sec = estimated_runtime_sec;

  g_alerts_sent++;
  airtime_governor::force_reserve_urgent(millis(),
      sizeof(MessageHeader) + sizeof(payload));
  return broadcast_message(MSG_POWER_ALERT, (uint8_t*)&payload, sizeof(payload));
}

bool broadcast_offline_imminent(AlertType reason, uint32_t final_seq, const uint8_t* final_chain_hash) {
  // This is a critical message - try to send even if not fully active
  if (!g_opera_config.configured) {
    return false;
  }

  OfflineImminentPayload payload;
  payload.reason = (uint8_t)reason;
  payload.final_seq = final_seq;
  memcpy(payload.final_chain_hash, final_chain_hash, 8);

  g_alerts_sent++;
  // OFFLINE_IMMINENT is the most urgent message in the protocol — we record
  // its cost (per-peer fan-out) but never gate it.
  airtime_governor::force_reserve_urgent(millis(),
      (sizeof(MessageHeader) + sizeof(payload)) * g_peer_count);

  // Send to all known peers regardless of connection state
  bool any_sent = false;
  for (uint8_t i = 0; i < g_peer_count; i++) {
    if (send_to_peer(&g_peers[i], MSG_OFFLINE_IMMINENT, (uint8_t*)&payload, sizeof(payload))) {
      any_sent = true;
    }
  }
  return any_sent;
}

const MeshAlert* get_alerts(size_t* count) {
  *count = g_alert_count;
  return g_alert_history;
}

void clear_alerts() {
  g_alert_count = 0;
  g_alert_head = 0;
  memset(g_alert_history, 0, sizeof(g_alert_history));
}

void set_alert_callback(AlertCallback callback) {
  g_alert_callback = callback;
}

void set_peer_state_callback(PeerStateCallback callback) {
  g_peer_state_callback = callback;
}

void set_pairing_callback(PairingCallback callback) {
  g_pairing_callback = callback;
}

void send_heartbeat() {
  if (!g_opera_config.configured) return;

  HeartbeatPayload payload;
  payload.status = 0;  // Online
  payload.uptime_sec = (millis() - g_start_time_ms) / 1000;
  payload.peer_count = g_peer_count;
  payload.battery_pct = 255;  // Unknown

  // Heartbeat is routine traffic — skip this tick if we'd blow the airtime
  // cap. The peer-stale timer (90 s) is long enough to tolerate a few skipped
  // heartbeats; the only consequence of skipping is a slightly delayed stale
  // transition for peers that were also being noisy.
  const size_t wire_bytes = sizeof(MessageHeader) + sizeof(payload);
  if (!airtime_governor::try_reserve_routine(millis(), wire_bytes)) {
    return;
  }

  broadcast_message(MSG_HEARTBEAT, (uint8_t*)&payload, sizeof(payload));
}

void get_message_stats(uint32_t* sent, uint32_t* received, uint32_t* errors) {
  if (sent) *sent = g_messages_sent;
  if (received) *received = g_messages_received;
  if (errors) *errors = g_message_errors;
}

// ════════════════════════════════════════════════════════════════════════════
// BEACON EVENT — canary-wap parity for PIO send_beacon_event +
// set_beacon_event_handler. Encodes the 25-byte payload via
// mesh_beacon::encode and calls send_to_peer for every connected
// peer in g_peers. Reuses ALL of the existing send_to_peer envelope
// machinery (Ed25519 sign + counter + opera_id), so wire-format
// parity with the PIO build is automatic.
// ════════════════════════════════════════════════════════════════════════════

size_t send_beacon_event(mesh_beacon::BeaconState state, const char* label) {
  if (!g_opera_config.configured) return 0;

  uint8_t payload[mesh_beacon::PAYLOAD_LEN];
  if (!mesh_beacon::encode(state, label, payload, sizeof(payload))) {
    return 0;
  }

  size_t accepted = 0;
  for (uint8_t i = 0; i < g_peer_count; ++i) {
    if (g_peers[i].state >= PEER_CONNECTED) {
      if (send_to_peer(&g_peers[i], MSG_BEACON_EVENT,
                       payload, sizeof(payload))) {
        ++accepted;
      }
    }
  }
  return accepted;
}

void set_beacon_event_handler(beacon_event_handler_fn fn) {
  g_beacon_event_handler = fn;
}

// ════════════════════════════════════════════════════════════════════════════
// CHANNEL LOCK — PR 4b coordinated channel-hop broadcast.
// Same send_to_peer envelope machinery as BEACON_EVENT above.
// ════════════════════════════════════════════════════════════════════════════

size_t send_channel_lock(uint8_t channel, mesh_channel_hop::Reason reason) {
  if (!g_opera_config.configured) return 0;

  uint8_t payload[mesh_channel_hop::PAYLOAD_LEN];
  if (!mesh_channel_hop::encode(channel, reason, payload, sizeof(payload))) {
    return 0;
  }

  size_t accepted = 0;
  for (uint8_t i = 0; i < g_peer_count; ++i) {
    if (g_peers[i].state >= PEER_CONNECTED) {
      if (!send_to_peer(&g_peers[i], MSG_CHANNEL_LOCK,
                        payload, sizeof(payload))) {
        break;
      }
      ++accepted;
    }
  }
  return accepted;
}

void set_channel_lock_handler(channel_lock_handler_fn fn) {
  g_channel_lock_handler = fn;
}

} // namespace mesh_network

#endif // FEATURE_MESH_NETWORK
