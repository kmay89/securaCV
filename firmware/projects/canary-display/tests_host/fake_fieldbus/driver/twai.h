// tests_host/fake_fieldbus/driver/twai.h — the ESP-IDF legacy TWAI driver as
// the display's fieldbus code uses it, for test_fieldbus_glue.cpp.
//
// Types and macros follow the IDF headers the two Arduino cores ship, which
// agree on every name used here: v4.4 (arduino-esp32 2.0.17,
// components/driver/include/driver/twai.h + hal/twai_types.h) and v5.5
// (arduino-esp32 3.3.x, components/driver/twai/include/driver/twai.h +
// hal/twai_types_deprecated.h): twai_message_t's flag union (extd, rtr, ss,
// self, dlc_non_comp) ahead of identifier / data_length_code / data[8],
// TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, mode), the TWAI_TIMING_CONFIG_*()
// presets, TWAI_FILTER_CONFIG_ACCEPT_ALL(), and twai_driver_install /
// _uninstall / start / transmit / receive / get_status_info /
// initiate_recovery. The timing presets carry v4.4's brp values (v5.5 states
// the same rates through a quanta resolution); a test tells the four rates
// apart by brp. Return codes are the ones the IDF documents for each call:
// transmit and receive answer ESP_ERR_INVALID_STATE when the driver is not
// running, ESP_ERR_TIMEOUT when nothing came in (receive) or no queue slot
// freed up (transmit) within the ticks; install answers ESP_ERR_INVALID_STATE
// when a driver is already installed. One tick is one millisecond, as on the
// S3 Arduino builds (CONFIG_FREERTOS_HZ=1000).
//
// The `fake_twai` namespace holds what the calls recorded and what the next
// ones answer.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <deque>
#include <vector>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef int gpio_num_t;
#define TWAI_IO_UNUSED ((gpio_num_t)-1)

typedef enum {
  TWAI_MODE_NORMAL,
  TWAI_MODE_NO_ACK,
  TWAI_MODE_LISTEN_ONLY,
} twai_mode_t;

typedef struct {
  twai_mode_t mode;
  gpio_num_t tx_io;
  gpio_num_t rx_io;
  gpio_num_t clkout_io;
  gpio_num_t bus_off_io;
  uint32_t tx_queue_len;
  uint32_t rx_queue_len;
  uint32_t alerts_enabled;
  uint32_t clkout_divider;
  int intr_flags;
} twai_general_config_t;

#define TWAI_ALERT_NONE 0x00000000
#define TWAI_GENERAL_CONFIG_DEFAULT(tx_io_num, rx_io_num, op_mode)        \
  {.mode = op_mode, .tx_io = tx_io_num, .rx_io = rx_io_num,               \
   .clkout_io = TWAI_IO_UNUSED, .bus_off_io = TWAI_IO_UNUSED,             \
   .tx_queue_len = 5, .rx_queue_len = 5, .alerts_enabled = TWAI_ALERT_NONE, \
   .clkout_divider = 0, .intr_flags = 0}

typedef struct {
  uint32_t brp;
  uint8_t tseg_1;
  uint8_t tseg_2;
  uint8_t sjw;
  bool triple_sampling;
} twai_timing_config_t;

#define TWAI_TIMING_CONFIG_125KBITS() {.brp = 32, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}
#define TWAI_TIMING_CONFIG_250KBITS() {.brp = 16, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}
#define TWAI_TIMING_CONFIG_500KBITS() {.brp = 8, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}
#define TWAI_TIMING_CONFIG_1MBITS()   {.brp = 4, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}

typedef struct {
  uint32_t acceptance_code;
  uint32_t acceptance_mask;
  bool single_filter;
} twai_filter_config_t;

#define TWAI_FILTER_CONFIG_ACCEPT_ALL() {.acceptance_code = 0, .acceptance_mask = 0xFFFFFFFF, .single_filter = true}

typedef struct {
  union {
    struct {
      uint32_t extd : 1;
      uint32_t rtr : 1;
      uint32_t ss : 1;
      uint32_t self : 1;
      uint32_t dlc_non_comp : 1;
      uint32_t reserved : 27;
    };
    uint32_t flags;
  };
  uint32_t identifier;
  uint8_t data_length_code;
  uint8_t data[8];
} twai_message_t;

typedef enum {
  TWAI_STATE_STOPPED,
  TWAI_STATE_RUNNING,
  TWAI_STATE_BUS_OFF,
  TWAI_STATE_RECOVERING,
} twai_state_t;

typedef struct {
  twai_state_t state;
  uint32_t msgs_to_tx;
  uint32_t msgs_to_rx;
  uint32_t tx_error_counter;
  uint32_t rx_error_counter;
  uint32_t tx_failed_count;
  uint32_t rx_missed_count;
  uint32_t rx_overrun_count;
  uint32_t arb_lost_count;
  uint32_t bus_error_count;
} twai_status_info_t;

namespace fake_twai {
struct Sent {
  twai_message_t msg;
  TickType_t ticks;
};
inline bool installed = false;
inline twai_state_t state = TWAI_STATE_STOPPED;
inline int installs = 0, uninstalls = 0, starts = 0, recoveries = 0;
inline int receives = 0;
inline twai_general_config_t g = {};
inline twai_timing_config_t t = {};
inline twai_filter_config_t f = {};
inline esp_err_t install_answer = ESP_OK;  // what the next install answers
inline esp_err_t start_answer = ESP_OK;
inline esp_err_t transmit_answer = ESP_OK; // when running
inline std::vector<Sent> sent;
inline std::vector<TickType_t> receive_ticks;
inline std::deque<twai_message_t> inbox;    // frames the bus has delivered
}  // namespace fake_twai

inline esp_err_t twai_driver_install(const twai_general_config_t* g_config,
                                     const twai_timing_config_t* t_config,
                                     const twai_filter_config_t* f_config) {
  if (!g_config || !t_config || !f_config) return ESP_ERR_INVALID_ARG;
  if (fake_twai::installed) return ESP_ERR_INVALID_STATE;
  fake_twai::installs++;
  fake_twai::g = *g_config;
  fake_twai::t = *t_config;
  fake_twai::f = *f_config;
  if (fake_twai::install_answer != ESP_OK) return fake_twai::install_answer;
  fake_twai::installed = true;
  fake_twai::state = TWAI_STATE_STOPPED;
  return ESP_OK;
}

inline esp_err_t twai_driver_uninstall(void) {
  if (!fake_twai::installed || fake_twai::state == TWAI_STATE_RUNNING) {
    return ESP_ERR_INVALID_STATE;
  }
  fake_twai::uninstalls++;
  fake_twai::installed = false;
  return ESP_OK;
}

inline esp_err_t twai_start(void) {
  if (!fake_twai::installed || fake_twai::state != TWAI_STATE_STOPPED) {
    return ESP_ERR_INVALID_STATE;
  }
  fake_twai::starts++;
  if (fake_twai::start_answer != ESP_OK) return fake_twai::start_answer;
  fake_twai::state = TWAI_STATE_RUNNING;
  return ESP_OK;
}

inline esp_err_t twai_transmit(const twai_message_t* message,
                               TickType_t ticks_to_wait) {
  if (!message) return ESP_ERR_INVALID_ARG;
  if (!fake_twai::installed || fake_twai::state != TWAI_STATE_RUNNING) {
    return ESP_ERR_INVALID_STATE;
  }
  fake_twai::sent.push_back({*message, ticks_to_wait});
  return fake_twai::transmit_answer;
}

inline esp_err_t twai_receive(twai_message_t* message,
                              TickType_t ticks_to_wait) {
  if (!message) return ESP_ERR_INVALID_ARG;
  fake_twai::receives++;
  fake_twai::receive_ticks.push_back(ticks_to_wait);
  if (!fake_twai::installed) return ESP_ERR_INVALID_STATE;
  if (fake_twai::inbox.empty()) return ESP_ERR_TIMEOUT;
  *message = fake_twai::inbox.front();
  fake_twai::inbox.pop_front();
  return ESP_OK;
}

inline esp_err_t twai_get_status_info(twai_status_info_t* status_info) {
  if (!status_info) return ESP_ERR_INVALID_ARG;
  if (!fake_twai::installed) return ESP_ERR_INVALID_STATE;
  *status_info = {};
  status_info->state = fake_twai::state;
  return ESP_OK;
}

inline esp_err_t twai_initiate_recovery(void) {
  if (!fake_twai::installed || fake_twai::state != TWAI_STATE_BUS_OFF) {
    return ESP_ERR_INVALID_STATE;
  }
  fake_twai::recoveries++;
  fake_twai::state = TWAI_STATE_RECOVERING;
  return ESP_OK;
}
