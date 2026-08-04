/**
 * @file sentinel_fusion.cpp
 * @brief Implementation of sentinel.fusion (see sentinel_fusion.h).
 *
 * Pure hosted C++ (no Arduino / RTOS). The scorer and the FSM are separated so
 * the scoring math can be unit-tested independently of the timing behavior.
 */

#include "sentinel_fusion.h"

namespace securacv {
namespace fusion {

namespace {

constexpr size_t kNCh = static_cast<size_t>(Channel::kCount);

inline uint8_t clamp_u8(int v) {
  if (v < 0) return 0;
  if (v > 100) return 100;
  return static_cast<uint8_t>(v);
}

inline int rank(Level l) {
  // Ordering on the presence ladder only. Loiter/Anomaly are overlays derived
  // separately and never flow through the debounce comparison.
  switch (l) {
    case Level::Clear:     return 0;
    case Level::Aware:     return 1;
    case Level::Present:   return 2;
    case Level::Confirmed: return 3;
    case Level::Loiter:    return 3;  // treated as Present-tier for debounce
    case Level::Anomaly:   return 4;
  }
  return 0;
}

inline int popcount8(uint8_t x) {
  int n = 0;
  while (x) { n += (x & 1); x >>= 1; }
  return n;
}

// A body-present modality is one that can see a still, device-free human:
// active radar reflection and passive WiFi channel perturbation.
inline bool is_body_modality(Modality m) {
  return m == Modality::RadioReflection || m == Modality::ChannelPerturb;
}

}  // namespace

Modality modality_of(Channel c) {
  switch (c) {
    case Channel::Pir:     return Modality::Thermal;
    case Channel::Radar:   return Modality::RadioReflection;
    case Channel::WifiCsi: return Modality::ChannelPerturb;
    case Channel::WifiRf:  return Modality::CarriedRadio;
    case Channel::Ble:     return Modality::CarriedRadio;
    case Channel::Light:   return Modality::Optical;
    case Channel::Vision:  return Modality::Optical;
    case Channel::Contact: return Modality::Mechanical;
    case Channel::Tamper:  return Modality::Mechanical;
    case Channel::kCount:  break;
  }
  return Modality::kCount;
}

const char* level_name(Level l) {
  switch (l) {
    case Level::Clear:     return "clear";
    case Level::Aware:     return "aware";
    case Level::Present:   return "present";
    case Level::Confirmed: return "confirmed";
    case Level::Loiter:    return "loiter";
    case Level::Anomaly:   return "anomaly";
  }
  return "clear";
}

const char* occupancy_name(Occupancy o) {
  switch (o) {
    case Occupancy::Zero:    return "0";
    case Occupancy::One:     return "1";
    case Occupancy::TwoPlus: return "2+";
    case Occupancy::Unknown: return "unknown";
  }
  return "unknown";
}

const char* range_band_name(RangeBand r) {
  switch (r) {
    case RangeBand::Near: return "near";
    case RangeBand::Mid:  return "mid";
    case RangeBand::Far:  return "far";
    case RangeBand::Unknown: return "unknown";
  }
  return "unknown";
}

// ─────────────────────────────────────────────────────────────────────────────

FusionEngine::FusionEngine() : cfg_() {}
FusionEngine::FusionEngine(const FusionConfig& cfg) : cfg_(cfg) {}

void FusionEngine::configure(const FusionConfig& cfg) { cfg_ = cfg; }

void FusionEngine::reset() {
  for (size_t i = 0; i < kNCh; ++i) slots_[i] = Slot{};
  occ_ = Occupancy::Unknown;
  range_ = RangeBand::Unknown;
  level_ = Level::Clear;
  candidate_level_ = Level::Clear;
  candidate_since_ = 0;
  level_since_ = 0;
  anomaly_until_ = 0;
  presence_ = Level::Clear;
  presence_since_ = 0;
  silent_body_ = false;
  last_ = FusionResult{};
}

void FusionEngine::observe(Channel c, Vote v, uint8_t quality, uint32_t now_ms) {
  const size_t i = static_cast<size_t>(c);
  if (i >= kNCh) return;
  if (!cfg_.channels[i].enabled) return;  // disabled channels never speak
  Slot& s = slots_[i];
  s.vote = v;
  s.quality = quality > 100 ? 100 : quality;
  s.ts_ms = now_ms;
  s.ever = true;
}

void FusionEngine::score(uint32_t now_ms, FusionResult& out) const {
  int evidence = 0;                 // base weighted evidence sum
  uint8_t weak_mask = 0;            // modality classes voting >= Weak
  uint8_t strong_mask = 0;          // modality classes voting Strong
  int denied_count = 0;
  bool body_strong = false;
  bool denied_any = false;

  for (size_t i = 0; i < kNCh; ++i) {
    const ChannelSpec& spec = cfg_.channels[i];
    if (!spec.enabled) continue;
    const Slot& s = slots_[i];

    // Decay: a vote older than the channel's staleness window is treated as
    // no reading at all (the sensor simply hasn't spoken recently).
    Vote v = s.vote;
    if (!s.ever) v = Vote::None;
    else if (now_ms - s.ts_ms > spec.stale_ms) v = Vote::None;

    const Modality m = modality_of(static_cast<Channel>(i));
    const uint8_t mbit = static_cast<uint8_t>(1u << static_cast<uint8_t>(m));

    switch (v) {
      case Vote::Strong:
        evidence += (spec.weight * s.quality) / 100;
        weak_mask |= mbit;
        strong_mask |= mbit;
        if (is_body_modality(m)) body_strong = true;
        break;
      case Vote::Weak:
        evidence += (spec.weight * s.quality) / 200;
        weak_mask |= mbit;
        break;
      case Vote::Denied:
        // Fraud-detection primitive: a channel that should be reporting and is
        // blinded/jammed/stalled contributes SUSPICION, never absence.
        denied_any = true;
        ++denied_count;
        break;
      case Vote::None:
      default:
        break;
    }
  }

  const int strong_classes = popcount8(strong_mask);

  // Independence bonus — the anti-evasion term. Corroboration across DISTINCT
  // physical modalities is rewarded super-linearly: each independent class
  // beyond the first adds `independence_bonus`. Defeating one modality removes
  // one class; defeating the score requires defeating several at once.
  int bonus = 0;
  if (strong_classes >= 2) {
    bonus = cfg_.independence_bonus * (strong_classes - 1);
  }

  // Anomaly accumulator (0..100). Blinded channels are suspicious; a channel
  // blinded WHILE a body is present is the textbook evasion attempt, so it is
  // amplified once.
  int anomaly = denied_count * cfg_.denied_suspicion;
  if (denied_count > 0 && body_strong) anomaly += cfg_.denied_suspicion;

  out.confidence = clamp_u8(evidence + bonus);
  out.anomaly = clamp_u8(anomaly);
  out.modality_bits = weak_mask;
  out.strong_modalities = static_cast<uint8_t>(strong_classes);
  out.denied_any = denied_any;

  // Silent body: a body-present modality is Strong but NOTHING else, even
  // weakly, corroborates it. Plausible (a still, device-free person) but
  // uncorroborated — it may never reach Confirmed, and if it dwells it is
  // surfaced as an anomaly rather than dismissed. Computed here, applied in
  // the FSM (needs the dwell timer).
  silent_body_ = cfg_.silent_body_is_anomaly && body_strong &&
                 strong_classes == 1 && popcount8(weak_mask) == 1;
}

FusionResult FusionEngine::evaluate(uint32_t now_ms) {
  FusionResult res;
  score(now_ms, res);
  res.occupancy = occ_;
  res.range = range_;

  // 1) Presence ladder from the fused score (timing-independent).
  Level rawp;
  if (res.confidence >= cfg_.confirmed_score &&
      res.strong_modalities >= cfg_.min_confirm_modalities) {
    rawp = Level::Confirmed;
  } else if (res.confidence >= cfg_.present_score) {
    rawp = Level::Present;
  } else if (res.confidence >= cfg_.clear_score) {
    rawp = Level::Aware;
  } else {
    rawp = Level::Clear;
  }

  // 2) Debounce the committed presence_ toward rawp. Rising transitions use
  //    present_debounce_ms; falling transitions use clear_debounce_ms.
  if (rawp == presence_) {
    candidate_level_ = presence_;
    candidate_since_ = now_ms;
  } else {
    if (rawp != candidate_level_) {
      candidate_level_ = rawp;
      candidate_since_ = now_ms;
    }
    const uint32_t need = (rank(rawp) > rank(presence_)) ? cfg_.present_debounce_ms
                                                         : cfg_.clear_debounce_ms;
    if (now_ms - candidate_since_ >= need) {
      const Level prev = presence_;
      presence_ = rawp;
      if (rank(presence_) >= rank(Level::Present) &&
          rank(prev) < rank(Level::Present)) {
        presence_since_ = now_ms;  // entered the present tier -> start dwell
      }
    }
  }
  if (rank(presence_) < rank(Level::Present)) presence_since_ = 0;

  // 3) Derive the reported level. Base is the committed presence.
  Level reported = presence_;

  const bool dwelling = (presence_since_ != 0) &&
                        (now_ms - presence_since_ >= cfg_.loiter_dwell_ms);
  if (dwelling && rank(presence_) >= rank(Level::Present)) {
    // A dwelling but UNCORROBORATED body is suspicious, not routine.
    reported = silent_body_ ? Level::Anomaly : Level::Loiter;
  }

  // 4) Anomaly overlay wins over everything and latches. A denied/blinded
  //    channel raises this immediately — evasion should not have to wait out a
  //    debounce timer.
  if (res.anomaly >= cfg_.anomaly_score) {
    reported = Level::Anomaly;
    anomaly_until_ = now_ms + cfg_.anomaly_latch_ms;
  } else if (now_ms < anomaly_until_) {
    reported = Level::Anomaly;
  }

  res.changed = (reported != level_);
  level_ = reported;
  if (res.changed) level_since_ = now_ms;
  res.level = reported;

  last_ = res;
  return res;
}

// ─────────────────────────────────────────────────────────────────────────────

#ifdef SENTINEL_FUSION_TEST_HOST
FusionConfig default_standard_config() {
  FusionConfig c;  // thresholds/timers keep their in-struct defaults (door)

  auto set = [&](Channel ch, bool en, uint8_t w, uint8_t ev, uint16_t stale) {
    ChannelSpec& s = c.channels[static_cast<size_t>(ch)];
    s.enabled = en;
    s.weight = w;
    s.evasion_cost = ev;
    s.stale_ms = stale;
  };
  //             channel          on    weight evasion stale_ms
  set(Channel::Pir,     true,  55, 25, 2500);
  set(Channel::Radar,   true,  80, 80, 2000);
  set(Channel::WifiCsi, true,  60, 70, 4000);
  set(Channel::WifiRf,  true,  35, 30, 8000);
  set(Channel::Ble,     true,  30, 30, 8000);
  set(Channel::Light,   true,  25, 20, 3000);
  set(Channel::Contact, false, 70, 60, 2000);
  set(Channel::Vision,  false, 75, 65, 2000);
  set(Channel::Tamper,  false, 90, 85, 2000);
  return c;
}
#endif

}  // namespace fusion
}  // namespace securacv
