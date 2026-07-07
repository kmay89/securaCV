/**
 * @file vault_snapshot.cpp
 * @brief Sealed-snapshot vault implementation. See header for the contract.
 *
 * Crypto construction (identical primitive set to mesh_network.cpp and the
 * Rust kernel's vault): per snapshot, ephemeral X25519 keypair → ECDH with
 * the operator's registered public key → HKDF-SHA256 (salt = ephemeral_pub
 * || operator_pub, info = "securacv/vault/seal/v1") → ChaCha20-Poly1305
 * with the 64-byte file header as AAD. The device stores only the
 * operator's PUBLIC key: sealing is write-only escrow by construction.
 * tools/unseal_snapshot.py implements the inverse off-device.
 */

#include "build_config.h"

#if FEATURE_VAULT_SNAPSHOT

#include "vault_snapshot.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <esp_camera.h>
#include <esp_random.h>

#include <Crypto.h>
#include <Curve25519.h>
#include <ChaChaPoly.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_util.h>  /* mbedtls_platform_zeroize */

#include "nvs_store.h"
#include "health_log.h"
#include "vault_events_module.h"
#include "csi_event.h"   /* csi_event_current_bucket (loop task) */

/* Ring rotation lives in data_mgmt_api.h, which transitively includes the
 * single-TU hardware_state.h (defines g_hw) — not includable here. The .ino
 * (the one TU that owns both) provides this thin hook. */
extern uint32_t vault_rotate_dir_hook(const char* dir, uint32_t keep);

/* Camera power manager hook (canary_wap.ino): wakes a standby camera and
 * stamps last-use. Runs on the seal worker task, where a ~1 s re-init is
 * harmless; returns false when the camera cannot be brought up. */
extern bool vault_camera_wake_hook(void);

namespace vault_snapshot {

using vault_logic::Trigger;
using vault_logic::VaultConfig;
using vault_logic::Decision;

/* ── Persisted state (loaded once; written by API handlers) ─────────── */

/* NVS keys (namespace "securacv", all <= 15 chars). */
static const char* NVS_KEY_PUB    = "vault_pub";
static const char* NVS_KEY_T3    = "vault_t3";
static const char* NVS_KEY_T4    = "vault_t4";
static const char* NVS_KEY_GLASS = "vault_glass";
static const char* NVS_KEY_MOT   = "vault_mot";
static const char* NVS_KEY_MESH  = "vault_mesh";
static const char* NVS_KEY_COOL  = "vault_cool_s";
static const char* NVS_KEY_SEQ   = "vault_seq";

static VaultConfig g_cfg = {false, false, false, false, false,
                            vault_logic::DEFAULT_COOLDOWN_S};
static uint8_t g_pubkey[vault_logic::PUBKEY_SIZE];
static bool    g_has_pubkey = false;
static uint8_t g_key_id[vault_logic::KEY_ID_SIZE];

/* Per-trigger cooldown stamps — loop-task-only (written at CAPTURE
 * decision time, read by the next decision). Index by Trigger value 1..3. */
static uint32_t g_last_capture_ms[vault_logic::COOLDOWN_SLOTS] = {0};
static bool     g_has_last[vault_logic::COOLDOWN_SLOTS]        = {false};

/* ── Worker handoff (single in-flight seal) ─────────────────────────── */

struct SealJob {
  Trigger  trigger;
  uint8_t  time_bucket;
  uint32_t seq;
  uint8_t  pubkey[vault_logic::PUBKEY_SIZE];  /* copied at request time */
  uint8_t  key_id[vault_logic::KEY_ID_SIZE];
};

struct SealResult {
  bool     ok;
  Trigger  trigger;
  uint32_t ct_len;
  char     ct_hash_hex16[17];
  char     filename[32];
  char     fail_reason[32];
};

/* Set by the loop before spawn, cleared by the loop after adoption. The
 * worker only writes g_result and then release-stores g_seal_done. */
static volatile bool g_worker_busy = false;
static volatile bool g_seal_done   = false;
static SealJob       g_job;         /* written by loop pre-spawn only */
static SealResult    g_result;      /* written by worker pre-done only */

/* ── Small helpers ──────────────────────────────────────────────────── */

static void sha256_key_id(const uint8_t pub[vault_logic::PUBKEY_SIZE],
                          uint8_t out[vault_logic::KEY_ID_SIZE]) {
  uint8_t digest[32];
  mbedtls_sha256(pub, vault_logic::PUBKEY_SIZE, digest, 0);
  memcpy(out, digest, vault_logic::KEY_ID_SIZE);
}

static bool hex_nibble(char c, uint8_t* out) {
  if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0');      return true; }
  if (c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return true; }
  if (c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return true; }
  return false;
}

static void persist_config() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return;
  nvs.putBool(NVS_KEY_T3,    g_cfg.t3_enabled);
  nvs.putBool(NVS_KEY_T4,    g_cfg.t4_enabled);
  nvs.putBool(NVS_KEY_GLASS, g_cfg.glass_enabled);
  nvs.putBool(NVS_KEY_MOT,   g_cfg.motion_enabled);
  nvs.putBool(NVS_KEY_MESH,  g_cfg.mesh_enabled);
  nvs.putUInt(NVS_KEY_COOL,  g_cfg.cooldown_s);
}

/* ── The seal worker (one-shot task; never the loop) ────────────────── */

static void fail_result(const char* reason) {
  g_result.ok = false;
  snprintf(g_result.fail_reason, sizeof(g_result.fail_reason), "%s", reason);
}

static void seal_task(void*) {
  /* Everything lives in a nested scope: vTaskDelete(NULL) never returns, so
   * C++ destructors (File's shared_ptr, ChaChaPoly) only run if the scope
   * closes first. */
  {
  const SealJob job = g_job;  /* private copy; loop won't rewrite while busy */
  memset(&g_result, 0, sizeof(g_result));
  g_result.trigger = job.trigger;

  uint8_t* staging = nullptr;
  uint32_t staging_len = 0;
  uint8_t* outbuf  = nullptr;
  File     f;
  char     tmp_path[48];
  char     final_path[48];
  bool     tmp_created = false;

  /* Init here, free in the cleanup block — an early break between starts()
   * and finish() must not leak the context. */
  mbedtls_sha256_context ct_sha;
  mbedtls_sha256_init(&ct_sha);

  /* Sensitive material — zeroized on every exit path below. */
  uint8_t eph_priv[32];
  uint8_t eph_pub[32];
  uint8_t shared[32];
  uint8_t key[32];

  do {
    /* 1. Wake the camera if the idle/battery manager put it in standby,
     * then grab one frame. Concurrent with the peek stream is fine
     * (fb_get is a shared queue); QR contention was excluded by the
     * decision. */
    if (!vault_camera_wake_hook()) { fail_result("camera_wake"); break; }
    camera_fb_t* fb = nullptr;
    for (int attempt = 0; attempt < 3 && !fb; attempt++) {
      fb = esp_camera_fb_get();
      if (!fb) vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (!fb) { fail_result("frame_capture"); break; }
    if (fb->len == 0 || fb->len > vault_logic::MAX_CIPHERTEXT) {
      esp_camera_fb_return(fb);
      fail_result("frame_size");
      break;
    }
    const uint32_t plain_len = (uint32_t)fb->len;

    /* Copy to PSRAM and release the framebuffer immediately — the camera
     * driver's queue must not be held for the SD write's duration. */
    staging = (uint8_t*)heap_caps_malloc(plain_len, MALLOC_CAP_SPIRAM);
    if (!staging) {
      esp_camera_fb_return(fb);
      fail_result("psram_alloc");
      break;
    }
    staging_len = plain_len;
    memcpy(staging, fb->buf, plain_len);
    esp_camera_fb_return(fb);

    /* 2. Sealed-box key agreement. */
    esp_fill_random(eph_priv, sizeof(eph_priv));
    eph_priv[0]  &= 248;   /* X25519 clamping */
    eph_priv[31] &= 127;
    eph_priv[31] |= 64;
    /* eval(result, scalar, point); point == NULL means the base point. */
    if (!Curve25519::eval(eph_pub, eph_priv, nullptr)) {
      fail_result("ecdh_base");
      break;
    }
    uint8_t pub_copy[32];
    memcpy(pub_copy, job.pubkey, 32);
    if (!Curve25519::eval(shared, eph_priv, pub_copy)) {
      fail_result("ecdh_shared");  /* low-order/all-zero rejected by eval */
      break;
    }

    uint8_t salt[64];
    memcpy(salt,      eph_pub,    32);
    memcpy(salt + 32, job.pubkey, 32);
    static const char INFO[] = "securacv/vault/seal/v1";
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_hkdf(md, salt, sizeof(salt), shared, sizeof(shared),
                            (const uint8_t*)INFO, sizeof(INFO) - 1, key, 32) != 0) {
      fail_result("hkdf");
      break;
    }

    /* 3. Header (also the AAD). */
    vault_logic::SealHeader h;
    memset(&h, 0, sizeof(h));
    h.trigger     = (uint8_t)job.trigger;
    h.time_bucket = job.time_bucket;
    memcpy(h.key_id, job.key_id, vault_logic::KEY_ID_SIZE);
    memcpy(h.ephemeral_pub, eph_pub, 32);
    esp_fill_random(h.nonce, vault_logic::NONCE_SIZE);
    h.ct_len = plain_len;

    uint8_t header_raw[vault_logic::HEADER_SIZE];
    vault_logic::header_build(h, header_raw);

    /* 4. Stream-encrypt to a temp file, then rename (atomic-enough for
     * FAT: a power cut leaves either no final file or a complete one). */
    char name[32];
    vault_logic::filename_build(job.seq, job.trigger, name, sizeof(name));
    snprintf(final_path, sizeof(final_path), "/VAULT/%s", name);
    snprintf(tmp_path, sizeof(tmp_path), "/VAULT/seal_tmp.bin");
    snprintf(g_result.filename, sizeof(g_result.filename), "%s", name);

    if (!SD.exists("/VAULT") && !SD.mkdir("/VAULT")) {
      fail_result("mkdir");
      break;
    }
    SD.remove(tmp_path);  /* stale tmp from an interrupted seal */
    f = SD.open(tmp_path, FILE_WRITE);
    if (!f) { fail_result("sd_open"); break; }
    tmp_created = true;

    if (f.write(header_raw, sizeof(header_raw)) != sizeof(header_raw)) {
      fail_result("sd_write");
      break;
    }

    ChaChaPoly aead;
    if (!aead.setKey(key, 32)) { fail_result("aead_key"); break; }
    aead.setIV(h.nonce, vault_logic::NONCE_SIZE);
    aead.addAuthData(header_raw, sizeof(header_raw));

    mbedtls_sha256_starts(&ct_sha, 0);

    constexpr size_t CHUNK = 4096;
    outbuf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!outbuf) { fail_result("psram_alloc"); break; }

    bool write_ok = true;
    for (uint32_t off = 0; off < plain_len; off += CHUNK) {
      const size_t n = (plain_len - off > CHUNK) ? CHUNK
                                                 : (size_t)(plain_len - off);
      aead.encrypt(outbuf, staging + off, n);
      mbedtls_sha256_update(&ct_sha, outbuf, n);
      if (f.write(outbuf, n) != n) { write_ok = false; break; }
    }
    if (!write_ok) {
      fail_result("sd_write");
      break;
    }

    uint8_t tag[vault_logic::TAG_SIZE];
    aead.computeTag(tag, sizeof(tag));
    aead.clear();
    if (f.write(tag, sizeof(tag)) != sizeof(tag)) {
      fail_result("sd_write");
      break;
    }
    f.close();

    uint8_t ct_digest[32];
    mbedtls_sha256_finish(&ct_sha, ct_digest);
    for (int i = 0; i < 8; i++) {
      sprintf(g_result.ct_hash_hex16 + 2 * i, "%02x", ct_digest[i]);
    }

    SD.remove(final_path);  /* seq collision after NVS rollback — replace */
    if (!SD.rename(tmp_path, final_path)) {
      fail_result("sd_rename");
      break;
    }
    tmp_created = false;

    g_result.ct_len = plain_len;
    g_result.ok = true;
  } while (false);

  /* Cleanup — every path. The staging held PLAINTEXT (the only raw frame
   * copy outside the camera driver); wipe before freeing. Wipes use
   * mbedtls_platform_zeroize, which the compiler cannot elide the way it
   * may a plain memset of about-to-die storage (CodeQL cpp/memset-may-be-
   * deleted). */
  if (f) f.close();
  if (tmp_created) SD.remove(tmp_path);
  if (staging) {
    mbedtls_platform_zeroize(staging, staging_len);
    heap_caps_free(staging);
  }
  if (outbuf) heap_caps_free(outbuf);
  mbedtls_sha256_free(&ct_sha);
  mbedtls_platform_zeroize(eph_priv, sizeof(eph_priv));
  mbedtls_platform_zeroize(shared,   sizeof(shared));
  mbedtls_platform_zeroize(key,      sizeof(key));
  }  /* nested scope closes: File / ChaChaPoly destructors run HERE,
      * before vTaskDelete makes this frame immortal. */

  __atomic_store_n(&g_seal_done, true, __ATOMIC_RELEASE);
  /* g_worker_busy stays set until the loop adopts the result, so a second
   * trigger can't interleave before rotation + witness emit happen. */
  vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────── */

void init() {
  NvsManager& nvs = NvsManager::instance();
  /* On a factory-fresh unit the namespace doesn't exist yet and a pure
   * read-only open fails; fall back to read-write, which creates it. The
   * defaults below are the fresh-unit truth either way (all off, no key). */
  if (!nvs.beginReadOnly() && !nvs.beginReadWrite()) return;
  g_cfg.t3_enabled    = nvs.getBool(NVS_KEY_T3, false);
  g_cfg.t4_enabled    = nvs.getBool(NVS_KEY_T4, false);
  g_cfg.glass_enabled  = nvs.getBool(NVS_KEY_GLASS, false);
  g_cfg.motion_enabled = nvs.getBool(NVS_KEY_MOT, false);
  g_cfg.mesh_enabled   = nvs.getBool(NVS_KEY_MESH, false);
  uint32_t cool = nvs.getUInt(NVS_KEY_COOL, vault_logic::DEFAULT_COOLDOWN_S);
  if (cool < 10)   cool = 10;
  if (cool > 3600) cool = 3600;
  g_cfg.cooldown_s = (uint16_t)cool;

  if (nvs.getBytesLength(NVS_KEY_PUB) == vault_logic::PUBKEY_SIZE) {
    nvs.getBytes(NVS_KEY_PUB, g_pubkey, vault_logic::PUBKEY_SIZE);
    sha256_key_id(g_pubkey, g_key_id);
    g_has_pubkey = true;
  }
}

Decision request_capture(Trigger t, bool camera_ok, bool qr_active,
                         bool sd_ok) {
  const uint8_t idx = (uint8_t)t;
  const bool has_last =
      (idx < vault_logic::COOLDOWN_SLOTS) ? g_has_last[idx] : false;
  const uint32_t last =
      (idx < vault_logic::COOLDOWN_SLOTS) ? g_last_capture_ms[idx] : 0;

  const Decision d = vault_logic::capture_decision(
      t, g_cfg, g_has_pubkey, sd_ok, camera_ok, qr_active,
      __atomic_load_n(&g_worker_busy, __ATOMIC_ACQUIRE),
      millis(), last, has_last);

  if (d != Decision::CAPTURE) {
    /* Disabled triggers skip silently (the common, intended case); every
     * other refusal is a configured-but-degraded state worth a log line. */
    if (d != Decision::SKIP_DISABLED) {
      log_health(SCV_LOG_WARNING, SCV_CAT_SENSOR,
                 "Vault capture skipped", vault_logic::decision_name(d));
    }
    return d;
  }

  /* Sequence number survives reboots (NVS). */
  NvsManager& nvs = NvsManager::instance();
  uint32_t seq = 1;
  if (nvs.beginReadWrite()) {
    seq = nvs.getUInt(NVS_KEY_SEQ, 0) + 1;
    nvs.putUInt(NVS_KEY_SEQ, seq);
  }

  g_job.trigger     = t;
  g_job.time_bucket = csi_event_current_bucket();  /* loop task — safe */
  g_job.seq         = seq;
  memcpy(g_job.pubkey, g_pubkey, sizeof(g_job.pubkey));
  memcpy(g_job.key_id, g_key_id, sizeof(g_job.key_id));

  /* Stamp the cooldown at request time: the alarm cadence keeps re-firing
   * while the seal is in flight, and worker_busy alone would stop gating
   * the moment the worker finishes. */
  if (idx < vault_logic::COOLDOWN_SLOTS) {
    g_last_capture_ms[idx] = millis();
    g_has_last[idx]        = true;
  }

  __atomic_store_n(&g_worker_busy, true, __ATOMIC_RELEASE);
  /* Internal-RAM stack (prebuilt core can't put task stacks in PSRAM);
   * priority 1 — the seal is not latency-sensitive. */
  if (xTaskCreate(seal_task, "vault_seal", 8192, nullptr, 1, nullptr)
      != pdPASS) {
    __atomic_store_n(&g_worker_busy, false, __ATOMIC_RELEASE);
    log_health(SCV_LOG_WARNING, SCV_CAT_SENSOR,
               "Vault capture skipped", "task_create");
    return Decision::SKIP_WORKER_BUSY;
  }
  return Decision::CAPTURE;
}

void poll_completion() {
  if (!__atomic_load_n(&g_seal_done, __ATOMIC_ACQUIRE)) return;
  const SealResult r = g_result;  /* worker is gone; safe to copy */
  __atomic_store_n(&g_seal_done, false, __ATOMIC_RELEASE);

  if (r.ok) {
    /* Chain existence + integrity through the chokepoint (loop task —
     * the emit contract). The note is pure integrity data: the first 16
     * hex chars of SHA-256(ciphertext), checkable off-device with
     * `unseal_snapshot.py inspect`. */
    vault_events_emit_frame_sealed(vault_logic::trigger_tag(r.trigger),
                                   r.ct_hash_hex16);
    log_health(SCV_LOG_NOTICE, SCV_CAT_SENSOR,
               "Alarm snapshot sealed", r.filename);
    /* Bound the ring AFTER the new file landed so the oldest one is the
     * casualty, never the newest evidence. */
    (void)vault_rotate_dir_hook("/VAULT", vault_logic::KEEP_FILES);
  } else {
    log_health(SCV_LOG_WARNING, SCV_CAT_SENSOR,
               "Alarm snapshot seal failed", r.fail_reason);
  }

  __atomic_store_n(&g_worker_busy, false, __ATOMIC_RELEASE);
}

bool set_pubkey_hex(const char* hex64) {
  if (!hex64) return false;
  /* Exact-length gate before any indexed access. (hex_nibble would reject
   * the terminating NUL and short-circuit anyway, but the explicit check
   * makes the bound obvious and rejects overlong input up front.) */
  if (strlen(hex64) != 2 * vault_logic::PUBKEY_SIZE) return false;
  uint8_t pub[vault_logic::PUBKEY_SIZE];
  for (size_t i = 0; i < vault_logic::PUBKEY_SIZE; i++) {
    uint8_t hi, lo;
    if (!hex_nibble(hex64[2 * i], &hi) || !hex_nibble(hex64[2 * i + 1], &lo)) {
      return false;
    }
    pub[i] = (uint8_t)((hi << 4) | lo);
  }

  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  if (nvs.putBytes(NVS_KEY_PUB, pub, sizeof(pub)) != sizeof(pub)) return false;

  memcpy(g_pubkey, pub, sizeof(g_pubkey));
  sha256_key_id(g_pubkey, g_key_id);
  g_has_pubkey = true;
  log_health(SCV_LOG_NOTICE, SCV_CAT_USER, "Vault unlock key registered", nullptr);
  return true;
}

void clear_pubkey() {
  NvsManager& nvs = NvsManager::instance();
  if (nvs.beginReadWrite()) {
    nvs.remove(NVS_KEY_PUB);
  }
  memset(g_pubkey, 0, sizeof(g_pubkey));
  memset(g_key_id, 0, sizeof(g_key_id));
  g_has_pubkey = false;
  /* A vault without a recipient must not stay armed. */
  g_cfg.t3_enabled = g_cfg.t4_enabled = g_cfg.glass_enabled = false;
    g_cfg.motion_enabled = g_cfg.mesh_enabled = false;
  persist_config();
  log_health(SCV_LOG_NOTICE, SCV_CAT_USER,
             "Vault unlock key cleared", "triggers disabled");
}

bool has_pubkey() { return g_has_pubkey; }

void key_id_hex(char* out) {
  if (!g_has_pubkey) { out[0] = '\0'; return; }
  for (size_t i = 0; i < vault_logic::KEY_ID_SIZE; i++) {
    sprintf(out + 2 * i, "%02x", g_key_id[i]);
  }
}

VaultConfig get_config() { return g_cfg; }

void set_config(const VaultConfig& cfg) {
  g_cfg = cfg;
  if (g_cfg.cooldown_s < 10)   g_cfg.cooldown_s = 10;
  if (g_cfg.cooldown_s > 3600) g_cfg.cooldown_s = 3600;
  /* No key, no arming — the UI disables the toggles, but the API must
   * enforce it too (fail closed). */
  if (!g_has_pubkey) {
    g_cfg.t3_enabled = g_cfg.t4_enabled = g_cfg.glass_enabled = false;
    g_cfg.motion_enabled = g_cfg.mesh_enabled = false;
  }
  persist_config();
}

bool worker_busy() {
  return __atomic_load_n(&g_worker_busy, __ATOMIC_ACQUIRE);
}

bool validate_name(const char* name) {
  uint32_t seq;
  Trigger t;
  return vault_logic::filename_parse(name, &seq, &t);
}

int list_items(ItemInfo* out, int max_items) {
  int count = 0;
  File dir = SD.open("/VAULT");
  if (!dir || !dir.isDirectory()) return 0;
  File entry;
  while (count < max_items && (entry = dir.openNextFile())) {
    const char* base = strrchr(entry.name(), '/');
    base = base ? base + 1 : entry.name();
    uint32_t seq;
    Trigger t;
    if (vault_logic::filename_parse(base, &seq, &t)) {
      ItemInfo* it = &out[count];
      snprintf(it->name, sizeof(it->name), "%s", base);
      it->trigger = (uint8_t)t;
      it->size    = (uint32_t)entry.size();
      it->time_bucket = 255;
      uint8_t raw[vault_logic::HEADER_SIZE];
      if (entry.read(raw, sizeof(raw)) == sizeof(raw)) {
        vault_logic::SealHeader h;
        if (vault_logic::header_parse(raw, &h)) it->time_bucket = h.time_bucket;
      }
      count++;
    }
    entry.close();
  }
  dir.close();
  return count;
}

bool delete_item(const char* name) {
  if (!validate_name(name)) return false;
  char path[48];
  snprintf(path, sizeof(path), "/VAULT/%s", name);
  return SD.remove(path);
}

}  // namespace vault_snapshot

#endif  /* FEATURE_VAULT_SNAPSHOT */
