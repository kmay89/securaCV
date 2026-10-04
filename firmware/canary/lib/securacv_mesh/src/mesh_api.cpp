/*
 * SecuraCV Canary — Mesh REST API JSON builders (PR-8) — Implementation
 *
 * Pure snprintf-based JSON rendering; no Arduino / ArduinoJson deps so
 * the mesh host-test harness can link + exercise these on every PR (CI's
 * [env:full] leg compiles the handlers; only these tests see the output
 * shape — see mesh_api.h).
 */

#include "mesh_api.h"

#include <stdio.h>
#include <string.h>

namespace mesh_api {

/* Lowercase-hex encode `n` bytes of `in` into `out` (needs 2n+1 bytes).
 * Mirrors securacv_network.cpp's hex_to_str output (lowercase) so the
 * opera_id / fingerprint strings match the rest of the firmware. */
static void to_hex(char* out, const uint8_t* in, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i]     = kHex[(in[i] >> 4) & 0xF];
    out[2 * i + 1] = kHex[in[i] & 0xF];
  }
  out[2 * n] = '\0';
}

/* Append a JSON-escaped copy of `s` (between quotes already written by
 * the caller) into out[*pos..cap). Returns false on overflow. Escapes
 * the JSON-mandatory set (", \, control chars); other bytes pass through.
 * Defends GET /api/mesh against a malicious opera_name / peer name
 * breaking the JSON envelope. */
static bool append_escaped(char* out, size_t cap, size_t* pos, const char* s) {
  if (s == nullptr) return true;
  for (size_t i = 0; s[i] != '\0'; ++i) {
    const unsigned char c = (unsigned char)s[i];
    char esc[8];
    const char* frag;
    size_t      frag_len;
    switch (c) {
      case '"':  frag = "\\\""; frag_len = 2; break;
      case '\\': frag = "\\\\"; frag_len = 2; break;
      case '\n': frag = "\\n";  frag_len = 2; break;
      case '\r': frag = "\\r";  frag_len = 2; break;
      case '\t': frag = "\\t";  frag_len = 2; break;
      default:
        if (c < 0x20) {
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          frag = esc; frag_len = 6;
        } else {
          esc[0] = (char)c; esc[1] = '\0';
          frag = esc; frag_len = 1;
        }
        break;
    }
    if (*pos + frag_len >= cap) return false;
    memcpy(out + *pos, frag, frag_len);
    *pos += frag_len;
  }
  return true;
}

bool build_mesh_status_json(char*  out,
                            size_t cap,
                            bool   enabled,
                            bool   has_opera,
                            const uint8_t opera_id[mesh_crypto::OPERA_ID_LEN],
                            const char*  opera_name,
                            mesh_pairing::State pairing_state,
                            size_t   peers_total,
                            size_t   peers_online,
                            uint32_t alerts_received,
                            uint32_t pairing_code,
                            const PairingReport* last_pairing) {
  if (out == nullptr || cap == 0) return false;

  const char* state = mesh_pairing::mesh_state_name(
      enabled, has_opera, pairing_state, peers_online);

  char opera_id_hex[mesh_crypto::OPERA_ID_LEN * 2 + 1] = {0};
  if (has_opera && opera_id != nullptr) {
    to_hex(opera_id_hex, opera_id, mesh_crypto::OPERA_ID_LEN);
  }

  size_t pos = 0;
  int n = snprintf(out + pos, cap - pos,
                   "{\"ok\":true,\"state\":\"%s\",\"opera_id\":\"%s\",\"opera_name\":\"",
                   state, opera_id_hex);
  if (n < 0 || (size_t)n >= cap - pos) return false;
  pos += (size_t)n;

  if (!append_escaped(out, cap, &pos, opera_name)) return false;

  n = snprintf(out + pos, cap - pos,
               "\",\"has_opera\":%s,\"enabled\":%s,"
               "\"peers_total\":%u,\"peers_online\":%u,\"alerts_received\":%u",
               has_opera ? "true" : "false",
               enabled   ? "true" : "false",
               (unsigned)peers_total, (unsigned)peers_online,
               (unsigned)alerts_received);
  if (n < 0 || (size_t)n >= cap - pos) return false;
  pos += (size_t)n;

  /* pairing_code is exposed ONLY in PAIRING_CONFIRM — no early leak of
   * the 6-digit code in any other state. The mapping is centralized in
   * mesh_state_name(); we compare its result rather than re-deriving the
   * pairing-state classification here. */
  if (strcmp(state, "PAIRING_CONFIRM") == 0) {
    n = snprintf(out + pos, cap - pos, ",\"pairing_code\":%u",
                 (unsigned)pairing_code);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;
  }

  /* F133: the last pairing's outcome, after every older field. The fail
   * reason is "none" unless the outcome is FAILED. */
  if (last_pairing != nullptr) {
    const bool failed = last_pairing->outcome == mesh_pairing::Outcome::FAILED;
    n = snprintf(out + pos, cap - pos,
                 ",\"pairing_seq\":%lu,\"pairing_result\":\"%s\",\"pairing_fail_reason\":\"%s\"",
                 (unsigned long)last_pairing->seq,
                 mesh_pairing::outcome_name(last_pairing->outcome),
                 mesh_pairing::fail_reason_name(failed ? last_pairing->fail_reason
                                                       : mesh_pairing::FailReason::NONE));
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;
  }

  if (pos + 1 >= cap) return false;
  out[pos++] = '}';
  out[pos]   = '\0';
  return true;
}

bool build_mesh_peers_json(char*  out,
                           size_t cap,
                           const PeerView* peers,
                           size_t          count) {
  if (out == nullptr || cap == 0) return false;
  if (count > 0 && peers == nullptr) return false;

  size_t pos = 0;
  int n = snprintf(out + pos, cap - pos, "{\"ok\":true,\"peers\":[");
  if (n < 0 || (size_t)n >= cap - pos) return false;
  pos += (size_t)n;

  for (size_t i = 0; i < count; ++i) {
    const PeerView& p = peers[i];
    n = snprintf(out + pos, cap - pos,
                 "%s{\"fingerprint\":\"%s\",\"name\":\"",
                 i == 0 ? "" : ",", p.fingerprint);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    if (!append_escaped(out, cap, &pos, p.name)) return false;

    n = snprintf(out + pos, cap - pos,
                 "\",\"state\":\"%s\",\"last_seen_sec\":%u,\"rssi\":%d,"
                 "\"alerts_received\":%u}",
                 p.state ? p.state : "OFFLINE",
                 (unsigned)p.last_seen_sec, p.rssi,
                 (unsigned)p.alerts_received);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;
  }

  if (pos + 2 >= cap) return false;
  out[pos++] = ']';
  out[pos++] = '}';
  out[pos]   = '\0';
  return true;
}

bool build_mesh_status_json_from_view(char* out, size_t cap,
                                      const mesh_session::StatusView& view) {
  PairingReport last;
  last.seq         = view.pairing_seq;
  last.outcome     = view.pairing_outcome;
  last.fail_reason = view.pairing_fail_reason;
  /* A published name is terminated by construction; terminate a copy anyway
   * rather than trust a struct's last byte. */
  char name[sizeof(view.opera_name)];
  memcpy(name, view.opera_name, sizeof(name));
  name[sizeof(name) - 1] = '\0';
  return build_mesh_status_json(out, cap, view.enabled, view.has_opera,
                                view.has_opera ? view.opera_id : nullptr, name,
                                view.pairing_state, view.peers_total, view.peers_online,
                                view.alerts_received, view.pairing_code, &last);
}

size_t peer_views_from_status(const mesh_session::StatusView& view,
                              const uint8_t* pubkeys, size_t count,
                              uint32_t now_ms, PeerView* out) {
  if (out == nullptr || (pubkeys == nullptr && count > 0)) return 0;
  const size_t members = view.member_count < mesh_session::MAX_TRUSTED_PEERS
                             ? (size_t)view.member_count
                             : mesh_session::MAX_TRUSTED_PEERS;
  for (size_t i = 0; i < count; ++i) {
    uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
    mesh_crypto::compute_fingerprint(pubkeys + i * mesh_crypto::PUBKEY_LEN, fp);
    PeerView& row = out[i];
    to_hex(row.fingerprint, fp, mesh_crypto::FINGERPRINT_LEN);
    row.name[0]         = '\0';          /* best-effort: name unknown */
    row.state           = "OFFLINE";     /* until a verified frame joins it */
    row.last_seen_sec   = 0xFFFFFFFFu;   /* "never" (the UI shows 'never') */
    row.rssi            = 0;
    row.alerts_received = 0;             /* until the view has a row for it */
    for (size_t m = 0; m < members; ++m) {
      const mesh_session::MemberView& mv = view.members[m];
      if (memcmp(mv.fp, fp, mesh_crypto::FINGERPRINT_LEN) != 0) continue;
      /* Per-peer alert attribution (F11) does not depend on liveness. */
      row.alerts_received = mv.alerts_received;
      if (mv.live) {
        switch (mv.link_state) {
          case mesh_transport::PeerState::ACTIVE: row.state = "CONNECTED"; break;
          case mesh_transport::PeerState::STALE:  row.state = "STALE";     break;
          default:                                row.state = "OFFLINE";   break;
        }
        row.last_seen_sec = (now_ms - mv.last_seen_ms) / 1000u;
        row.rssi          = mv.rssi_dbm;
      }
      break;
    }
  }
  return count;
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parse_fingerprint_hex(const char* hex,
                           uint8_t     out[mesh_crypto::FINGERPRINT_LEN]) {
  if (hex == nullptr || out == nullptr) return false;
  constexpr size_t N = mesh_crypto::FINGERPRINT_LEN;
  if (strnlen(hex, 2 * N + 1) != 2 * N) return false;
  uint8_t tmp[N];
  for (size_t i = 0; i < N; ++i) {
    const int hi = hex_nibble(hex[2 * i]);
    const int lo = hex_nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    tmp[i] = (uint8_t)((hi << 4) | lo);
  }
  memcpy(out, tmp, N);
  return true;
}

bool build_mesh_alerts_json(char*                     out,
                            size_t                    cap,
                            const mesh_alert::Record* alerts,
                            size_t                    count,
                            uint32_t                  now_ms) {
  if (out == nullptr || cap == 0) return false;
  if (count > 0 && alerts == nullptr) return false;

  size_t pos = 0;
  /* uptime_ms: what each timestamp_ms is measured against (F33 part 7). */
  int n = snprintf(out + pos, cap - pos,
                   "{\"ok\":true,\"count\":%u,\"uptime_ms\":%lu,\"alerts\":[",
                   (unsigned)count, (unsigned long)now_ms);
  if (n < 0 || (size_t)n >= cap - pos) return false;
  pos += (size_t)n;

  for (size_t i = 0; i < count; ++i) {
    const mesh_alert::Record& a = alerts[i];
    char fp_hex[sizeof(a.sender_fp) * 2 + 1];
    to_hex(fp_hex, a.sender_fp, sizeof(a.sender_fp));
    /* Every string here is a fixed template or hex — nothing
     * sender-authored — so no escaping pass is needed. */
    n = snprintf(out + pos, cap - pos,
                 "%s{\"timestamp_ms\":%lu,\"type\":\"%s\",\"severity\":%u,"
                 "\"sender_fp\":\"%s\",\"sender_name\":\"\",\"detail\":\"%s\","
                 "\"witness_seq\":%lu}",
                 i == 0 ? "" : ",",
                 (unsigned long)a.timestamp_ms, mesh_alert::type_name(),
                 (unsigned)a.severity, fp_hex, mesh_alert::kind_name(a.kind),
                 (unsigned long)a.witness_seq);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;
  }

  if (pos + 2 >= cap) return false;
  out[pos++] = ']';
  out[pos++] = '}';
  out[pos]   = '\0';
  return true;
}

}  /* namespace mesh_api */
