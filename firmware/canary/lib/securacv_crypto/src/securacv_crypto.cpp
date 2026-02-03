/*
 * SecuraCV Canary — Cryptographic Primitives Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_crypto.h"
#include "canary_config.h"

#include <Crypto.h>
#include <Ed25519.h>
#include "esp_random.h"
#include "esp_mac.h"
#include "mbedtls/sha256.h"

// ════════════════════════════════════════════════════════════════════════════
// NVS MANAGER IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

NvsManager::NvsManager() : m_open(false), m_readOnly(false) {}

NvsManager::~NvsManager() {
  end();
}

NvsManager& NvsManager::instance() {
  static NvsManager s_instance;
  return s_instance;
}

bool NvsManager::begin(bool readOnly) {
  if (m_open) {
    if (m_readOnly && !readOnly) {
      m_prefs.end();
      m_open = false;
    } else {
      return true;
    }
  }
  m_open = m_prefs.begin(NVS_MAIN_NS, readOnly);
  m_readOnly = readOnly;
  return m_open;
}

void NvsManager::end() {
  if (m_open) {
    m_prefs.end();
    m_open = false;
  }
}

bool NvsManager::getBool(const char* key, bool defaultValue) {
  return m_prefs.getBool(key, defaultValue);
}

size_t NvsManager::putBool(const char* key, bool value) {
  return m_prefs.putBool(key, value);
}

uint8_t NvsManager::getUChar(const char* key, uint8_t defaultValue) {
  return m_prefs.getUChar(key, defaultValue);
}

size_t NvsManager::putUChar(const char* key, uint8_t value) {
  return m_prefs.putUChar(key, value);
}

uint32_t NvsManager::getUInt(const char* key, uint32_t defaultValue) {
  return m_prefs.getUInt(key, defaultValue);
}

size_t NvsManager::putUInt(const char* key, uint32_t value) {
  return m_prefs.putUInt(key, value);
}

size_t NvsManager::getBytesLength(const char* key) {
  return m_prefs.getBytesLength(key);
}

size_t NvsManager::getBytes(const char* key, void* buf, size_t maxLen) {
  return m_prefs.getBytes(key, buf, maxLen);
}

size_t NvsManager::putBytes(const char* key, const void* value, size_t len) {
  return m_prefs.putBytes(key, value, len);
}

bool NvsManager::isKey(const char* key) {
  return m_prefs.isKey(key);
}

bool NvsManager::remove(const char* key) {
  return m_prefs.remove(key);
}

bool NvsManager::clear() {
  return m_prefs.clear();
}

// ════════════════════════════════════════════════════════════════════════════
// SHA-256 WITH DOMAIN SEPARATION
// ════════════════════════════════════════════════════════════════════════════

void sha256_raw(const uint8_t* data, size_t n, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, data, n);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

void sha256_domain(const char* domain, const uint8_t* data, size_t n, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  mbedtls_sha256_update(&ctx, (const uint8_t*)domain, strlen(domain));
  uint8_t sep = 0x00;
  mbedtls_sha256_update(&ctx, &sep, 1);

  if (data && n > 0) {
    mbedtls_sha256_update(&ctx, data, n);
  }

  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

// ════════════════════════════════════════════════════════════════════════════
// ED25519 CRYPTO
// ════════════════════════════════════════════════════════════════════════════

bool crypto_generate_keypair(uint8_t priv[32], uint8_t pub[32]) {
  esp_fill_random(priv, 32);
  Ed25519::derivePublicKey(pub, priv);
  return true;
}

void crypto_sign(const uint8_t priv[32], const uint8_t pub[32],
                 const uint8_t* msg, size_t len, uint8_t sig[64]) {
  Ed25519::sign(sig, priv, pub, msg, len);
}

bool crypto_verify(const uint8_t pub[32], const uint8_t* msg, size_t len, const uint8_t sig[64]) {
  return Ed25519::verify(sig, pub, msg, len);
}

void crypto_fingerprint(const uint8_t pub[32], uint8_t fp[8]) {
  uint8_t hash[32];
  sha256_domain("securacv:pubkey:fingerprint", pub, 32, hash);
  memcpy(fp, hash, 8);
}

// ════════════════════════════════════════════════════════════════════════════
// CHAIN OPERATIONS
// ════════════════════════════════════════════════════════════════════════════

void compute_chain_hash(const uint8_t prev[32], const uint8_t payload_hash[32],
                        uint32_t seq, uint32_t time_bucket, uint8_t out[32]) {
  uint8_t buf[32 + 32 + 4 + 4];
  memcpy(buf, prev, 32);
  memcpy(buf + 32, payload_hash, 32);
  buf[64] = (seq >> 24) & 0xFF;
  buf[65] = (seq >> 16) & 0xFF;
  buf[66] = (seq >> 8) & 0xFF;
  buf[67] = seq & 0xFF;
  buf[68] = (time_bucket >> 24) & 0xFF;
  buf[69] = (time_bucket >> 16) & 0xFF;
  buf[70] = (time_bucket >> 8) & 0xFF;
  buf[71] = time_bucket & 0xFF;

  sha256_domain("securacv:chain:v1", buf, sizeof(buf), out);
}

// ════════════════════════════════════════════════════════════════════════════
// NVS PERSISTENCE HELPERS
// ════════════════════════════════════════════════════════════════════════════

bool nvs_load_key(uint8_t priv[32]) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;
  size_t n = nvs.getBytesLength(NVS_KEY_PRIV);
  if (n != 32) { nvs.end(); return false; }
  nvs.getBytes(NVS_KEY_PRIV, priv, 32);
  nvs.end();
  return true;
}

bool nvs_store_key(const uint8_t priv[32]) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  nvs.putBytes(NVS_KEY_PRIV, priv, 32);
  nvs.end();
  return true;
}

uint32_t nvs_load_u32(const char* key, uint32_t def) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return def;
  uint32_t v = nvs.getUInt(key, def);
  nvs.end();
  return v;
}

bool nvs_store_u32(const char* key, uint32_t val) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  nvs.putUInt(key, val);
  nvs.end();
  return true;
}

bool nvs_load_bytes(const char* key, uint8_t* out, size_t len) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;
  size_t n = nvs.getBytesLength(key);
  if (n != len) { nvs.end(); return false; }
  nvs.getBytes(key, out, len);
  nvs.end();
  return true;
}

bool nvs_store_bytes(const char* key, const uint8_t* data, size_t len) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  nvs.putBytes(key, data, len);
  nvs.end();
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// CBOR WRITER IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

CborWriter::CborWriter(uint8_t* buf, size_t cap)
  : buf_(buf), cap_(cap), pos_(0), error_(false) {}

void CborWriter::write_byte(uint8_t b) {
  if (pos_ < cap_) {
    buf_[pos_++] = b;
  } else {
    error_ = true;
  }
}

void CborWriter::write_map(size_t n) {
  if (n <= 23) {
    write_byte(0xA0 + n);
  } else if (n <= 255) {
    write_byte(0xB8);
    write_byte(n);
  } else {
    write_byte(0xB9);
    write_byte((n >> 8) & 0xFF);
    write_byte(n & 0xFF);
  }
}

void CborWriter::write_text(const char* s) {
  size_t len = strlen(s);
  if (len <= 23) {
    write_byte(0x60 + len);
  } else if (len <= 255) {
    write_byte(0x78);
    write_byte(len);
  } else {
    write_byte(0x79);
    write_byte((len >> 8) & 0xFF);
    write_byte(len & 0xFF);
  }
  for (size_t i = 0; i < len; i++) {
    write_byte(s[i]);
  }
}

void CborWriter::write_uint(uint64_t v) {
  if (v <= 23) {
    write_byte(v);
  } else if (v <= 255) {
    write_byte(0x18);
    write_byte(v);
  } else if (v <= 65535) {
    write_byte(0x19);
    write_byte((v >> 8) & 0xFF);
    write_byte(v & 0xFF);
  } else if (v <= 0xFFFFFFFF) {
    write_byte(0x1A);
    write_byte((v >> 24) & 0xFF);
    write_byte((v >> 16) & 0xFF);
    write_byte((v >> 8) & 0xFF);
    write_byte(v & 0xFF);
  } else {
    write_byte(0x1B);
    for (int i = 7; i >= 0; i--) {
      write_byte((v >> (i * 8)) & 0xFF);
    }
  }
}

void CborWriter::write_int(int64_t v) {
  if (v >= 0) {
    write_uint((uint64_t)v);
  } else {
    uint64_t neg = (uint64_t)(-(v + 1));
    if (neg <= 23) {
      write_byte(0x20 + neg);
    } else if (neg <= 255) {
      write_byte(0x38);
      write_byte(neg);
    } else if (neg <= 65535) {
      write_byte(0x39);
      write_byte((neg >> 8) & 0xFF);
      write_byte(neg & 0xFF);
    } else {
      write_byte(0x3A);
      write_byte((neg >> 24) & 0xFF);
      write_byte((neg >> 16) & 0xFF);
      write_byte((neg >> 8) & 0xFF);
      write_byte(neg & 0xFF);
    }
  }
}

void CborWriter::write_bool(bool v) {
  write_byte(v ? 0xF5 : 0xF4);
}

void CborWriter::write_null() {
  write_byte(0xF6);
}

void CborWriter::write_float(double v) {
  write_byte(0xFB);
  union { double d; uint64_t u; } conv;
  conv.d = v;
  for (int i = 7; i >= 0; i--) {
    write_byte((conv.u >> (i * 8)) & 0xFF);
  }
}

void CborWriter::write_bytes(const uint8_t* data, size_t len) {
  if (len <= 23) {
    write_byte(0x40 + len);
  } else if (len <= 255) {
    write_byte(0x58);
    write_byte(len);
  } else {
    write_byte(0x59);
    write_byte((len >> 8) & 0xFF);
    write_byte(len & 0xFF);
  }
  for (size_t i = 0; i < len; i++) {
    write_byte(data[i]);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

void secure_zero(void* p, size_t n) {
  volatile uint8_t* vp = (volatile uint8_t*)p;
  while (n--) *vp++ = 0;
}

void hex_to_str(char* out, const uint8_t* d, size_t n) {
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < n; i++) {
    out[i*2]   = hex[d[i] >> 4];
    out[i*2+1] = hex[d[i] & 0x0F];
  }
  out[n*2] = 0;
}

void generate_device_id(char* out, size_t cap, const char* prefix) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, cap, "%s%02X%02X", prefix, mac[4], mac[5]);
}

void generate_ap_ssid(char* out, size_t cap) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, cap, "SecuraCV-%02X%02X", mac[4], mac[5]);
}
