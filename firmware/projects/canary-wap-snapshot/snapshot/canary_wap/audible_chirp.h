/*
 * SecuraCV Canary — Audible Chirp (Local Alert Tones)
 *
 * Local audible/visual alert system. NOT BLE broadcast — this is a literal
 * chirp sound from a buzzer (or LED blink pattern as fallback).
 *
 * Architecture:
 * - Non-blocking state machine: start_pattern() queues a pattern,
 *   update() advances one note at a time using millis() timing.
 * - Never blocks the main loop — patterns play across multiple loop() ticks.
 * - Boot chirp uses the blocking play() variant (acceptable at setup time).
 *
 * Uses:
 * - "I'm here" confirmation during setup
 * - Alert when presence threshold is crossed
 * - Tamper detection audible alarm
 * - Factory reset confirmation
 * - Test chirp from dashboard
 *
 * Hardware:
 * - Primary: PWM passive buzzer on configurable GPIO (default GPIO 2)
 * - Fallback: LED_BUILTIN blink patterns when no buzzer connected
 * - ESP32 LEDC peripheral for tone generation
 */

#ifndef SECURACV_AUDIBLE_CHIRP_H
#define SECURACV_AUDIBLE_CHIRP_H

#include "build_config.h"

#if FEATURE_AUDIBLE_CHIRP

#include <Arduino.h>

namespace audible_chirp {

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

#ifndef CHIRP_GPIO
  #if defined(HARDWARE_XIAO_ESP32S3)
    #define CHIRP_GPIO  2    // Safe GPIO on XIAO ESP32-S3
  #elif defined(HARDWARE_XIAO_ESP32C3)
    #define CHIRP_GPIO  3    // Safe GPIO on XIAO ESP32-C3
  #else
    #define CHIRP_GPIO  2
  #endif
#endif

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

struct ChirpNote {
  uint16_t freq;          // Frequency in Hz (0 = silence, 1 = LED-only)
  uint16_t duration_ms;   // Duration in ms (0 = end-of-pattern sentinel)
};

enum ChirpPattern : uint8_t {
  PATTERN_CONFIRM = 0,   // Two short beeps — "I'm here"
  PATTERN_ALERT,         // Rising three-tone — attention needed
  PATTERN_TAMPER,        // Rapid high-pitched — tamper detected
  PATTERN_SUCCESS,       // Pleasant ascending — operation succeeded
  PATTERN_ERROR,         // Descending buzz — operation failed
  PATTERN_COUNT
};

// ════════════════════════════════════════════════════════════════════════════
// PATTERNS
// ════════════════════════════════════════════════════════════════════════════

// "I'm here" — two short beeps
static const ChirpNote PATTERN_CONFIRM_NOTES[] = {
  {2000, 100}, {0, 50}, {2000, 100}, {0, 0}
};

// "Alert" — rising three-tone
static const ChirpNote PATTERN_ALERT_NOTES[] = {
  {1000, 150}, {0, 50}, {1500, 150}, {0, 50}, {2000, 200}, {0, 0}
};

// "Tamper" — rapid high-pitched
static const ChirpNote PATTERN_TAMPER_NOTES[] = {
  {3000, 80}, {0, 30}, {3000, 80}, {0, 30}, {3000, 80}, {0, 30},
  {3000, 80}, {0, 30}, {3000, 80}, {0, 0}
};

// "Success" — pleasant ascending (C5-E5-G5)
static const ChirpNote PATTERN_SUCCESS_NOTES[] = {
  {523, 100}, {0, 30}, {659, 100}, {0, 30}, {784, 150}, {0, 0}
};

// "Error" — descending buzz
static const ChirpNote PATTERN_ERROR_NOTES[] = {
  {400, 200}, {0, 50}, {300, 300}, {0, 0}
};

static const ChirpNote* PATTERNS[] = {
  PATTERN_CONFIRM_NOTES,
  PATTERN_ALERT_NOTES,
  PATTERN_TAMPER_NOTES,
  PATTERN_SUCCESS_NOTES,
  PATTERN_ERROR_NOTES
};

static const char* PATTERN_NAMES[] = {
  "confirm", "alert", "tamper", "success", "error"
};

// ════════════════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════════════════

static bool g_initialized = false;
static bool g_visual_only = true;   // Fallback: LED blink only
static uint8_t g_gpio = CHIRP_GPIO;
static uint32_t g_chirps_played = 0;

// Non-blocking playback state machine
static const ChirpNote* g_active_pattern = nullptr;  // Currently playing pattern (null = idle)
static uint8_t  g_note_idx = 0;                      // Current note index
static uint32_t g_note_start_ms = 0;                 // When current note started
static bool     g_note_started = false;               // Whether current note's output is active

// ════════════════════════════════════════════════════════════════════════════
// INITIALIZATION
// ════════════════════════════════════════════════════════════════════════════

static bool init(uint8_t gpio = CHIRP_GPIO) {
  g_gpio = gpio;

  // Configure GPIO for PWM tone output
  pinMode(g_gpio, OUTPUT);
  digitalWrite(g_gpio, LOW);

  // ESP32 Arduino Core 3.x LEDC API
  ledcAttach(g_gpio, 2000, 8);
  ledcWrite(g_gpio, 0);  // Start silent

  g_initialized = true;
  g_visual_only = false;  // Assume buzzer may be connected

  Serial.printf("[CHIRP] Audible chirp initialized on GPIO %d\n", g_gpio);
  Serial.println("[CHIRP] Connect a passive buzzer for audio alerts, otherwise LED blink is used");

  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// NON-BLOCKING PLAYBACK — call update() from loop()
// ════════════════════════════════════════════════════════════════════════════

// Begin a note's output (buzzer tone or LED on)
static void begin_note(const ChirpNote* note) {
  if (note->freq == 0) {
    // Silence: ensure everything off
    ledcWrite(g_gpio, 0);
    #ifdef LED_BUILTIN
    digitalWrite(LED_BUILTIN, LOW);
    #endif
  } else if (g_visual_only || note->freq == 1) {
    // Visual chirp (LED blink)
    #ifdef LED_BUILTIN
    digitalWrite(LED_BUILTIN, HIGH);
    #endif
  } else {
    // Audio chirp via PWM
    ledcWriteTone(g_gpio, note->freq);
    ledcWrite(g_gpio, 128);  // 50% duty cycle
  }
}

// End a note's output
static void end_note() {
  ledcWrite(g_gpio, 0);
  #ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, LOW);
  #endif
}

// Start playing a pattern (non-blocking — advances via update())
static void start_pattern(const ChirpNote* pattern) {
  if (!g_initialized || !pattern) return;
  // If already playing, the new pattern preempts the old one
  end_note();
  g_active_pattern = pattern;
  g_note_idx = 0;
  g_note_started = false;
  g_note_start_ms = millis();
}

// Advance the state machine. Call this from loop().
// Returns true if a pattern is currently playing.
static bool update() {
  if (!g_active_pattern) return false;

  const ChirpNote* note = &g_active_pattern[g_note_idx];

  // Check for end-of-pattern sentinel
  if (note->duration_ms == 0 && note->freq == 0) {
    end_note();
    g_active_pattern = nullptr;
    g_chirps_played++;
    return false;
  }

  // Start the current note if not yet started
  if (!g_note_started) {
    begin_note(note);
    g_note_started = true;
    g_note_start_ms = millis();
    return true;
  }

  // Check if current note duration has elapsed
  if (millis() - g_note_start_ms >= note->duration_ms) {
    end_note();
    g_note_idx++;
    g_note_started = false;
  }

  return true;
}

// Check if currently playing
static bool is_playing() { return g_active_pattern != nullptr; }

// ════════════════════════════════════════════════════════════════════════════
// BLOCKING PLAYBACK — only for boot/setup, not loop() context
// ════════════════════════════════════════════════════════════════════════════

// Blocking play (use only at boot time or from API handlers where brief
// blocking is acceptable). Patterns are short (<600ms worst case).
static void play_blocking(const ChirpNote* pattern) {
  if (!g_initialized) return;

  for (int i = 0; pattern[i].duration_ms > 0 || pattern[i].freq > 0; i++) {
    begin_note(&pattern[i]);
    if (pattern[i].duration_ms > 0) {
      delay(pattern[i].duration_ms);
    }
    end_note();
  }
  g_chirps_played++;
}

// ════════════════════════════════════════════════════════════════════════════
// CONVENIENCE FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

// Non-blocking (for loop() context — requires update() calls)
static void play_pattern(ChirpPattern pat) {
  if (pat < PATTERN_COUNT) {
    start_pattern(PATTERNS[pat]);
  }
}

// By name — non-blocking
static bool play_by_name(const char* name) {
  for (uint8_t i = 0; i < PATTERN_COUNT; i++) {
    if (strcmp(name, PATTERN_NAMES[i]) == 0) {
      start_pattern(PATTERNS[i]);
      return true;
    }
  }
  return false;
}

// Blocking convenience (for boot/setup and API handlers)
static void chirp_confirm()  { play_blocking(PATTERN_CONFIRM_NOTES); }
static void chirp_alert()    { play_blocking(PATTERN_ALERT_NOTES); }
static void chirp_tamper()   { play_blocking(PATTERN_TAMPER_NOTES); }
static void chirp_success()  { play_blocking(PATTERN_SUCCESS_NOTES); }
static void chirp_error()    { play_blocking(PATTERN_ERROR_NOTES); }

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION ACCESSORS
// ════════════════════════════════════════════════════════════════════════════

static void set_visual_only(bool visual) { g_visual_only = visual; }
static bool is_visual_only() { return g_visual_only; }
static bool is_available() { return g_initialized; }
static uint8_t get_gpio() { return g_gpio; }
static uint32_t get_chirps_played() { return g_chirps_played; }

static const char* pattern_name(ChirpPattern pat) {
  if (pat < PATTERN_COUNT) return PATTERN_NAMES[pat];
  return "unknown";
}

} // namespace audible_chirp

#else // !FEATURE_AUDIBLE_CHIRP

namespace audible_chirp {
  static inline bool init(uint8_t gpio = 2) { (void)gpio; return false; }
  static inline bool update() { return false; }
  static inline bool is_playing() { return false; }
  static inline void play_pattern(uint8_t) {}
  static inline bool play_by_name(const char*) { return false; }
  static inline void chirp_confirm() {}
  static inline void chirp_alert() {}
  static inline void chirp_tamper() {}
  static inline void chirp_success() {}
  static inline void chirp_error() {}
  static inline void set_visual_only(bool) {}
  static inline bool is_visual_only() { return true; }
  static inline bool is_available() { return false; }
  static inline uint8_t get_gpio() { return 0; }
  static inline uint32_t get_chirps_played() { return 0; }
  static inline const char* pattern_name(uint8_t) { return "unavailable"; }
}

#endif // FEATURE_AUDIBLE_CHIRP

#endif // SECURACV_AUDIBLE_CHIRP_H
