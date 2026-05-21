/*
 * SecuraCV Canary — Mesh session bridge — Implementation
 *
 * Singleton bridge wiring mesh_transport ↔ mesh_pairing.
 *
 * Recv path:
 *   mesh_transport::process() invokes our recv_cb on the main loop →
 *   decode the 1-byte MsgType envelope → mesh_pairing::receive(),
 *   which returns an Action → dispatch_action() forwards it back
 *   through mesh_transport, firing the integration-layer's callbacks
 *   on terminal states.
 *
 * Send path:
 *   start_pairing_*, confirm_pairing_code, cancel_pairing all return
 *   an Action from mesh_pairing, which dispatch_action() forwards.
 *
 * Tick path:
 *   process() drives mesh_pairing::tick() once per call. The initiator
 *   NOTIFY_PAIRED arrives via this path; joiner NOTIFY_PAIRED arrives
 *   inline from the COMPLETE recv handler.
 */

#include "mesh_session.h"
#include "mesh_envelope.h"

#include <string.h>

#ifndef CSI_TEST_HOST_BUILD
  #include <Arduino.h>
#endif

namespace mesh_session {

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool                       s_initialized = false;
static bool                       s_running     = false;
static mesh_pairing::PairingContext s_ctx;
static uint8_t                    s_device_pub [mesh_crypto::PUBKEY_LEN];
static uint8_t                    s_device_priv[mesh_crypto::PRIVKEY_LEN];

static PairedCallback     s_paired_cb     = nullptr;
static FailedCallback     s_failed_cb     = nullptr;
static CodeReadyCallback  s_code_ready_cb = nullptr;

/* ──────────────────────────────────────────────────────────────────────────
 * INTERNAL HELPERS
 * ────────────────────────────────────────────────────────────────────────── */

/* secure_zero: volatile-loop + asm memory barrier so the compiler can't
 * dead-store-eliminate the wipe. Same pattern as mesh_crypto.cpp and
 * mesh_pairing.cpp; file-local to keep this module standalone. */
static inline void secure_zero(void* p, size_t n) {
  volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
  while (n--) *b++ = 0;
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" ::: "memory");
#endif
}

/* Convert mesh_pairing::MsgType (used internally by the state machine)
 * to mesh_session::MsgType (used on the wire). Today these are 1:1 by
 * design; the helper exists so a future divergence is a single edit. */
static inline MsgType pairing_msg_to_session(mesh_pairing::MsgType m) {
  return static_cast<MsgType>(static_cast<uint8_t>(m));
}

/* Map mesh_pairing::Action to a session-frame outgoing MsgType. Returns
 * (out_msg_type, dest_mac, payload, len) by reference; caller decides
 * unicast vs broadcast based on the Action::type. */
static bool action_to_wire(const mesh_pairing::Action& a,
                           MsgType*  out_msg_type) {
  switch (a.type) {
    case mesh_pairing::ActionType::BROADCAST_DISCOVER:
      *out_msg_type = MsgType::PAIR_DISCOVER; return true;
    case mesh_pairing::ActionType::SEND_OFFER:
      *out_msg_type = MsgType::PAIR_OFFER;    return true;
    case mesh_pairing::ActionType::SEND_ACCEPT:
      *out_msg_type = MsgType::PAIR_ACCEPT;   return true;
    case mesh_pairing::ActionType::SEND_CONFIRM:
      *out_msg_type = MsgType::PAIR_CONFIRM;  return true;
    case mesh_pairing::ActionType::SEND_COMPLETE:
      *out_msg_type = MsgType::PAIR_COMPLETE; return true;
    default:
      return false;   /* non-wire action (NOTIFY_*, NONE) */
  }
}

/* Forward an Action returned by the pairing state machine. Sends any
 * outbound payload via mesh_transport (broadcast for DISCOVER, unicast
 * otherwise). Fires the integration-layer callback on terminal actions
 * (NOTIFY_*). */
static void dispatch_action(const mesh_pairing::Action& a) {
  MsgType wire_type;
  if (action_to_wire(a, &wire_type)) {
    /* Build the on-wire frame: 1-byte MsgType + payload bytes. */
    uint8_t frame[MAX_SESSION_FRAME];
    frame[0] = static_cast<uint8_t>(wire_type);
    if (a.payload_len > 0) {
      memcpy(frame + MSGTYPE_HEADER_LEN, a.payload, a.payload_len);
    }
    const size_t frame_len = MSGTYPE_HEADER_LEN + a.payload_len;

    if (a.type == mesh_pairing::ActionType::BROADCAST_DISCOVER) {
      /* Pre-membership: the joiner is not yet a peer of the initiator
       * (and vice versa), so mesh_transport::send_to_peer would reject
       * the FF MAC. send_raw bypasses the peer-table check and routes
       * straight to esp_now_send (which has the FF MAC pre-registered
       * by mesh_transport::init). */
      mesh_transport::send_raw(a.peer_mac, frame, frame_len);
    } else {
      mesh_transport::send_to_peer(a.peer_mac, frame, frame_len);
    }
  }

  /* Integration-layer callbacks for non-wire actions. */
  switch (a.type) {
    case mesh_pairing::ActionType::NOTIFY_CODE_READY:
      if (s_code_ready_cb) s_code_ready_cb(a.confirmation_code);
      break;
    case mesh_pairing::ActionType::NOTIFY_PAIRED: {
      uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
      const bool have_secret =
          mesh_pairing::consume_opera_secret(s_ctx, opera_secret);
      if (s_paired_cb) {
        s_paired_cb(have_secret ? opera_secret : nullptr, a.confirmation_code);
      }
      /* Wipe the local copy after the callback returns — the integration
       * layer was responsible for persisting it. secure_zero (volatile +
       * asm barrier) so the compiler can't elide this. */
      secure_zero(opera_secret, sizeof(opera_secret));
      break;
    }
    case mesh_pairing::ActionType::NOTIFY_FAILED:
      if (s_failed_cb) s_failed_cb();
      break;
    default:
      break;
  }
}

/* mesh_transport recv callback. Decodes the 1-byte MsgType envelope
 * and routes to mesh_pairing::receive(). */
static void on_transport_recv(const uint8_t mac[6],
                              const uint8_t* data, size_t len,
                              int8_t /*rssi*/) {
  if (!s_running || data == nullptr || len < MSGTYPE_HEADER_LEN) return;
  const uint8_t type_byte = data[0];

  /* Only PAIR_* msg types are routable in PR 2f. Higher values are
   * reserved for PR 2g (opera-authenticated traffic) and are silently
   * dropped here today. */
  if (type_byte > static_cast<uint8_t>(MsgType::PAIR_COMPLETE)) return;

  const mesh_pairing::MsgType pair_type =
      static_cast<mesh_pairing::MsgType>(type_byte);
  const uint8_t* payload = data + MSGTYPE_HEADER_LEN;
  const size_t   payload_len = len - MSGTYPE_HEADER_LEN;

  /* now_ms isn't readily available in this callback context, but
   * mesh_pairing::receive uses it only for the tamper-path nothing-
   * else, so 0 is acceptable. The tick() path supplies a real now_ms
   * for timeout enforcement. */
  mesh_pairing::Action a = mesh_pairing::receive(s_ctx, mac, pair_type,
                                                  payload, payload_len, 0);
  dispatch_action(a);
}

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const uint8_t device_pubkey [mesh_crypto::PUBKEY_LEN],
          const uint8_t device_privkey[mesh_crypto::PRIVKEY_LEN]) {
  if (s_initialized) return true;
  if (device_pubkey == nullptr || device_privkey == nullptr) return false;

  memcpy(s_device_pub,  device_pubkey,  mesh_crypto::PUBKEY_LEN);
  memcpy(s_device_priv, device_privkey, mesh_crypto::PRIVKEY_LEN);
  mesh_pairing::context_init(s_ctx);
  mesh_transport::set_recv_callback(&on_transport_recv);
  s_initialized = true;
  return true;
}

void deinit() {
  if (!s_initialized) return;
  mesh_transport::set_recv_callback(nullptr);
  mesh_pairing::context_init(s_ctx);   /* wipes ephem/session/secret */
  secure_zero(s_device_priv, sizeof(s_device_priv));
  s_paired_cb = nullptr;
  s_failed_cb = nullptr;
  s_code_ready_cb = nullptr;
  s_running = false;
  s_initialized = false;
}

bool start() {
  if (!s_initialized) return false;
  s_running = true;
  return true;
}

void stop() {
  s_running = false;
}

bool is_running() { return s_running; }

void set_paired_callback    (PairedCallback     cb) { s_paired_cb     = cb; }
void set_failed_callback    (FailedCallback     cb) { s_failed_cb     = cb; }
void set_code_ready_callback(CodeReadyCallback  cb) { s_code_ready_cb = cb; }

/* ──────────────────────────────────────────────────────────────────────────
 * PAIRING ENTRY POINTS
 * ────────────────────────────────────────────────────────────────────────── */

bool start_pairing_initiator(const uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN],
                             const char*   opera_name,
                             uint32_t      now_ms) {
  if (!s_running) return false;
  mesh_pairing::Action a =
      mesh_pairing::start_initiator(s_ctx, s_device_pub, s_device_priv,
                                    opera_secret, opera_name, now_ms);
  if (a.type == mesh_pairing::ActionType::NONE) return false;
  dispatch_action(a);
  return true;
}

bool start_pairing_joiner(uint32_t now_ms) {
  if (!s_running) return false;
  mesh_pairing::Action a =
      mesh_pairing::start_joiner(s_ctx, s_device_pub, s_device_priv, now_ms);
  if (a.type == mesh_pairing::ActionType::NONE) return false;
  dispatch_action(a);
  return true;
}

bool confirm_pairing_code(uint32_t now_ms) {
  if (!s_running) return false;
  mesh_pairing::Action a = mesh_pairing::confirm_code(s_ctx, now_ms);
  if (a.type == mesh_pairing::ActionType::NONE) return false;
  dispatch_action(a);
  return true;
}

void cancel_pairing() {
  if (!s_running) return;
  mesh_pairing::Action a = mesh_pairing::cancel(s_ctx);
  dispatch_action(a);
}

mesh_pairing::State pairing_state()        { return s_ctx.state; }
uint32_t            pairing_confirmation_code() { return s_ctx.confirmation_code; }

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN LOOP
 * ────────────────────────────────────────────────────────────────────────── */

void process(uint32_t now_ms) {
  if (!s_running) return;
  mesh_pairing::Action a = mesh_pairing::tick(s_ctx, now_ms);
  dispatch_action(a);
}

/* ──────────────────────────────────────────────────────────────────────────
 * OPERA-AUTHENTICATED BROADCAST (PR 5c-3)
 *
 * Three pieces of state added here:
 *
 *   s_opera_id_set   — false until set_opera_secret() succeeds.
 *   s_opera_id       — 16 bytes; sha256_domain(DOMAIN_OPERA_ID,
 *                      opera_secret) truncated. Cached so we don't
 *                      re-hash on every send.
 *   s_sender_fp      — 8 bytes; sha256_domain(DOMAIN_FINGERPRINT,
 *                      s_device_pub) truncated. Cached at first
 *                      set_opera_secret() since device_pub doesn't
 *                      change post-init().
 *   s_outbound_counter — monotonic per-process. PR 5c-3 keeps it in
 *                      RAM only; PR 5c-4 will persist to NVS so a
 *                      reboot doesn't replay-reset counters at peers.
 * ────────────────────────────────────────────────────────────────────────── */

static bool     s_opera_id_set       = false;
static uint8_t  s_opera_id [mesh_crypto::OPERA_ID_LEN];
static uint8_t  s_sender_fp[mesh_crypto::FINGERPRINT_LEN];
static uint64_t s_outbound_counter   = 0;

bool set_opera_secret(const uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN]) {
  if (opera_secret == nullptr) return false;
  if (!s_initialized) return false;   /* device keypair must be loaded first */

  mesh_crypto::compute_opera_id(opera_secret, s_opera_id);
  mesh_crypto::compute_fingerprint(s_device_pub, s_sender_fp);
  s_opera_id_set = true;
  return true;
}

bool has_opera_secret() {
  return s_opera_id_set;
}

bool send_beacon_event(mesh_beacon::BeaconState state,
                       const char*              label,
                       uint32_t                 now_ms) {
  if (!s_initialized || !s_running) return false;
  if (!s_opera_id_set)             return false;

  /* 1. Encode the 25-byte BEACON_EVENT payload. */
  uint8_t payload[mesh_beacon::PAYLOAD_LEN];
  if (!mesh_beacon::encode(state, label, payload, sizeof(payload))) {
    return false;
  }

  /* 2. Build the envelope header. The outbound counter is bumped FIRST
   * so two simultaneous calls (which shouldn't happen — single-task
   * discipline — but the increment is local anyway) produce distinct
   * counters. */
  mesh_envelope::Header header;
  header.version   = mesh_envelope::PROTOCOL_VERSION;
  header.msg_type  = static_cast<uint8_t>(mesh_envelope::MsgType::BEACON_EVENT);
  memcpy(header.opera_id,  s_opera_id,  sizeof(header.opera_id));
  memcpy(header.sender_fp, s_sender_fp, sizeof(header.sender_fp));
  header.counter   = ++s_outbound_counter;
  header.timestamp = now_ms;

  /* 3. Serialize + sign. The signed frame is HEADER_LEN(38) +
   * PAYLOAD_LEN(25) + SIGNATURE_LEN(64) = 127 bytes. We then prepend
   * a 1-byte session msg type so the same wire dispatch that handles
   * PAIR_* frames can route this too: receivers see frame[0]=22 and
   * forward frame[1..] into mesh_envelope::parse_and_verify (peer-
   * table lookup added in PR 5c-4 / PR 4b). */
  uint8_t session_frame[1 + mesh_envelope::MAX_FRAME_LEN];
  session_frame[0] = static_cast<uint8_t>(mesh_envelope::MsgType::BEACON_EVENT);
  const size_t env_len = mesh_envelope::serialize_signed(
      header, payload, sizeof(payload),
      s_device_priv, s_device_pub,
      session_frame + 1, sizeof(session_frame) - 1);
  if (env_len == 0) return false;

  /* 4. Broadcast to every paired peer. mesh_transport::broadcast
   * returns the number of peers that accepted; 0 means no peers
   * known yet (legitimate during early boot before pairing). We
   * still consider that a failure for the send_beacon_event return
   * so the caller can choose to retry / queue. */
  const size_t n = mesh_transport::broadcast(session_frame, 1 + env_len);
  return n > 0;
}

}  /* namespace mesh_session */
