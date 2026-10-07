// The Vision Doorbell's speaker — one PWM pin, a sample timer and NVS
// around firmware/common/doorbell/doorbell_audio.h. See canary/doorbell_audio.h.
//
// The pin is a 10-bit LEDC channel at 78.125 kHz (the 80 MHz LEDC clock's
// 10-bit ceiling on both the C3 and the S3): a carrier far above hearing
// and above the amplifier's input filter, whose duty is the audio. A
// hardware timer at the voice's 8 kHz sample rate renders one sample per
// tick in its ISR and writes the duty. The ISR does integer work only
// (the C3 has no FPU, and an ISR must not touch one anyway).

#include "canary/doorbell_audio.h"

#include <Arduino.h>
#include <Preferences.h>

#include "pins.h"
#include "canary/log.h"

#if defined(DOORBELL_AUDIO_PIN)
#define CV_DOORBELL_AUDIO 1
#include <esp_arduino_version.h>
#include "driver/ledc.h"
#else
#define CV_DOORBELL_AUDIO 0
#endif

namespace canary::doorbell_audio {

#if CV_DOORBELL_AUDIO

namespace {

constexpr const char* NVS_NS = "securacv";   // same namespace as the doorbell's db_ keys

// 10 bits at 78.125 kHz: the LEDC source is 80 MHz on both XIAO hosts.
constexpr uint32_t kCarrierHz = 78125;
constexpr uint8_t  kBits = 10;
constexpr uint16_t kIdleDuty = 512;          // mid-scale: the coupling capacitor sees no step
#if !(defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3)
constexpr uint8_t kChannel = 5;              // the glow ring holds channel 4
#endif
// the LEDC channel the ISR writes directly (ledcWrite is not ISR-safe)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
ledc_channel_t s_ledc_ch = LEDC_CHANNEL_0;
#else
constexpr ledc_channel_t s_ledc_ch = (ledc_channel_t)kChannel;
#endif
constexpr ledc_mode_t s_ledc_mode = LEDC_LOW_SPEED_MODE;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
hw_timer_t* s_timer = nullptr;
bool s_inited = false;
bool s_timer_ok = false;
volatile bool s_running = false;             // the ISR renders while a phrase plays
::doorbell::VoiceState s_voice{};
volatile uint8_t s_volume = ::doorbell::kVolumeDefault;
::doorbell::Phrase s_last_reply = ::doorbell::Phrase::NONE;

inline void IRAM_ATTR write_duty_isr(uint16_t duty) {
  // register-level duty write: this channel is this file's alone
  ledc_set_duty(s_ledc_mode, s_ledc_ch, duty);
  ledc_update_duty(s_ledc_mode, s_ledc_ch);
}

void IRAM_ATTR on_tick() {
  if (!s_running) return;
  portENTER_CRITICAL_ISR(&s_mux);
  const int16_t x = ::doorbell::audio_sample(&s_voice);
  const bool active = ::doorbell::audio_active(&s_voice);
  portEXIT_CRITICAL_ISR(&s_mux);
  write_duty_isr(active ? ::doorbell::duty10(x) : kIdleDuty);
  if (!active) s_running = false;
}

void pwm_setup() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(DOORBELL_AUDIO_PIN, kCarrierHz, kBits);
  ledcWrite(DOORBELL_AUDIO_PIN, kIdleDuty);
  // the channel the core picked for this pin, for the ISR's direct writes:
  // the one whose duty now reads the idle value we just wrote
  for (int ch = 0; ch < LEDC_CHANNEL_MAX; ++ch) {
    if (ledc_get_duty(s_ledc_mode, (ledc_channel_t)ch) == kIdleDuty) { s_ledc_ch = (ledc_channel_t)ch; break; }
  }
#else
  ledcSetup(kChannel, kCarrierHz, kBits);
  ledcAttachPin(DOORBELL_AUDIO_PIN, kChannel);
  ledcWrite(kChannel, kIdleDuty);
#endif
}

bool timer_setup() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  s_timer = timerBegin(1000000);                       // 1 MHz tick
  if (!s_timer) return false;
  timerAttachInterrupt(s_timer, &on_tick);
  timerAlarm(s_timer, 1000000 / ::doorbell::kSampleRate, true, 0);   // 125 us, auto-reload
  return true;
#else
  s_timer = timerBegin(1, 80, true);                   // timer 1, 80 MHz / 80 = 1 MHz
  if (!s_timer) return false;
  timerAttachInterrupt(s_timer, &on_tick, true);
  timerAlarmWrite(s_timer, 1000000 / ::doorbell::kSampleRate, true);
  timerAlarmEnable(s_timer);
  return true;
#endif
}

bool persist_uchar(const char* key, uint8_t v) {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, /*readOnly=*/false)) return false;
  prefs.putUChar(key, v);
  prefs.end();
  return true;
}

}  // namespace

bool available() { return true; }

void init() {
  if (s_inited) return;
  {
    Preferences prefs;
    bool opened = prefs.begin(NVS_NS, /*readOnly=*/true);
    if (!opened) opened = prefs.begin(NVS_NS, /*readOnly=*/false);
    if (opened) {
      s_volume = ::doorbell::clamp_volume(prefs.getUChar("db_vol", ::doorbell::kVolumeDefault));
      prefs.end();
    }
  }
  s_voice = ::doorbell::audio_begin(s_volume);
  pwm_setup();
  s_timer_ok = timer_setup();
  s_inited = true;
  if (!s_timer_ok) {
    canary::log_line("BELL", "Speaker timer failed to start - the doorbell is silent (the ring still swells).");
  }
  canary::dbg_serial().printf("[BELL] Speaker on D3 (PWM %lu Hz, %u-bit, %lu samples/s), volume %u%%\n",
                              (unsigned long)kCarrierHz, (unsigned)kBits,
                              (unsigned long)::doorbell::kSampleRate, (unsigned)s_volume);
}

void play(::doorbell::Phrase p) {
  if (!s_inited || !s_timer_ok || p == ::doorbell::Phrase::NONE) return;
  if (p != ::doorbell::Phrase::CHIME && p != ::doorbell::Phrase::TICK) s_last_reply = p;
  portENTER_CRITICAL(&s_mux);
  s_voice.volume = s_volume;
  ::doorbell::audio_play(&s_voice, p);
  portEXIT_CRITICAL(&s_mux);
  s_running = true;
}

bool playing() { return s_running; }

uint8_t volume() { return s_volume; }

bool set_volume(uint8_t pct) {
  const uint8_t v = ::doorbell::clamp_volume(pct);
  if (v == s_volume) return false;
  s_volume = v;
  portENTER_CRITICAL(&s_mux);
  s_voice.volume = v;      // a phrase in progress follows the slider
  portEXIT_CRITICAL(&s_mux);
  persist_uchar("db_vol", v);
  return true;
}

::doorbell::Phrase last_reply() { return s_last_reply; }

bool fault() { return s_inited && !s_timer_ok; }

#else  // no speaker pin on this board

bool available() { return false; }
void init() {}
void play(::doorbell::Phrase) {}
bool playing() { return false; }
uint8_t volume() { return ::doorbell::kVolumeDefault; }
bool set_volume(uint8_t) { return false; }
::doorbell::Phrase last_reply() { return ::doorbell::Phrase::NONE; }
bool fault() { return false; }

#endif

}  // namespace canary::doorbell_audio
