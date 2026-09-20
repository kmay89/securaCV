/* The slice of <esp_wifi.h> the CSI HAL touches: the legacy (ESP32/S2/S3/C3)
 * wifi_csi_config_t shape csi_idf_compat.h fills, the wifi_csi_info_t fields
 * the privacy barrier reads (rx_ctrl.rssi/channel/cwb, buf, len,
 * first_word_invalid — mac/dmac/hdr/payload exist so the "never copied"
 * claim is about a struct that has them), and the four driver calls. The
 * test defines the functions and captures the one callback registration.
 * C linkage, as in the real header. */
#ifndef STUB_CSI_HAL_ESP_WIFI_H
#define STUB_CSI_HAL_ESP_WIFI_H
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int8_t rssi; uint8_t channel; uint8_t cwb; } wifi_pkt_rx_ctrl_t;
typedef struct {
  wifi_pkt_rx_ctrl_t rx_ctrl;
  uint8_t  mac[6];
  uint8_t  dmac[6];
  bool     first_word_invalid;
  int8_t*  buf;
  uint16_t len;
  uint8_t* hdr;
  uint8_t* payload;
  uint16_t payload_len;
} wifi_csi_info_t;
typedef struct {
  bool lltf_en, htltf_en, stbc_htltf2_en, ltf_merge_en, channel_filter_en, manu_scale;
  uint8_t shift;
} wifi_csi_config_t;
typedef void (*wifi_csi_cb_t)(void* ctx, wifi_csi_info_t* data);
typedef enum { WIFI_SECOND_CHAN_NONE = 0 } wifi_second_chan_t;
/* The associated-AP record the transmitter filter reads the BSSID from
 * (csi_hal.h "TRANSMITTER FILTER"); the SSID is there so a test can prove
 * the HAL wipes the whole record, not only the six bytes it compares. */
typedef struct {
  uint8_t bssid[6];
  uint8_t ssid[33];
  uint8_t primary;
  int8_t  rssi;
} wifi_ap_record_t;
esp_err_t esp_wifi_set_csi_config(const wifi_csi_config_t* config);
esp_err_t esp_wifi_set_csi_rx_cb(wifi_csi_cb_t cb, void* ctx);
esp_err_t esp_wifi_set_csi(bool en);
esp_err_t esp_wifi_set_channel(uint8_t primary, wifi_second_chan_t second);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap_info);
#ifdef __cplusplus
}
#endif
#endif
