// src/trust.cpp — TOFU pin store (NVS) + Ed25519 chain verification.
//
// The verify path mirrors custom_components/securacv/signature.py
// (build_chain_canonical + _verify_raw) and the signer in
// firmware/common/identity/device_signature.cpp. Any change to the
// canonical format lands in all three in lockstep (SCHEMA_V bump).
#include "trust.h"

#include <string.h>
#include <stdio.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <Ed25519.h>
#include "log.h"
#endif

namespace canary::trust {

namespace {

constexpr int MAX_PINS = 24;   // > both flavors' fleet caps
constexpr const char* SIG_PREFIX = "securacv-canary-sig";
constexpr int SCHEMA_V = 1;

struct Pin {
  bool used = false;
  char id[48] = {0};
  uint8_t pubkey[32] = {0};
  bool mismatch = false;  // a second, different key was seen — sticky
};

Pin s_pins[MAX_PINS];

// NVS keys are capped at 15 chars; device_ids are longer. Key = 'p' + 8-hex
// FNV-1a of the device_id. A collision inside one household's <=24-device
// fleet is astronomically unlikely; the value stored is id-prefixed anyway
// so a collision degrades to "pin not found", never to a wrong pin.
uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  for (; s && *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
  return h;
}

void nvs_key_for(const char* device_id, char* out, size_t cap) {
  snprintf(out, cap, "p%08lx", (unsigned long)fnv1a(device_id));
}

bool hex_decode(const char* hex, uint8_t* out, size_t out_len) {
  if (!hex) return false;
  for (size_t i = 0; i < out_len; i++) {
    int v = 0;
    for (int j = 0; j < 2; j++) {
      const char c = hex[2 * i + j];
      v <<= 4;
      if (c >= '0' && c <= '9')      v |= (c - '0');
      else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
      else return false;
    }
    out[i] = (uint8_t)v;
  }
  return hex[2 * out_len] == '\0';
}

// base64url (no padding) -> bytes. Returns decoded length or -1.
int b64url_decode(const char* in, uint8_t* out, size_t out_cap) {
  if (!in) return -1;
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
  };
  size_t n = strlen(in);
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; i++) {
    const int v = val(in[i]);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= out_cap) return -1;
      out[o++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return (int)o;
}

Pin* find_pin(const char* device_id) {
  for (int i = 0; i < MAX_PINS; i++)
    if (s_pins[i].used && strcmp(s_pins[i].id, device_id) == 0) return &s_pins[i];
  return nullptr;
}

#ifdef ARDUINO
// Persisted value layout: "<device_id>:<64-hex-pubkey>" so a hash-key
// collision is detectable (id prefix mismatch => ignore stored value).
void persist_pin(const Pin& p) {
  Preferences prefs;
  if (!prefs.begin("cvtrust", /*readOnly=*/false)) return;
  char key[16];
  nvs_key_for(p.id, key, sizeof(key));
  char val[48 + 1 + 64 + 1];
  char hex[65];
  static const char H[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    hex[2 * i]     = H[(p.pubkey[i] >> 4) & 0xF];
    hex[2 * i + 1] = H[p.pubkey[i] & 0xF];
  }
  hex[64] = '\0';
  snprintf(val, sizeof(val), "%s:%s", p.id, hex);
  prefs.putString(key, val);
  prefs.end();
}

bool load_pin(const char* device_id, uint8_t out[32]) {
  Preferences prefs;
  if (!prefs.begin("cvtrust", /*readOnly=*/true)) return false;
  char key[16];
  nvs_key_for(device_id, key, sizeof(key));
  String val = prefs.getString(key, "");
  prefs.end();
  if (val.length() != (int)(strlen(device_id) + 1 + 64)) return false;
  if (strncmp(val.c_str(), device_id, strlen(device_id)) != 0) return false;
  if (val[strlen(device_id)] != ':') return false;
  return hex_decode(val.c_str() + strlen(device_id) + 1, out, 32);
}
#endif

}  // namespace

void init() {
  // Pins hydrate lazily per device (load_pin) — nothing to enumerate here;
  // the RAM table just starts empty each boot.
  for (int i = 0; i < MAX_PINS; i++) s_pins[i] = Pin{};
#ifdef ARDUINO
  // Factory-fresh NVS: a pure read-only begin() fails while the namespace
  // doesn't exist yet, and read-only opens are the hot path (load_pin runs
  // on every chain/health arrival from a not-yet-pinned device). Touch the
  // namespace read-write once so every later read-only open succeeds.
  // (#843 review catch)
  Preferences prefs;
  if (prefs.begin("cvtrust", /*readOnly=*/false)) prefs.end();
#endif
}

bool note_pubkey(const char* device_id, const char* pubkey_hex) {
  if (!device_id || !device_id[0] || !pubkey_hex) return false;
  uint8_t key[32];
  if (!hex_decode(pubkey_hex, key, sizeof(key))) return false;

  Pin* p = find_pin(device_id);
  if (!p) {
    // Try the NVS pin first — a reboot must not re-TOFU.
    uint8_t stored[32];
    bool have_stored = false;
#ifdef ARDUINO
    have_stored = load_pin(device_id, stored);
#endif
    for (int i = 0; i < MAX_PINS; i++) {
      if (s_pins[i].used) continue;
      p = &s_pins[i];
      p->used = true;
      strncpy(p->id, device_id, sizeof(p->id) - 1);
      if (have_stored) {
        memcpy(p->pubkey, stored, 32);
      } else {
        memcpy(p->pubkey, key, 32);
#ifdef ARDUINO
        persist_pin(*p);
        log_line("TRUST", "Pinned new witness pubkey (TOFU).");
#endif
      }
      break;
    }
    if (!p) return false;  // table full
  }

  if (memcmp(p->pubkey, key, 32) != 0) {
    // A different key for a pinned identity. Never re-pin silently.
    if (!p->mismatch) {
#ifdef ARDUINO
      log_line("TRUST", "PUBKEY MISMATCH for pinned device — flagging.");
#endif
    }
    p->mismatch = true;
    return false;
  }
  return true;
}

canary::fleet::Badge evaluate_chain(const char* device_id,
                                    uint32_t length,
                                    const char* latest_hash_hex,
                                    const char* sig_b64url) {
  using canary::fleet::Badge;
  if (!device_id || !latest_hash_hex || !latest_hash_hex[0]) return Badge::Unknown;
  if (!sig_b64url || !sig_b64url[0]) return Badge::Unsigned;

  Pin* p = find_pin(device_id);
#ifdef ARDUINO
  if (!p) {
    // Chain can arrive before health on a fresh subscribe; check NVS.
    uint8_t stored[32];
    if (load_pin(device_id, stored)) {
      for (int i = 0; i < MAX_PINS; i++) {
        if (s_pins[i].used) continue;
        p = &s_pins[i];
        p->used = true;
        strncpy(p->id, device_id, sizeof(p->id) - 1);
        memcpy(p->pubkey, stored, 32);
        break;
      }
    }
  }
#endif
  if (!p) return Badge::Signed;          // signature present, no pin yet
  if (p->mismatch) return Badge::Failed; // identity conflict is a fail, loudly

  // Rebuild the locked canonical (see signature.py / device_signature.cpp).
  char canonical[192];
  const int n = snprintf(canonical, sizeof(canonical), "%s|v%d|chain|%s|%lu|%s",
                         SIG_PREFIX, SCHEMA_V, device_id,
                         (unsigned long)length, latest_hash_hex);
  if (n <= 0 || (size_t)n >= sizeof(canonical)) return Badge::Failed;

  uint8_t sig[64];
  if (b64url_decode(sig_b64url, sig, sizeof(sig)) != 64) return Badge::Failed;

#ifdef ARDUINO
  const bool ok = Ed25519::verify(sig, p->pubkey,
                                  (const uint8_t*)canonical, (size_t)n);
  return ok ? Badge::Verified : Badge::Failed;
#else
  // Host build: no crypto backend wired; report Signed so host tests can
  // exercise the pin/canonical plumbing without pulling an Ed25519 impl.
  return Badge::Signed;
#endif
}

int pinned_count() {
  int n = 0;
  for (int i = 0; i < MAX_PINS; i++) if (s_pins[i].used) n++;
  return n;
}

bool pinned_pubkey_hex(const char* device_id, char out[65]) {
  if (!device_id || !out) return false;
  const Pin* p = find_pin(device_id);
  if (!p || p->mismatch) return false;
  static const char H[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[2 * i]     = H[(p->pubkey[i] >> 4) & 0xF];
    out[2 * i + 1] = H[p->pubkey[i] & 0xF];
  }
  out[64] = '\0';
  return true;
}

}  // namespace canary::trust
