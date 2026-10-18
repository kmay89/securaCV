/**
 * @file onboarding_fsm.h
 * @brief The fleet's ONE onboarding state machine — pure, host-tested.
 *
 * Every networked Canary goes through the same five states between the box
 * and the broker, and until this header each portal spelled them with its own
 * timers (a 25 s AP linger on three trees, 120 s on the WAP, 8 s on the PIO
 * canary; a factory-reset hold on one tree at 5 s that wiped the identity key
 * too; a recovery rule under which a unit that had EVER been online never
 * raised its setup network again). docs/FIRMWARE_MAP.md §3 has the table.
 *
 * The states, in the words `GET /api/status` reports (`onboarding`):
 *
 *   unprovisioned  no saved network; the setup AP (and, where the board has a
 *                  radio for it, the Improv BLE door) is up and waiting.
 *   provisioning   a phone on the AP or the door has handed credentials and
 *                  the node has not started testing them yet.
 *   joining        a join is in flight — the first test of handed credentials,
 *                  the boot-time join of a saved network, or a backoff retry.
 *   provisioned    associated with an IP. The setup AP may still be up for the
 *                  hold below; the node IS on the network.
 *   recovery       saved networks exist, none is reachable, and the setup AP
 *                  (+ door) has been re-raised WITHOUT wiping anything; the
 *                  saved networks keep being retried underneath.
 *
 * The three decisions every board used to take on its own, taken here once:
 *
 *   1. AP HOLD AFTER A JOIN. The AP stays up `ap_hold_after_join_ms` (2 min)
 *      after the join succeeds so the phone can confirm — an instant teardown
 *      makes every successful provision look failed on the phone (WAP lesson,
 *      re-learned on the 4.3" display). Once the phone HAS confirmed (its
 *      /status poll or BLE read saw "success"), the AP's job is done and it
 *      closes after `ap_hold_after_ack_ms`, the bench-tuned beat the phone
 *      needs to render its own "done". A node with no phone to confirm
 *      (a quiet background rejoin) pre-acks and gets only the beat.
 *
 *   2. RECOVERY RE-RAISE. A node WITH saved networks that has been offline
 *      `recovery_reraise_ms` (10 min) re-raises its setup AP and door without
 *      wiping — a renamed SSID, a changed password or a moved house is a human
 *      problem a headless node can only hand back to a human. The never-online
 *      path (a wrong password three attempts in) is `wifi_should_open_setup`
 *      in wifi_join_policy.h and still fires first; this closes the gap for
 *      a unit that worked yesterday.
 *
 *   3. FACTORY RESET BY HOLD. `factory_reset_hold_ms` (10 s) on the board's
 *      BOOT/user button. The DEFAULT scope is credentials and settings — the
 *      node returns to the box state ON THE NETWORK and keeps its witness key,
 *      because the key is the device's birth certificate and every hub and
 *      display that pinned it would otherwise see a stranger (the HA / cvtrust /
 *      fleet_peers TOFU pins in docs/TRUST_MODEL.md). Shedding the identity is
 *      a second, longer, opt-in stage (common/io/reset_hold.h `wipe_ms`).
 *
 * The setup AP is named here too: `SecuraCV-<node type>-<4 chars>`, the four
 * characters from the salted, MAC-free pseudonym or the pubkey fingerprint —
 * never the MAC (AGENTS.md Invariant II; the program brief asked for the MAC's
 * last four and this is the one deliberate departure, noted in PARITY.md). The
 * node type is new: a household unboxing a Sense and a glass together used to
 * see two "SecuraCV-XXXX" and guess. A per-session tag is appended only when
 * the AP password could not be made durable (the store-refused path), so a
 * phone never remembers a name whose key changed.
 *
 * Pure: no Arduino, no heap. Wrap-safe signed-delta time math throughout.
 * firmware/tests_host/test_onboarding_fsm.cpp pins every rule; the Swift twin
 * of is_setup_ssid is SetupPortal.isSetupNetwork, the JavaScript twin lives in
 * canary-local/assets/flash-core.js.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "provision_core.h"  // wifi_qr_payload for the serial QR line

namespace canary {
namespace net {
namespace onboard {

// ── The node type words (the `dt` vocabulary mDNS already speaks) ─────────
static constexpr const char* NODE_CANARY   = "canary";
static constexpr const char* NODE_WAP      = "wap";
static constexpr const char* NODE_VISION   = "vision";
static constexpr const char* NODE_SENSE    = "sense";
static constexpr const char* NODE_SENTINEL = "sentinel";
static constexpr const char* NODE_DISPLAY  = "display";

/// The node type word from a board's device-type string: "canary-sense" →
/// "sense". A string without the family prefix is returned as is (so a
/// board may pass a NODE_* word directly).
inline const char* node_type_from_device_type(const char* dt) {
  if (dt == nullptr) return nullptr;
  if (strncmp(dt, "canary-", 7) == 0 && dt[7] != '\0') return dt + 7;
  return dt;
}

static constexpr const char* SSID_PREFIX = "SecuraCV-";
static constexpr size_t SSID_SUFFIX_CHARS = 4;
/// Longest SSID this builder emits: prefix 9 + type 8 + '-' + 4 + "-tt" + NUL.
static constexpr size_t SSID_CAP = 32 + 1;

// ── States ────────────────────────────────────────────────────────────────
enum class State : uint8_t {
  Unprovisioned = 0,
  Provisioning  = 1,
  Joining       = 2,
  Provisioned   = 3,
  Recovery      = 4,
};

inline const char* state_label(State s) {
  switch (s) {
    case State::Unprovisioned: return "unprovisioned";
    case State::Provisioning:  return "provisioning";
    case State::Joining:       return "joining";
    case State::Provisioned:   return "provisioned";
    case State::Recovery:      return "recovery";
  }
  return "unknown";
}

// ── Timing (one set of numbers for the whole fleet) ───────────────────────
struct Timing {
  /// Keep the setup AP up this long after a successful join (the brief's
  /// "two minutes so the phone can confirm").
  uint32_t ap_hold_after_join_ms = 120000;
  /// Once the phone has confirmed, this beat, then teardown (bench-tuned).
  uint32_t ap_hold_after_ack_ms = 1600;
  /// A node with saved networks offline this long re-raises its AP + door.
  uint32_t recovery_reraise_ms = 600000;
  /// BOOT held this long = factory reset (credentials + settings).
  uint32_t factory_reset_hold_ms = 10000;
};

// ── Facts → state (a derivation, so every surface says the same word) ─────
struct Facts {
  bool have_credentials = false;      ///< at least one saved network
  bool ap_up = false;                 ///< the setup AP is raised right now
  bool join_in_flight = false;        ///< a credential test or (re)join is running
  bool sta_online = false;            ///< associated with an address
  bool credentials_submitted = false; ///< a phone handed credentials this AP session
};

inline State derive(const Facts& f) {
  if (f.sta_online) return State::Provisioned;
  if (f.join_in_flight) return State::Joining;
  if (!f.have_credentials) {
    return f.credentials_submitted ? State::Provisioning : State::Unprovisioned;
  }
  // Saved networks exist and none is up: the AP being raised is what makes
  // this recovery rather than a quiet retry between backoff steps.
  return f.ap_up ? State::Recovery : State::Joining;
}

// ── Decision 1: the AP hold after a join ──────────────────────────────────
inline bool ap_teardown_due(uint32_t now_ms, uint32_t joined_at_ms,
                            bool phone_acked, uint32_t acked_at_ms,
                            const Timing& t) {
  if (phone_acked &&
      (int32_t)(now_ms - acked_at_ms) > (int32_t)t.ap_hold_after_ack_ms) {
    return true;
  }
  return (int32_t)(now_ms - joined_at_ms) > (int32_t)t.ap_hold_after_join_ms;
}

// ── Decision 2: recovery re-raise for a node that has saved networks ──────
/**
 * @param lost_since_ms  when the current outage began (the supervisor's clock)
 * @param ever_online    associated at least once this power cycle. A node that
 *                       never has is wifi_should_open_setup's case (fixable
 *                       failures after three attempts); this rule is for the
 *                       node whose network went away under it, and it also
 *                       covers the never-online node failing for an
 *                       UNFIXABLE-looking reason (weak signal, silent DHCP)
 *                       for the full ten minutes — at that point a human is
 *                       the only remaining fix either way.
 */
inline bool recovery_reraise_due(uint32_t now_ms, bool have_credentials,
                                 bool sta_online, bool ap_up, bool ever_online,
                                 uint32_t lost_since_ms, const Timing& t) {
  (void)ever_online;
  if (!have_credentials || sta_online || ap_up) return false;
  return (int32_t)(now_ms - lost_since_ms) >= (int32_t)t.recovery_reraise_ms;
}

// ── The setup AP's name ───────────────────────────────────────────────────
/**
 * @brief `SecuraCV-<type>-<4 of suffix>[-<tag>]`.
 *
 * @param node_type   one of the NODE_* words (lowercase ASCII, 1..8 chars);
 *                    null or empty falls back to the legacy `SecuraCV-XXXX`.
 * @param suffix      the device-unique, MAC-free token; its first four chars
 *                    are used, "CNRY" when it is empty.
 * @param session_tag optional 2-char tag for the store-refused path; null or
 *                    empty appends nothing.
 * @return bytes written (excl. NUL), 0 on a cap too small or bad args.
 */
inline size_t ap_ssid(char* out, size_t cap, const char* node_type,
                      const char* suffix, const char* session_tag) {
  if (out == nullptr || cap == 0) return 0;
  const char* sfx = (suffix && suffix[0]) ? suffix : "CNRY";
  char four[SSID_SUFFIX_CHARS + 1];
  size_t i = 0;
  for (; i < SSID_SUFFIX_CHARS && sfx[i]; i++) four[i] = sfx[i];
  while (i < SSID_SUFFIX_CHARS) four[i++] = 'X';
  four[SSID_SUFFIX_CHARS] = '\0';
  int n;
  const bool typed = node_type && node_type[0] && strlen(node_type) <= 8;
  if (typed && session_tag && session_tag[0]) {
    n = snprintf(out, cap, "%s%s-%s-%.2s", SSID_PREFIX, node_type, four, session_tag);
  } else if (typed) {
    n = snprintf(out, cap, "%s%s-%s", SSID_PREFIX, node_type, four);
  } else if (session_tag && session_tag[0]) {
    n = snprintf(out, cap, "%s%s-%.2s", SSID_PREFIX, four, session_tag);
  } else {
    n = snprintf(out, cap, "%s%s", SSID_PREFIX, four);
  }
  if (n <= 0 || (size_t)n >= cap || (size_t)n > 32) { out[0] = '\0'; return 0; }
  return (size_t)n;
}

namespace detail {
inline bool alnum_run(const char* s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (!ok) return false;
  }
  return true;
}
inline bool lower_run(const char* s, size_t n) {
  for (size_t i = 0; i < n; i++) if (s[i] < 'a' || s[i] > 'z') return false;
  return true;
}
}  // namespace detail

/**
 * @brief Is this SSID one of ours? Accepts every shape ap_ssid() ever emitted:
 * `SecuraCV-XXXX`, `SecuraCV-XXXX-tt`, `SecuraCV-type-XXXX`,
 * `SecuraCV-type-XXXX-tt`. The phone app and the Lab mirror this exactly.
 */
inline bool is_setup_ssid(const char* ssid) {
  if (ssid == nullptr) return false;
  const size_t plen = strlen(SSID_PREFIX);
  if (strncmp(ssid, SSID_PREFIX, plen) != 0) return false;
  const char* rest = ssid + plen;
  // Split on '-' into at most three parts.
  const char* parts[3] = {rest, nullptr, nullptr};
  size_t lens[3] = {0, 0, 0};
  int np = 1;
  for (const char* p = rest; *p; p++) {
    if (*p == '-') {
      if (np == 3) return false;
      parts[np++] = p + 1;
    } else {
      lens[np - 1]++;
    }
  }
  if (np == 1) return lens[0] == 4 && detail::alnum_run(parts[0], 4);
  if (np == 2) {
    // XXXX-tt  or  type-XXXX
    if (lens[0] == 4 && lens[1] == 2) {
      return detail::alnum_run(parts[0], 4) && detail::alnum_run(parts[1], 2);
    }
    return lens[0] >= 1 && lens[0] <= 8 && detail::lower_run(parts[0], lens[0]) &&
           lens[1] == 4 && detail::alnum_run(parts[1], 4);
  }
  // type-XXXX-tt
  return lens[0] >= 1 && lens[0] <= 8 && detail::lower_run(parts[0], lens[0]) &&
         lens[1] == 4 && detail::alnum_run(parts[1], 4) &&
         lens[2] == 2 && detail::alnum_run(parts[2], 2);
}

// ── The serial announcement (the label a flasher reads off the wire) ──────
/**
 * @brief The one line both flashers parse for the setup network and its key:
 * `[SETUP] Setup network up: "<ssid>"  password <key>  (channel <n>)`.
 * canary-local/assets/flash-core.js SETUP_NETWORK_LINE_RES pins the shape.
 */
inline int announce_line(char* out, size_t cap, const char* ssid,
                         const char* pass, unsigned channel) {
  if (out == nullptr || cap == 0) return 0;
  const int n = snprintf(out, cap,
                         "[SETUP] Setup network up: \"%s\"  password %s  (channel %u)",
                         ssid ? ssid : "", pass ? pass : "", channel);
  if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
  return n;
}

/**
 * @brief The same network as a scannable code, for the serial label and any
 * surface that prints a QR: `[SETUP] QR WIFI:T:WPA;S:<ssid>;P:<key>;;` — the
 * phone-universal format provision_core.h already escapes.
 */
inline size_t qr_line(char* out, size_t cap, const char* ssid, const char* pass) {
  if (out == nullptr || cap == 0) return 0;
  static const char kLead[] = "[SETUP] QR ";
  const size_t lead = sizeof(kLead) - 1;
  if (cap <= lead + 1) { out[0] = '\0'; return 0; }
  memcpy(out, kLead, lead);
  const size_t n = wifi_qr_payload(ssid, pass, out + lead, cap - lead);
  if (n == 0) { out[0] = '\0'; return 0; }
  return lead + n;
}

}  // namespace onboard
}  // namespace net
}  // namespace canary
