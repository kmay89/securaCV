// Host-test stub for the legacy ESP-IDF I2S driver. i2s_read() is backed by
// a test-scripted amplitude timeline (see test_audio_cadence.cpp), which is
// how the test feeds synthetic alarm cadences into the real pipeline.
#ifndef HOST_STUB_DRIVER_I2S_H
#define HOST_STUB_DRIVER_I2S_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef int i2s_port_t;
#define I2S_NUM_0 0

typedef int i2s_mode_t;
#define I2S_MODE_MASTER (1 << 0)
#define I2S_MODE_RX     (1 << 1)
#define I2S_MODE_PDM    (1 << 2)

typedef int i2s_bits_per_sample_t;
#define I2S_BITS_PER_SAMPLE_16BIT 16

typedef int i2s_channel_fmt_t;
#define I2S_CHANNEL_FMT_ONLY_LEFT 1

typedef int i2s_comm_format_t;
#define I2S_COMM_FORMAT_STAND_I2S 1

#define I2S_PIN_NO_CHANGE (-1)

typedef struct {
  i2s_mode_t            mode;
  uint32_t              sample_rate;
  i2s_bits_per_sample_t bits_per_sample;
  i2s_channel_fmt_t     channel_format;
  i2s_comm_format_t     communication_format;
  int                   intr_alloc_flags;
  int                   dma_buf_count;
  int                   dma_buf_len;
  bool                  use_apll;
  bool                  tx_desc_auto_clear;
  int                   fixed_mclk;
} i2s_config_t;

typedef struct {
  int bck_io_num;
  int ws_io_num;
  int data_out_num;
  int data_in_num;
  int mck_io_num;
} i2s_pin_config_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2s_driver_install(i2s_port_t port, const i2s_config_t* cfg,
                             int queue_size, void* queue);
esp_err_t i2s_driver_uninstall(i2s_port_t port);
esp_err_t i2s_set_pin(i2s_port_t port, const i2s_pin_config_t* pins);
esp_err_t i2s_zero_dma_buffer(i2s_port_t port);
esp_err_t i2s_read(i2s_port_t port, void* dest, size_t size,
                   size_t* bytes_read, int timeout_ticks);

#ifdef __cplusplus
}
#endif

#endif
