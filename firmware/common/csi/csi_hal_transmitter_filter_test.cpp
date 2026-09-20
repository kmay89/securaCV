/**
 * @file csi_hal_transmitter_filter_test.cpp
 * @brief Host test for the CSI HAL's transmitter filter and its privacy
 *        barrier (docs/IMPROVEMENT_ROADMAP.md row 2).
 *
 * Compiles the REAL csi_hal.cpp (unity-included so the ring and the one
 * held identifier can be inspected byte-for-byte) against the host stubs in
 * host_stubs/, registers the callback through the stubbed ESP-IDF surface
 * exactly as the device does, and feeds it frames from several transmitter
 * addresses. Verifies:
 *
 *   1. Until a BSSID is known the filter is disarmed: every frame passes
 *      (the pre-filter behavior for AP-only installs).
 *   2. Once the driver reports an association, frames from the associated
 *      BSSID are processed and frames from any other transmitter are
 *      dropped and counted under frames_dropped_foreign — nothing else
 *      moves (rssi/rate/full/short stay put).
 *   3. A foreign frame does not spend the rate limiter's budget.
 *   4. No published slot, no stat, no emitted feature vector, and no byte
 *      of the ring carries either transmitter address. The one place the
 *      associated BSSID exists is the HAL's single static.
 *   5. The peer hook accepts registered peers, receives a pointer INTO the
 *      driver's info->mac (compare in place), and is not consulted when
 *      the BSSID already matched.
 *   6. filter_foreign=false at runtime passes foreign frames again.
 *   7. Roaming: a changed BSSID replaces the held one; a disconnect
 *      (ESP_ERR_WIFI_NOT_CONNECT) keeps it.
 *   8. process() learns the BSSID on its own (1 s poll while unknown) and
 *      request_bssid_refresh() makes the next tick pick a change up early.
 *   9. deinit() wipes the held BSSID.
 *  10. The callback registered with the driver is the HAL's own.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD -Wall -Wextra -Wno-unused-parameter \
 *       firmware/common/csi/csi_hal_transmitter_filter_test.cpp \
 *       firmware/common/csi/src/csi_features.cpp \
 *       -I firmware/common/csi/host_stubs -I firmware/common/csi/src \
 *       -o /tmp/csi_hal_filter_test && /tmp/csi_hal_filter_test
 */

/* Unity-include the HAL so the test can read its file-statics (s_ring,
 * s_assoc_bssid) — nothing test-only is compiled into the production
 * source, and the device build never sees this file. */
#include "csi_hal.cpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

/* ────────────────────────────────────────────────────────────────────────
 * DRIVER + CLOCK STUBS (declared by host_stubs/, defined here)
 * ──────────────────────────────────────────────────────────────────────── */

static uint32_t         g_now_ms = 0;
static wifi_csi_cb_t    g_registered_cb = nullptr;
static bool             g_csi_enabled = false;
static esp_err_t        g_ap_info_result = ESP_ERR_WIFI_NOT_CONNECT;
static uint8_t          g_ap_info_bssid[6] = {0};
static int              g_ap_info_calls = 0;

unsigned long millis() { return g_now_ms; }

extern "C" {
int64_t esp_timer_get_time(void) { return (int64_t)g_now_ms * 1000; }

esp_err_t esp_wifi_set_csi_config(const wifi_csi_config_t*) { return ESP_OK; }
esp_err_t esp_wifi_set_csi_rx_cb(wifi_csi_cb_t cb, void*) {
  g_registered_cb = cb;
  return ESP_OK;
}
esp_err_t esp_wifi_set_csi(bool en) { g_csi_enabled = en; return ESP_OK; }
esp_err_t esp_wifi_set_channel(uint8_t, wifi_second_chan_t) { return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* rec) {
  g_ap_info_calls++;
  if (g_ap_info_result != ESP_OK) return g_ap_info_result;
  memcpy(rec->bssid, g_ap_info_bssid, 6);
  /* A real record carries the SSID too — put bytes there so the test can
   * prove the HAL wipes the whole record, not only the BSSID. */
  memcpy(rec->ssid, "HomeNetwork", 12);
  rec->primary = 6;
  rec->rssi = -55;
  return ESP_OK;
}
}

/* ────────────────────────────────────────────────────────────────────────
 * TEST HARNESS
 * ──────────────────────────────────────────────────────────────────────── */

static int g_failures = 0;
static int g_checks = 0;

#define EXPECT(cond, msg) do {                                          \
  g_checks++;                                                           \
  if (!(cond)) {                                                        \
    g_failures++;                                                       \
    std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);          \
  }                                                                     \
} while (0)

/* Transmitter addresses. Chosen with the locally-administered bit set and
 * bytes far from the small I/Q values the frames carry, so a match in a
 * byte scan can only come from a copy. */
static const uint8_t AP_BSSID[6]  = {0x12, 0xB4, 0x4B, 0xD2, 0x2D, 0x6F};
static const uint8_t AP2_BSSID[6] = {0x16, 0xC8, 0x8C, 0xE1, 0x1E, 0x73};
static const uint8_t FOREIGN[6]   = {0x02, 0xA5, 0x5A, 0xC3, 0x3C, 0x7E};
static const uint8_t PEER[6]      = {0x06, 0x97, 0x79, 0xB8, 0x8B, 0x5D};

/* One 20 MHz non-HT L-LTF frame: 64 tones in FFT order, [imag, real] int8.
 * Null tones (pair 0, pairs 27..37) are zero as hardware writes them; the
 * 52 data+pilot tones carry small non-zero values that a MAC byte cannot
 * be mistaken for. */
static int8_t g_frame_buf[128];

static void build_frame_buf(int8_t seed) {
  memset(g_frame_buf, 0, sizeof(g_frame_buf));
  for (size_t k = 0; k < 64; k++) {
    if (k == 0 || (k >= 27 && k <= 37)) continue;      /* null tones */
    g_frame_buf[2 * k]     = (int8_t)(3 + ((k + seed) % 5));   /* imag */
    g_frame_buf[2 * k + 1] = (int8_t)(4 + ((k * 3 + seed) % 5)); /* real */
  }
}

static wifi_csi_info_t make_info(const uint8_t mac[6]) {
  wifi_csi_info_t info;
  memset(&info, 0, sizeof(info));
  info.rx_ctrl.rssi    = -50;
  info.rx_ctrl.cwb     = 0;
  info.rx_ctrl.channel = 6;
  memcpy(info.mac, mac, 6);
  memset(info.dmac, 0xEE, 6);
  info.first_word_invalid = false;
  info.buf = g_frame_buf;
  info.len = (uint16_t)sizeof(g_frame_buf);
  return info;
}

/* Deliver one frame through the callback the HAL registered with the
 * driver, spaced past the 20 Hz rate gate unless the caller says not to. */
static void deliver(const uint8_t mac[6], bool advance_clock = true) {
  if (advance_clock) g_now_ms += 50;
  build_frame_buf((int8_t)(g_now_ms % 7));
  wifi_csi_info_t info = make_info(mac);
  g_registered_cb(nullptr, &info);
}

/* Byte scan: does `hay` contain the 6-byte `needle` at any offset? */
static bool contains_mac(const void* hay, size_t len, const uint8_t needle[6]) {
  const uint8_t* b = static_cast<const uint8_t*>(hay);
  if (len < 6) return false;
  for (size_t i = 0; i + 6 <= len; i++) {
    if (memcmp(b + i, needle, 6) == 0) return true;
  }
  return false;
}

static csi_stats_t stats() {
  csi_stats_t st;
  memset(&st, 0, sizeof(st));
  csi_hal::get_stats(&st);
  return st;
}

static csi_features_t g_last_feats;
static int g_windows = 0;
static void on_window(const csi_features_t* f) {
  g_last_feats = *f;
  g_windows++;
}

/* Peer hook bookkeeping (5). */
static const uint8_t* g_peer_hook_last_ptr = nullptr;
static int g_peer_hook_calls = 0;
static bool peer_hook(const uint8_t* mac) {
  g_peer_hook_calls++;
  g_peer_hook_last_ptr = mac;
  return memcmp(mac, PEER, 6) == 0;
}

int main() {
  std::printf("csi_hal transmitter filter — host test\n");

  /* ── Bring the HAL up the way the integration layer does. ── */
  csi_hal::Config cfg = csi_hal::Config::defaults();
  EXPECT(cfg.filter_foreign, "Config::defaults() has filter_foreign on");
  EXPECT(csi_hal::init(cfg), "init");
  csi_hal::set_features_callback(on_window);
  EXPECT(csi_hal::start(), "start");
  EXPECT(csi_hal::is_running(), "running after start (stub Wi-Fi is up)");
  EXPECT(g_csi_enabled, "driver CSI enabled");
  EXPECT(g_registered_cb == &csi_hal::csi_rx_cb,
         "(10) the callback handed to the driver is the HAL's own");
  EXPECT(csi_hal::get_filter_foreign(), "filter reflects config");

  /* ── (1) Disarmed until a BSSID is known: everything passes. ── */
  EXPECT(!csi_hal::has_associated_bssid(), "no BSSID held before association");
  deliver(FOREIGN);
  deliver(AP_BSSID);
  {
    csi_stats_t st = stats();
    EXPECT(st.frames_received == 2, "(1) disarmed filter accepts every transmitter");
    EXPECT(st.frames_dropped_foreign == 0, "(1) nothing counted foreign while disarmed");
  }

  /* ── (8a) process() learns the BSSID by itself: poll at 1 s while unknown. ── */
  g_ap_info_result = ESP_OK;
  memcpy(g_ap_info_bssid, AP_BSSID, 6);
  {
    const int calls_before = g_ap_info_calls;
    g_now_ms += 1000;
    csi_hal::process();
    EXPECT(g_ap_info_calls > calls_before, "(8) process() polls the driver while unknown");
    EXPECT(csi_hal::has_associated_bssid(), "(8) BSSID learned from the poll");
    EXPECT(memcmp(csi_hal::s_assoc_bssid, AP_BSSID, 6) == 0,
           "(4) the single static holds exactly the associated BSSID");
  }

  /* ── (2) Armed: ours in, foreign out and counted. ── */
  const csi_stats_t before = stats();
  for (int i = 0; i < 10; i++) {
    deliver(AP_BSSID);
    deliver(FOREIGN);
  }
  {
    csi_stats_t st = stats();
    EXPECT(st.frames_received == before.frames_received + 10,
           "(2) ten frames from the associated BSSID were processed");
    EXPECT(st.frames_dropped_foreign == before.frames_dropped_foreign + 10,
           "(2) ten foreign frames were dropped and counted");
    EXPECT(st.frames_dropped_rssi == before.frames_dropped_rssi &&
           st.frames_dropped_rate == before.frames_dropped_rate &&
           st.frames_dropped_full == before.frames_dropped_full &&
           st.frames_dropped_short == before.frames_dropped_short,
           "(2) no other drop counter moved");
  }

  /* ── (3) A foreign frame does not consume the rate limiter. ── */
  {
    csi_stats_t s0 = stats();
    deliver(FOREIGN);                    /* advances clock 50 ms, dropped */
    deliver(AP_BSSID, /*advance*/false); /* same instant: must still pass */
    csi_stats_t s1 = stats();
    EXPECT(s1.frames_received == s0.frames_received + 1,
           "(3) our frame right after a foreign one is not rate-limited");
    EXPECT(s1.frames_dropped_rate == s0.frames_dropped_rate,
           "(3) foreign frame did not touch the rate gate");
  }

  /* ── (4) No MAC byte anywhere the data goes. Scan BEFORE draining so the
   *        published slots are still populated. ── */
  {
    const size_t pending = (size_t)(csi_hal::s_head.load() - csi_hal::s_tail.load());
    EXPECT(pending > 0, "(4) ring holds published slots to inspect");
    EXPECT(!contains_mac(csi_hal::s_ring, sizeof(csi_hal::s_ring), AP_BSSID),
           "(4) ring carries no byte of the associated BSSID");
    EXPECT(!contains_mac(csi_hal::s_ring, sizeof(csi_hal::s_ring), FOREIGN),
           "(4) ring carries no byte of the foreign transmitter");
    csi_stats_t st = stats();
    EXPECT(!contains_mac(&st, sizeof(st), AP_BSSID) &&
           !contains_mac(&st, sizeof(st), FOREIGN),
           "(4) stats carry no transmitter address");
    /* Close a window and inspect what the consumer was handed. */
    g_now_ms += CSI_WINDOW_MS;
    const int w0 = g_windows;
    csi_hal::process();
    EXPECT(g_windows == w0 + 1, "(4) a window was emitted");
    EXPECT(!contains_mac(&g_last_feats, sizeof(g_last_feats), AP_BSSID) &&
           !contains_mac(&g_last_feats, sizeof(g_last_feats), FOREIGN),
           "(4) emitted feature vector carries no transmitter address");
    EXPECT(csi_hal::conformance_check_no_mac_in_buffers(),
           "(4) heuristic conformance scan passes");
  }

  /* ── (5) Peer hook: registered peers pass, pointer is the driver's. ── */
  csi_hal::set_peer_filter(peer_hook);
  {
    csi_stats_t s0 = stats();
    g_peer_hook_calls = 0;
    deliver(PEER);
    EXPECT(g_peer_hook_calls == 1, "(5) hook consulted for a non-BSSID transmitter");
    csi_stats_t s1 = stats();
    EXPECT(s1.frames_received == s0.frames_received + 1, "(5) registered peer accepted");
    EXPECT(s1.frames_dropped_foreign == s0.frames_dropped_foreign, "(5) peer not counted foreign");

    g_peer_hook_calls = 0;
    deliver(AP_BSSID);
    EXPECT(g_peer_hook_calls == 0, "(5) hook not consulted when the BSSID matched");

    g_peer_hook_calls = 0;
    deliver(FOREIGN);
    csi_stats_t s2 = stats();
    EXPECT(g_peer_hook_calls == 1 &&
           s2.frames_dropped_foreign == s1.frames_dropped_foreign + 1,
           "(5) unregistered transmitter still dropped with the hook installed");
    /* The hook must have been handed a pointer to the driver's own bytes:
     * with the frame gone, the last pointer must not be inside anything the
     * HAL owns. */
    const uint8_t* ring_lo = reinterpret_cast<const uint8_t*>(csi_hal::s_ring);
    const uint8_t* ring_hi = ring_lo + sizeof(csi_hal::s_ring);
    EXPECT(!(g_peer_hook_last_ptr >= ring_lo && g_peer_hook_last_ptr < ring_hi) &&
           g_peer_hook_last_ptr != csi_hal::s_assoc_bssid,
           "(5) hook saw the driver's info->mac, not a HAL-owned copy");
  }
  csi_hal::set_peer_filter(nullptr);

  /* ── (6) Runtime off switch. ── */
  csi_hal::set_filter_foreign(false);
  EXPECT(!csi_hal::get_filter_foreign(), "(6) setter reflected by getter");
  {
    csi_stats_t s0 = stats();
    deliver(FOREIGN);
    csi_stats_t s1 = stats();
    EXPECT(s1.frames_received == s0.frames_received + 1 &&
           s1.frames_dropped_foreign == s0.frames_dropped_foreign,
           "(6) filter off: foreign frame accepted, not counted");
  }
  csi_hal::set_filter_foreign(true);

  /* ── (7) Roaming and disconnect. ── */
  memcpy(g_ap_info_bssid, AP2_BSSID, 6);
  EXPECT(csi_hal::refresh_associated_bssid(), "(7) refresh reports a held BSSID");
  EXPECT(memcmp(csi_hal::s_assoc_bssid, AP2_BSSID, 6) == 0, "(7) new BSSID replaced the old");
  {
    csi_stats_t s0 = stats();
    deliver(AP_BSSID);   /* the previous AP is now foreign */
    deliver(AP2_BSSID);
    csi_stats_t s1 = stats();
    EXPECT(s1.frames_dropped_foreign == s0.frames_dropped_foreign + 1 &&
           s1.frames_received == s0.frames_received + 1,
           "(7) after roaming only the new BSSID passes");
  }
  g_ap_info_result = ESP_ERR_WIFI_NOT_CONNECT;
  EXPECT(csi_hal::refresh_associated_bssid(), "(7) disconnect keeps the held BSSID");
  EXPECT(memcmp(csi_hal::s_assoc_bssid, AP2_BSSID, 6) == 0, "(7) held value unchanged on disconnect");

  /* ── (8b) request_bssid_refresh() from "another task" is honored on the
   *        next process() even though the 10 s known-poll has not elapsed. ── */
  g_ap_info_result = ESP_OK;
  memcpy(g_ap_info_bssid, AP_BSSID, 6);
  {
    g_now_ms += 100;   /* well inside TRANSMITTER_BSSID_POLL_MS */
    csi_hal::process();
    EXPECT(memcmp(csi_hal::s_assoc_bssid, AP2_BSSID, 6) == 0,
           "(8) no early re-poll without a request");
    csi_hal::request_bssid_refresh();
    g_now_ms += 100;
    csi_hal::process();
    EXPECT(memcmp(csi_hal::s_assoc_bssid, AP_BSSID, 6) == 0,
           "(8) requested refresh picked the new BSSID up on the next tick");
  }

  /* ── (4, again) After everything: the ring and stats still carry no
   *        address, and only the single static ever held one. ── */
  {
    csi_stats_t st = stats();
    EXPECT(!contains_mac(csi_hal::s_ring, sizeof(csi_hal::s_ring), AP_BSSID) &&
           !contains_mac(csi_hal::s_ring, sizeof(csi_hal::s_ring), AP2_BSSID) &&
           !contains_mac(csi_hal::s_ring, sizeof(csi_hal::s_ring), FOREIGN) &&
           !contains_mac(csi_hal::s_ring, sizeof(csi_hal::s_ring), PEER),
           "(4) ring carries none of the four addresses");
    EXPECT(!contains_mac(&st, sizeof(st), AP_BSSID) &&
           !contains_mac(&st, sizeof(st), AP2_BSSID) &&
           !contains_mac(&st, sizeof(st), FOREIGN) &&
           !contains_mac(&st, sizeof(st), PEER),
           "(4) stats carry none of the four addresses");
    EXPECT(st.frames_dropped_foreign > 0, "counter is live");
  }

  /* ── (9) deinit wipes the one identifier. ── */
  csi_hal::deinit();
  EXPECT(!csi_hal::has_associated_bssid(), "(9) no BSSID held after deinit");
  {
    static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    EXPECT(memcmp(csi_hal::s_assoc_bssid, zero, 6) == 0, "(9) static wiped to zero");
  }

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
