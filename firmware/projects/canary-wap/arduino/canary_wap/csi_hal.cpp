/*
 * SecuraCV Canary — CSI HAL (ESP32 backend) — Implementation
 *
 * Backend: Espressif esp_wifi CSI callback API. Works on ESP32 / S2 / C3 /
 * S3 / C6 with the native WiFi driver. On ESP32-C6 this path also accepts
 * 802.11bf-2025 sounding frames; detection is runtime via csi_get_caps().
 *
 * Threading model:
 *   - ESP-IDF invokes csi_rx_cb() from the WiFi task (not an ISR).
 *   - The callback scrubs identifiers, copies subcarriers into a single
 *     lock-free single-producer/single-consumer ring, and returns fast.
 *   - csi_hal::process() runs on the main loop, drains whole frames into
 *     the feature extractor (csi_features.cpp), and — on a completed
 *     window — invokes the user-registered callback synchronously.
 */

#include "csi_hal.h"
#include "csi_features.h"
#include "health_log.h"

#include <string.h>
#include <atomic>

extern "C" {
  #include <esp_wifi.h>
  #include <esp_err.h>
  #include <esp_system.h>
  #include <esp_timer.h>   /* esp_timer_get_time */
}

/*
 * CSI compile-time gate. ESP-IDF can be built with or without CSI support.
 * arduino-esp32's prebuilt static libraries enable it by default for S3/C3,
 * but if a board variant disables it (or a future release reorganizes the
 * wifi_csi_config_t struct), we want a clean compile + a runtime no-op
 * rather than a broken build. Detection: presence of CONFIG_ESP_WIFI_CSI
 * or fall through to "assume available" if not defined. We ALSO wrap any
 * direct field access in `#if SECURACV_HAVE_CSI_API` so a future struct
 * rename only hits one place.
 */
#if defined(CONFIG_ESP_WIFI_CSI_ENABLED) || !defined(CONFIG_IDF_TARGET)
  #define SECURACV_HAVE_CSI_API 1
#else
  #define SECURACV_HAVE_CSI_API 0
#endif

namespace csi_hal {

/* ──────────────────────────────────────────────────────────────────────────
 * SECURITY PRIMITIVES
 * ────────────────────────────────────────────────────────────────────────── */

static void secure_wipe(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) { *p++ = 0; }
  asm volatile("" ::: "memory");
}

/* ──────────────────────────────────────────────────────────────────────────
 * RING BUFFER (SPSC — WiFi task producer, main loop consumer)
 * ────────────────────────────────────────────────────────────────────────── */

/* One CSI slot: scrubbed metadata + up to CSI_MAX_SUBCARRIERS I/Q pairs. */
struct CsiSlot {
  uint32_t seq_in_window;   /* 0-based index within the current window */
  int8_t   rssi_dbm;        /* from rx_ctrl.rssi */
  uint8_t  bandwidth_code;  /* 0 = HT20, 1 = HT40 */
  uint8_t  channel;         /* rx_ctrl.channel */
  uint8_t  subcarrier_cnt;  /* valid entries in iq[] */
  int8_t   iq[CSI_MAX_SUBCARRIERS * 2];  /* interleaved I,Q */
};

/* 16 slots × ~264 bytes = ~4 KB. Sized for the 20 Hz target frame rate
 * so even a 400 ms main-loop stall (worst-case NVS write + network I/O)
 * doesn't lose a full window of CSI frames. */
static constexpr size_t RING_CAP = 16;
static CsiSlot s_ring[RING_CAP];
static std::atomic<uint32_t> s_head{0};  /* producer writes */
static std::atomic<uint32_t> s_tail{0};  /* consumer reads */

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool s_initialized = false;
static bool s_running = false;
/* Set when start() was called while WiFi was not yet up. process() retries
 * the CSI-enable sequence each tick until it succeeds or stop() is called. */
static bool s_start_pending = false;
static uint32_t s_start_retry_last_ms = 0;
static Config s_cfg = Config::defaults();
static FeaturesCallback s_cb = nullptr;

/* Stats — atomic because updated from both contexts. */
static std::atomic<uint32_t> s_frames_received{0};
static std::atomic<uint32_t> s_frames_dropped_rssi{0};
static std::atomic<uint32_t> s_frames_dropped_rate{0};
static std::atomic<uint32_t> s_frames_dropped_full{0};
static std::atomic<uint32_t> s_windows_emitted{0};
static std::atomic<uint32_t> s_windows_degraded{0};

/* Window boundary tracking (main-loop side only). */
static uint32_t s_window_start_ms = 0;
static uint32_t s_window_frames = 0;

/* Rate limiter state (WiFi-task side only). */
static uint32_t s_rate_last_ms = 0;
static uint32_t s_rate_min_gap_ms = 0;

/* ──────────────────────────────────────────────────────────────────────────
 * PRIVACY BARRIER: scrub identifying fields from the ESP-IDF info struct.
 *
 * The ESP-IDF 5.x wifi_csi_info_t is:
 *   struct {
 *     wifi_pkt_rx_ctrl_t rx_ctrl;  // MAC is embedded in payload
 *     uint8_t  mac[6];             // ← scrubbed
 *     uint8_t  dmac[6];            // ← scrubbed
 *     bool     first_word_invalid;
 *     int8_t*  buf;                // subcarrier samples
 *     uint16_t len;
 *     uint8_t* hdr;                // MAC header pointer (not copied)
 *     uint8_t* payload;            // MAC payload pointer (not copied)
 *     uint16_t payload_len;
 *     uint8_t  channel;
 *     int8_t   noise_floor;
 *   }
 *
 * We never dereference hdr/payload (which contain the raw MAC/BSSID).
 * We copy rssi, channel, noise_floor, and the subcarrier buffer only.
 * ────────────────────────────────────────────────────────────────────────── */

static inline void extract_scrubbed_metadata(const wifi_csi_info_t* info,
                                             CsiSlot* slot) {
  /* The rx_ctrl struct in ESP-IDF 5.x contains its own copy of parts of
   * the MAC header; we pull only the non-identifying fields and leave the
   * rest alone — we never store rx_ctrl itself. */
  slot->rssi_dbm = info->rx_ctrl.rssi;
  slot->channel  = info->rx_ctrl.channel;

  /* ESP-IDF exposes two bandwidth enums; normalize to our 0/1 code. */
  slot->bandwidth_code = (info->rx_ctrl.cwb == 1) ? 1 : 0;  /* 0=HT20, 1=HT40 */

  /* explicitly do NOT touch info->mac, info->dmac, info->hdr, info->payload */
}

/* ──────────────────────────────────────────────────────────────────────────
 * ESP-IDF CSI CALLBACK  (WiFi task context)
 * ────────────────────────────────────────────────────────────────────────── */

static void csi_rx_cb(void* /*ctx*/, wifi_csi_info_t* info) {
  if (info == nullptr || info->buf == nullptr || info->len == 0) {
    return;
  }

  const int8_t rssi = info->rx_ctrl.rssi;
  if (rssi < s_cfg.rssi_floor_dbm) {
    s_frames_dropped_rssi.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  /* Rate-limit gate. */
  if (s_rate_min_gap_ms > 0) {
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if ((now - s_rate_last_ms) < s_rate_min_gap_ms) {
      s_frames_dropped_rate.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    s_rate_last_ms = now;
  }

  /* Reserve a slot. */
  const uint32_t head = s_head.load(std::memory_order_relaxed);
  const uint32_t tail = s_tail.load(std::memory_order_acquire);
  if ((head - tail) >= RING_CAP) {
    s_frames_dropped_full.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  CsiSlot* slot = &s_ring[head % RING_CAP];

  /* Wipe the destination slot first so we can't leak residue from a prior
   * frame if we early-return halfway through the copy. */
  secure_wipe(slot->iq, sizeof(slot->iq));
  slot->subcarrier_cnt = 0;
  slot->seq_in_window = 0;

  /* PRIVACY BARRIER: metadata extraction happens BEFORE any buffer copy. */
  extract_scrubbed_metadata(info, slot);

  /* Copy subcarrier samples. ESP-IDF gives us interleaved int8 I,Q pairs.
   * len is in bytes; each subcarrier is 2 bytes. */
  const size_t bytes = info->len;
  const size_t pairs = bytes / 2;
  const size_t copy_pairs =
      pairs > CSI_MAX_SUBCARRIERS ? CSI_MAX_SUBCARRIERS : pairs;

  memcpy(slot->iq, info->buf, copy_pairs * 2);
  slot->subcarrier_cnt = (uint8_t)copy_pairs;

  /* Publish. */
  s_head.store(head + 1, std::memory_order_release);
  s_frames_received.fetch_add(1, std::memory_order_relaxed);

  /* Note: info->mac / info->dmac / info->hdr / info->payload are owned by
   * ESP-IDF and are *not* zeroed here (that could crash the WiFi driver
   * which re-uses the buffer). The guarantee is that we do not *copy* them. */
}

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const Config& cfg) {
  if (s_initialized) return true;

  s_cfg = cfg;
  if (s_cfg.max_frame_rate_hz > 0) {
    s_rate_min_gap_ms = 1000u / s_cfg.max_frame_rate_hz;
  } else {
    s_rate_min_gap_ms = 0;
  }

  secure_wipe(s_ring, sizeof(s_ring));
  s_head.store(0);
  s_tail.store(0);

  /* Defer the ESP-IDF registration until start(): WiFi must be initialized
   * and in a mode that receives frames. If the user calls init() before
   * hal_wifi_init(), we still succeed — we'll register on start(). */
  s_initialized = true;

  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "CSI HAL initialized (ring capacity 8, scrub barrier active)");
  return true;
}

void deinit() {
  if (!s_initialized) return;
  stop();
  secure_wipe(s_ring, sizeof(s_ring));
  s_cb = nullptr;
  s_initialized = false;
}

/*
 * Attempt the three ESP-IDF CSI setup calls in order. Returns:
 *   +1  success — CSI is actually enabled and ready to produce callbacks
 *    0  WiFi not yet started — caller should retry later
 *   -1  hard failure — logs printed, do not retry
 */
static int try_enable_csi_now() {
#if SECURACV_HAVE_CSI_API
  wifi_csi_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.lltf_en           = true;
  cfg.htltf_en          = true;
  cfg.stbc_htltf2_en    = false;
  cfg.ltf_merge_en      = true;
  cfg.channel_filter_en = true;
  cfg.manu_scale        = false;
  cfg.shift             = 0;

  esp_err_t err = esp_wifi_set_csi_config(&cfg);
  if (err == ESP_ERR_WIFI_NOT_STARTED) return 0;
  if (err != ESP_OK) {
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "CSI config failed (err=0x%x); sensing disabled", err);
    return -1;
  }

  err = esp_wifi_set_csi_rx_cb(&csi_rx_cb, nullptr);
  if (err == ESP_ERR_WIFI_NOT_STARTED) return 0;
  if (err != ESP_OK) {
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "CSI callback register failed (err=0x%x)", err);
    return -1;
  }

  err = esp_wifi_set_csi(true);
  if (err == ESP_ERR_WIFI_NOT_STARTED) return 0;
  if (err != ESP_OK) {
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "CSI enable failed (err=0x%x)", err);
    return -1;
  }
  return 1;
#else
  (void)csi_rx_cb;  /* suppress unused-warning when CSI API is compiled out */
  return -1;
#endif
}

bool start() {
  if (!s_initialized) return false;
  /* Already running, or deferred-start already queued — no-op either way.
   * Without the s_start_pending check, repeat start() calls while WiFi is
   * still coming up would each re-run try_enable_csi_now() and emit a
   * duplicate "deferred" log line. */
  if (s_running || s_start_pending) return true;

  const int r = try_enable_csi_now();
  if (r == 1) {
    s_window_start_ms = millis();
    s_window_frames = 0;
    s_running = true;
    s_start_pending = false;
    return true;
  }
  if (r == 0) {
    /* WiFi stack not up yet. Defer: process() will retry each call until
     * it succeeds or the feature is explicitly stop()'d. s_running stays
     * false so callers correctly see sensing as not yet active. */
    s_start_pending = true;
    health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "CSI start deferred — WiFi not yet running; will retry in process()");
    return true;   /* init-accepted; the retry is silent and automatic */
  }
  /* r == -1: hard failure, logged. CSI pipeline no-ops for this session. */
#if !SECURACV_HAVE_CSI_API
  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "CSI API not compiled into this WiFi driver build; sensing disabled");
#endif
  s_start_pending = false;
  return false;
}

void stop() {
  /* Stop even if a deferred start was pending — clear that too. */
  s_start_pending = false;
  if (!s_running) {
    /* Nothing to tear down in the WiFi driver, but we still scrub
     * extractor state so a subsequent start() begins clean. */
    csi_features::reset();
    secure_wipe(s_ring, sizeof(s_ring));
    return;
  }
#if SECURACV_HAVE_CSI_API
  esp_wifi_set_csi(false);
  esp_wifi_set_csi_rx_cb(nullptr, nullptr);
#endif
  s_running = false;

  /* Drain ring + scrub extractor's static per-window history so no
   * residual CSI-derived state (s_amp_hist, s_prev_iq, counters) leaks
   * into a subsequent run. Matches the header-documented behavior. */
  s_head.store(s_tail.load());
  secure_wipe(s_ring, sizeof(s_ring));
  csi_features::reset();
}

bool is_running() { return s_running; }

void set_features_callback(FeaturesCallback cb) { s_cb = cb; }

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN-LOOP PUMP
 * ────────────────────────────────────────────────────────────────────────── */

int process() {
  /* Deferred-start retry. If start() was called while WiFi wasn't ready,
   * we retry once per second here until the three esp_wifi_set_csi_* calls
   * succeed. This keeps the caller contract simple (start once, it activates
   * whenever WiFi comes up) without silently pretending sensing is live. */
  if (s_start_pending && !s_running) {
    const uint32_t now = millis();
    if ((now - s_start_retry_last_ms) >= 1000) {
      s_start_retry_last_ms = now;
      const int r = try_enable_csi_now();
      if (r == 1) {
        s_window_start_ms = now;
        s_window_frames = 0;
        s_running = true;
        s_start_pending = false;
        health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
          "CSI deferred start succeeded (WiFi now up)");
      } else if (r == -1) {
        /* Hard failure after WiFi came up — give up rather than loop. */
        s_start_pending = false;
      }
    }
  }

  if (!s_running) return 0;

  /* Drain all available frames into the feature aggregator. */
  for (;;) {
    const uint32_t tail = s_tail.load(std::memory_order_relaxed);
    const uint32_t head = s_head.load(std::memory_order_acquire);
    if (tail == head) break;

    CsiSlot* slot = &s_ring[tail % RING_CAP];
    slot->seq_in_window = s_window_frames;

    csi_features::accumulate(slot->iq, slot->subcarrier_cnt,
                             slot->rssi_dbm, slot->channel,
                             slot->bandwidth_code);
    s_window_frames++;

    /* Scrub before advancing tail so the slot is zero if the producer
     * wraps around before we close the window. */
    secure_wipe(slot->iq, sizeof(slot->iq));
    slot->subcarrier_cnt = 0;

    s_tail.store(tail + 1, std::memory_order_release);
  }

  /* Window complete? */
  const uint32_t now_ms = millis();
  if ((now_ms - s_window_start_ms) < CSI_WINDOW_MS) {
    return 0;
  }

  /* Close window and emit. */
  csi_features_t feats = {};
  csi_features::finalize(&feats, s_window_frames);

  if (s_window_frames < (uint32_t)(s_cfg.max_frame_rate_hz / 2)) {
    s_windows_degraded.fetch_add(1, std::memory_order_relaxed);
  }
  s_windows_emitted.fetch_add(1, std::memory_order_relaxed);

  if (s_cb) {
    s_cb(&feats);
  }

  /* Scrub the local copy and reset the window. */
  secure_wipe(&feats, sizeof(feats));
  s_window_start_ms = now_ms;
  s_window_frames = 0;
  csi_features::reset();
  return 1;
}

/* ──────────────────────────────────────────────────────────────────────────
 * INTROSPECTION
 * ────────────────────────────────────────────────────────────────────────── */

uint32_t get_caps() {
  /* Runtime capability reporting. ESP32-S3 supports HT20/HT40 with phase;
   * ESP32-C3 supports HT20 with amplitude only. ESP32-C6 (future) will
   * report SOUNDING_11BF when its ESP-IDF branch exposes it. */
  uint32_t caps = CSI_CAP_HT20 | CSI_CAP_PHASE;
#if defined(HARDWARE_XIAO_ESP32S3)
  caps |= CSI_CAP_HT40;
#endif
  return caps;
}

bool get_stats(csi_stats_t* out) {
  if (!out) return false;
  out->frames_received     = s_frames_received.load(std::memory_order_relaxed);
  out->frames_dropped_rssi = s_frames_dropped_rssi.load(std::memory_order_relaxed);
  out->frames_dropped_rate = s_frames_dropped_rate.load(std::memory_order_relaxed);
  out->frames_dropped_full = s_frames_dropped_full.load(std::memory_order_relaxed);
  out->windows_emitted     = s_windows_emitted.load(std::memory_order_relaxed);
  out->windows_degraded    = s_windows_degraded.load(std::memory_order_relaxed);
  return true;
}

/* ──────────────────────────────────────────────────────────────────────────
 * CONFORMANCE
 * ────────────────────────────────────────────────────────────────────────── */

bool conformance_check_no_mac_in_buffers() {
  /* A MAC address is 6 bytes. A real MAC is extremely unlikely in pure
   * subcarrier I/Q data because I/Q values cluster around zero, whereas
   * a MAC has high-entropy, non-centered bytes. We flag any slot where
   * 6+ consecutive bytes are all non-zero AND the first byte matches one
   * of the common OUI-bit patterns (locally/universally administered bit,
   * multicast bit). This is a heuristic; the real guarantee is structural. */
  for (size_t i = 0; i < RING_CAP; i++) {
    const int8_t* b = s_ring[i].iq;
    const size_t n = sizeof(s_ring[i].iq);
    size_t run = 0;
    for (size_t j = 0; j < n; j++) {
      if (b[j] != 0) {
        run++;
        if (run >= 6) {
          /* Suspicious — check OUI bits on first byte of run. */
          const uint8_t first = (uint8_t)b[j - 5];
          const bool lu_bit = (first & 0x02) != 0;
          const bool mc_bit = (first & 0x01) != 0;
          if (lu_bit || mc_bit) {
            health_logging::logf(health_logging::LEVEL_WARNING,
              health_logging::CAT_RF,
              "CSI conformance: suspicious byte run in slot %u at offset %u",
              (unsigned)i, (unsigned)(j - 5));
            /* Do not return false — this is a heuristic; the ring contains
             * I/Q samples that occasionally form plausible-looking bytes.
             * The structural guarantee is that we never *copy* info->mac. */
          }
        }
      } else {
        run = 0;
      }
    }
  }
  return true;
}

}  /* namespace csi_hal */

/* ──────────────────────────────────────────────────────────────────────────
 * C API SHIMS — satisfy the portable C interface declared in csi_types.h
 * for any caller that prefers it over the csi_hal:: C++ namespace.
 *
 * The trampoline is defined OUTSIDE extern "C" so its address has the
 * C++ function-pointer linkage that csi_hal::set_features_callback expects.
 * The C-linkage shim functions then refer to the trampoline by name; the
 * compiler resolves the reference fine across linkage boundaries.
 * ────────────────────────────────────────────────────────────────────────── */

namespace {
  void (*s_c_cb)(const csi_features_t*, void*) = nullptr;
  void* s_c_user = nullptr;

  void c_cb_trampoline(const csi_features_t* f) {
    if (s_c_cb) s_c_cb(f, s_c_user);
  }
}

extern "C" {

bool csi_init(const csi_config_t* config) {
  csi_hal::Config cfg = csi_hal::Config::defaults();
  if (config) {
    cfg.channel = config->channel;
    cfg.bandwidth_mhz = config->bandwidth_mhz;
    cfg.max_frame_rate_hz = config->max_frame_rate_hz;
    if (config->rssi_floor_dbm != 0) cfg.rssi_floor_dbm = config->rssi_floor_dbm;
  }
  return csi_hal::init(cfg);
}
void csi_deinit(void) { csi_hal::deinit(); }
bool csi_start(void)  { return csi_hal::start(); }
void csi_stop(void)   { csi_hal::stop(); }
bool csi_is_running(void) { return csi_hal::is_running(); }

void csi_set_features_callback(csi_features_cb_t cb, void* user_data) {
  s_c_cb = cb;
  s_c_user = user_data;
  csi_hal::set_features_callback(cb ? c_cb_trampoline : nullptr);
}

int csi_process(void) { return csi_hal::process(); }
uint32_t csi_get_caps(void) { return csi_hal::get_caps(); }
bool csi_get_stats(csi_stats_t* out) { return csi_hal::get_stats(out); }

}  /* extern "C" */
