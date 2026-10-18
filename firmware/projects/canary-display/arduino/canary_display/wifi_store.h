/**
 * @file wifi_store.h
 * @brief Saved Wi-Fi networks — a small ring every Canary keeps, pure and
 *        host-tested.
 *
 * Every firmware in the tree saved exactly one network (`wifi_ssid` /
 * `wifi_pass`), so a node carried between a house and a workshop, or a
 * household with a guest network and a main one, had to be re-provisioned
 * each time. This header is the one store: up to MAX_NETWORKS saved networks,
 * which one worked last, how often each has failed, and a pick rule that
 * prefers what the radio can actually see.
 *
 * It is DATA LOGIC ONLY. The board owns the bytes (an NVS blob under
 * `wifi_nets` in the `securacv` namespace) and the radio; this header owns
 * add / remove / pick / serialize, so the rules are identical on every tree
 * and `firmware/tests_host/test_wifi_store.cpp` pins them.
 *
 * Migration: the legacy pair stays in NVS as the CURRENT pick — the flashers
 * seed it at install time and every reader of `wifi_ssid` keeps working — and
 * `import_legacy()` folds it into the ring on first load. The blob is
 * versioned, magic-tagged and CRC-checked so a torn write parses as "no
 * store" rather than as a network nobody typed.
 *
 * Credentials at rest: this header does not encrypt. Where the bytes sleep
 * (plaintext NVS on every shipped build today) is reported honestly by
 * `creds_at_rest` in GET /api/status (common/identity/key_at_rest.h labels)
 * and the secure kit in firmware/provisioning/ is the path up —
 * docs/PARITY.md says so per target rather than pretending.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "wifi_credential_policy.h"

namespace canary {
namespace net {
namespace wifi_store {

static constexpr uint8_t MAX_NETWORKS = 4;
static constexpr uint8_t VERSION = 1;
static constexpr uint8_t FAILS_MAX = 250;
/// NVS key for the serialized ring (namespace "securacv").
static constexpr const char* NVS_KEY = "wifi_nets";

struct Network {
  char ssid[wifi_credential::SSID_MAX_BYTES + 1];
  char pass[wifi_credential::PASSWORD_MAX_BYTES + 1];
  uint8_t fails;   ///< consecutive failed joins, saturating
  uint8_t flags;   ///< reserved (0)
};

struct Store {
  uint8_t count = 0;
  uint8_t last_good = 0;   ///< index of the network that most recently worked
  Network nets[MAX_NETWORKS] = {};
};

static constexpr size_t ENTRY_BYTES = sizeof(Network::ssid) + sizeof(Network::pass) + 2;
/// magic(4) version(1) count(1) last_good(1) reserved(1) entries crc(4)
static constexpr size_t BLOB_BYTES = 8 + MAX_NETWORKS * ENTRY_BYTES + 4;

inline void init(Store& s) {
  s.count = 0;
  s.last_good = 0;
  memset(s.nets, 0, sizeof(s.nets));
}

inline int find(const Store& s, const char* ssid) {
  if (ssid == nullptr) return -1;
  for (uint8_t i = 0; i < s.count && i < MAX_NETWORKS; i++) {
    if (strcmp(s.nets[i].ssid, ssid) == 0) return (int)i;
  }
  return -1;
}

namespace detail {
inline void copy_field(char* dst, size_t cap, const char* src) {
  size_t n = 0;
  if (src) { while (src[n] && n + 1 < cap) { dst[n] = src[n]; n++; } }
  dst[n] = '\0';
}
inline uint32_t crc32(const uint8_t* p, size_t n) {
  // Reflected CRC-32 (IEEE), table-less: a few hundred bytes once per save.
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}
}  // namespace detail

/**
 * @brief Save a network that just WORKED (callers persist only on success,
 * the portal rule). An existing SSID has its password replaced and its
 * failure count cleared; a new one takes a free slot or evicts the most
 * failing network that is not the last good one. Either way it becomes
 * `last_good`. False (and no change) when the credentials are out of bounds.
 */
inline bool add(Store& s, const char* ssid, const char* pass) {
  if (wifi_credential::check(ssid, pass) != wifi_credential::Verdict::Ok) return false;
  int idx = find(s, ssid);
  if (idx < 0) {
    if (s.count < MAX_NETWORKS) {
      idx = s.count++;
    } else {
      // Evict: the most-failing entry that is not last_good; ties → oldest slot.
      int victim = -1;
      for (uint8_t i = 0; i < MAX_NETWORKS; i++) {
        if (i == s.last_good) continue;
        if (victim < 0 || s.nets[i].fails > s.nets[victim].fails) victim = (int)i;
      }
      idx = victim < 0 ? 0 : victim;
    }
    detail::copy_field(s.nets[idx].ssid, sizeof(s.nets[idx].ssid), ssid);
  }
  detail::copy_field(s.nets[idx].pass, sizeof(s.nets[idx].pass), pass ? pass : "");
  s.nets[idx].fails = 0;
  s.nets[idx].flags = 0;
  s.last_good = (uint8_t)idx;
  return true;
}

/** @brief Forget one network. Slots above it shift down; last_good follows. */
inline bool remove(Store& s, const char* ssid) {
  const int idx = find(s, ssid);
  if (idx < 0) return false;
  for (int i = idx; i + 1 < (int)s.count; i++) s.nets[i] = s.nets[i + 1];
  s.count--;
  memset(&s.nets[s.count], 0, sizeof(Network));
  if (s.count == 0) {
    s.last_good = 0;
  } else if (s.last_good == (uint8_t)idx) {
    s.last_good = 0;
  } else if (s.last_good > (uint8_t)idx) {
    s.last_good--;
  }
  return true;
}

/** @brief Record a join verdict for the network at `idx`. */
inline void note_result(Store& s, int idx, bool ok) {
  if (idx < 0 || idx >= (int)s.count) return;
  if (ok) {
    s.nets[idx].fails = 0;
    s.last_good = (uint8_t)idx;
  } else if (s.nets[idx].fails < FAILS_MAX) {
    s.nets[idx].fails++;
  }
}

/**
 * @brief Which saved network to try next.
 *
 * With a scan in hand: the strongest VISIBLE saved network (ties → last_good,
 * then the lower slot). Without a scan (or none visible): last_good when
 * `previous` is -1 (a fresh boot, a fresh outage), otherwise the slot after
 * `previous`, round-robin — so a NotFound on the last good network moves on
 * to the next saved one instead of probing an absent SSID forever.
 *
 * @param visible  SSIDs the radio saw (may be null with n_visible 0)
 * @param rssi     parallel to `visible` (dBm; higher is stronger)
 * @return slot index, or -1 when the store is empty.
 */
inline int pick(const Store& s, const char* const* visible, const int32_t* rssi,
                size_t n_visible, int previous) {
  if (s.count == 0) return -1;
  int best = -1;
  int32_t best_rssi = 0;
  if (visible && rssi) {
    for (size_t v = 0; v < n_visible; v++) {
      const int idx = find(s, visible[v]);
      if (idx < 0) continue;
      const bool better = best < 0 || rssi[v] > best_rssi ||
                          (rssi[v] == best_rssi && idx == (int)s.last_good);
      if (better) { best = idx; best_rssi = rssi[v]; }
    }
  }
  if (best >= 0) return best;
  if (previous < 0 || previous >= (int)s.count) return (int)s.last_good;
  return (previous + 1) % (int)s.count;
}

/** @brief Fold the legacy single pair into the ring (first load). */
inline bool import_legacy(Store& s, const char* ssid, const char* pass) {
  if (ssid == nullptr || ssid[0] == '\0') return false;
  if (find(s, ssid) >= 0) return false;
  return add(s, ssid, pass);
}

/** @brief Serialize to exactly BLOB_BYTES. Returns 0 if `cap` is too small. */
inline size_t serialize(const Store& s, uint8_t* out, size_t cap) {
  if (out == nullptr || cap < BLOB_BYTES) return 0;
  memset(out, 0, BLOB_BYTES);
  out[0] = 'S'; out[1] = 'C'; out[2] = 'V'; out[3] = 'W';
  out[4] = VERSION;
  out[5] = s.count <= MAX_NETWORKS ? s.count : MAX_NETWORKS;
  out[6] = s.last_good < MAX_NETWORKS ? s.last_good : 0;
  out[7] = 0;
  size_t o = 8;
  for (uint8_t i = 0; i < MAX_NETWORKS; i++) {
    memcpy(out + o, s.nets[i].ssid, sizeof(Network::ssid)); o += sizeof(Network::ssid);
    memcpy(out + o, s.nets[i].pass, sizeof(Network::pass)); o += sizeof(Network::pass);
    out[o++] = s.nets[i].fails;
    out[o++] = s.nets[i].flags;
  }
  const uint32_t crc = detail::crc32(out, o);
  out[o++] = (uint8_t)(crc & 0xff);
  out[o++] = (uint8_t)((crc >> 8) & 0xff);
  out[o++] = (uint8_t)((crc >> 16) & 0xff);
  out[o++] = (uint8_t)((crc >> 24) & 0xff);
  return o;
}

/**
 * @brief Parse a blob. False (and `out` cleared) on a wrong magic, an unknown
 * version, a short buffer, a bad CRC, or an entry whose strings are not
 * NUL-terminated within their fields — a torn write is "no store".
 */
inline bool parse(const uint8_t* in, size_t len, Store& out) {
  init(out);
  if (in == nullptr || len < BLOB_BYTES) return false;
  if (in[0] != 'S' || in[1] != 'C' || in[2] != 'V' || in[3] != 'W') return false;
  if (in[4] != VERSION) return false;
  const size_t body = BLOB_BYTES - 4;
  const uint32_t want = (uint32_t)in[body] | ((uint32_t)in[body + 1] << 8) |
                        ((uint32_t)in[body + 2] << 16) | ((uint32_t)in[body + 3] << 24);
  if (detail::crc32(in, body) != want) return false;
  const uint8_t count = in[5];
  if (count > MAX_NETWORKS) return false;
  size_t o = 8;
  for (uint8_t i = 0; i < MAX_NETWORKS; i++) {
    Network& n = out.nets[i];
    memcpy(n.ssid, in + o, sizeof(n.ssid)); o += sizeof(n.ssid);
    memcpy(n.pass, in + o, sizeof(n.pass)); o += sizeof(n.pass);
    n.fails = in[o++];
    n.flags = in[o++];
    if (n.ssid[sizeof(n.ssid) - 1] != '\0' || n.pass[sizeof(n.pass) - 1] != '\0') {
      init(out);
      return false;
    }
    if (i < count && n.ssid[0] == '\0') { init(out); return false; }
  }
  out.count = count;
  out.last_good = (count > 0 && in[6] < count) ? in[6] : 0;
  return true;
}

}  // namespace wifi_store
}  // namespace net
}  // namespace canary
