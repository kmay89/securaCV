// include/canary/care/comfort.h — bedroom comfort words (nightstand wave).
//
// Turns the temp/humidity the fleet already publishes into words a sleepy
// person can act on. Bands follow the sleep-science consensus (Sleep
// Foundation / Cleveland Clinic: 15.5–19.5 °C ideal for sleep; 40–60% RH
// ideal, <30% dries airways, >60% blocks evaporative cooling). Hysteresis
// (0.5 °C / 3%) stops the word flickering at a band edge all night.
//
// Pure logic, no Arduino types — host-tested like attention.h/rhythm.h.
#pragma once
#include <stdint.h>

namespace canary::care {

enum class TempBand : uint8_t { None, Cold, Cool, Ideal, Warm, TooWarm };
enum class RhBand : uint8_t { None, TooDry, Dry, Ideal, Humid, TooHumid };

// Band edges in tenths of °C. Order matches TempBand: below EDGE[i] is
// band i+1 (Cold < 130, Cool < 155, Ideal < 195, Warm < 220, else TooWarm).
inline TempBand temp_band(int c10, TempBand prev) {
  static const int EDGE[4] = {130, 155, 195, 220};
  static const int HYST = 5;  // 0.5 °C
  int lo = 0, hi = 0;
  switch (prev) {  // widen the previous band by the hysteresis margin
    case TempBand::Cold:    lo = -32768;      hi = EDGE[0] + HYST; break;
    case TempBand::Cool:    lo = EDGE[0] - HYST; hi = EDGE[1] + HYST; break;
    case TempBand::Ideal:   lo = EDGE[1] - HYST; hi = EDGE[2] + HYST; break;
    case TempBand::Warm:    lo = EDGE[2] - HYST; hi = EDGE[3] + HYST; break;
    case TempBand::TooWarm: lo = EDGE[3] - HYST; hi = 32767; break;
    default: break;
  }
  if (prev != TempBand::None && c10 >= lo && c10 < hi) return prev;
  if (c10 < EDGE[0]) return TempBand::Cold;
  if (c10 < EDGE[1]) return TempBand::Cool;
  if (c10 < EDGE[2]) return TempBand::Ideal;
  if (c10 < EDGE[3]) return TempBand::Warm;
  return TempBand::TooWarm;
}

inline RhBand rh_band(int pct, RhBand prev) {
  static const int EDGE[4] = {30, 40, 60, 70};
  static const int HYST = 3;
  int lo = 0, hi = 0;
  switch (prev) {
    case RhBand::TooDry:   lo = -128;         hi = EDGE[0] + HYST; break;
    case RhBand::Dry:      lo = EDGE[0] - HYST; hi = EDGE[1] + HYST; break;
    case RhBand::Ideal:    lo = EDGE[1] - HYST; hi = EDGE[2] + HYST; break;
    case RhBand::Humid:    lo = EDGE[2] - HYST; hi = EDGE[3] + HYST; break;
    case RhBand::TooHumid: lo = EDGE[3] - HYST; hi = 127; break;
    default: break;
  }
  if (prev != RhBand::None && pct >= lo && pct < hi) return prev;
  if (pct < EDGE[0]) return RhBand::TooDry;
  if (pct < EDGE[1]) return RhBand::Dry;
  if (pct < EDGE[2]) return RhBand::Ideal;
  if (pct < EDGE[3]) return RhBand::Humid;
  return RhBand::TooHumid;
}

inline const char* temp_word(TempBand b) {
  switch (b) {
    case TempBand::Cold:    return "cold";
    case TempBand::Cool:    return "cool";
    case TempBand::Ideal:   return "just right";
    case TempBand::Warm:    return "warm";
    case TempBand::TooWarm: return "too warm";
    default:                return "";
  }
}

inline const char* rh_word(RhBand b) {
  switch (b) {
    case RhBand::TooDry:   return "very dry air";
    case RhBand::Dry:      return "dry air";
    case RhBand::Ideal:    return "";   // ideal humidity earns silence
    case RhBand::Humid:    return "humid";
    case RhBand::TooHumid: return "very humid";
    default:               return "";
  }
}

}  // namespace canary::care
