// canary-local/emulator/src/emu_support.cpp — the silicon boundary.
//
// Implements the Arduino/ESP shim surface for the wasm build: virtual
// clock (scenario-scalable), NVS as a JS-mirrored map, Serial → the
// page's serial panel, LEDC → backlight/chime callbacks, entropy from
// the page. Everything here is I/O plumbing; no firmware behavior lives
// in this file — that is the point.
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <LittleFS.h>

#include "canary/log.h"

#include <emscripten.h>

#include <map>
#include <string>
#include <vector>

#include "emu_bus.h"

// ── JS imports ──────────────────────────────────────────────────────────
EM_JS(void, js_serial_write, (const char* s), {
  if (Module.onSerial) Module.onSerial(UTF8ToString(s));
});
EM_JS(void, js_backlight, (int level, int is_night_profile, int duty13), {
  if (Module.onBacklight) Module.onBacklight(level, !!is_night_profile, duty13);
});
EM_JS(void, js_tone, (int freq_hz, int gain_permille), {
  if (Module.onTone) Module.onTone(freq_hz, gain_permille / 1000);
});
EM_JS(void, js_nvs_write, (const char* ns, const char* key, const char* hex), {
  if (Module.onNvsWrite) Module.onNvsWrite(UTF8ToString(ns), UTF8ToString(key), UTF8ToString(hex));
});
EM_JS(void, js_reboot_request, (), {
  if (Module.onReboot) Module.onReboot();
});
EM_JS(double, js_entropy, (), { return Math.random(); });

// ── Virtual clock ───────────────────────────────────────────────────────
// millis() = (wall ms since boot, accumulated through the scale knob).
// The scale only stretches the *future*: changing it never rewinds what
// the firmware has already seen (monotonic by construction).
namespace {
double g_virt_ms = 0.0;
double g_last_wall_ms = 0.0;
double g_scale = 1.0;
double g_epoch_offset_s = 0.0;  // added to real epoch for time() wrapping
bool g_clock_started = false;

double wall_now_ms() { return emscripten_get_now(); }

void clock_tick() {
  const double now = wall_now_ms();
  if (!g_clock_started) {
    g_last_wall_ms = now;
    g_clock_started = true;
  }
  g_virt_ms += (now - g_last_wall_ms) * g_scale;
  g_last_wall_ms = now;
}
}  // namespace

extern "C" {

uint32_t millis(void) {
  clock_tick();
  return (uint32_t)g_virt_ms;
}

uint32_t micros(void) { return millis() * 1000u; }

void delay(uint32_t ms) {
  // Asyncify: yield to the browser event loop for the firmware's own
  // pacing. Under time-lapse the wall wait shrinks so the emulated device
  // actually runs faster, exactly as the virtual clock claims.
  clock_tick();
  double wall = (double)ms / (g_scale > 0.0 ? g_scale : 1.0);
  if (wall < 0.0) wall = 0.0;
  if (wall > 250.0) wall = 250.0;  // stay responsive during long story beats
  emscripten_sleep((unsigned)wall);
  clock_tick();
}

// Scenario knobs (exported in emu_bindings.cpp).
void emu_clock_set_scale(double scale) {
  clock_tick();
  g_scale = scale <= 0.0 ? 1.0 : scale;
}
double emu_clock_get_scale(void) { return g_scale; }
void emu_clock_step(double ms) { g_virt_ms += ms > 0.0 ? ms : 0.0; }
void emu_clock_set_epoch_offset(double s) { g_epoch_offset_s = s; }

// time() wrap (linked with -Wl,--wrap=time): virtual epoch = browser
// epoch + scenario offset + how far the virtual clock has outrun the
// wall, so time-lapse moves the wall clock and quiet-hours schedule
// together with millis(). At scale 1 the drift term stays ~0 and the
// emulated device simply lives in the visitor's real present.
time_t __real_time(time_t* t);
time_t __wrap_time(time_t* t) {
  clock_tick();
  const double real_epoch = (double)__real_time(nullptr);
  const time_t out =
      (time_t)(real_epoch + g_epoch_offset_s + (g_virt_ms - wall_now_ms()) / 1000.0);
  if (t) *t = out;
  return out;
}

// ── LEDC ────────────────────────────────────────────────────────────────
// Channel map (from the board pins + chime engine): the backlight rides
// one channel, the chime tone another. We track per-channel config to
// tell "backlight duty" apart from "tone frequency".
namespace {
struct LedcChan {
  uint32_t freq_hz = 0;
  uint32_t duty = 0;
  uint8_t res_bits = 8;
  bool attached = false;
  bool is_tone = false;
};
LedcChan g_ledc[16];

// Voice the chime channel to Web Audio. A passive piezo is loudest as its duty
// approaches 50%, and the chime engine writes a duty in [0, max/2] to shape
// each note's envelope + volume (voice_score.h). Map that to a 0..1 gain so
// the browser reproduces the REAL firmware-driven envelope/glissando/warble —
// not a flat beep. A zero frequency (a rest, or the phrase end) is silence.
void emit_tone(uint8_t channel) {
  const LedcChan& c = g_ledc[channel];
  int gain_permille = 0;
  if (c.freq_hz > 0) {
    const uint32_t maxduty =
        c.res_bits >= 31 ? 0xFFFFFFFFu : ((1u << c.res_bits) - 1u);
    const uint32_t half = maxduty / 2 ? maxduty / 2 : 1;
    const uint32_t d = c.duty > half ? half : c.duty;
    gain_permille = (int)((uint64_t)d * 1000u / half);
  }
  js_tone(c.freq_hz > 0 ? (int)c.freq_hz : 0, gain_permille);
}
}  // namespace

void ledcSetup(uint8_t channel, uint32_t freq_hz, uint8_t resolution_bits) {
  if (channel >= 16) return;
  g_ledc[channel].freq_hz = freq_hz;
  g_ledc[channel].res_bits = resolution_bits;
}
void ledcAttachPin(uint8_t /*pin*/, uint8_t channel) {
  if (channel < 16) g_ledc[channel].attached = true;
}
void ledcWrite(uint8_t channel, uint32_t duty) {
  if (channel >= 16) return;
  g_ledc[channel].duty = duty;
  // The chime rides its own LEDC channel (marked is_tone by ledcWriteTone); its
  // duty is the note envelope, so it drives audio gain, NOT the backlight glow.
  if (g_ledc[channel].is_tone) {
    emit_tone(channel);
    return;
  }
  emu_bus_ledc_write(channel, duty, g_ledc[channel].freq_hz,
                     g_ledc[channel].res_bits);
}
void ledcWriteTone(uint8_t channel, uint32_t freq_hz) {
  if (channel >= 16) return;
  g_ledc[channel].is_tone = true;
  g_ledc[channel].freq_hz = freq_hz;
  emit_tone(channel);  // authoritative gain follows in the paired ledcWrite
}

// ── Entropy ─────────────────────────────────────────────────────────────
static uint64_t g_rng_state = 0x9E3779B97F4A7C15ull;
static bool g_rng_seeded = false;

static uint32_t rng_next() {
  if (!g_rng_seeded) {
    g_rng_state ^= (uint64_t)(js_entropy() * 4294967296.0);
    g_rng_state ^= ((uint64_t)(js_entropy() * 4294967296.0)) << 32;
    g_rng_seeded = true;
  }
  // xorshift64*
  g_rng_state ^= g_rng_state >> 12;
  g_rng_state ^= g_rng_state << 25;
  g_rng_state ^= g_rng_state >> 27;
  return (uint32_t)((g_rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}

uint32_t esp_random(void) { return rng_next(); }
void esp_fill_random(void* buf, size_t len) {
  uint8_t* p = (uint8_t*)buf;
  for (size_t i = 0; i < len; i++) p[i] = (uint8_t)(rng_next() & 0xFF);
}
void emu_rng_seed(uint32_t seed) {
  g_rng_state = seed ? seed : 1;
  g_rng_seeded = true;
}

long emu_random_below(long max_exclusive) {
  if (max_exclusive <= 0) return 0;
  return (long)(rng_next() % (uint32_t)max_exclusive);
}
long emu_random_between(long lo, long hi) {
  if (hi <= lo) return lo;
  return lo + (long)(rng_next() % (uint32_t)(hi - lo));
}
void emu_random_seed(unsigned long seed) { emu_rng_seed((uint32_t)seed); }

}  // extern "C"

// ── Serial ──────────────────────────────────────────────────────────────
void EmuSerial::write_str(const char* s) { js_serial_write(s); }
EmuSerial Serial;

// log.h's optional second sink (defined by glass_web.cpp on silicon —
// that TU is the real WebServer, replaced here). The page's serial panel
// already hears every line through Serial, so no second sink is wired.
namespace canary {
LogSink g_log_sink = nullptr;
}

// ── ESP object ──────────────────────────────────────────────────────────
uint32_t EmuESP::getFreeHeap() { return 220 * 1024; }
void EmuESP::restart() {
  js_reboot_request();
  // The page tears this instance down and boots a fresh module — a real
  // reboot, including the splash. Sleep forever until it does.
  for (;;) emscripten_sleep(1000);
}
EmuESP ESP;

void configTzTime(const char* tz, const char*, const char*, const char*) {
  if (tz && tz[0]) {
    setenv("TZ", tz, 1);
    tzset();
  }
}

// ── WiFi globals ────────────────────────────────────────────────────────
wl_status_t WiFiClass::status() {
  return emu_bus_wifi_up() ? WL_CONNECTED : WL_DISCONNECTED;
}
int32_t WiFiClass::RSSI() { return emu_bus_wifi_rssi(); }
WiFiClass WiFi;
LittleFSClass LittleFS;

// ── NVS (Preferences) ───────────────────────────────────────────────────
// One flat map of "ns/key" → bytes. JS preloads it before boot (persisted
// page-side) and hears every write, so the inspector panel can show the
// device's memory forming in real time.
namespace {
std::map<std::string, std::vector<uint8_t>>& nvs() {
  static std::map<std::string, std::vector<uint8_t>> m;
  return m;
}
std::string nvs_key(const char* ns, const char* key) {
  return std::string(ns) + "/" + key;
}
const char* hex_of(const std::vector<uint8_t>& v) {
  static std::string s;
  static const char H[] = "0123456789abcdef";
  s.clear();
  for (uint8_t b : v) {
    s.push_back(H[b >> 4]);
    s.push_back(H[b & 0xF]);
  }
  return s.c_str();
}
}  // namespace

extern "C" {
void emu_nvs_put(const char* ns, const char* key, const uint8_t* data, int len) {
  nvs()[nvs_key(ns, key)] = std::vector<uint8_t>(data, data + len);
}
void emu_nvs_wipe(void) { nvs().clear(); }
int emu_nvs_get(const char* ns, const char* key, uint8_t* out, int cap) {
  auto it = nvs().find(nvs_key(ns, key));
  if (it == nvs().end()) return -1;
  const int n = (int)it->second.size() < cap ? (int)it->second.size() : cap;
  memcpy(out, it->second.data(), n);
  return n;
}
}

bool Preferences::begin(const char* ns, bool) {
  strncpy(ns_, ns ? ns : "", sizeof(ns_) - 1);
  ns_[sizeof(ns_) - 1] = 0;
  return true;
}
void Preferences::end() { ns_[0] = 0; }

static void put_raw(const char* ns, const char* key, const void* p, size_t n) {
  auto& slot = nvs()[nvs_key(ns, key)];
  slot.assign((const uint8_t*)p, (const uint8_t*)p + n);
  js_nvs_write(ns, key, hex_of(slot));
}
static bool get_raw(const char* ns, const char* key, void* out, size_t n) {
  auto it = nvs().find(nvs_key(ns, key));
  if (it == nvs().end() || it->second.size() != n) return false;
  memcpy(out, it->second.data(), n);
  return true;
}

uint8_t Preferences::getUChar(const char* key, uint8_t def) {
  uint8_t v;
  return get_raw(ns_, key, &v, 1) ? v : def;
}
size_t Preferences::putUChar(const char* key, uint8_t v) {
  put_raw(ns_, key, &v, 1);
  return 1;
}
uint16_t Preferences::getUShort(const char* key, uint16_t def) {
  uint16_t v;
  return get_raw(ns_, key, &v, 2) ? v : def;
}
size_t Preferences::putUShort(const char* key, uint16_t v) {
  put_raw(ns_, key, &v, 2);
  return 2;
}
int16_t Preferences::getShort(const char* key, int16_t def) {
  int16_t v;
  return get_raw(ns_, key, &v, 2) ? v : def;
}
size_t Preferences::putShort(const char* key, int16_t v) {
  put_raw(ns_, key, &v, 2);
  return 2;
}
uint32_t Preferences::getUInt(const char* key, uint32_t def) {
  uint32_t v;
  return get_raw(ns_, key, &v, 4) ? v : def;
}
size_t Preferences::putUInt(const char* key, uint32_t v) {
  put_raw(ns_, key, &v, 4);
  return 4;
}
int32_t Preferences::getInt(const char* key, int32_t def) {
  int32_t v;
  return get_raw(ns_, key, &v, 4) ? v : def;
}
size_t Preferences::putInt(const char* key, int32_t v) {
  put_raw(ns_, key, &v, 4);
  return 4;
}
int64_t Preferences::getLong64(const char* key, int64_t def) {
  int64_t v;
  return get_raw(ns_, key, &v, 8) ? v : def;
}
size_t Preferences::putLong64(const char* key, int64_t v) {
  put_raw(ns_, key, &v, 8);
  return 8;
}
String Preferences::getString(const char* key, const String& def) {
  auto it = nvs().find(nvs_key(ns_, key));
  if (it == nvs().end()) return def;
  return String(std::string(it->second.begin(), it->second.end()));
}
size_t Preferences::getString(const char* key, char* out, size_t cap) {
  auto it = nvs().find(nvs_key(ns_, key));
  if (it == nvs().end() || cap == 0) return 0;
  size_t n = it->second.size();
  if (n >= cap) n = cap - 1;
  memcpy(out, it->second.data(), n);
  out[n] = 0;
  return n;
}
size_t Preferences::putString(const char* key, const char* v) {
  const size_t n = strlen(v);
  put_raw(ns_, key, v, n);
  return n;
}
size_t Preferences::getBytes(const char* key, void* out, size_t cap) {
  auto it = nvs().find(nvs_key(ns_, key));
  if (it == nvs().end()) return 0;
  size_t n = it->second.size();
  if (n > cap) n = cap;
  memcpy(out, it->second.data(), n);
  return n;
}
size_t Preferences::putBytes(const char* key, const void* data, size_t len) {
  put_raw(ns_, key, data, len);
  return len;
}
size_t Preferences::getBytesLength(const char* key) {
  auto it = nvs().find(nvs_key(ns_, key));
  return it == nvs().end() ? 0 : it->second.size();
}
bool Preferences::isKey(const char* key) {
  return nvs().count(nvs_key(ns_, key)) != 0;
}
bool Preferences::remove(const char* key) {
  return nvs().erase(nvs_key(ns_, key)) != 0;
}
bool Preferences::clear() {
  const std::string prefix = std::string(ns_) + "/";
  for (auto it = nvs().begin(); it != nvs().end();) {
    if (it->first.rfind(prefix, 0) == 0) it = nvs().erase(it);
    else ++it;
  }
  return true;
}

// ── rweather/Crypto RNG global (Ed25519.cpp references it for sign paths
//    the display never calls; verify needs nothing random) ──────────────
#include "RNG.h"
RNGClass::RNGClass() {}
RNGClass::~RNGClass() {}
void RNGClass::rand(uint8_t* data, size_t len) { esp_fill_random(data, len); }
RNGClass RNG;
