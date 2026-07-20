// canary-fence-guard — requirements as code (CONCEPT, nothing ships).
//
// R1–R8 from README.md, written as named constants so the future
// firmware imports its requirements instead of re-typing them, and so
// a requirements change is a diff here — reviewable, not folklore.
//
// Values marked TBD are decided by the research dossier
// (docs/hardware/canary_fence_guard_research.md) and the open
// questions in README.md. Do not fill them in from memory.

#pragma once

// R1 — on-device vibration classification; raw waveform never leaves
enum class FenceEvent : uint8_t {
  kQuiet = 0,
  kRattle,   // brief, low-energy — wind band, never alarms alone (R8)
  kLean,     // sustained load shift
  kClimb,    // periodic high-energy signature
  kCut,      // sharp transient + tension release
};

// R2/R4 — mesh transport: Meshtastic, position broadcast OFF, always
constexpr bool kMeshPositionBroadcast = false;

// R3 — trust surface: same signer as the rest of the fleet
// canonical: "securacv-canary-sig|v1|fence|<device_id>|<len>|<hash>"
//            (domain pending open question 4 — may reuse `sense`)
constexpr char kSigDomain[] = "fence";  // TBD: open question 4

// R5/R6 — power plane targets (numbers land with the dossier)
// constexpr uint32_t kPanelMinMw       = TBD;  // shaded-sky budget
// constexpr uint32_t kCellMinMah       = TBD;  // multi-day overcast
// constexpr uint32_t kSleepBudgetUa    = TBD;  // decides S3 vs nRF52

// R8 — false-alarm discipline: debounce floors (tuning TBD on hardware)
// constexpr uint32_t kRattleIgnoreMs   = TBD;
// constexpr uint32_t kClimbConfirmMs   = TBD;
