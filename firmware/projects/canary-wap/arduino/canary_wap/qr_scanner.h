/**
 * @file qr_scanner.h
 * @brief Camera-based QR code scanner for WiFi provisioning.
 *
 * The camera is switched to PIXFORMAT_GRAYSCALE during scanning so
 * frames can be fed directly to quirc without JPEG decode overhead.
 * All large buffers live in PSRAM.
 */

#ifndef SECURACV_QR_SCANNER_H
#define SECURACV_QR_SCANNER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"

extern "C" {
#include "quirc.h"
}

namespace qr_scanner {

static constexpr int SCAN_W = 320;
static constexpr int SCAN_H = 240;

static struct quirc* s_qr = nullptr;

inline bool init() {
  if (s_qr) return true;

  s_qr = quirc_new();
  if (!s_qr) return false;

  if (quirc_resize(s_qr, SCAN_W, SCAN_H) < 0) {
    quirc_destroy(s_qr);
    s_qr = nullptr;
    return false;
  }
  return true;
}

inline void deinit() {
  if (s_qr) { quirc_destroy(s_qr); s_qr = nullptr; }
}

// Decode one grayscale frame. The camera must be in PIXFORMAT_GRAYSCALE
// and FRAMESIZE_QVGA before calling. Returns 1 if QR found, 0 if not,
// -1 on error.
inline int scan_frame(camera_fb_t* fb, char* payload_out, size_t payload_cap) {
  if (!s_qr || !fb) return -1;
  if (fb->format != PIXFORMAT_GRAYSCALE) return -1;
  if ((int)fb->width != SCAN_W || (int)fb->height != SCAN_H) return -1;

  uint8_t* qr_image = quirc_begin(s_qr, nullptr, nullptr);
  memcpy(qr_image, fb->buf, SCAN_W * SCAN_H);
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
