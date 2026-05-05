/*
 * SecuraCV Canary — IR remote-control activity (RMT RX)
 *
 * Wraps the legacy IDF 4.4 RMT driver (driver/rmt.h) — works under
 * arduino-esp32 v2.0.14. Decodes the leader pulses of NEC / RC5 / Sony,
 * derives a per-session-salted 4-bit hash of the payload, and emits
 * appliance_ir_activity events. No raw IR codes ever leave the module.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_ir.h"

#include <Arduino.h>
#include <string.h>

#include "log_level.h"
#include "securacv_witness.h"   /* log_health() */

extern "C" {
  #include <driver/rmt.h>
  #include <esp_err.h>
  #include <esp_random.h>
  #include <freertos/FreeRTOS.h>
  #include <freertos/ringbuf.h>
  #include <mbedtls/md.h>
}

namespace ir {

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool s_initialized = false;
static bool s_running = false;
static ir_config_t s_cfg = IR_CONFIG_DEFAULT;
static ir_event_cb_t s_cb = nullptr;

/* RX channel — RMT_CHANNEL_4..7 are RX-capable on ESP32-S3; we pick
 * channel 4 to leave 0..3 free for any TX use elsewhere in firmware. */
static constexpr rmt_channel_t IR_RMT_CHAN = RMT_CHANNEL_4;
static RingbufHandle_t s_rb = nullptr;

/* Per-session salt for the privacy hash, set in init() from esp_random.
 * Rotates on every boot — same physical remote produces a different
 * hash bucket across reboots, so cross-session correlation is
 * structurally impossible. */
static uint8_t s_salt[32] = {0};

/* Storm cap: at most one event per 250 ms (a remote auto-repeating at
 * 110 ms cadence would otherwise flood the dashboard). */
static uint32_t s_last_event_ms = 0;

static ir_stats_t s_stats = {0};

/* ──────────────────────────────────────────────────────────────────────────
 * SECURITY
 * ────────────────────────────────────────────────────────────────────────── */

static void secure_wipe(void* p, size_t n) {
  volatile uint8_t* b = (volatile uint8_t*)p;
  while (n--) *b++ = 0;
  asm volatile("" ::: "memory");
}

/* HMAC-SHA256(salt, msg) → bottom 4 bits.
 * Failure path returns 0 — a stuck-zero bucket is the most defensive
 * default (no information leakage). */
static uint8_t hash_bucket(const uint8_t* msg, size_t n) {
  uint8_t mac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) { mbedtls_md_free(&ctx); return 0; }
  if (mbedtls_md_setup(&ctx, info, /*hmac=*/1) != 0) {
    mbedtls_md_free(&ctx); return 0;
  }
  if (mbedtls_md_hmac_starts(&ctx, s_salt, sizeof(s_salt)) != 0 ||
      mbedtls_md_hmac_update(&ctx, msg, n) != 0 ||
      mbedtls_md_hmac_finish(&ctx, mac) != 0) {
    mbedtls_md_free(&ctx);
    secure_wipe(mac, sizeof(mac));
    return 0;
  }
  mbedtls_md_free(&ctx);
  const uint8_t bucket = mac[0] & 0x0F;
  secure_wipe(mac, sizeof(mac));
  return bucket;
}

/* ──────────────────────────────────────────────────────────────────────────
 * RMT BRING-UP / TEAR-DOWN
 * ────────────────────────────────────────────────────────────────────────── */

static bool rmt_open() {
  rmt_config_t cfg = {};
  cfg.rmt_mode = RMT_MODE_RX;
  cfg.channel  = IR_RMT_CHAN;
  cfg.gpio_num = (gpio_num_t)s_cfg.gpio_num;
  cfg.mem_block_num = 1;
  /* 80 MHz APB / 80 = 1 MHz tick = 1 µs resolution. */
  cfg.clk_div = 80;
  cfg.rx_config.filter_en = true;
  cfg.rx_config.filter_ticks_thresh =
      (uint8_t)(s_cfg.filter_threshold_us > 255 ? 255 : s_cfg.filter_threshold_us);
  cfg.rx_config.idle_threshold = s_cfg.idle_threshold_us;

  esp_err_t err = rmt_config(&cfg);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "config err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "IR: rmt_config failed", d);
    return false;
  }

  err = rmt_driver_install(IR_RMT_CHAN, /*rx_buf_size=*/1024, 0);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "install err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "IR: rmt_driver_install failed", d);
    return false;
  }

  err = rmt_get_ringbuf_handle(IR_RMT_CHAN, &s_rb);
  if (err != ESP_OK || s_rb == nullptr) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "IR: ringbuf handle failed", nullptr);
    rmt_driver_uninstall(IR_RMT_CHAN);
    s_rb = nullptr;
    return false;
  }

  err = rmt_rx_start(IR_RMT_CHAN, /*rx_idx_rst=*/true);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "start err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "IR: rmt_rx_start failed", d);
    rmt_driver_uninstall(IR_RMT_CHAN);
    s_rb = nullptr;
    return false;
  }
  return true;
}

static void rmt_close() {
  if (s_running) {
    rmt_rx_stop(IR_RMT_CHAN);
  }
  if (s_rb) {
    rmt_driver_uninstall(IR_RMT_CHAN);
    s_rb = nullptr;
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * PROTOCOL DECODERS
 *
 * All three decoders below take the raw RMT item array and return a
 * tuple { protocol, ok, payload_bytes, payload_len, confidence }.
 * Item layout: each rmt_item32_t represents one (mark, space) pair,
 * each duration is 15 bits in microseconds (since clk_div=80).
 * ────────────────────────────────────────────────────────────────────────── */

struct DecodeResult {
  ir_protocol_t protocol;
  bool          ok;
  uint8_t       payload[8];
  uint8_t       payload_len;
  uint8_t       confidence;
};

static inline bool near(uint16_t v, uint16_t target, uint16_t tol) {
  const int32_t d = (int32_t)v - (int32_t)target;
  return (d >= -((int32_t)tol)) && (d <= (int32_t)tol);
}

/* NEC: 9 ms mark + 4.5 ms space leader, then 32 data bits, each
 * { 562 µs mark, 562 µs space (=0) | 1690 µs space (=1) }, then a
 * trailing 562 µs mark. Total 67 RMT items. Repeat frames are
 * 9 ms + 2.25 ms + 562 µs (3 items). */
static DecodeResult decode_nec(const rmt_item32_t* items, size_t n) {
  DecodeResult r = { IR_PROTOCOL_UNKNOWN, false, {0}, 0, 0 };
  if (n < 1) return r;
  /* Repeat frame (3 items): 9 ms mark, 2.25 ms space, 562 µs mark. */
  if (n == 3 &&
      near(items[0].duration0, 9000, 1500) &&
      near(items[0].duration1, 2250, 500)) {
    r.protocol = IR_PROTOCOL_NEC;
    r.ok = true;
    r.payload_len = 0;       /* repeat — no fresh payload */
    r.confidence = 70;
    return r;
  }
  if (n < 33) return r;
  if (!near(items[0].duration0, 9000, 1500) ||
      !near(items[0].duration1, 4500, 1000)) return r;
  /* 32 data bits across items[1..32]. Each item's duration0 is the
   * 562 µs mark; duration1 distinguishes 0 vs 1. */
  uint32_t code = 0;
  int errors = 0;
  for (int i = 1; i <= 32; i++) {
    if (!near(items[i].duration0, 562, 250)) errors++;
    bool one;
    if (near(items[i].duration1, 1690, 500)) one = true;
    else if (near(items[i].duration1, 562, 250)) one = false;
    else { errors++; one = false; }
    code = (code << 1) | (one ? 1u : 0u);
  }
  r.protocol = IR_PROTOCOL_NEC;
  r.ok = (errors <= 4);
  if (!r.ok) return r;
  r.payload[0] = (uint8_t)(code >> 24);
  r.payload[1] = (uint8_t)(code >> 16);
  r.payload[2] = (uint8_t)(code >> 8);
  r.payload[3] = (uint8_t)(code & 0xFF);
  r.payload_len = 4;
  r.confidence = (uint8_t)(100 - errors * 7);
  return r;
}

/* Sony SIRC: 2.4 ms mark / 0.6 ms space leader, then 12/15/20 bits,
 * each { 1200 µs mark = 1, 600 µs mark = 0, with 600 µs spaces }. */
static DecodeResult decode_sony(const rmt_item32_t* items, size_t n) {
  DecodeResult r = { IR_PROTOCOL_UNKNOWN, false, {0}, 0, 0 };
  if (n < 13) return r;
  if (!near(items[0].duration0, 2400, 500) ||
      !near(items[0].duration1, 600,  200)) return r;
  /* 12, 15, or 20 data bits; allow any of those. */
  size_t bits = 0;
  if      (n >= 21) bits = 20;
  else if (n >= 16) bits = 15;
  else              bits = 12;
  uint32_t code = 0;
  int errors = 0;
  for (size_t i = 1; i <= bits; i++) {
    bool one;
    if (near(items[i].duration0, 1200, 300)) one = true;
    else if (near(items[i].duration0, 600, 200)) one = false;
    else { errors++; one = false; }
    code = (code << 1) | (one ? 1u : 0u);
  }
  r.protocol = IR_PROTOCOL_SONY;
  r.ok = (errors <= 2);
  if (!r.ok) return r;
  r.payload[0] = (uint8_t)bits;
  r.payload[1] = (uint8_t)(code >> 16);
  r.payload[2] = (uint8_t)(code >> 8);
  r.payload[3] = (uint8_t)(code & 0xFF);
  r.payload_len = 4;
  r.confidence = (uint8_t)(100 - errors * 10);
  return r;
}

/* RC5 (Philips): biphase Manchester with 889 µs half-bit. We implement
 * a leader-only check + count of items so we can flag the protocol
 * family without a full Manchester decoder; the hash bucket then runs
 * over the raw item-count + a short prefix of pulse durations. This is
 * sufficient for "RC5 remote pressed" semantics. */
static DecodeResult decode_rc5(const rmt_item32_t* items, size_t n) {
  DecodeResult r = { IR_PROTOCOL_UNKNOWN, false, {0}, 0, 0 };
  if (n < 12 || n > 24) return r;
  /* RC5's first transition is the start bit; it's about 889 µs.
   * Accept anything in 700–1100 µs and check the second item too. */
  if (!near(items[0].duration0, 889, 250)) return r;
  if (!near(items[0].duration1, 889, 250)) return r;
  /* Build a deterministic 4-byte signature from the durations of the
   * first 4 items so the hash differs per RC5 keypress. */
  r.protocol = IR_PROTOCOL_RC5;
  r.ok = true;
  r.payload[0] = (uint8_t)(items[0].duration0 / 32);
  r.payload[1] = (uint8_t)(items[1].duration0 / 32);
  r.payload[2] = (uint8_t)(items[2].duration0 / 32);
  r.payload[3] = (uint8_t)(n & 0xFF);
  r.payload_len = 4;
  r.confidence = 60;
  return r;
}

/* ──────────────────────────────────────────────────────────────────────────
 * EVENT EMITTER
 * ────────────────────────────────────────────────────────────────────────── */

static void try_emit(const DecodeResult& dr, uint32_t now_ms) {
  if ((now_ms - s_last_event_ms) < 250) return;
  s_last_event_ms = now_ms;

  uint8_t msg[16] = {0};
  /* Mix protocol id into the hash so two different protocols with the
   * same payload bytes don't collide. */
  msg[0] = (uint8_t)dr.protocol;
  size_t n = 1;
  for (size_t i = 0; i < dr.payload_len && n < sizeof(msg); i++) {
    msg[n++] = dr.payload[i];
  }
  const uint8_t bucket = hash_bucket(msg, n);
  secure_wipe(msg, sizeof(msg));

  ir_event_t evt = {};
  evt.event_type  = 1;
  evt.category    = (uint8_t)dr.protocol;
  evt.hash_bucket = bucket;
  evt.confidence  = dr.confidence;
  evt.time_bucket = (uint8_t)((now_ms / (10UL * 60UL * 1000UL)) % 144);

  if (s_cb) s_cb(&evt);
  s_stats.events_emitted++;

  secure_wipe(&evt, sizeof(evt));
}

}  /* namespace ir */

/* ──────────────────────────────────────────────────────────────────────────
 * C API
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" {

bool ir_init(const ir_config_t* config) {
  using namespace ir;
  if (s_initialized) return true;
  s_cfg = config ? *config : (ir_config_t)IR_CONFIG_DEFAULT;
  if (s_cfg.idle_threshold_us == 0)   s_cfg.idle_threshold_us   = 9500;
  if (s_cfg.filter_threshold_us == 0) s_cfg.filter_threshold_us = 100;

  /* Generate the per-session salt. esp_random is hardware-RNG-backed. */
  for (size_t i = 0; i < sizeof(s_salt); i += 4) {
    const uint32_t r = esp_random();
    s_salt[i + 0] = (uint8_t)(r >> 24);
    s_salt[i + 1] = (uint8_t)(r >> 16);
    s_salt[i + 2] = (uint8_t)(r >> 8);
    s_salt[i + 3] = (uint8_t)(r >> 0);
  }
  memset(&s_stats, 0, sizeof(s_stats));
  s_last_event_ms = 0;
  s_initialized = true;
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
             "IR HAL initialized", "RMT RX, NEC/RC5/Sony decoders");
  return true;
}

void ir_deinit(void) {
  using namespace ir;
  if (!s_initialized) return;
  if (s_running) ir_stop();
  secure_wipe(s_salt, sizeof(s_salt));
  s_cb = nullptr;
  s_initialized = false;
}

bool ir_start(void) {
  using namespace ir;
  if (!s_initialized) return false;
  if (s_running) return true;
  if (!rmt_open()) return false;
  s_running = true;
  return true;
}

void ir_stop(void) {
  using namespace ir;
  if (!s_running) return;
  rmt_close();
  s_running = false;
}

bool ir_is_running(void) { return ir::s_running; }

void ir_set_event_callback(ir_event_cb_t cb) { ir::s_cb = cb; }

int ir_process(void) {
  using namespace ir;
  if (!s_running || s_rb == nullptr) return 0;

  int frames_this_call = 0;
  /* Drain up to 4 frames per process() call so a slow main loop can
   * catch up without blocking. */
  for (int i = 0; i < 4; i++) {
    size_t length = 0;
    rmt_item32_t* items =
        (rmt_item32_t*)xRingbufferReceive(s_rb, &length, /*ticks=*/0);
    if (items == nullptr) break;

    s_stats.frames_received++;
    frames_this_call++;
    const size_t n = length / sizeof(rmt_item32_t);

    if (n >= 1) {
      const uint32_t now = millis();
      DecodeResult dr = decode_nec(items, n);
      if (!dr.ok) dr = decode_sony(items, n);
      if (!dr.ok) dr = decode_rc5(items, n);

      if (dr.ok) {
        s_stats.frames_decoded++;
        if (dr.payload_len > 0) {
          /* NEC repeat frames have payload_len=0; we only emit on
           * fresh keypresses to keep the dashboard quiet. */
          try_emit(dr, now);
        }
      } else {
        s_stats.frames_unknown++;
      }
      /* Wipe the decode result so the raw payload doesn't outlive
       * the call. */
      secure_wipe(&dr, sizeof(dr));
    }

    vRingbufferReturnItem(s_rb, (void*)items);
  }

  return frames_this_call;
}

bool ir_get_stats(ir_stats_t* out) {
  using namespace ir;
  if (!out) return false;
  *out = s_stats;
  return true;
}

const char* ir_protocol_name(uint8_t cat) {
  switch (cat) {
    case IR_PROTOCOL_UNKNOWN: return "unknown";
    case IR_PROTOCOL_NEC:     return "nec";
    case IR_PROTOCOL_RC5:     return "rc5";
    case IR_PROTOCOL_SONY:    return "sony";
    default:                  return "?";
  }
}

}  /* extern "C" */
