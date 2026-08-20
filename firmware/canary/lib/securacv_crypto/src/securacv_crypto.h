/*
 * SecuraCV Canary — Cryptographic Primitives
 *
 * Ed25519 key management, SHA256 hash chain with domain separation,
 * and CBOR encoding helpers.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CRYPTO_H
#define SECURACV_CRYPTO_H

#include <Arduino.h>
#include <Preferences.h>
#include <stdint.h>
#include <stddef.h>

// ════════════════════════════════════════════════════════════════════════════
// NVS MANAGER
// ════════════════════════════════════════════════════════════════════════════

class NvsManager {
public:
  static NvsManager& instance();

  bool begin(bool readOnly = false);
  bool beginReadOnly() { return begin(true); }
  bool beginReadWrite() { return begin(false); }
  void end();

  bool isOpen() const { return m_open; }
  bool isReadOnly() const { return m_readOnly; }

  // Boolean operations
  bool getBool(const char* key, bool defaultValue = false);
  size_t putBool(const char* key, bool value);

  // Integer operations
  uint8_t getUChar(const char* key, uint8_t defaultValue = 0);
  size_t putUChar(const char* key, uint8_t value);
  uint32_t getUInt(const char* key, uint32_t defaultValue = 0);
  size_t putUInt(const char* key, uint32_t value);

  // Byte array operations
  size_t getBytesLength(const char* key);
  size_t getBytes(const char* key, void* buf, size_t maxLen);
  size_t putBytes(const char* key, const void* value, size_t len);

  // String operations. Copies the value into buf NUL-terminated and returns
  // chars copied (0 when the key is absent, not string-typed, or the value
  // plus NUL does not fit in maxLen).
  size_t getString(const char* key, char* buf, size_t maxLen);

  // Key management
  bool isKey(const char* key);
  bool remove(const char* key);
  bool clear();

  NvsManager(const NvsManager&) = delete;
  NvsManager& operator=(const NvsManager&) = delete;

private:
  NvsManager();
  ~NvsManager();

  Preferences m_prefs;
  bool m_open;
  bool m_readOnly;
};

// ════════════════════════════════════════════════════════════════════════════
// SHA-256 WITH DOMAIN SEPARATION
// ════════════════════════════════════════════════════════════════════════════

// Raw SHA-256 hash
void sha256_raw(const uint8_t* data, size_t n, uint8_t out[32]);

// Domain-separated SHA-256: H(domain || 0x00 || data)
void sha256_domain(const char* domain, const uint8_t* data, size_t n, uint8_t out[32]);

// ════════════════════════════════════════════════════════════════════════════
// ED25519 CRYPTO
// ════════════════════════════════════════════════════════════════════════════

// Generate new Ed25519 keypair using hardware RNG
bool crypto_generate_keypair(uint8_t priv[32], uint8_t pub[32]);

// Sign message with Ed25519
void crypto_sign(const uint8_t priv[32], const uint8_t pub[32],
                 const uint8_t* msg, size_t len, uint8_t sig[64]);

// Verify Ed25519 signature
bool crypto_verify(const uint8_t pub[32], const uint8_t* msg, size_t len, const uint8_t sig[64]);

// Compute 8-byte fingerprint from public key
void crypto_fingerprint(const uint8_t pub[32], uint8_t fp[8]);

// ════════════════════════════════════════════════════════════════════════════
// HMAC-SHA256 + HKDF-STYLE API TOKEN DERIVATION
// ════════════════════════════════════════════════════════════════════════════

// HMAC-SHA256 using mbedtls. Returns true on success.
bool hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* data, size_t data_len,
                 uint8_t out[32]);

// Base62-encode `in_len` bytes into `output` using unbiased rejection
// sampling (reject bytes >= 248 since 248 = 62 * 4). Prefix is "cv_" and
// the output target is 32 base62 chars — so `out_len` must be >= 36.
// Always null-terminates when out_len >= 1.
void format_api_token_string(const uint8_t* input, size_t in_len,
                             char* output, size_t out_len);

// Derive the device's API bearer token from the Ed25519 private key via
// two-step key separation:
//   step 1: token_key = HMAC(priv, "securacv:token-key-derive:v1")
//   step 2: token_hash = HMAC(token_key, "securacv:api-token:v1" || MAC[6])
//   step 3: base62-encode first 24 bytes of token_hash with "cv_" prefix
// The Ed25519 signing key never directly touches the token context.
// token_str must be at least 36 bytes ("cv_" + 32 chars + NUL). Returns
// true on success.
bool derive_api_token(const uint8_t privkey[32],
                      char* token_str, size_t token_str_len);

// ════════════════════════════════════════════════════════════════════════════
// CHAIN OPERATIONS
// ════════════════════════════════════════════════════════════════════════════

// Compute chain hash: H("securacv:fw:chain:v1" || 0x00 || prev || payload_hash || seq || time_bucket)
void compute_chain_hash(const uint8_t prev[32], const uint8_t payload_hash[32],
                        uint32_t seq, uint32_t time_bucket, uint8_t out[32]);

// ════════════════════════════════════════════════════════════════════════════
// NVS PERSISTENCE HELPERS
// ════════════════════════════════════════════════════════════════════════════

bool nvs_load_key(uint8_t priv[32]);
bool nvs_store_key(const uint8_t priv[32]);
uint32_t nvs_load_u32(const char* key, uint32_t def = 0);
bool nvs_store_u32(const char* key, uint32_t val);
bool nvs_load_bytes(const char* key, uint8_t* out, size_t len);
bool nvs_store_bytes(const char* key, const uint8_t* data, size_t len);

// ════════════════════════════════════════════════════════════════════════════
// CBOR WRITER
// ════════════════════════════════════════════════════════════════════════════

class CborWriter {
public:
  CborWriter(uint8_t* buf, size_t cap);

  void write_map(size_t n);
  void write_text(const char* s);
  void write_uint(uint64_t v);
  void write_int(int64_t v);
  void write_bool(bool v);
  void write_null();
  void write_float(double v);
  void write_bytes(const uint8_t* data, size_t len);

  size_t size() const { return pos_; }
  bool ok() const { return !error_; }

private:
  void write_byte(uint8_t b);

  uint8_t* buf_;
  size_t cap_;
  size_t pos_;
  bool error_;
};

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

// Securely zero memory
void secure_zero(void* p, size_t n);

// Convert bytes to hex string
void hex_to_str(char* out, const uint8_t* d, size_t n);

// Build the device ID / AP SSID suffix from the Ed25519 pubkey fingerprint
// (fp[0..1]), never the hardware MAC (event_contract §10 / privacy Invariant
// III). Caller must populate the fingerprint first. Encoded in the unambiguous
// alphabet (no 0/O/o, 1/I/i/l/L); stable for the device's lifetime.
void generate_device_id(char* out, size_t cap, const char* prefix,
                        const uint8_t fp[2]);
void generate_ap_ssid(char* out, size_t cap, const uint8_t fp[2]);

#endif // SECURACV_CRYPTO_H
