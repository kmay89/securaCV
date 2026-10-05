// The Vision Doorbell's button and glow ring — pins, ISR, LEDC and NVS
// around firmware/common/doorbell/doorbell_logic.h. See canary/doorbell.h.

#include "canary/doorbell.h"

#include <Arduino.h>
#include <Preferences.h>

#include "pins.h"
#include "canary/log.h"

#if defined(DOORBELL_BUTTON_PIN) && defined(DOORBELL_GLOW_PIN)
#define CV_DOORBELL 1
#include <esp_arduino_version.h>
#include <esp_timer.h>
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#else
#define CV_DOORBELL 0
#endif

namespace canary::doorbell_hw {

#if CV_DOORBELL

namespace {

// Same NVS namespace as runtime_config/detect_config; db_ keys.
constexpr const char* NVS_NS = "securacv";

// 20 kHz: above hearing (no coil whine from the LED's MOSFET) and far past
// the flicker a visitor's phone camera would band on. 10 bits at 20 kHz
// fits the LEDC clock on both the C3 and the S3.
constexpr uint32_t kPwmHz = 20000;
constexpr uint8_t kPwmBits = 10;
#if !(defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3)
constexpr uint8_t kPwmChannel = 4;  // nothing else in this firmware uses LEDC
#endif
constexpr uint32_t kGlowTickUs = 20000;  // 50 Hz glow updates

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// ISR edge queue: every level change on the button pin, timestamped, so a
// tap that begins and ends inside one long NPU invoke is still seen.
struct Edge {
  uint32_t ms;
  bool high;  // pin level after the edge (HIGH = released)
};
constexpr uint8_t kEdgeCap = 16;
Edge s_edges[kEdgeCap];
volatile uint8_t s_edge_head = 0;  // next write
volatile uint8_t s_edge_len = 0;

::doorbell::ButtonState s_btn{};
::doorbell::GlowState s_glow{};
bool s_inited = false;
bool s_pwm_attached = false;
esp_timer_handle_t s_timer = nullptr;

volatile bool s_enabled = false;
volatile bool s_hub_ok = false;
volatile uint8_t s_pct = ::doorbell::kGlowPctDefault;
uint32_t s_rings = 0;

inline bool pin_high_isr() {
  return ((REG_READ(GPIO_IN_REG) >> DOORBELL_BUTTON_PIN) & 1u) != 0;
}

void IRAM_ATTR on_edge() {
  const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
  const bool high = pin_high_isr();
  portENTER_CRITICAL_ISR(&s_mux);
  if (s_edge_len < kEdgeCap) {
    s_edges[s_edge_head] = Edge{ms, high};
    s_edge_head = (uint8_t)((s_edge_head + 1) % kEdgeCap);
    s_edge_len = (uint8_t)(s_edge_len + 1);
  }
  // A full queue drops the edge; the loop's own poll of the level still
  // catches anything that lasts until it looks.
  portEXIT_CRITICAL_ISR(&s_mux);
}

void pwm_attach() {
  if (s_pwm_attached) return;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(DOORBELL_GLOW_PIN, kPwmHz, kPwmBits);
  ledcWrite(DOORBELL_GLOW_PIN, 0);
#else
  ledcSetup(kPwmChannel, kPwmHz, kPwmBits);
  ledcAttachPin(DOORBELL_GLOW_PIN, kPwmChannel);
  ledcWrite(kPwmChannel, 0);
#endif
  s_pwm_attached = true;
}

void pwm_write(uint16_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(DOORBELL_GLOW_PIN, duty);
#else
  ledcWrite(kPwmChannel, duty);
#endif
}

// 50 Hz, on the esp_timer task: the glow keeps breathing smoothly while
// the main loop sits in an NPU invoke or a broker connect attempt.
void glow_tick(void*) {
  const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  uint16_t duty;
  portENTER_CRITICAL(&s_mux);
  s_glow.mode = ::doorbell::glow_mode_for(s_enabled, s_btn.stuck, s_hub_ok);
  s_glow.pct = s_pct;
  duty = ::doorbell::glow_step(&s_glow, now);
  portEXIT_CRITICAL(&s_mux);
  if (s_pwm_attached) pwm_write(duty);
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
      s_enabled = prefs.getUChar("db_on", 0) != 0;
      s_pct = ::doorbell::clamp_glow_pct(prefs.getUChar("db_glow", ::doorbell::kGlowPctDefault));
      prefs.end();
    }
  }

  pinMode(DOORBELL_BUTTON_PIN, INPUT_PULLUP);
  delay(2);  // let the pull-up charge the line before the first read
  const uint32_t now = millis();
  s_btn = ::doorbell::begin(digitalRead(DOORBELL_BUTTON_PIN) == LOW, now);
  s_glow = ::doorbell::glow_begin(s_pct, now);
  attachInterrupt(digitalPinToInterrupt(DOORBELL_BUTTON_PIN), on_edge, CHANGE);

  // The glow pin stays high-impedance until the doorbell is on (the MOSFET's
  // gate pull-down holds the LED dark), so a plain Vision never drives D2.
  if (s_enabled) pwm_attach();

  esp_timer_create_args_t args{};
  args.callback = &glow_tick;
  args.arg = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "cv_glow";
  if (esp_timer_create(&args, &s_timer) == ESP_OK) {
    esp_timer_start_periodic(s_timer, kGlowTickUs);
  }
  s_inited = true;

  canary::log_header("BELL");
  canary::dbg_serial().printf("Doorbell %s (button D1, glow D2, glow %u%%)%s\n",
                              s_enabled ? "on" : "off - the first press turns it on",
                              (unsigned)s_pct,
                              s_btn.armed ? "" : " - button held at boot: armed after release");
}

void poll(uint32_t now_ms, void (*on_event)(::doorbell::ButtonEvent, uint32_t)) {
  if (!s_inited) return;
  Edge batch[kEdgeCap];
  uint8_t n = 0;
  portENTER_CRITICAL(&s_mux);
  while (s_edge_len > 0) {
    const uint8_t tail = (uint8_t)((s_edge_head + kEdgeCap - s_edge_len) % kEdgeCap);
    batch[n++] = s_edges[tail];
    s_edge_len = (uint8_t)(s_edge_len - 1);
  }
  portEXIT_CRITICAL(&s_mux);

  auto feed = [&](bool pressed, uint32_t t) {
    ::doorbell::ButtonEvent ev;
    portENTER_CRITICAL(&s_mux);
    ev = ::doorbell::sample(&s_btn, pressed, t);
    portEXIT_CRITICAL(&s_mux);
    if (ev == ::doorbell::ButtonEvent::RING) ++s_rings;
    if (ev != ::doorbell::ButtonEvent::NONE && on_event) on_event(ev, t);
  };
  for (uint8_t i = 0; i < n; ++i) feed(!batch[i].high, batch[i].ms);
  // The level as of now (an edge stamped a hair after now_ms is fed as now).
  uint32_t t = now_ms;
  if (n && (int32_t)(batch[n - 1].ms - t) > 0) t = batch[n - 1].ms;
  feed(digitalRead(DOORBELL_BUTTON_PIN) == LOW, t);
}

void set_hub_ok(bool ok) { s_hub_ok = ok; }

void swell(uint32_t now_ms) {
  portENTER_CRITICAL(&s_mux);
  ::doorbell::glow_swell(&s_glow, now_ms);
  portEXIT_CRITICAL(&s_mux);
}

bool enabled() { return s_enabled; }

bool set_enabled(bool on) {
  if (s_enabled == on) return false;
  if (on) pwm_attach();
  s_enabled = on;  // off: the timer eases the glow down to dark
  persist_uchar("db_on", on ? 1 : 0);
  canary::log_line("BELL", on ? "Doorbell on." : "Doorbell off (the ring goes dark).");
  return true;
}

uint8_t glow_pct() { return s_pct; }

bool set_glow_pct(uint8_t pct) {
  const uint8_t v = ::doorbell::clamp_glow_pct(pct);
  if (v == s_pct) return false;
  s_pct = v;
  persist_uchar("db_glow", v);
  return true;
}

bool stuck() { return s_btn.stuck; }
uint32_t repeats() { return s_btn.repeats; }
uint32_t rings() { return s_rings; }

#else  // no doorbell pins on this board

bool available() { return false; }
void init() {}
void poll(uint32_t, void (*)(::doorbell::ButtonEvent, uint32_t)) {}
void set_hub_ok(bool) {}
void swell(uint32_t) {}
bool enabled() { return false; }
bool set_enabled(bool) { return false; }
uint8_t glow_pct() { return ::doorbell::kGlowPctDefault; }
bool set_glow_pct(uint8_t) { return false; }
bool stuck() { return false; }
uint32_t repeats() { return 0; }
uint32_t rings() { return 0; }

#endif

}  // namespace canary::doorbell_hw
