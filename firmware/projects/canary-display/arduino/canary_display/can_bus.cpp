// CAN / TWAI transport — see canary/io/can_bus.h.
//
// The whole translation unit is behind FEATURE_CAN so the default and emulator
// builds compile it to nothing (byte-neutral wasm). No stubs are needed: every
// caller is under the same gate.

#include <config.h>

#if defined(FEATURE_CAN) && FEATURE_CAN

#include <Arduino.h>
#include <driver/twai.h>

#include "pins.h"  // CAN_PIN_TX/RX, CAN_BITRATE_DEFAULT (board -I path)
#include "canary/io/can_bus.h"
#include "canary/io/can_frame.h"
#include "canary/log.h"

namespace canary {
namespace io {

namespace {
bool s_ready = false;
uint32_t s_next_drain_ms = 0;

constexpr uint32_t DRAIN_EVERY_MS = 1000;   // bench heartbeat cadence
constexpr uint32_t DRAIN_BUDGET_MS = 0;     // non-blocking: drain queued frames only

// Map a standard ISO 11898 bit rate to the matching TWAI timing preset. Returns
// false for anything without a preset, so can_begin() refuses to guess.
bool timing_for(uint32_t bitrate, twai_timing_config_t* out) {
  switch (bitrate) {
    case can::BITRATE_125K: *out = TWAI_TIMING_CONFIG_125KBITS(); return true;
    case can::BITRATE_250K: *out = TWAI_TIMING_CONFIG_250KBITS(); return true;
    case can::BITRATE_500K: *out = TWAI_TIMING_CONFIG_500KBITS(); return true;
    case can::BITRATE_1M:   *out = TWAI_TIMING_CONFIG_1MBITS();   return true;
    default: return false;
  }
}

// can::Frame -> twai_message_t (zero the flag union first, then set bitfields).
twai_message_t to_twai(const can::Frame& f) {
  twai_message_t m = {};
  m.extd = f.extended ? 1 : 0;
  m.rtr = f.rtr ? 1 : 0;
  m.identifier = f.id;
  m.data_length_code = f.dlc > can::MAX_DLC ? can::MAX_DLC : f.dlc;
  for (uint8_t i = 0; i < m.data_length_code; i++) m.data[i] = f.data[i];
  return m;
}

// twai_message_t -> can::Frame.
can::Frame from_twai(const twai_message_t& m) {
  can::Frame f = {};
  f.extended = m.extd != 0;
  f.rtr = m.rtr != 0;
  f.id = m.identifier;
  f.dlc = m.data_length_code > can::MAX_DLC ? can::MAX_DLC : m.data_length_code;
  for (uint8_t i = 0; i < f.dlc; i++) f.data[i] = m.data[i];
  return f;
}
}  // namespace

bool can_begin(uint32_t bitrate) {
  if (s_ready) return true;
  if (!can::valid_bitrate(bitrate)) {
    log_line("CAN", "refused: unsupported bitrate (use 125k/250k/500k/1M)");
    return false;
  }
  twai_timing_config_t t;
  if (!timing_for(bitrate, &t)) return false;

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)CAN_PIN_TX, (gpio_num_t)CAN_PIN_RX, TWAI_MODE_NORMAL);
  twai_filter_config_t filt = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g, &t, &filt) != ESP_OK) {
    log_line("CAN", "driver install failed");
    return false;
  }
  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    log_line("CAN", "start failed");
    return false;
  }
  s_ready = true;
  char msg[72];
  snprintf(msg, sizeof(msg), "up on H/L (TX=%d RX=%d) @ %lu bit/s", CAN_PIN_TX,
           CAN_PIN_RX, (unsigned long)bitrate);
  log_line("CAN", msg);
  return true;
}

bool can_ready() { return s_ready; }

int can_transmit(const can::Frame& f, uint32_t timeout_ms) {
  if (!s_ready) return CAN_ERR_NOT_READY;
  if (!can::valid_frame(f)) return CAN_ERR_ARG;
  twai_message_t m = to_twai(f);
  const esp_err_t r = twai_transmit(&m, pdMS_TO_TICKS(timeout_ms));
  if (r == ESP_OK) return CAN_OK;
  if (r == ESP_ERR_TIMEOUT) return CAN_ERR_TIMEOUT;
  return CAN_ERR_BUS;
}

int can_receive(can::Frame* out, uint32_t timeout_ms) {
  if (!s_ready) return CAN_ERR_NOT_READY;
  if (out == nullptr) return CAN_ERR_ARG;
  twai_message_t m = {};
  const esp_err_t r = twai_receive(&m, pdMS_TO_TICKS(timeout_ms));
  if (r == ESP_ERR_TIMEOUT) return CAN_ERR_TIMEOUT;
  if (r != ESP_OK) return CAN_ERR_BUS;
  *out = from_twai(m);
  return CAN_OK;
}

void can_loop(uint32_t now) {
  if (!s_ready) return;
  if ((int32_t)(now - s_next_drain_ms) < 0) return;
  s_next_drain_ms = now + DRAIN_EVERY_MS;

  // Self-heal a bus-off: enough TX errors put the TWAI controller into BUS_OFF,
  // where it neither sends nor receives until recovery runs (ISO 11898). Drive
  // recovery across ticks, non-blocking — BUS_OFF: kick off recovery; once the
  // bus is clean the driver lands in STOPPED: restart it; RECOVERING: wait.
  twai_status_info_t status;
  if (twai_get_status_info(&status) == ESP_OK && status.state != TWAI_STATE_RUNNING) {
    if (status.state == TWAI_STATE_BUS_OFF) {
      twai_initiate_recovery();
      log_line("CAN", "bus-off - initiating recovery");
    } else if (status.state == TWAI_STATE_STOPPED) {
      if (twai_start() == ESP_OK) log_line("CAN", "recovered - restarted");
    }
    return;  // don't drain until the controller is running again
  }

  // Drain whatever arrived since the last tick (non-blocking, bounded so a busy
  // bus can't monopolize the loop), logging each frame in a stable ASCII form.
  for (int i = 0; i < 8; i++) {
    can::Frame f;
    if (can_receive(&f, DRAIN_BUDGET_MS) != CAN_OK) break;
    char line[80];
    if (can::format_frame(f, line, sizeof(line)) > 0) log_line("CAN", line);
  }
}

}  // namespace io
}  // namespace canary

#endif  // FEATURE_CAN
