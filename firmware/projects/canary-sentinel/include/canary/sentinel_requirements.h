// canary-sentinel — requirements as code.
//
// R1–R10 from the design doc (docs/canary_sentinel_fusion_design.md) and this
// project's README, written as named constants/enums so the firmware imports
// its requirements instead of re-typing them, and so a requirements change is
// a reviewable diff here rather than folklore. Same house style as
// canary-fence-guard's requirements header.

#pragma once

#include <stdint.h>

namespace canary {
namespace sentinel {

// R1 — Fuse PHYSICALLY INDEPENDENT modalities. Corroboration is only counted
// across distinct modality classes (see securacv::fusion::Modality); channels
// in the same class (WiFi-RF + BLE; light + vision) never fake independence.
constexpr uint8_t kMinConfirmModalities = 2;  // mirrors FusionConfig default

// R2 — A blinded/denied channel is SUSPICION, not absence. Covering, jamming or
// unplugging a sensor raises the anomaly accumulator; doing it while a body is
// present escalates to Anomaly with no debounce grace.
constexpr bool kDeniedChannelIsSuspicion = true;

// R3 — An uncorroborated dwelling body is surfaced, never dismissed. A
// body-present modality (radar/CSI) alone that lingers past dwell -> Anomaly.
constexpr bool kSilentBodyIsAnomaly = true;  // preset-overridable (hallway=off)

// R4 — Privacy chokepoint: only the coarse securacv::fusion::FusionResult may
// leave the device (level, 0..100 confidence, 0/1/2+ occupancy, near/mid/far
// band, corroborating modality classes). No MAC, no centimeters, no per-target
// track, no imagery, no vitals — ever.
constexpr bool kExportRawMeasurements = false;

// R5 — Every published transition is Ed25519-signed over a `sentinel` v1
// canonical and hash-chained, reusing common/identity + common/witness exactly
// as canary-sense does (TOFU-pinned pubkey, HA "device-verified ✓").
constexpr char kSigDomain[] = "sentinel";

// R6 — Degrade honestly. A stalled radar UART, a blinded light sensor, a lost
// hub link, a WiFi outage — each is a health/anomaly claim, never a silent gap.
constexpr bool kDegradeHonestly = true;

// R7 — Three cost tiers, one brain. Lite (PIR+RF+BLE+light), Standard (+radar
// +CSI), Heavy (+contact+vision+tamper, dual-board). The fusion core is
// identical; the tier is a board plus a set of enabled channels.
enum class Tier : uint8_t { kLite = 0, kStandard, kHeavy };

// R8 — Fully modular. Every channel is compile-time selectable (FEATURE_*) and
// runtime-weightable (ChannelSpec). Adding a sensor is a driver + an adapter +
// a weight; the fusion core does not change.

// R9 — Honest labeling. Lite IS evadable by a slow, device-free, still
// intruder and the product says so. Standard closes the front-door gaps.
constexpr bool kAdvertiseTierLimits = true;

// R10 — Bounded, debounced reflexes. Rising transitions debounce against
// flapping; Anomaly latches; dwell promotes to Loiter. Timers are preset data.

}  // namespace sentinel
}  // namespace canary
