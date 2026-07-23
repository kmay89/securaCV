// Acoustic alarm listener runtime (see include/canary/io/mic_alarm.h).
// The whole TU is empty unless FEATURE_MIC_ALARM on a mic-bearing board,
// so every other display build (and the emulator) stays byte-identical.

#include "config.h"
#include "pins.h"

#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM && \
    defined(HAS_MICROPHONE) && HAS_MICROPHONE

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <driver/i2s.h>  // legacy I2S API — both supported core lines
#include <lvgl.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "mic_alarm.h"
#include "mic_logic.h"
#include "fleet_instance.h"

namespace canary {
namespace io {

namespace {

using namespace canary::io::mic;

// One ~20 ms frame at 16 kHz mono; the buffer lives one function deep and
// is ZEROED after the RMS — the privacy barrier (mic_alarm.h).
constexpr uint32_t SAMPLE_RATE_HZ = 16000;
constexpr size_t FRAME_SAMPLES = 320;
constexpr i2s_port_t MIC_I2S_PORT = I2S_NUM_0;

// Arm persistence: its own namespace (the siren-arm pattern) — nothing
// leaks into the glass settings blob or the mic-free builds.
constexpr const char* MIC_NS = "scv-mic";
constexpr const char* MIC_KEY = "armed";

// Detection rate ceiling: a standing alarm re-raises at most once/30 s
// (the fleet model's own tamper-style dedupe is event-name blind).
constexpr uint32_t REDETECT_MS = 30000;

Gate s_gate;
Envelope s_env;
CadenceDetector s_det;
char s_self_id[48] = {0};
bool s_display_ok = false;
bool s_begun = false;
uint16_t s_level = 0;
// The cadence detector runs on the AUDIO stream's own clock, not the main
// loop's: every 320-sample frame is 20 ms of sound regardless of when the
// loop got around to reading it. Stamping frames with millis() would let a
// UI stall (LVGL redraw, WiFi work) collapse several beeps into one edge
// pair and break the duration windows. Anchored to millis() at Start and
// re-anchored if the stream ever falls >1 s behind (dropped audio).
uint32_t s_frame_ms = 0;
uint32_t s_last_snap_ms = 0;
uint32_t s_last_event_ms = 0;
// The cooldown only exists BETWEEN events — the first detection must land
// immediately, even inside the first 30 s after boot (an alarm already
// sounding at power-up is the urgent case, not the ignorable one).
bool s_event_fired = false;
lv_obj_t* s_chip = nullptr;

void say_evt(const char* fmt, ...) {
  char body[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  Serial.printf("MIC1 %lu EVT %s\r\n", (unsigned long)millis(), body);
}

bool pins_ok() {
  return AUDIO_PIN_I2S_SCLK >= 0 && AUDIO_PIN_I2S_LRCK >= 0 &&
         AUDIO_PIN_I2S_SDIN >= 0;
}

// The chip is created/destroyed ONLY from apply_action(), in the same
// breath as the driver — the visible half of the Gate's running bit.
void chip_show() {
  if (!s_display_ok || s_chip) return;
  s_chip = lv_label_create(lv_layer_top());
  lv_obj_set_style_text_font(s_chip, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_chip, lv_color_hex(0x111111), 0);
  lv_obj_set_style_bg_color(s_chip, lv_color_hex(0xFFB300), 0);
  lv_obj_set_style_bg_opa(s_chip, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(s_chip, 8, 0);
  lv_obj_set_style_pad_ver(s_chip, 3, 0);
  lv_obj_set_style_radius(s_chip, 9, 0);
  lv_label_set_text(s_chip, LV_SYMBOL_VOLUME_MAX " MIC");
  lv_obj_align(s_chip, LV_ALIGN_BOTTOM_LEFT, 10, -8);
}

void chip_hide() {
  if (!s_chip) return;
  lv_obj_del(s_chip);
  s_chip = nullptr;
}

// ES7210 front-end note (VERIFY at bench): the ADC ships in a default
// state that may need register init before it clocks samples out. The
// probe below only proves the silicon ACKs; if SNAP levels sit at 0 with
// a live room, the codec init is the bench follow-up (board README §
// bring-up — fill it in next to the pin values it needs anyway).
bool codec_ack() {
  Wire.beginTransmission((uint8_t)AUDIO_ES7210_ADDR);
  return Wire.endTransmission() == 0;
}

bool driver_install() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE_HZ;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  // 8 x 20 ms = 160 ms of DMA depth (~10 KB internal RAM): a main-loop
  // stall shorter than that loses no audio — with only 80 ms, a long LVGL
  // redraw could silently clip a beep mid-group.
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = (int)FRAME_SAMPLES;
  if (i2s_driver_install(MIC_I2S_PORT, &cfg, 0, nullptr) != ESP_OK) {
    return false;
  }
  i2s_pin_config_t pins = {};
#if defined(I2S_PIN_NO_CHANGE)
  pins.mck_io_num = AUDIO_PIN_I2S_MCLK >= 0 ? AUDIO_PIN_I2S_MCLK
                                            : I2S_PIN_NO_CHANGE;
#else
  pins.mck_io_num = AUDIO_PIN_I2S_MCLK;
#endif
  pins.bck_io_num = AUDIO_PIN_I2S_SCLK;
  pins.ws_io_num = AUDIO_PIN_I2S_LRCK;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = AUDIO_PIN_I2S_SDIN;
  if (i2s_set_pin(MIC_I2S_PORT, &pins) != ESP_OK) {
    i2s_driver_uninstall(MIC_I2S_PORT);
    return false;
  }
  i2s_zero_dma_buffer(MIC_I2S_PORT);
  return true;
}

void driver_uninstall() {
  i2s_driver_uninstall(MIC_I2S_PORT);  // pins released — the hard mute
}

// Every session starts from silence and every stop forgets: envelope and
// cadence state carry NOTHING across a mute. Without this, re-arming near
// a sounding alarm would inherit a stale loud bit and a half-built beep
// group from the last session, polluting the first new group and delaying
// a real detection — and "disarm forgets everything it heard" is also the
// honest reading of the privacy contract.
void reset_acoustic_state() {
  s_env = Envelope();
  s_det = CadenceDetector();
  s_level = 0;
  // A fresh listening session earns a fresh "first detection is immediate":
  // re-arming next to a sounding alarm should surface it now, not after the
  // 30 s throttle it never entered.
  s_event_fired = false;
}

// Perform the Gate's verdict: hardware and indicator together, always.
void apply_action(Action a) {
  if (a == Action::Start) {
    if (driver_install()) {
      reset_acoustic_state();
      s_frame_ms = millis();
      chip_show();
      say_evt("start listening codec_ack=%d", codec_ack() ? 1 : 0);
    } else {
      // Install failed: the gate must not claim a running driver.
      s_gate.running = false;
      say_evt("start FAILED (i2s install)");
    }
  } else if (a == Action::Stop) {
    driver_uninstall();
    chip_hide();
    reset_acoustic_state();
    say_evt("stop (driver uninstalled, pins released, state forgotten)");
  }
}

// One capture frame -> one scalar (+ the frame's own duration, so the
// caller can advance the audio-timeline clock). Samples die here, every
// pass.
bool read_frame_rms(uint16_t* rms_out, uint32_t* frame_ms_out) {
  static int16_t buf[FRAME_SAMPLES];
  size_t got = 0;
  if (i2s_read(MIC_I2S_PORT, buf, sizeof(buf), &got, 0) != ESP_OK ||
      got < sizeof(int16_t)) {
    return false;
  }
  const size_t n = got / sizeof(int16_t);
  *frame_ms_out = (uint32_t)(n / (SAMPLE_RATE_HZ / 1000));  // 320 -> 20 ms
  int64_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += buf[i];
  const int32_t dc = (int32_t)(sum / (int64_t)n);
  uint64_t acc = 0;
  for (size_t i = 0; i < n; i++) {
    const int32_t d = (int32_t)buf[i] - dc;
    acc += (uint64_t)((int64_t)d * d);
  }
  memset(buf, 0, sizeof(buf));  // the privacy barrier: nothing outlives this
  uint64_t mean = acc / n;
  uint32_t root = (uint32_t)sqrtf((float)mean);
  *rms_out = root > 0xFFFF ? 0xFFFF : (uint16_t)root;
  return true;
}

}  // namespace

void mic_begin(const char* self_id, bool display_ok) {
  strncpy(s_self_id, self_id ? self_id : "", sizeof(s_self_id) - 1);
  s_display_ok = display_ok;
  s_gate.pins_ok = pins_ok();
  {
    Preferences p;
    if (p.begin(MIC_NS, /*readOnly=*/true)) {
      s_gate.armed = p.getBool(MIC_KEY, false);  // OFF is the default
      p.end();
    }
  }
  s_begun = true;
  Serial.printf("MIC1 HELLO armed=%d pins=%s codec=0x%02X\r\n",
                s_gate.armed ? 1 : 0, s_gate.pins_ok ? "ok" : "UNSET(VERIFY)",
                (unsigned)AUDIO_ES7210_ADDR);
  if (!s_gate.pins_ok) {
    say_evt("pins unset — mics provably un-driven until bench fills pins.h");
  }
  apply_action(s_gate.update());
}

void mic_loop(uint32_t now) {
  if (!s_begun) return;
  apply_action(s_gate.update());
  if (!s_gate.running) return;

  // If the stream fell far behind the wall clock (audio dropped during a
  // very long stall), jump the audio clock forward rather than replaying
  // the lost time as phantom silence-free continuity.
  if ((int32_t)(now - s_frame_ms) > 1000) s_frame_ms = now;

  uint16_t rms = 0;
  uint32_t frame_ms = 0;
  while (read_frame_rms(&rms, &frame_ms)) {
    s_level = rms;
    s_frame_ms += frame_ms;
    const bool loud = s_env.update(rms);
    const Detection d = s_det.update(loud, s_frame_ms);
    if (d.event != Event::None &&
        (!s_event_fired ||
         (int32_t)(now - s_last_event_ms) >= (int32_t)REDETECT_MS)) {
      s_event_fired = true;
      s_last_event_ms = now;
      const char* name = event_wire_name(d.event);
      canary::fleet::the_fleet().on_event(s_self_id, name,
                                          /*signed_flag=*/false, now);
      say_evt("detect %s cycles=%u conf=%u", name, (unsigned)d.cycles,
              (unsigned)d.confidence);
    }
  }
  if ((int32_t)(now - s_last_snap_ms) >= 1000) {
    s_last_snap_ms = now;
    Serial.printf("MIC1 %lu SNAP rms=%u loud=%d armed=1 listening=1\r\n",
                  (unsigned long)now, (unsigned)s_level,
                  s_env.loud ? 1 : 0);
  }
}

void mic_set_armed(bool armed) {
  s_gate.armed = armed;
  {
    Preferences p;
    if (p.begin(MIC_NS, /*readOnly=*/false)) {
      p.putBool(MIC_KEY, armed);
      p.end();
    }
  }
  say_evt("%s", armed ? "armed" : "disarmed");
  apply_action(s_gate.update());
}

bool mic_armed() { return s_gate.armed; }
bool mic_listening() { return s_gate.running; }
// Computed from the pin map, not the gate: callers in the non-fleet gears
// (debug's System page) read this before mic_begin ever ran there — the
// gears never listen, and this must still report the BOARD truthfully.
bool mic_pins_ok() { return pins_ok(); }
uint16_t mic_level() { return s_level; }

}  // namespace io
}  // namespace canary

#endif  // FEATURE_MIC_ALARM && HAS_MICROPHONE
