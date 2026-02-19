/*
 * SecuraCV Canary — Audible Chirp (Local Alert Tones)
 *
 * Local audible/visual alert system. NOT BLE broadcast — this is a literal
 * chirp sound from a buzzer (or LED blink pattern as fallback).
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
// PLAYBACK
// ════════════════════════════════════════════════════════════════════════════

// Play a chirp pattern (blocking — patterns are short)
static void play(const ChirpNote* pattern) {
  if (!g_initialized) return;

  for (int i = 0; pattern[i].duration_ms > 0 || pattern[i].freq > 0; i++) {
    if (pattern[i].freq == 0) {
      // Silence gap
      ledcWrite(g_gpio, 0);
      delay(pattern[i].duration_ms);
    } else if (g_visual_only || pattern[i].freq == 1) {
      // Visual chirp (LED blink)
      #ifdef LED_BUILTIN
      digitalWrite(LED_BUILTIN, HIGH);
      delay(pattern[i].duration_ms);
      digitalWrite(LED_BUILTIN, LOW);
      #else
      delay(pattern[i].duration_ms);
      #endif
    } else {
      // Audio chirp via PWM
      ledcWriteTone(g_gpio, pattern[i].freq);
      ledcWrite(g_gpio, 128);  // 50% duty cycle
      delay(pattern[i].duration_ms);
      ledcWrite(g_gpio, 0);
    }
  }
  // Ensure silence after pattern
  ledcWrite(g_gpio, 0);
  g_chirps_played++;
}

// Play by pattern enum
static void play_pattern(ChirpPattern pat) {
  if (pat < PATTERN_COUNT) {
    play(PATTERNS[pat]);
  }
}

// Play by pattern name string
static bool play_by_name(const char* name) {
  for (uint8_t i = 0; i < PATTERN_COUNT; i++) {
    if (strcmp(name, PATTERN_NAMES[i]) == 0) {
      play(PATTERNS[i]);
      return true;
    }
  }
  return false;
}

// Convenience functions
static void chirp_confirm()  { play_pattern(PATTERN_CONFIRM); }
static void chirp_alert()    { play_pattern(PATTERN_ALERT); }
static void chirp_tamper()   { play_pattern(PATTERN_TAMPER); }
static void chirp_success()  { play_pattern(PATTERN_SUCCESS); }
static void chirp_error()    { play_pattern(PATTERN_ERROR); }

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
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
