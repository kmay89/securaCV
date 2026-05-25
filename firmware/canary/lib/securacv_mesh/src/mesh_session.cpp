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

/* Opera-authenticated broadcast state (PR 5c-3). Declared here at file
 * scope alongside the other lifecycle-managed state so deinit() can
 * wipe it in one place. Without that wipe a deinit()/init() cycle
 * leaves has_opera_secret() returning true from the prior run and the
 * next send_beacon_event() would sign frames with a stale opera_id /
 * sender_fp / continuing counter (codex P1 catch on #472 — missed at
 * merge time, addressed in this PR). */
static bool     s_opera_id_set       = false;
static uint8_t  s_opera_id [mesh_crypto::OPERA_ID_LEN];
static uint8_t  s_sender_fp[mesh_crypto::FINGERPRINT_LEN];
static uint64_t s_outbound_counter   = 0;

/* Receive-side state (PR 5c-4). Trusted-peer table — small fixed
 * array indexed by sender_fp at recv time, with a per-peer monotonic
 * last_counter for replay defense. Entries are populated by the
 * integration layer via register_trusted_peer() after pairing
 * succeeds. Wiped on deinit() and on clear_trusted_peers(). */
struct TrustedPeer {
  uint8_t  sender_fp [mesh_crypto::FINGERPRINT_LEN];
  uint8_t  pubkey    [mesh_crypto::PUBKEY_LEN];
  uint64_t last_counter;
  bool     in_use;
};
static TrustedPeer s_trusted_peers[MAX_TRUSTED_PEERS];

static beacon_event_received_fn s_beacon_event_cb = nullptr;
static channel_lock_received_fn s_channel_lock_cb = nullptr;
static hub_election_received_fn s_hub_election_cb = nullptr;

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

/* PR 5c-4 helper: look up a trusted peer by sender_fp. Returns nullptr
 * if no match. O(N) with N=MAX_TRUSTED_PEERS=8 — sub-microsecond. */
static TrustedPeer* find_trusted_peer(
    const uint8_t sender_fp[mesh_crypto::FINGERPRINT_LEN]) {
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (!s_trusted_peers[i].in_use) continue;
    if (mesh_crypto::ct_equal(s_trusted_peers[i].sender_fp, sender_fp,
                              mesh_crypto::FINGERPRINT_LEN)) {
      return &s_trusted_peers[i];
    }
  }
  return nullptr;
}

/* Dispatch a verified opera-authenticated frame by envelope msg_type.
 * Called from on_opera_frame after parse_and_verify + counter check
 * have both passed. */
static void dispatch_verified(const TrustedPeer&         peer,
                              const mesh_envelope::Header& hdr,
                              const uint8_t*             payload,
                              size_t                     payload_len) {
  switch (static_cast<mesh_envelope::MsgType>(hdr.msg_type)) {
    case mesh_envelope::MsgType::BEACON_EVENT: {
      if (s_beacon_event_cb == nullptr) return;
      mesh_beacon::BeaconState state;
      char                     label[mesh_beacon::MAX_LABEL_BYTES + 1];
      if (!mesh_beacon::decode(payload, payload_len,
                               &state, label, sizeof(label))) {
        return;   /* malformed payload — drop silently */
      }
      s_beacon_event_cb(peer.sender_fp, state, label);
      break;
    }
    case mesh_envelope::MsgType::CHANNEL_LOCK: {
      if (s_channel_lock_cb == nullptr) return;
      uint8_t                    channel;
      mesh_channel_hop::Reason   reason;
      if (!mesh_channel_hop::decode(payload, payload_len,
                                    &channel, &reason)) {
        return;
      }
      s_channel_lock_cb(peer.sender_fp, channel, reason);
      break;
    }
    case mesh_envelope::MsgType::HUB_ELECTION: {
      if (s_hub_election_cb == nullptr) return;
      mesh_hub_election::Event event;
      uint8_t                  elected_fp[mesh_hub_election::FINGERPRINT_LEN];
      if (!mesh_hub_election::decode(payload, payload_len,
                                     &event, elected_fp)) {
        return;
      }
      s_hub_election_cb(peer.sender_fp, event, elected_fp);
      break;
    }
    default:
      break;
  }
}

/* PR 5c-4: handle an opera-authenticated frame (type_byte >= 16). The
 * full signed envelope (38B header + payload + 64B signature) starts
 * at data + 1. We must:
 *   1. Validate frame_len is at least HEADER_LEN + SIG_LEN.
 *   2. Peek the sender_fp from the header without verifying yet.
 *   3. Look up the trusted peer by sender_fp.
 *   4. parse_and_verify with that peer's pubkey.
 *   5. Reject if opera_id doesn't match our own (cross-opera leak).
 *   6. Reject if counter <= peer.last_counter (replay).
 *   7. Update peer.last_counter and dispatch by msg_type.
 *
 * Steps 1-7 ALL drop silently on failure — there's no error feedback
 * to the (possibly malicious) sender. */
static void on_opera_frame(const uint8_t* data, size_t len) {
  /* data[0] is the session msg-type byte; the envelope starts at +1. */
  const uint8_t* env       = data + MSGTYPE_HEADER_LEN;
  const size_t   env_len   = len   - MSGTYPE_HEADER_LEN;
  if (env_len < mesh_envelope::MIN_FRAME_LEN) return;

  /* Step 2: peek sender_fp via the canonical offset constant rather
   * than hand-rolled byte arithmetic — keeps the header layout pinned
   * in mesh_envelope.h. */
  const uint8_t* sender_fp_in_frame = env + mesh_envelope::OFFSET_SENDER_FP;

  TrustedPeer* peer = find_trusted_peer(sender_fp_in_frame);
  if (peer == nullptr) return;            /* unknown sender */

  /* Step 4: parse + signature verify. */
  mesh_envelope::Header  hdr;
  const uint8_t*         payload     = nullptr;
  size_t                 payload_len = 0;
  if (!mesh_envelope::parse_and_verify(env, env_len, peer->pubkey,
                                       &hdr, &payload, &payload_len)) {
    return;                                /* forged or corrupt */
  }

  /* Step 5: cross-opera leak. parse_and_verify already checked version
   * and signature; we additionally check the opera_id matches ours so
   * a different opera that happened to pair with this same sender
   * pubkey can't deliver events into our world. */
  if (!s_opera_id_set) return;
  if (!mesh_crypto::ct_equal(hdr.opera_id, s_opera_id,
                             mesh_crypto::OPERA_ID_LEN)) {
    return;
  }

  /* Step 6: replay defense — strict monotonic counter per-peer. The
   * sender's outbound counter increments per send (PR 5c-3); the
   * receiver tracks last_counter per peer. counter==last_counter is
   * a replay; only counter>last_counter advances. */
  if (hdr.counter <= peer->last_counter) return;
  peer->last_counter = hdr.counter;

  /* Step 7: dispatch by envelope msg_type. */
  dispatch_verified(*peer, hdr, payload, payload_len);
}

/* mesh_transport recv callback. Decodes the 1-byte MsgType envelope
 * and routes to either the pairing state machine (type_byte <= 4) or
 * the opera-authenticated dispatch (type_byte >= 16). */
static void on_transport_recv(const uint8_t mac[6],
                              const uint8_t* data, size_t len,
                              int8_t /*rssi*/) {
  if (!s_running || data == nullptr || len < MSGTYPE_HEADER_LEN) return;
  const uint8_t type_byte = data[0];

  /* PAIR_* (0..4) — pre-membership pairing traffic, no envelope. */
  if (type_byte <= static_cast<uint8_t>(MsgType::PAIR_COMPLETE)) {
    const mesh_pairing::MsgType pair_type =
        static_cast<mesh_pairing::MsgType>(type_byte);
    const uint8_t* payload     = data + MSGTYPE_HEADER_LEN;
    const size_t   payload_len = len  - MSGTYPE_HEADER_LEN;
    /* now_ms isn't readily available in this callback context, but
     * mesh_pairing::receive uses it only for the tamper-path nothing-
     * else, so 0 is acceptable. The tick() path supplies a real now_ms
     * for timeout enforcement. */
    mesh_pairing::Action a = mesh_pairing::receive(s_ctx, mac, pair_type,
                                                    payload, payload_len, 0);
    dispatch_action(a);
    return;
  }

  /* 5..15 — reserved for future pairing extensions. Drop silently. */
  if (type_byte < static_cast<uint8_t>(mesh_envelope::MsgType::HEARTBEAT)) return;

  /* >=16 — opera-authenticated traffic. PR 5c-4 routes it here; the
   * peer table + signature verify + replay check happen inside. */
  on_opera_frame(data, len);
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
  /* PR 5c-3 follow-up: clear opera-auth state so a deinit()/init()
   * cycle starts clean — without this, has_opera_secret() would lie
   * about a stale opera_id from the prior run. The opera_id and
   * sender_fp aren't secret (they're advertised in every signed
   * broadcast) but the staleness alone causes incorrect identity
   * binding on the next send. */
  s_opera_id_set     = false;
  secure_zero(s_opera_id,  sizeof(s_opera_id));
  secure_zero(s_sender_fp, sizeof(s_sender_fp));
  s_outbound_counter = 0;
  /* PR 5c-4: wipe the trusted-peer table + handler so a deinit()/init()
   * cycle doesn't carry stale peers or replay counters into the next
   * session. The pubkeys aren't secret but the staleness alone would
   * let an attacker that scraped a paired peer's pubkey replay any
   * recorded frame whose counter is <= the cached last_counter — a
   * real (if narrow) freshness violation. */
  memset(s_trusted_peers, 0, sizeof(s_trusted_peers));
  s_beacon_event_cb = nullptr;
  s_channel_lock_cb = nullptr;
  s_hub_election_cb = nullptr;
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

bool get_paired_peer_pubkey(uint8_t out[mesh_crypto::PUBKEY_LEN]) {
  if (out == nullptr) return false;
  /* peer_pubkey is captured at OFFER (initiator side) / ACCEPT
   * (joiner side); both happen well before PAIRED. We gate on
   * AWAITING_CONFIRM-or-later so the accessor doesn't expose a
   * stale buffer from a prior pairing attempt before the new one
   * has populated it. */
  switch (s_ctx.state) {
    case mesh_pairing::State::AWAITING_CONFIRM:
    case mesh_pairing::State::AWAITING_CONFIRM_PEER:
    case mesh_pairing::State::PAIRED:
      memcpy(out, s_ctx.peer_pubkey, mesh_crypto::PUBKEY_LEN);
      return true;
    default:
      return false;
  }
}

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
 * The supporting state lives at the top of this TU alongside the
 * other lifecycle-managed state so deinit() can wipe it in one place.
 *
 *   s_opera_id_set   — false until set_opera_secret() succeeds.
 *   s_opera_id       — 16 bytes; sha256_domain(DOMAIN_OPERA_ID, secret)
 *                      truncated. Cached so we don't re-hash on every send.
 *   s_sender_fp      — 8 bytes; sha256_domain(DOMAIN_FINGERPRINT,
 *                      s_device_pub) truncated. Cached at first
 *                      set_opera_secret() since device_pub doesn't
 *                      change post-init().
 *   s_outbound_counter — monotonic per-process. PR 5c-3 keeps it in
 *                      RAM only; PR 5c-4 will persist to NVS so a
 *                      reboot doesn't replay-reset counters at peers.
 * ────────────────────────────────────────────────────────────────────────── */


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

/* ──────────────────────────────────────────────────────────────────────────
 * PR 5c-4 — TRUSTED PEER TABLE + BEACON_EVENT RECEIVER
 * ────────────────────────────────────────────────────────────────────────── */

bool register_trusted_peer(const uint8_t pubkey[mesh_crypto::PUBKEY_LEN]) {
  if (pubkey == nullptr) return false;

  /* Compute fingerprint once so we can both dedup and use it as the
   * lookup key. */
  uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(pubkey, fp);

  /* Dedup: refuse re-registration of the same pubkey. Otherwise a
   * naive re-register call would zero last_counter and re-open the
   * replay window between (old last_counter, 0]. Callers that NEED
   * to rotate a peer's pubkey should clear_trusted_peers() first
   * and re-add the entire set. */
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (s_trusted_peers[i].in_use &&
        mesh_crypto::ct_equal(s_trusted_peers[i].sender_fp, fp,
                              mesh_crypto::FINGERPRINT_LEN)) {
      return false;
    }
  }

  /* Find a free slot. */
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (!s_trusted_peers[i].in_use) {
      memcpy(s_trusted_peers[i].pubkey,    pubkey, mesh_crypto::PUBKEY_LEN);
      memcpy(s_trusted_peers[i].sender_fp, fp,     sizeof(fp));
      s_trusted_peers[i].last_counter = 0;
      s_trusted_peers[i].in_use       = true;
      return true;
    }
  }
  return false;   /* table full */
}

void clear_trusted_peers() {
  memset(s_trusted_peers, 0, sizeof(s_trusted_peers));
}

size_t trusted_peer_count() {
  size_t n = 0;
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (s_trusted_peers[i].in_use) ++n;
  }
  return n;
}

void set_beacon_event_handler(beacon_event_received_fn fn) {
  s_beacon_event_cb = fn;
}

bool send_channel_lock(uint8_t channel,
                       mesh_channel_hop::Reason reason,
                       uint32_t now_ms) {
  if (!s_initialized || !s_running) return false;
  if (!s_opera_id_set)             return false;

  uint8_t payload[mesh_channel_hop::PAYLOAD_LEN];
  if (!mesh_channel_hop::encode(channel, reason, payload, sizeof(payload))) {
    return false;
  }

  mesh_envelope::Header header;
  header.version   = mesh_envelope::PROTOCOL_VERSION;
  header.msg_type  = static_cast<uint8_t>(mesh_envelope::MsgType::CHANNEL_LOCK);
  memcpy(header.opera_id,  s_opera_id,  sizeof(header.opera_id));
  memcpy(header.sender_fp, s_sender_fp, sizeof(header.sender_fp));
  header.counter   = ++s_outbound_counter;
  header.timestamp = now_ms;

  uint8_t session_frame[1 + mesh_envelope::MAX_FRAME_LEN];
  session_frame[0] = static_cast<uint8_t>(mesh_envelope::MsgType::CHANNEL_LOCK);
  const size_t env_len = mesh_envelope::serialize_signed(
      header, payload, sizeof(payload),
      s_device_priv, s_device_pub,
      session_frame + 1, sizeof(session_frame) - 1);
  if (env_len == 0) return false;

  const size_t n = mesh_transport::broadcast(session_frame, 1 + env_len);
  return n > 0;
}

void set_channel_lock_handler(channel_lock_received_fn fn) {
  s_channel_lock_cb = fn;
}

bool send_hub_election(mesh_hub_election::Event event,
                       const uint8_t fingerprint[mesh_crypto::FINGERPRINT_LEN],
                       uint32_t now_ms) {
  if (!s_initialized || !s_running) return false;
  if (!s_opera_id_set)             return false;

  uint8_t payload[mesh_hub_election::PAYLOAD_LEN];
  if (!mesh_hub_election::encode(event, fingerprint, payload, sizeof(payload))) {
    return false;
  }

  mesh_envelope::Header header;
  header.version   = mesh_envelope::PROTOCOL_VERSION;
  header.msg_type  = static_cast<uint8_t>(mesh_envelope::MsgType::HUB_ELECTION);
  memcpy(header.opera_id,  s_opera_id,  sizeof(header.opera_id));
  memcpy(header.sender_fp, s_sender_fp, sizeof(header.sender_fp));
  header.counter   = ++s_outbound_counter;
  header.timestamp = now_ms;

  uint8_t session_frame[1 + mesh_envelope::MAX_FRAME_LEN];
  session_frame[0] = static_cast<uint8_t>(mesh_envelope::MsgType::HUB_ELECTION);
  const size_t env_len = mesh_envelope::serialize_signed(
      header, payload, sizeof(payload),
      s_device_priv, s_device_pub,
      session_frame + 1, sizeof(session_frame) - 1);
  if (env_len == 0) return false;

  const size_t n = mesh_transport::broadcast(session_frame, 1 + env_len);
  return n > 0;
}

void set_hub_election_handler(hub_election_received_fn fn) {
  s_hub_election_cb = fn;
}

}  /* namespace mesh_session */
