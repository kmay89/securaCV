/**
 * @file sentinel_fusion.h
 * @brief sentinel.fusion — independence-weighted, anti-evasion evidence fusion
 *        for multi-modal people detection at a doorway or window.
 *
 * This is the board-agnostic brain of Canary Sentinel. It takes coarse votes
 * from physically INDEPENDENT sensing channels — passive infrared (heat),
 * 60GHz radar (radio reflection), WiFi CSI (channel perturbation), WiFi/BLE
 * device counting (carried-radio emission), ambient light (optical), and
 * door-contact / tamper (mechanical) — and fuses them into a single debounced
 * decision with a 0..100 confidence and a coarse, privacy-preserving vocabulary.
 *
 * ── Why fusion, and why THIS fusion ──────────────────────────────────────────
 * OR-ing sensors maximises false alarms; AND-ing them hands an intruder a
 * single channel to defeat. Sentinel does neither. It scores EVIDENCE, and it
 * rewards agreement across *independent physical modalities*. To be invisible
 * to a fully-populated Sentinel a person would have to, at the same instant,
 * emit no body heat, reflect no radar, not perturb the room's WiFi, carry no
 * powered radio, cast no optical change, and touch nothing mechanical. Each is
 * individually evadable; all six at once, at a threshold a body must cross, is
 * not. That is the whole thesis — corroboration across independent physics.
 *
 * ── The fraud-detection posture (the part that makes it "the real deal") ─────
 * Borrowed directly from ATM anti-fraud / anti-skimming practice: you never
 * trust one channel, you look for CONSISTENCY across independent channels, and
 * — critically — you treat the *absence of expected corroboration* and any
 * *blinded/denied channel* as SUSPICION, not as "all clear". A sensor that
 * should be reporting and has gone silent (covered, jammed, unplugged) is
 * itself evidence. Sentinel encodes this: a `Denied` vote raises the anomaly
 * accumulator instead of lowering the score, and a body seen by a
 * body-present modality with zero corroboration from any other is surfaced as
 * an anomaly to be looked at, never silently dismissed.
 *
 * ── Layer & purity ──────────────────────────────────────────────────────────
 * Leaf module (like common/csi/core_multilink_fusion): allocation-free, holds
 * no globals beyond its own fixed-size tables, no Arduino / ESP-IDF / RTOS
 * dependency, and it MUST NOT reach into any other module's state. The
 * composition layer (projects/canary-sentinel) adapts each sensor driver's
 * output into a `Vote` and feeds it here; the coarsened result is what the
 * witness signs and publishes. Time is injected (`now_ms`) so the engine is
 * fully deterministic and host-testable under g++ -Wall -Wextra -Werror.
 *
 * Privacy: this engine never sees a MAC, a distance in centimeters, a per-
 * target track, imagery or vitals. It sees votes. It emits an ordinal level,
 * a 0/1/2+ occupant bucket, a near/mid/far band, a confidence, and which
 * *modality classes* corroborated — never which device, never who.
 */

#ifndef SECURACV_FUSION_SENTINEL_FUSION_H
#define SECURACV_FUSION_SENTINEL_FUSION_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace securacv {
namespace fusion {

// ─────────────────────────────────────────────────────────────────────────────
// Channels & physical modality classes
// ─────────────────────────────────────────────────────────────────────────────

/**
 * A sensing channel. Each maps to one physical Modality (below); the
 * independence bonus counts distinct MODALITIES, not channels, so adding a
 * second channel in the same modality does not fake independence.
 */
enum class Channel : uint8_t {
  Pir = 0,    // passive infrared — body heat in motion
  Radar,      // 60GHz FMCW mmWave — micro-motion & breathing (still bodies)
  WifiCsi,    // WiFi channel-state-info — device-free motion/breathing
  WifiRf,     // aggregate WiFi probe/beacon device count (no MAC stored)
  Ble,        // aggregate BLE device count (no MAC stored)
  Light,      // ambient-light delta — shadow/threshold crossing, blinding
  Contact,    // door/window reed contact — the opening itself
  Vision,     // optical person-detection corroboration (heavy tier)
  Tamper,     // accelerometer / enclosure tamper (heavy tier)
  kCount
};

/**
 * Physical modality class. Evasion is defeated PER CLASS: leave your phone at
 * home and both WifiRf and Ble die together (one class), so they must not each
 * count as independent corroboration. Grouping is the honest core of the
 * "hard to evade" claim — it is deliberately conservative.
 */
enum class Modality : uint8_t {
  Thermal = 0,       // Pir
  RadioReflection,   // Radar
  ChannelPerturb,    // WifiCsi
  CarriedRadio,      // WifiRf, Ble  (share one class on purpose)
  Optical,           // Light, Vision
  Mechanical,        // Contact, Tamper
  kCount
};

/** Map a channel to its physical modality class. */
Modality modality_of(Channel c);

// ─────────────────────────────────────────────────────────────────────────────
// Votes & observations
// ─────────────────────────────────────────────────────────────────────────────

/**
 * A channel's coarse verdict for the current window. `Denied` is the
 * fraud-detection primitive: the channel is enabled and *expected* to report
 * but is blinded/jammed/stalled — evidence of evasion, scored as suspicion.
 */
enum class Vote : uint8_t {
  None = 0,   // channel reports nothing (genuine quiet)
  Weak,       // a low-confidence indication
  Strong,     // a high-confidence indication
  Denied      // channel should be reporting and is silent/blinded/stalled
};

/**
 * Per-channel evidence weighting, set by the active preset (configs/). All
 * values are data — no behavior forks by preset, only these numbers do.
 */
struct ChannelSpec {
  bool     enabled      = false;  // channel present in this build/tier
  uint8_t  weight       = 0;      // base evidence weight, 0..100 (preset)
  uint8_t  evasion_cost = 0;      // how hard this modality is to defeat, 0..100
  uint16_t stale_ms     = 3000;   // a vote older than this decays to None
};

/**
 * Fusion tuning. Thresholds are on the 0..100 fused score; the debounce/dwell
 * timers gate the FSM. Defaults are the neutral "door" starting point; presets
 * override them as data.
 */
struct FusionConfig {
  ChannelSpec channels[static_cast<size_t>(Channel::kCount)];

  uint8_t  present_score      = 45;   // score >= -> eligible for Present
  uint8_t  confirmed_score    = 70;   // score >= AND >=2 modalities -> Confirmed
  uint8_t  clear_score        = 25;   // score <  -> eligible to clear

  // Independence: a super-linear reward for distinct modalities agreeing. Two
  // independent modalities is the floor for "Confirmed"; each extra class adds
  // this many points (clamped into the score). This is the anti-evasion term.
  uint8_t  independence_bonus = 18;   // points per corroborating modality >=2
  uint8_t  min_confirm_modalities = 2;

  // Anomaly / fraud logic.
  uint8_t  denied_suspicion   = 34;   // anomaly points per Denied channel
  uint8_t  anomaly_score      = 55;   // anomaly accumulator >= -> Anomaly level
  // A body-present modality (Radar/WifiCsi) Strong with NO corroboration from
  // any other modality is a "silent body" — plausible-but-uncorroborated.
  // It cannot reach Confirmed and, past dwell, is surfaced as Anomaly.
  bool     silent_body_is_anomaly = true;

  // FSM timing (milliseconds).
  uint32_t present_debounce_ms = 1200;  // sustained evidence before Present
  uint32_t clear_debounce_ms   = 8000;  // sustained quiet before Clear
  uint32_t loiter_dwell_ms     = 30000; // Present/Confirmed held this long -> Loiter
  uint32_t anomaly_latch_ms    = 15000; // Anomaly holds at least this long
};

// ─────────────────────────────────────────────────────────────────────────────
// Decision output
// ─────────────────────────────────────────────────────────────────────────────

/** Ordinal decision level. Monotone in "how much should a human care". */
enum class Level : uint8_t {
  Clear = 0,   // nothing corroborated
  Aware,       // a single weak / low-independence indication
  Present,     // corroborated presence over the present threshold
  Confirmed,   // >= min_confirm_modalities independent classes agree
  Loiter,      // Present/Confirmed sustained past the dwell timer
  Anomaly      // inconsistent / blinded / tampered — the fraud flag
};

/** Coarse occupant bucket — never a per-target count. */
enum class Occupancy : uint8_t { Zero = 0, One, TwoPlus, Unknown };

/** Coarse range band, mirrored from the radar channel when present. */
enum class RangeBand : uint8_t { Unknown = 0, Near, Mid, Far };

const char* level_name(Level l);
const char* occupancy_name(Occupancy o);
const char* range_band_name(RangeBand r);

/**
 * The fused result. This — and only this — is what the composition layer is
 * allowed to publish. It carries no identifiers and no raw measurements.
 */
struct FusionResult {
  Level     level          = Level::Clear;
  uint8_t   confidence     = 0;     // 0..100 fused evidence score
  uint8_t   anomaly        = 0;     // 0..100 suspicion accumulator
  Occupancy occupancy      = Occupancy::Unknown;
  RangeBand range          = RangeBand::Unknown;
  uint8_t   modality_bits  = 0;     // bitmask of Modality classes voting >=Weak
  uint8_t   strong_modalities = 0;  // count of distinct classes voting Strong
  bool      denied_any     = false; // at least one channel is blinded/stalled
  bool      changed        = false; // level changed on this evaluate()
};

// ─────────────────────────────────────────────────────────────────────────────
// Engine
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Allocation-free fusion engine. Usage per tick:
 *   1. For every channel that produced a reading this tick, call observe(...).
 *      Channels tick at different rates; the latest vote per channel is
 *      retained and decays to None after its ChannelSpec.stale_ms.
 *   2. Call evaluate(now_ms) once. It returns the current FusionResult and
 *      sets result.changed when the ordinal level transitioned.
 *
 * The engine never calls millis(); the caller injects now_ms so the whole
 * thing is deterministic on the host.
 */
class FusionEngine {
 public:
  FusionEngine();
  explicit FusionEngine(const FusionConfig& cfg);

  /** Replace the whole config (e.g. after loading a preset from NVS). */
  void configure(const FusionConfig& cfg);
  const FusionConfig& config() const { return cfg_; }

  /**
   * Submit the latest coarse vote for one channel.
   *  @param quality  channel-internal confidence 0..100 (scales its weight).
   *  @param now_ms   caller's monotonic clock at observation time.
   * Ignored (no-op) if the channel is disabled in the config.
   */
  void observe(Channel c, Vote v, uint8_t quality, uint32_t now_ms);

  /** Optional coarse side-band: occupant bucket from a counting channel. */
  void set_occupancy(Occupancy o) { occ_ = o; }
  /** Optional coarse side-band: range band from the radar channel. */
  void set_range(RangeBand r) { range_ = r; }

  /** Fuse and advance the FSM. Call once per tick. */
  FusionResult evaluate(uint32_t now_ms);

  /** Last result without advancing the FSM. */
  const FusionResult& last() const { return last_; }

  /** Clear all votes and reset the FSM to Clear (e.g. on arm/disarm). */
  void reset();

 private:
  struct Slot {
    Vote     vote    = Vote::None;
    uint8_t  quality = 0;
    uint32_t ts_ms   = 0;
    bool     ever    = false;  // has this channel ever reported this session
  };

  // Pure scorer: fills a fresh FusionResult's score/anomaly/modality fields
  // from the current (decayed) slots. No FSM, no timers — unit-testable alone.
  void score(uint32_t now_ms, FusionResult& out) const;

  FusionConfig cfg_;
  Slot         slots_[static_cast<size_t>(Channel::kCount)];
  Occupancy    occ_   = Occupancy::Unknown;
  RangeBand    range_ = RangeBand::Unknown;

  // FSM memory.
  Level    level_            = Level::Clear;  // last reported level
  Level    presence_         = Level::Clear;  // committed presence-ladder level
  uint32_t presence_since_   = 0;   // when presence_ entered the Present tier (dwell)
  Level    candidate_level_  = Level::Clear;  // pending presence transition target
  uint32_t candidate_since_  = 0;   // when the pending transition condition began
  uint32_t level_since_      = 0;   // when the reported level was entered
  uint32_t anomaly_until_    = 0;   // Anomaly latch expiry
  mutable bool silent_body_  = false;  // set by score(), consumed by evaluate()
  FusionResult last_;
};

// ── Test hooks (host build only) ─────────────────────────────────────────────
#ifdef SENTINEL_FUSION_TEST_HOST
// A sensible baseline "standard tier, door preset" config for tests and as a
// documented reference. Presets in configs/ are expressed as deltas on this.
FusionConfig default_standard_config();
#endif

}  // namespace fusion
}  // namespace securacv

#endif  // SECURACV_FUSION_SENTINEL_FUSION_H
