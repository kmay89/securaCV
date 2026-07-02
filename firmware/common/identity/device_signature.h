/**
 * @file device_signature.h
 * @brief Per-device Ed25519 signatures over MQTT-published claims (shared).
 *
 * Canonical shared copy of the signing surface proven in the canary-wap
 * COMPATIBILITY tree (`firmware/projects/canary-wap/arduino/canary_wap/
 * device_signature.{h,cpp}`), minus that tree's HTTP enrollment endpoints
 * (an esp_http_server surface the headless MQTT variants don't have).
 * The canonical message formats are LOCKED against
 * `custom_components/securacv/signature.py` — any change must bump
 * SCHEMA_V and land in lockstep on the HA side. The wap sketch keeps its
 * committed copy (same pattern as the OTA engine / boot banner).
 *
 * The signed payload is NOT the JSON body — JSON canonicalization is
 * fragile across snprintf and Python. Instead each kind of publish has
 * a fixed canonical message format:
 *
 *   securacv-canary-sig|v1|chain|<device_id>|<length>|<latest_hash_hex>
 *   securacv-canary-sig|v1|event|<device_id>|<event_id>|<state>|
 *                                <category>|<privacy>|<motion>|<breath>|<bpm>
 *   securacv-canary-sig|v1|counts|<device_id>|<total>
 *   securacv-canary-sig|v1|sense|<device_id>|<seq>|<event>|<presence>|
 *                                <occupants>|<range>|<bucket_uptime_s>
 *
 * The `sense` kind is the canary-sense radar witness's event canonical:
 * only the coarse chokepoint vocabulary (state names, 0/1/2+ bucket,
 * near/mid/far band, 10-minute uptime bucket) — the canonical carries
 * exactly what the JSON body publishes, nothing finer.
 *
 * Output sigs are base64url-encoded with no padding (86 chars for the
 * 64-byte Ed25519 sig).
 */

#ifndef SECURACV_COMMON_DEVICE_SIGNATURE_H
#define SECURACV_COMMON_DEVICE_SIGNATURE_H

#include <stddef.h>
#include <stdint.h>

namespace device_signature {

constexpr int         SCHEMA_V    = 1;
constexpr const char* ALG_NAME    = "ed25519";
constexpr const char* SIG_PREFIX  = "securacv-canary-sig";

/* Ed25519 sig is 64 bytes → 86 chars b64url (no padding) + NUL. */
constexpr size_t SIG_B64URL_LEN = 86;
constexpr size_t SIG_B64URL_CAP = SIG_B64URL_LEN + 1;

/* Cold-boot init. The caller hands over copies of the device's keypair
 * + identity strings; the module keeps its own storage so callers can
 * re-use their stack buffers. Idempotent — subsequent calls overwrite
 * the cached identity. */
void init(const uint8_t priv[32],
          const uint8_t pub[32],
          const char*   device_id,
          const char*   fingerprint_hex);

/* Sign the canonical "chain" message. Returns true on success and
 * writes the b64url-encoded 64-byte signature into sig_out. sig_cap
 * must be >= SIG_B64URL_CAP. latest_hash_32 is the 32-byte binary
 * chain head (the same value the publisher hex-encodes for the wire). */
bool sign_chain(uint32_t       length,
                const uint8_t  latest_hash_32[32],
                char*          sig_b64url_out,
                size_t         sig_cap);

/* Sign the canonical "event" message (CSI event vocabulary). The
 * state/category/privacy strings are embedded verbatim — callers
 * guarantee they're alphanumeric + underscore (state, category) or
 * p0/p1/p2 (privacy), so no escaping is needed. */
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

/* Sign the canonical "sense" message (canary-sense radar witness event).
 * The string fields are the chokepoint's fixed vocabulary — event names
 * (presence_detected / presence_cleared / occupancy_changed), presence
 * states (unknown/clear/present), occupant buckets (0/1/2+), range
 * bands (unknown/near/mid/far) — so no escaping is needed. */
bool sign_sense(uint32_t    seq,
                const char* event_name,
                const char* presence,
                const char* occupants,
                const char* range,
                uint32_t    bucket_uptime_s,
                char*       sig_b64url_out,
                size_t      sig_cap);

/* Identity accessors — both return pointers into module-local storage
 * with static lifetime. Callers must not free. fingerprint_hex is the
 * 16-char hex of the pubkey fingerprint; pubkey_hex is the 64-char hex
 * of the full 32-byte pubkey (HA pins on either form). */
const char* fingerprint_hex();
const char* pubkey_hex();
const char* device_id();

/* Test-only canonical builders. Return the number of bytes written
 * (excluding NUL), 0 on truncation. Exposed so host tests can verify a
 * known input produces a known canonical string before the sig step. */
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

size_t build_sense_canonical(uint32_t    seq,
                             const char* event_name,
                             const char* presence,
                             const char* occupants,
                             const char* range,
                             uint32_t    bucket_uptime_s,
                             const char* device_id,
                             char*       out,
                             size_t      cap);

/* Test-only: base64url-encode (no padding) `in_len` bytes from `in`
 * into `out`. Returns the number of chars written (excluding NUL). */
size_t b64url_encode_nopad(const uint8_t* in,
                           size_t         in_len,
                           char*          out,
                           size_t         out_cap);

}  /* namespace device_signature */

#endif  /* SECURACV_COMMON_DEVICE_SIGNATURE_H */
