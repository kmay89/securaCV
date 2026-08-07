/**
 * @file device_signature.h
 * @brief Per-device Ed25519 signatures over MQTT-published events.
 *
 * The Canary already holds an Ed25519 keypair (generated on first boot,
 * persisted in NVS at securacv/privkey). Until now we used it only to
 * sign witness records on disk. This module extends it to the outbound
 * MQTT surface: every chain advance, event commit, and counts publish
 * carries a signature so Home Assistant can verify the broker hasn't
 * been forged and a hostile peer with broker access can't impersonate
 * a Canary.
 *
 * The signed payload is NOT the JSON body — JSON canonicalization is
 * fragile across snprintf and Python. Instead each kind of publish has
 * a fixed canonical message format:
 *
 *   securacv-canary-sig|v1|chain|<device_id>|<length>|<latest_hash_hex>
 *   securacv-canary-sig|v1|event|<device_id>|<event_id>|<state>|
 *                                <category>|<privacy>|<motion>|<breath>|<bpm>
 *   securacv-canary-sig|v1|counts|<device_id>|<total>
 *
 * Both sides reconstruct this string from the parsed JSON fields and
 * verify the Ed25519 sig over its raw UTF-8 bytes. The schema version
 * (v1) lets us evolve fields without breaking deployed devices —
 * older HA installs ignore signatures they can't parse.
 *
 * Output sigs are base64url-encoded with no padding (86 chars for the
 * 64-byte Ed25519 sig). The fingerprint is the same 16-char hex
 * everything else uses (g_device.fingerprint_hex).
 */

#ifndef SECURACV_DEVICE_SIGNATURE_H
#define SECURACV_DEVICE_SIGNATURE_H

#include <stddef.h>
#include <stdint.h>

namespace device_signature {

constexpr int         SCHEMA_V    = 1;
constexpr const char* ALG_NAME    = "ed25519";
constexpr const char* SIG_PREFIX  = "securacv-canary-sig";

/* Ed25519 sig is 64 bytes → 86 chars b64url (no padding) + NUL. */
constexpr size_t SIG_B64URL_LEN = 86;
constexpr size_t SIG_B64URL_CAP = SIG_B64URL_LEN + 1;

/* Cold-boot init. Caller (canary_wap.ino setup) hands over copies of
 * the device's keypair + identity strings. The module keeps its own
 * storage so callers can re-use their stack buffers. Idempotent —
 * subsequent calls overwrite the cached identity. */
void init(const uint8_t priv[32],
          const uint8_t pub[32],
          const char*   device_id,
          const char*   fingerprint_hex);

/* Sign the canonical "chain" message. Returns true on success and
 * writes the b64url-encoded 64-byte signature into sig_out. sig_cap
 * must be >= SIG_B64URL_CAP. latest_hash_32 is the 32-byte binary
 * chain head (the same value csi_mqtt::publish_chain hex-encodes for
 * the wire). */
bool sign_chain(uint32_t       length,
                const uint8_t  latest_hash_32[32],
                char*          sig_b64url_out,
                size_t         sig_cap);

/* Sign the canonical "event" message. The state, category, and
 * privacy strings are embedded verbatim — csi_event guarantees they're
 * alphanumeric + underscore (state, category) or p0/p1/p2 (privacy),
 * so no escaping is needed. motion/breathing/bpm are the same integers
 * the JSON body carries (0-255 range from csi_event_values_t). */
bool sign_event(uint32_t      event_id,
                const char*   state,
                const char*   category_str,
                const char*   privacy_str,
                int           motion,
                int           breath,
                int           bpm,
                char*         sig_b64url_out,
                size_t        sig_cap);

/* Sign the canonical "counts" message. */
bool sign_counts(uint32_t total,
                 char*    sig_b64url_out,
                 size_t   sig_cap);

/* Ed25519 sig as hex: 64 bytes → 128 chars + NUL. */
constexpr size_t SIG_HEX_CAP = 129;

/* Sign the canonical "whoami" presence proof — a caller-supplied nonce
 * bound to this device's identity:
 *
 *   securacv-canary-sig|v1|whoami|<device_id>|<nonce_hex>
 *
 * This is what lets a fleet surface (the desktop Flasher's fleet book)
 * prove that the mDNS answer it is about to hand a bearer token to
 * actually holds this device's key, instead of trusting an announcement
 * anyone on the LAN can forge. The nonce is the CALLER's freshness — the
 * HTTP handler validates it as 16-64 lowercase hex chars before this is
 * reached, so the canonical stays deterministic and this key never signs
 * arbitrary attacker bytes (the domain prefix + kind field separate it
 * from every other message this key signs: 32-byte chain hashes, the
 * beacon canonicals under "securacv:beacon:canonical:v0", and the
 * chain/event/counts kinds above).
 *
 * The signature is returned as 128 hex chars, not b64url: its consumer
 * is the Flasher's Rust verifier and hex round-trips through every stack
 * without an alphabet argument. sig_cap must be >= SIG_HEX_CAP. */
bool sign_whoami(const char* nonce_hex,
                 char*       sig_hex_out,
                 size_t      sig_cap);

/* Identity accessors — both return pointers into module-local storage
 * with static lifetime. Callers must not free. fingerprint_hex is the
 * 16-char hex of g_device.pubkey_fp; pubkey_hex is the 64-char hex of
 * the full 32-byte pubkey (HA pins on either form). */
const char* fingerprint_hex();
const char* pubkey_hex();
const char* device_id();

/* Test-only: build the canonical "chain" message into out. Returns the
 * number of bytes written (excluding NUL). Exposed so host tests can
 * verify a known input produces a known canonical string before the
 * sig step. */
size_t build_chain_canonical(uint32_t      length,
                             const uint8_t latest_hash_32[32],
                             const char*   device_id,
                             char*         out,
                             size_t        cap);

size_t build_event_canonical(uint32_t      event_id,
                             const char*   state,
                             const char*   category_str,
                             const char*   privacy_str,
                             int           motion,
                             int           breath,
                             int           bpm,
                             const char*   device_id,
                             char*         out,
                             size_t        cap);

size_t build_counts_canonical(uint32_t      total,
                              const char*   device_id,
                              char*         out,
                              size_t        cap);

size_t build_whoami_canonical(const char*   nonce_hex,
                              const char*   device_id,
                              char*         out,
                              size_t        cap);

/* Test-only: the nonce gate the HTTP handler applies before signing —
 * 16-64 chars, lowercase hex only, so the proof canonical is
 * deterministic and the identity key never signs arbitrary bytes. */
bool whoami_nonce_ok(const char* nonce_hex);

/* Test-only: base64url-encode (no padding) `len` bytes from `in` into
 * `out`. Returns the number of chars written (excluding NUL). Exposed
 * so host tests can compare against a vendored encoder. */
size_t b64url_encode_nopad(const uint8_t* in,
                           size_t         in_len,
                           char*          out,
                           size_t         out_cap);

}  /* namespace device_signature */

/* ──────────────────────────────────────────────────────────────────
 * HTTP enrollment endpoint
 *
 * GET /api/device/enroll → JSON identity card. Unauthenticated by
 * design: pubkey + fingerprint are PUBLIC information, and the whole
 * point of enrollment is for an operator to read these off the
 * device's HTTP surface before they've configured any auth. The
 * response includes `device_id`, `pubkey_hex`, `fingerprint_hex`,
 * `alg`, and the canonical schema version so HA can match the same
 * `v` it expects on signed payloads.
 *
 * GET /enroll → human-readable HTML page rendering the same data in
 * a captive-portal-friendly layout (big fingerprint, "type this into
 * HA" copy). Same data, different content-type.
 *
 * Both are registered from canary_wap.ino's start_http_server().
 *
 * The handlers live in this header's namespace rather than
 * device_signature::, because esp_http_server.h is an Arduino-only
 * include and we want device_signature.cpp itself to stay
 * host-testable without dragging in the IDF HTTP stack.
 */
#ifdef ARDUINO
#include "esp_http_server.h"
namespace device_identity_api {
esp_err_t handle_enroll_json(httpd_req_t* req);
esp_err_t handle_enroll_html(httpd_req_t* req);
}  /* namespace device_identity_api */
#endif

#endif  /* SECURACV_DEVICE_SIGNATURE_H */
