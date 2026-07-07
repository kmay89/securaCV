// src/hal/chime.cpp — non-blocking LEDC tone patterns (spec §5).
#include "canary/hal/chime.h"

#include <Arduino.h>

namespace canary::hal {

namespace {

// LEDC channel 1: the watch backlight owns channel 0; the dash uses none.
constexpr uint8_t LEDC_CH = 1;

struct Step {
  uint16_t freq_hz;  // 0 = rest
  uint16_t ms;
};

// Tier 1 — urgency grammar: fast burst, wide pitch movement (IEC 60601-1-8
// high-priority shape). Re-voicing until ack is the caller's job.
constexpr Step TIER1[] = {
    {2600, 70}, {0, 40}, {3100, 70}, {0, 40}, {2600, 70}, {0, 40},
    {3100, 70}, {0, 40}, {2600, 70}, {0, 40}, {3100, 70}, {0, 200},
    {2600, 70}, {0, 40}, {3100, 70}, {0, 40}, {2600, 70}, {0, 40},
    {3100, 70}, {0, 0},
};

// Tier 2 — three slower, softer pulses. Distinct tempo AND pitch from
// tier 1: a fault must never sound like an intruder.
constexpr Step TIER2[] = {
    {1800, 140}, {0, 240}, {1800, 140}, {0, 240}, {1800, 140}, {0, 0},
};

// All-clear — falling two-tone: resolution has a sound, so silence keeps
// meaning "nothing new".
constexpr Step ALL_CLEAR[] = {
    {1300, 160}, {0, 60}, {900, 220}, {0, 0},
};

int s_pin = -1;
const Step* s_seq = nullptr;
int s_len = 0;
int s_idx = -1;
uint32_t s_step_until_ms = 0;

void tone_out(uint16_t freq) {
  if (s_pin < 0) return;
  if (freq == 0) {
    ledcWriteTone(LEDC_CH, 0);
  } else {
    ledcWriteTone(LEDC_CH, freq);
    // Passive piezo at ~50% duty; ledcWriteTone sets its own duty, this
    // keeps loudness deterministic across core versions.
    ledcWrite(LEDC_CH, 127);
  }
}

}  // namespace

void chime_init(int pin) {
  if (pin < 0) return;
  s_pin = pin;
  ledcSetup(LEDC_CH, 2000, 8);
  ledcAttachPin((uint8_t)pin, LEDC_CH);
  ledcWriteTone(LEDC_CH, 0);
}

void chime_play(Chime c) {
  if (s_pin < 0) return;
  switch (c) {
    case Chime::Tier1Alarm: s_seq = TIER1;     s_len = sizeof(TIER1) / sizeof(TIER1[0]);         break;
    case Chime::Tier2Warn:  s_seq = TIER2;     s_len = sizeof(TIER2) / sizeof(TIER2[0]);         break;
    case Chime::AllClear:   s_seq = ALL_CLEAR; s_len = sizeof(ALL_CLEAR) / sizeof(ALL_CLEAR[0]); break;
  }
  s_idx = -1;
  s_step_until_ms = 0;  // advance immediately on the next loop pass
}

void chime_loop(uint32_t now_ms) {
  if (s_pin < 0 || !s_seq) return;
  if (s_idx >= 0 && (int32_t)(now_ms - s_step_until_ms) < 0) return;

  s_idx++;
  if (s_idx >= s_len || (s_seq[s_idx].freq_hz == 0 && s_seq[s_idx].ms == 0)) {
    tone_out(0);
    s_seq = nullptr;
    s_idx = -1;
    return;
  }
  tone_out(s_seq[s_idx].freq_hz);
  s_step_until_ms = now_ms + s_seq[s_idx].ms;
}

}  // namespace canary::hal
