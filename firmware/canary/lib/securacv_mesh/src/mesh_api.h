/*
 * SecuraCV Canary — Mesh REST API JSON builders (PR-8)
 * Version 0.1.0
 *
 * Pure, I/O-free helpers that render the JSON bodies for the two GET
 * mesh endpoints:
 *
 *   GET /api/mesh        → build_mesh_status_json()
 *   GET /api/mesh/peers  → build_mesh_peers_json()
 *
 * Why a separate, pure module:
 *   The HTTP handlers in securacv_network.cpp live behind
 *   FEATURE_MESH_NETWORK, which the dev/release CI envs compile out
 *   (platformio.ini sets -DFEATURE_MESH_NETWORK=0 for the only two envs
 *   CI builds). That means the handler bodies get ZERO CI compile
 *   coverage. Extracting the JSON-building logic here — taking plain
 *   structs/params, writing to a caller-supplied char buffer, no
 *   httpd_req_t, no ArduinoJson — lets the mesh host-test harness
 *   (which links every securacv_mesh src TU) compile and exercise the
 *   real response shape on every PR.
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

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_api {

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
 * Emits {ok:true, peers:[{fingerprint, name, state, last_seen_sec, rssi}]}.
 * No per-peer alerts field (the UI does not read one).
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
};

bool build_mesh_peers_json(char*  out,
                           size_t cap,
                           const PeerView* peers,
                           size_t          count);

}  /* namespace mesh_api */

#endif  /* SECURACV_MESH_API_H */
