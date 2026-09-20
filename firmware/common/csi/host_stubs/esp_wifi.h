/* Host stand-in for <esp_wifi.h> — see README.md.
 *
 * Models only what csi_hal.cpp touches. The real wifi_pkt_rx_ctrl_t is a
 * bitfield struct; the HAL reads .rssi, .channel and .cwb, so those are the
 * members here. wifi_csi_info_t keeps the real member set and order of the
 * ESP-IDF 5.x struct so the HAL's "we read mac, never dmac/hdr/payload"
 * claim is exercised against the same field names. The functions are
 * defined by the test. */
#ifndef SECURACV_CSI_HOST_STUB_ESP_WIFI_H
#define SECURACV_CSI_HOST_STUB_ESP_WIFI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
  int8_t  rssi;
  uint8_t cwb;        /* 0 = 20 MHz, 1 = 40 MHz */
  uint8_t channel;
  int8_t  noise_floor;
} wifi_pkt_rx_ctrl_t;

typedef struct {
  wifi_pkt_rx_ctrl_t rx_ctrl;
  uint8_t  mac[6];              /* transmitter (source) address */
  uint8_t  dmac[6];             /* destination address */
  bool     first_word_invalid;
  int8_t*  buf;
  uint16_t len;
  uint8_t* hdr;
  uint8_t* payload;
  uint16_t payload_len;
} wifi_csi_info_t;

typedef struct {
  uint8_t bssid[6];
  uint8_t ssid[33];
  uint8_t primary;
  int8_t  rssi;
} wifi_ap_record_t;

typedef enum {
  WIFI_SECOND_CHAN_NONE = 0,
  WIFI_SECOND_CHAN_ABOVE,
  WIFI_SECOND_CHAN_BELOW,
} wifi_second_chan_t;

/* Legacy (ESP32/S3/C3, IDF 5.x) shape — csi_idf_compat.h fills this one
 * when CONFIG_SOC_WIFI_HE_SUPPORT is not defined, which it is not here. */
typedef struct {
  bool    lltf_en;
  bool    htltf_en;
  bool    stbc_htltf2_en;
  bool    ltf_merge_en;
  bool    channel_filter_en;
  bool    manu_scale;
  uint8_t shift;
  bool    dump_ack_en;
} wifi_csi_config_t;

typedef void (*wifi_csi_cb_t)(void* ctx, wifi_csi_info_t* data);

esp_err_t esp_wifi_set_csi_config(const wifi_csi_config_t* config);
esp_err_t esp_wifi_set_csi_rx_cb(wifi_csi_cb_t cb, void* ctx);
esp_err_t esp_wifi_set_csi(bool en);
esp_err_t esp_wifi_set_channel(uint8_t primary, wifi_second_chan_t second);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap_info);

#endif /* SECURACV_CSI_HOST_STUB_ESP_WIFI_H */
