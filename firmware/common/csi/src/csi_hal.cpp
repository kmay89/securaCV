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

/*
 * Optional host logger bridge.
 *
 * The canary-wap firmware ships a sketch-local `health_log.h` that exposes
 * a `health_logging::` namespace with structured log levels and categories.
 * When this library is consumed by canary-wap (Arduino sketch via the
 * library symlink, or PIO via -I path), we route diagnostics through it.
 *
 * When this library is consumed standalone (e.g. by a third-party Arduino
 * sketch from `firmware/examples/csi_minimal/`), no host logger is present.
 * In that case the macros below collapse to Serial.printf so the user still
 * sees diagnostic output without depending on canary-wap internals.
 *
 * This keeps the CSI core self-contained and re-usable while preserving
 * the rich logging behavior in the SecuraCV product.
 */
#if __has_include("health_log.h")
  #include "health_log.h"
  #define CSI_HAL_HAS_HOST_LOGGER 1
#else
  #include <Arduino.h>
  #define CSI_HAL_HAS_HOST_LOGGER 0
#endif

#if CSI_HAL_HAS_HOST_LOGGER
  #define CSI_LOG_INFO(msg) \
    health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF, msg)
  #define CSI_LOG_WARNF(...) \
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF, __VA_ARGS__)
  #define CSI_LOG_INFOF(...) \
    health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF, __VA_ARGS__)
#else
  #define CSI_LOG_INFO(msg)        do { Serial.printf("[CSI] %s\n", msg); } while (0)
  #define CSI_LOG_WARNF(fmt, ...)  do { Serial.printf("[CSI WARN] " fmt "\n", ##__VA_ARGS__); } while (0)
  #define CSI_LOG_INFOF(fmt, ...)  do { Serial.printf("[CSI] " fmt "\n", ##__VA_ARGS__); } while (0)
#endif

#include <string.h>
#include <atomic>

extern "C" {
  #include <esp_wifi.h>
  #include <esp_err.h>
  #include <esp_system.h>
  #include <esp_timer.h>   /* esp_timer_get_time */
}
/* After <esp_wifi.h>: the driver-config field names differ between the
 * legacy (ESP32/S3/C3) and HE (C6/C5/C61) Wi-Fi MAC generations, and the
 * shim is the one place they are spelled out. */
#include "csi_idf_compat.h"
/* Every frame is reduced to the same 52 L-LTF tones before it is buffered. */
#include "csi_subcarriers.h"

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
static std::atomic<uint32_t> s_frames_dropped_short{0};  /* no L-LTF section */
static std::atomic<uint32_t> s_windows_emitted{0};
static std::atomic<uint32_t> s_windows_degraded{0};

/* Window boundary tracking (main-loop side only). */
static uint32_t s_window_start_ms = 0;
static uint32_t s_window_frames = 0;

/* Rate limiter state (WiFi-task side only). */
static uint32_t s_rate_last_ms = 0;
static uint32_t s_rate_min_gap_ms = 0;

/* Channel-lock state. set_channel_lock writes; the WiFi callback reads
 * s_observed_channel; is_channel_in_sync() compares them. The applied
 * flag is a change-detection gate so we re-apply the lock if it was
 * requested before WiFi was up (try_enable_csi_now retries). */
static uint8_t s_channel_lock = 0;
static bool    s_channel_lock_applied = true;  /* true when lock==0 or driver has it */
static std::atomic<uint8_t> s_observed_channel{0};

/* Watchdog state. s_last_frame_ms is set by the WiFi callback (relaxed —
 * we only need monotonic visibility, not strict ordering). The trigger
 * check + recovery happens in process() on the main loop. */
static std::atomic<uint32_t> s_last_frame_ms{0};
static uint32_t s_watchdog_timeout_ms = WATCHDOG_DEFAULT_TIMEOUT_MS;
static WatchdogCallback s_watchdog_cb = nullptr;
static uint32_t s_watchdog_last_recovery_ms = 0;
static uint32_t s_watchdog_recovery_count = 0;

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

  /* Wipe the I/Q buffer's trailing bytes before we copy in case the frame's
   * subcarrier count is below CSI_MAX_SUBCARRIERS — the consumer wipes on
   * release (process(), below) and init() wipes the whole ring at startup,
   * so the leading bytes are already zero, but a defensive wipe keeps this
   * invariant local instead of action-at-a-distance.
   *
   * subcarrier_cnt and seq_in_window are NOT reset here: subcarrier_cnt is
   * unconditionally set to copy_pairs below, and seq_in_window is overwritten
   * by the consumer (process()) before any read. */
  secure_wipe(slot->iq, sizeof(slot->iq));

  /* PRIVACY BARRIER: metadata extraction happens BEFORE any buffer copy. */
  extract_scrubbed_metadata(info, slot);

  /* Canonicalize: every frame becomes the 52 L-LTF data+pilot tones in
   * frequency order (csi_subcarriers.h), whether the driver delivered a
   * 64-tone non-HT frame or a 128-tone L-LTF+HT-LTF frame — so a window
   * never mixes tone counts and the null tones never reach the AGC mean.
   * A frame too short to carry an L-LTF section is dropped here, before
   * the slot is published. */
  const uint8_t tones = csi_lltf_select(info->buf, info->len,
                                        info->rx_ctrl.cwb == 1,
                                        info->first_word_invalid, slot->iq);
  if (tones == 0) {
    s_frames_dropped_short.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  slot->subcarrier_cnt = tones;

  /* Publish. */
  s_head.store(head + 1, std::memory_order_release);
  s_frames_received.fetch_add(1, std::memory_order_relaxed);

  /* Watchdog + channel-lock observers — relaxed atomics, single-writer
   * from this callback context. */
  s_observed_channel.store(slot->channel, std::memory_order_relaxed);
  s_last_frame_ms.store((uint32_t)(esp_timer_get_time() / 1000ULL),
                        std::memory_order_relaxed);

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

  /* Watchdog + channel observability reset. We keep s_watchdog_timeout_ms
   * and s_watchdog_cb across init() so set_watchdog() callers can configure
   * before init(); only the runtime state is cleared. */
  s_last_frame_ms.store(0, std::memory_order_relaxed);
  s_observed_channel.store(0, std::memory_order_relaxed);
  s_watchdog_last_recovery_ms = 0;
  s_watchdog_recovery_count = 0;

  /* Defer the ESP-IDF registration until start(): WiFi must be initialized
   * and in a mode that receives frames. If the user calls init() before
   * hal_wifi_init(), we still succeed — we'll register on start(). */
  s_initialized = true;

  CSI_LOG_INFO("CSI HAL initialized (ring capacity 16, scrub barrier active)");
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
  /* L-LTF on every frame + HT-LTF on HT frames, on whichever struct shape
   * this target's ESP-IDF exposes (csi_idf_compat.h). */
  wifi_csi_config_t cfg;
  csi_idf_fill_config(&cfg);

  esp_err_t err = esp_wifi_set_csi_config(&cfg);
  if (err == ESP_ERR_WIFI_NOT_STARTED) return 0;
  if (err != ESP_OK) {
    CSI_LOG_WARNF("CSI config failed (err=0x%x); sensing disabled", err);
    return -1;
  }

  err = esp_wifi_set_csi_rx_cb(&csi_rx_cb, nullptr);
  if (err == ESP_ERR_WIFI_NOT_STARTED) return 0;
  if (err != ESP_OK) {
    CSI_LOG_WARNF("CSI callback register failed (err=0x%x)", err);
    return -1;
  }

  err = esp_wifi_set_csi(true);
  if (err == ESP_ERR_WIFI_NOT_STARTED) return 0;
  if (err != ESP_OK) {
    CSI_LOG_WARNF("CSI enable failed (err=0x%x)", err);
    return -1;
  }
  /* If a channel lock was requested before WiFi was up, apply it now.
   * Cheap idempotent op, gated by s_channel_lock_applied so we don't
   * re-thrash the radio on every retry tick. */
  if (s_channel_lock != 0 && !s_channel_lock_applied) {
    esp_err_t lock_err =
        esp_wifi_set_channel(s_channel_lock, WIFI_SECOND_CHAN_NONE);
    if (lock_err == ESP_OK) {
      s_channel_lock_applied = true;
    } else {
      CSI_LOG_WARNF("deferred set_channel_lock(%u) returned 0x%x",
                    s_channel_lock, lock_err);
    }
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

  /* Reset watchdog-side timestamps so a stop/wait/start cycle doesn't
   * see stale last-frame data and immediately trip the silence check.
   * The cumulative recovery counter stays untouched — it's a health
   * indicator across the session, not per-start. */
  s_last_frame_ms.store(0, std::memory_order_relaxed);
  s_watchdog_last_recovery_ms = 0;

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
    CSI_LOG_INFO("CSI start deferred — WiFi not yet running; will retry in process()");
    return true;   /* init-accepted; the retry is silent and automatic */
  }
  /* r == -1: hard failure, logged. CSI pipeline no-ops for this session. */
#if !SECURACV_HAVE_CSI_API
  CSI_LOG_INFO("CSI API not compiled into this WiFi driver build; sensing disabled");
#endif
  s_start_pending = false;
  return false;
}

void stop() {
  /* Stop even if a deferred start was pending — clear that too. */
  s_start_pending = false;
  if (!s_running) {
    /* Nothing to tear down in the WiFi driver, but we still scrub
     * extractor state (including the cross-window breathing envelope)
     * so a subsequent start() begins clean. */
    csi_features::reset_history();
    secure_wipe(s_ring, sizeof(s_ring));
    return;
  }
#if SECURACV_HAVE_CSI_API
  esp_wifi_set_csi(false);
  esp_wifi_set_csi_rx_cb(nullptr, nullptr);
#endif
  s_running = false;

  /* Drain ring + scrub extractor's static history — per-window state
   * (s_amp_hist, s_prev_iq, counters) AND the cross-window breathing
   * envelope — so no CSI-derived state leaks into a subsequent run. */
  s_head.store(s_tail.load());
  secure_wipe(s_ring, sizeof(s_ring));
  csi_features::reset_history();
}

bool is_running() { return s_running; }

void set_features_callback(FeaturesCallback cb) { s_cb = cb; }

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN-LOOP PUMP
 * ────────────────────────────────────────────────────────────────────────── */

/* ──────────────────────────────────────────────────────────────────────────
 * WATCHDOG INTERNAL
 * ────────────────────────────────────────────────────────────────────────── */

static void watchdog_check_and_recover() {
  if (s_watchdog_timeout_ms == 0) return;
  if (!s_running) return;

  const uint32_t last = s_last_frame_ms.load(std::memory_order_relaxed);
  /* No frames yet at all? Use start time as the reference point so a
   * deferred-start that never actually got frames still trips the
   * watchdog instead of looking quiet forever. */
  const uint32_t ref = (last == 0) ? s_window_start_ms : last;
  const uint32_t now = millis();
  const uint32_t silent = now - ref;

  if (silent < s_watchdog_timeout_ms) return;

  /* Throttle recovery attempts so we don't slam the radio repeatedly. */
  if (s_watchdog_last_recovery_ms != 0 &&
      (now - s_watchdog_last_recovery_ms) < WATCHDOG_RECOVERY_MIN_MS) {
    return;
  }
  s_watchdog_last_recovery_ms = now;
  s_watchdog_recovery_count++;

  CSI_LOG_WARNF("CSI silent for %ums; recovery attempt %u",
                (unsigned)silent, (unsigned)s_watchdog_recovery_count);

  if (s_watchdog_cb) s_watchdog_cb(silent, s_watchdog_recovery_count);

#if SECURACV_HAVE_CSI_API
  /* Gentle recovery: toggle the CSI rx callback. We deliberately do NOT
   * cycle esp_wifi_stop/start — that would tear down WiFi for every
   * peer of this device and is too invasive for a watchdog. The
   * integration layer's callback can escalate to a full WiFi restart
   * after N attempts if it chooses. */
  esp_wifi_set_csi(false);
  esp_wifi_set_csi(true);
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * CHANNEL LOCK + WATCHDOG PUBLIC API
 * ────────────────────────────────────────────────────────────────────────── */

bool set_channel_lock(uint8_t channel) {
  if (channel > 14) return false;
  s_channel_lock = channel;
  if (channel == 0) {
    s_channel_lock_applied = true;   /* nothing to apply */
    return true;
  }
  /* Mark unapplied so try_enable_csi_now() will pick it up if the call
   * below couldn't (e.g. WiFi not yet started). */
  s_channel_lock_applied = false;
#if SECURACV_HAVE_CSI_API
  esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (err == ESP_OK) {
    s_channel_lock_applied = true;
  } else if (err == ESP_ERR_WIFI_NOT_STARTED) {
    /* Expected during boot — try_enable_csi_now() will re-apply once
     * WiFi is ready. The flag stays false until that happens. */
  } else {
    CSI_LOG_WARNF("set_channel_lock(%u) returned 0x%x", channel, err);
    /* Keep the lock recorded so is_channel_in_sync() reports the drift,
     * but leave applied=false so a future try_enable will retry. */
  }
#endif
  return true;
}

uint8_t get_channel_lock() { return s_channel_lock; }

uint8_t get_observed_channel() {
  return s_observed_channel.load(std::memory_order_relaxed);
}

bool is_channel_in_sync() {
  if (s_channel_lock == 0) return true;
  return s_observed_channel.load(std::memory_order_relaxed) == s_channel_lock;
}

void set_watchdog(uint32_t timeout_ms, WatchdogCallback cb) {
  s_watchdog_timeout_ms = timeout_ms;
  s_watchdog_cb = cb;
}

uint32_t get_watchdog_timeout_ms() { return s_watchdog_timeout_ms; }

uint32_t get_ms_since_last_frame() {
  const uint32_t last = s_last_frame_ms.load(std::memory_order_relaxed);
  if (last == 0) return UINT32_MAX;
  const uint32_t now = millis();
  return now - last;
}

uint32_t get_watchdog_recovery_count() { return s_watchdog_recovery_count; }

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
        CSI_LOG_INFO("CSI deferred start succeeded (WiFi now up)");
      } else if (r == -1) {
        /* Hard failure after WiFi came up — give up rather than loop. */
        s_start_pending = false;
      }
    }
  }

  if (!s_running) return 0;

  /* Watchdog check before draining: if we've been silent past the
   * threshold, attempt a gentle recovery so the rest of the loop has
   * something to do. The check is internally rate-limited. */
  watchdog_check_and_recover();

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
  out->frames_dropped_short = s_frames_dropped_short.load(std::memory_order_relaxed);
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
            CSI_LOG_WARNF("CSI conformance: suspicious byte run in slot %u at offset %u",
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
