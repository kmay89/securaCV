/**
 * @file qr_scanner.h
 * @brief Camera-based QR code scanner for WiFi provisioning.
 *
 * Pipeline: JPEG frame → RGB888 (via esp-camera's fmt2rgb888) → grayscale → quirc.
 * Falls back to treating raw frames as grayscale if conversion fails.
 * All large buffers live in PSRAM when available.
 */

#ifndef SECURACV_QR_SCANNER_H
#define SECURACV_QR_SCANNER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_camera.h"

extern "C" {
#include "quirc.h"

/* fmt2rgb888 is provided by the esp32-camera component's img_converters.
 * Declare it here so we don't depend on the header's include path,
 * which varies between Arduino core versions and PlatformIO. */
bool fmt2rgb888(const uint8_t *src_buf, size_t src_len,
                pixformat_t format, uint8_t *rgb_buf);
}

namespace qr_scanner {

static constexpr int SCAN_W = 320;
static constexpr int SCAN_H = 240;

static struct quirc* s_qr   = nullptr;
static uint8_t*      s_rgb  = nullptr;
static uint8_t*      s_gray = nullptr;

inline bool init() {
  if (s_qr) return true;

  s_qr = quirc_new();
  if (!s_qr) return false;

  if (quirc_resize(s_qr, SCAN_W, SCAN_H) < 0) {
    quirc_destroy(s_qr);
    s_qr = nullptr;
    return false;
  }

  s_rgb  = (uint8_t*)heap_caps_malloc(SCAN_W * SCAN_H * 3,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_gray = (uint8_t*)heap_caps_malloc(SCAN_W * SCAN_H,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_rgb || !s_gray) {
    /* PSRAM unavailable — try regular heap */
    if (!s_rgb)  s_rgb  = (uint8_t*)malloc(SCAN_W * SCAN_H * 3);
    if (!s_gray) s_gray = (uint8_t*)malloc(SCAN_W * SCAN_H);
  }
  if (!s_rgb || !s_gray) {
    deinit();
    return false;
  }
  return true;
}

inline void deinit() {
  if (s_qr)   { quirc_destroy(s_qr); s_qr = nullptr; }
  if (s_rgb)  { free(s_rgb);  s_rgb = nullptr; }
  if (s_gray) { free(s_gray); s_gray = nullptr; }
}

// Decode one camera frame. Returns 1 if QR found (payload copied to out),
// 0 if no QR detected, -1 on decode error.
inline int scan_frame(camera_fb_t* fb, char* payload_out, size_t payload_cap) {
  if (!s_qr || !s_rgb || !s_gray || !fb) return -1;
  if ((int)fb->width > SCAN_W || (int)fb->height > SCAN_H) return -1;

  int w = (int)fb->width;
  int h = (int)fb->height;

  if (fb->format == PIXFORMAT_JPEG) {
    if (!fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, s_rgb))
      return -1;
    for (int i = 0; i < w * h; i++) {
      int si = i * 3;
      s_gray[i] = (uint8_t)((s_rgb[si] * 77 + s_rgb[si+1] * 150 + s_rgb[si+2] * 29) >> 8);
    }
  } else if (fb->format == PIXFORMAT_GRAYSCALE) {
    memcpy(s_gray, fb->buf, w * h);
  } else {
    return -1;
  }

  uint8_t* qr_image = quirc_begin(s_qr, nullptr, nullptr);
  memcpy(qr_image, s_gray, w * h);
  quirc_end(s_qr);

  int count = quirc_count(s_qr);
  for (int i = 0; i < count; i++) {
    struct quirc_code code;
    struct quirc_data data;
    quirc_extract(s_qr, i, &code);
    if (quirc_decode(&code, &data) == QUIRC_SUCCESS) {
      size_t len = (size_t)data.payload_len;
      if (len >= payload_cap) len = payload_cap - 1;
      memcpy(payload_out, data.payload, len);
      payload_out[len] = '\0';
      return 1;
    }
  }
  return 0;
}

// Parse WIFI:T:WPA;S:<ssid>;P:<password>;; format
inline bool parse_wifi(const char* payload,
                       char* ssid, size_t ssid_cap,
                       char* pass, size_t pass_cap) {
  if (strncmp(payload, "WIFI:", 5) != 0) return false;
  const char* p = payload + 5;

  ssid[0] = '\0';
  pass[0] = '\0';

  while (*p && !(p[0] == ';' && p[1] == ';')) {
    if (*p == ';') { p++; continue; }

    char field = *p;
    if (p[1] != ':') { p++; continue; }
    p += 2;

    char* dst = nullptr;
    size_t cap = 0;
    if (field == 'S') { dst = ssid; cap = ssid_cap; }
    else if (field == 'P') { dst = pass; cap = pass_cap; }

    size_t di = 0;
    while (*p && *p != ';') {
      char c = *p++;
      if (c == '\\' && *p) c = *p++;
      if (dst && di + 1 < cap) dst[di++] = c;
    }
    if (dst) dst[di] = '\0';
  }

  return ssid[0] != '\0';
}

// Parse SECURACV:S:<ssid>;P:<password>;T:<token>;; format
inline bool parse_securacv(const char* payload,
                           char* ssid, size_t ssid_cap,
                           char* pass, size_t pass_cap,
                           char* token, size_t token_cap) {
  if (strncmp(payload, "SECURACV:", 9) != 0) return false;
  const char* p = payload + 9;

  ssid[0] = '\0';
  pass[0] = '\0';
  if (token) token[0] = '\0';

  while (*p && !(p[0] == ';' && p[1] == ';')) {
    if (*p == ';') { p++; continue; }

    char field = *p;
    if (p[1] != ':') { p++; continue; }
    p += 2;

    char* dst = nullptr;
    size_t cap = 0;
    if (field == 'S') { dst = ssid; cap = ssid_cap; }
    else if (field == 'P') { dst = pass; cap = pass_cap; }
    else if (field == 'T' && token) { dst = token; cap = token_cap; }

    size_t di = 0;
    while (*p && *p != ';') {
      char c = *p++;
      if (c == '\\' && *p) c = *p++;
      if (dst && di + 1 < cap) dst[di++] = c;
    }
    if (dst) dst[di] = '\0';
  }

  return ssid[0] != '\0';
}

} // namespace qr_scanner

#endif // SECURACV_QR_SCANNER_H
