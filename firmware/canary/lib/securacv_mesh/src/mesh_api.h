/*
 * SecuraCV Canary — Mesh REST API JSON builders (PR-8)
 * Version 0.1.0
 *
 * Pure, I/O-free helpers that render the JSON bodies for the GET mesh
 * endpoints:
 *
 *   GET /api/mesh         → build_mesh_status_json()
 *   GET /api/mesh/peers   → build_mesh_peers_json()
 *   GET /api/mesh/alerts  → build_mesh_alerts_json()   (F10)
 *
 * Why a separate, pure module:
 *   The HTTP handlers in securacv_network.cpp live behind
 *   FEATURE_MESH_NETWORK. CI does compile them — [env:full]
 *   (platformio.ini, -DFEATURE_MESH_NETWORK=1) is one of the canary's
 *   build_envs in firmware/flavors.json, so the "PlatformIO Build" job
 *   builds the handler bodies on every PR — but a compile proves only
 *   that they build, not what they emit. Extracting the JSON-building
 *   logic here — taking plain structs/params, writing to a
 *   caller-supplied char buffer, no httpd_req_t, no ArduinoJson — lets
 *   the mesh host-test harness (which links every securacv_mesh src TU)
 *   exercise the real response shape on every PR.
 *
 * JSON is emitted by hand via snprintf so the module needs neither
 * ArduinoJson (device-only) nor Arduino String (device-only). The field
 * set is pinned by host tests against the strings the active web UI
 * (securacv_webui.cpp refreshOpera / loadPeers) actually reads.
 *
 * All builders return true on success, false if the output buffer was
 * too small (the buffer contents are then unspecified — callers should
 * treat false as a 500-class internal error). Strings that originate
 * from untrusted/device input (opera_name, peer name) are JSON-escaped.
 */

#ifndef SECURACV_MESH_API_H
#define SECURACV_MESH_API_H

#include "mesh_crypto.h"    /* OPERA_ID_LEN, FINGERPRINT_LEN */
#include "mesh_pairing.h"   /* mesh_pairing::State, mesh_state_name */
#include "mesh_alert.h"     /* mesh_alert::Record (F10) */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_api {

/* Response-buffer sizes the REST handlers allocate. Pinned by host tests
 * against the worst-case body (test_mesh_session.cpp
 * test_rest_buffers_fit_worst_case) so a new field cannot silently push a
 * full table past its buffer and turn the endpoint into a 500:
 *   PEERS_JSON_CAP  — 8 trusted peers (mesh_state::MAX_TRUSTED_PEERS),
 *                     empty names (the handler has no name source), every
 *                     number at its widest.
 *   ALERTS_JSON_CAP — MAX_ALERTS_JSON rows (mesh_session::MAX_ALERT_HISTORY)
 *                     at their widest. */
constexpr size_t PEERS_JSON_CAP   = 1536;
constexpr size_t MAX_ALERTS_JSON  = 16;
constexpr size_t ALERTS_JSON_CAP  = 3072;

/* ──────────────────────────────────────────────────────────────────────────
 * GET /api/mesh — status
 *
 * Emits exactly the fields refreshOpera() reads:
 *   ok, state, opera_id, opera_name, has_opera, enabled,
 *   peers_total, peers_online, alerts_received
 * plus pairing_code — but ONLY when the resolved state is
 * "PAIRING_CONFIRM" (no early leak of the code in any other state).
 *
 * opera_id is rendered as lowercase hex (OPERA_ID_LEN*2 chars). When
 * has_opera is false, opera_id is emitted as "" and opera_name as
 * whatever was passed (typically "").
 *
 * pairing_code is the 6-digit confirmation code; it is only serialized
 * when pairing_state maps to PAIRING_CONFIRM.
 * ────────────────────────────────────────────────────────────────────────── */
bool build_mesh_status_json(char*  out,
                            size_t cap,
                            bool   enabled,
                            bool   has_opera,
                            const uint8_t opera_id[mesh_crypto::OPERA_ID_LEN], /* may be null */
                            const char*  opera_name,
                            mesh_pairing::State pairing_state,
                            size_t   peers_total,
                            size_t   peers_online,
                            uint32_t alerts_received,
                            uint32_t pairing_code);

/* ──────────────────────────────────────────────────────────────────────────
 * GET /api/mesh/peers — peer list
 *
 * Emits {ok:true, peers:[{fingerprint, name, state, last_seen_sec, rssi,
 * alerts_received}]}. alerts_received is the spec §8.2 per-peer field
 * (verified TAMPER_ALERT frames from that peer this boot); the web UI
 * does not read it today, but HA and the spec do.
 *
 * The handler builds an array of PeerView from the persisted trusted-peer
 * set, best-effort-joined against the live transport peer table. `state`
 * is one of the strings the UI styles: "CONNECTED" / "STALE" / "OFFLINE"
 * (a peer with no live match defaults to "OFFLINE"). fingerprint is the
 * 16-hex-char (FINGERPRINT_LEN*2) lowercase fingerprint.
 * ────────────────────────────────────────────────────────────────────────── */

struct PeerView {
  char     fingerprint[mesh_crypto::FINGERPRINT_LEN * 2 + 1];  /* lowercase hex, null-term */
  char     name[24 + 1];          /* best-effort; "" if unknown */
  const char* state;              /* "CONNECTED" / "STALE" / "OFFLINE" */
  uint32_t last_seen_sec;         /* seconds since last_seen; large if never */
  int      rssi;                  /* dBm; 0 if unknown */
  uint32_t alerts_received;       /* verified TAMPER_ALERTs from this peer (F11) */
};

bool build_mesh_peers_json(char*  out,
                           size_t cap,
                           const PeerView* peers,
                           size_t          count);

/* ──────────────────────────────────────────────────────────────────────────
 * GET /api/mesh/alerts — received alert history (F10)
 *
 * Emits {ok:true, count:N, alerts:[{timestamp_ms, type, severity,
 * sender_fp, sender_name, detail, witness_seq}]} in the order given
 * (the session hands them over newest-first). The field names are the
 * ones the web UI's loadOperaAlerts() reads — type, severity,
 * sender_name, detail, timestamp_ms — plus sender_fp and witness_seq,
 * which canary-wap also emits. `type` is always "TAMPER"
 * (mesh_alert::type_name), `detail` is the template name for the kind
 * (mesh_alert::kind_name — never sender-authored text), `sender_fp` is
 * 16 lowercase hex chars, and `sender_name` is "" until a peer-metadata
 * store exists (the UI renders "Unknown"). timestamp_ms is the
 * receiver's uptime at receipt, the same basis canary-wap uses.
 * ────────────────────────────────────────────────────────────────────────── */

bool build_mesh_alerts_json(char*                     out,
                            size_t                    cap,
                            const mesh_alert::Record* alerts,
                            size_t                    count);

/* ──────────────────────────────────────────────────────────────────────────
 * POST /api/mesh/remove {fingerprint} — request parsing (F10-rekey)
 *
 * The web UI sends the `fingerprint` string GET /api/mesh/peers emitted:
 * exactly FINGERPRINT_LEN*2 (16) hex digits. Upper case is accepted too.
 * Returns false (out untouched) on null, any other length, or a non-hex
 * character — the handler answers 400 invalid_fingerprint.
 * ────────────────────────────────────────────────────────────────────────── */

bool parse_fingerprint_hex(const char* hex,
                           uint8_t     out[mesh_crypto::FINGERPRINT_LEN]);

}  /* namespace mesh_api */

#endif  /* SECURACV_MESH_API_H */
