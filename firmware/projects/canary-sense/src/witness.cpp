#include "canary/witness.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include <Ed25519.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>

#include "canary/config.h"
#include "canary/log.h"
#include "canary/runtime_config.h"
#include "identity/device_signature.h"  // shared signer (common/identity)

namespace canary::witness {

namespace {

// Domain strings shared with common/witness/witness_chain.h and the
// canary-wap tree — reproduced as literals because that header's C API
// (witness_chain_t etc.) is a larger surface than this witness needs.
constexpr const char* DOMAIN_FINGERPRINT = "securacv:pubkey:fingerprint";
constexpr const char* DOMAIN_PAYLOAD     = "securacv:fw:payload:v1";
constexpr const char* DOMAIN_CHAIN       = "securacv:fw:chain:v1";
constexpr const char* DOMAIN_BOOT        = "securacv:fw:boot:v1";

// NVS layout (namespace "securacv", shared with runtime_config):
//   privkey    32 B  Ed25519 private key (first boot generates)
//   chain_head 32 B  current hash-chain head
//   chain_len  u32   number of chained events
constexpr const char* NVS_NAMESPACE  = "securacv";
constexpr const char* KEY_PRIV       = "privkey";
constexpr const char* KEY_CHAIN_HEAD = "chain_head";
constexpr const char* KEY_CHAIN_LEN  = "chain_len";

uint8_t  s_priv[32]   = {};
uint8_t  s_pub[32]    = {};
uint8_t  s_head[32]   = {};
uint32_t s_length     = 0;
bool     s_ready      = false;

// Domain-separated SHA-256: SHA256(domain || 0x00 || part1 [|| part2]).
// Same construction as the canary-wap tree's sha256_domain, extended to
// two parts for the chain-advance hash. mbedtls_sha256_* return int on
// core 3.x (void on 2.x); the results are ignored uniformly — a SHA-256
// software fallback cannot fail for these inputs.
void sha256_domain2(const char* domain,
                    const uint8_t* p1, size_t n1,
                    const uint8_t* p2, size_t n2,
                    uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  (void)mbedtls_sha256_starts(&ctx, 0);
  (void)mbedtls_sha256_update(&ctx, (const uint8_t*)domain, strlen(domain));
  const uint8_t sep = 0x00;
  (void)mbedtls_sha256_update(&ctx, &sep, 1);
  if (p1 && n1) (void)mbedtls_sha256_update(&ctx, p1, n1);
  if (p2 && n2) (void)mbedtls_sha256_update(&ctx, p2, n2);
  (void)mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

void sha256_domain(const char* domain, const uint8_t* data, size_t n,
                   uint8_t out[32]) {
  sha256_domain2(domain, data, n, nullptr, 0, out);
}

// Reliable scrub for key material (volatile pointer defeats dead-store
// elimination — same helper the pseudonym module uses).
void secure_zero(void* p, size_t n) {
  volatile uint8_t* vp = static_cast<volatile uint8_t*>(p);
  while (n--) *vp++ = 0;
}

bool load_or_generate_keypair() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    log_line("WITNESS", "NVS unavailable — signing disabled this boot.");
    return false;
  }

  bool have = false;
  if (prefs.getBytesLength(KEY_PRIV) == sizeof(s_priv)) {
    have = (prefs.getBytes(KEY_PRIV, s_priv, sizeof(s_priv)) == sizeof(s_priv));
  }
  // An all-zero blob is not a key — regenerate rather than sign with it.
  if (have) {
    bool nonzero = false;
    for (size_t i = 0; i < sizeof(s_priv); i++) {
      if (s_priv[i] != 0) { nonzero = true; break; }
    }
    have = nonzero;
  }

  if (!have) {
    // First boot: 32 bytes from the hardware RNG, exactly like the
    // canary-wap tree's generate_keypair.
    esp_fill_random(s_priv, sizeof(s_priv));
    if (prefs.putBytes(KEY_PRIV, s_priv, sizeof(s_priv)) != sizeof(s_priv)) {
      log_line("WITNESS", "Key persist FAILED — identity will not survive reboot.");
    } else {
      log_line("WITNESS", "Generated new Ed25519 identity (first boot).");
    }
  }

  prefs.end();
  Ed25519::derivePublicKey(s_pub, s_priv);
  return true;
}

void load_or_start_chain() {
  Preferences prefs;
  bool loaded = false;
  if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    if (prefs.getBytesLength(KEY_CHAIN_HEAD) == sizeof(s_head) &&
        prefs.getBytes(KEY_CHAIN_HEAD, s_head, sizeof(s_head)) == sizeof(s_head)) {
      s_length = prefs.getULong(KEY_CHAIN_LEN, 0);
      loaded = true;
    }
    prefs.end();
  }
  if (!loaded) {
    // Genesis binds the chain to this device's key.
    sha256_domain(DOMAIN_BOOT, s_pub, sizeof(s_pub), s_head);
    s_length = 0;
  }
}

void persist_chain() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return;
  prefs.putBytes(KEY_CHAIN_HEAD, s_head, sizeof(s_head));
  prefs.putULong(KEY_CHAIN_LEN, s_length);
  prefs.end();
}

}  // namespace

bool init() {
  if (s_ready) return true;

  if (!load_or_generate_keypair()) {
    s_ready = false;
    return false;
  }

  // Fingerprint: wap formula — SHA256(domain || 0x00 || pubkey)[0..8] hex.
  uint8_t fp_hash[32];
  sha256_domain(DOMAIN_FINGERPRINT, s_pub, sizeof(s_pub), fp_hash);
  char fp_hex[17];
  static const char H[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++) {
    fp_hex[2 * i]     = H[(fp_hash[i] >> 4) & 0xF];
    fp_hex[2 * i + 1] = H[(fp_hash[i] >> 0) & 0xF];
  }
  fp_hex[16] = '\0';

  device_signature::init(s_priv, s_pub, canary::cfg::get().device_id, fp_hex);
  load_or_start_chain();
  s_ready = true;

  log_header("WITNESS");
  canary::dbg_serial().printf(
      "Ed25519 ready  fp=%s  chain_len=%lu\n",
      fp_hex, (unsigned long)s_length);
  return true;
}

bool ready() { return s_ready; }

bool sign_event_envelope(uint32_t    seq,
                         const char* event_name,
                         const char* presence,
                         const char* occupants,
                         const char* range,
                         uint32_t    bucket_uptime_s,
                         char*       out,
                         size_t      cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!s_ready) return false;

  char sig_b64[device_signature::SIG_B64URL_CAP];
  if (!device_signature::sign_sense(seq, event_name, presence, occupants,
                                    range, bucket_uptime_s,
                                    sig_b64, sizeof(sig_b64))) {
    return false;
  }

  const int n = snprintf(out, cap,
                         ",\"v\":%d,\"alg\":\"%s\",\"fp\":\"%s\",\"sig\":\"%s\"",
                         device_signature::SCHEMA_V,
                         device_signature::ALG_NAME,
                         device_signature::fingerprint_hex(),
                         sig_b64);
  if (n <= 0 || (size_t)n >= cap) {
    // Never emit a half-built envelope — the truncated JSON would break
    // the whole event payload downstream.
    out[0] = '\0';
    return false;
  }
  return true;
}

void chain_advance(uint32_t    seq,
                   const char* event_name,
                   const char* presence,
                   const char* occupants,
                   const char* range,
                   uint32_t    bucket_uptime_s) {
  if (!s_ready) return;

  char canonical[256];
  const size_t n = device_signature::build_sense_canonical(
      seq, event_name, presence, occupants, range, bucket_uptime_s,
      device_signature::device_id(), canonical, sizeof(canonical));
  if (n == 0) return;

  uint8_t payload_hash[32];
  sha256_domain(DOMAIN_PAYLOAD,
                reinterpret_cast<const uint8_t*>(canonical), n, payload_hash);

  uint8_t new_head[32];
  sha256_domain2(DOMAIN_CHAIN, s_head, sizeof(s_head),
                 payload_hash, sizeof(payload_hash), new_head);
  memcpy(s_head, new_head, sizeof(s_head));
  s_length++;

  persist_chain();
  secure_zero(payload_hash, sizeof(payload_hash));
}

uint32_t chain_length() { return s_length; }
const uint8_t* chain_head() { return s_head; }

} // namespace canary::witness
