/*
 * SecuraCV Canary — Differential-privacy utilities (Phase 7)
 * Version 0.3.0 (budget enforced, refilled only by the clock)
 *
 * WHY THIS EXISTS
 * ===============
 * rf_presence, household, familiar, and baseline each expose counters
 * over the device's HTTP / MQTT surface (how many BLE advertisements
 * were dropped, how many RPAs resolved, how many anomaly events fired
 * in the last hour). Those raw counters leak information about a
 * household: counter(9 am) >> counter(10 am) tells a passive observer
 * that 9 am is when people come and go.
 *
 * The Gaussian mechanism of differential privacy masks each exported
 * counter with calibrated noise:
 *     noisy(x) = clamp(x + N(0, σ²))
 *     σ = sensitivity · √(2 · ln(1.25 / δ)) / ε
 * so that a single additional event (sensitivity = 1) changes the
 * observable by O(σ), which is information-theoretically bounded by
 * (ε, δ). The local firmware ALWAYS uses the raw counter — only values
 * crossing the export boundary pick up noise.
 *
 * Status (2026-09): the *_for_export() functions that draw through this
 * module exist in notify, baseline, household, familiar and federated,
 * but no HTTP route, MQTT topic or mesh sender calls them yet (the only
 * callers are wizard::get_status_for_export, itself uncalled, and the
 * on-device conformance self-tests). The counters the device serves today
 * come from the raw get_stats() paths. This module is the gate those
 * exports must pass once they are wired; it is not yet protecting a live
 * surface.
 *
 * HOW TO USE
 * ==========
 *   // Inside a get_stats_for_export(): one Release per export, sized to
 *   // the number of noisy values it will draw.
 *   dp::Release rel(3);                      // reserves 3 x ε, all or nothing
 *   if (!rel.ok()) { memset(out, 0, sizeof(*out)); return false; }
 *   out->a = rel.u32(out->a, 1);             // sensitivity = 1
 *   out->b = rel.u32(out->b, 1);
 *   out->c = rel.u32(out->c, 1);
 *   if (!rel.complete()) { memset(out, 0, sizeof(*out)); return false; }
 *
 *   // Default is epsilon = 1.0, delta = 1e-5 per draw. Override per release:
 *   //   dp::Release rel(draws, epsilon_x1000, delta_inv);
 *
 * Do NOT use this on values used for local decisions (is_anomaly,
 * resolve_rpa, is_ambient). Those must stay noise-free so the firmware
 * doesn't silently quiet or fire alerts based on random drift.
 *
 * BUDGET (enforced)
 * =================
 * Differential privacy composes: every noisy draw spends ε, and the
 * per-window budget (DEFAULT_BUDGET_X1000) caps the total. The budget is
 * ENFORCED and fails closed:
 *   - A Release reserves draws x ε up front, atomically, all or nothing.
 *     If the window's budget cannot cover the whole release it reserves
 *     nothing and ok() is false: the exporter withholds the export and
 *     reports failure. No partial releases.
 *   - A draw the release did not pay for (more draws than reserved, or a
 *     release that was refused) returns 0, a value independent of the
 *     input, and marks the release incomplete. So does a draw whose noise
 *     would round to nothing (sensitivity 0, or ε so large that σ < 0.5):
 *     that would be the raw value with a DP label on it.
 *   - There is no other way to get noise from this module: the old
 *     free noisy_*() functions and consume_budget() are gone, so no caller
 *     can draw without paying.
 * Accounting is basic sequential composition: a release of k values at ε
 * each costs k x ε, whatever the values' correlation. That is the
 * conservative bound; it over-charges releases whose coordinates cannot
 * all move together (a histogram whose buckets are disjoint), and with
 * the 4 ε window budget it means a release of more than 4 values is
 * always withheld. Sizing the budget, or charging disjoint buckets by
 * parallel composition, is a design decision for a human, not this file.
 *
 * The budget is per WINDOW, not per request: it refills only when
 * BUDGET_WINDOW_MS (4 h, rf_presence's timed-rotation period) of uptime has
 * passed since the last refill or since boot (refill_if_due(), called from
 * rf_presence::update()). Nothing a caller can ask for refills it:
 *   - A manual rotation (the owner's POST /api/rf/rotate, the opt-in token
 *     rotation check of GET /api/rf/conformance) rotates the session tokens
 *     and the epoch, and carries the spend over. It used to reset the
 *     budget, so any holder of the API token could refill ε on demand and
 *     compose as many releases as it liked; it no longer can. Within one
 *     window the 4 ε cap holds however many rotations happen.
 *   - A reboot does not refill it either. Each reservation (and each
 *     refill) persists the spend with the epoch it was charged in
 *     (ledger_store, NVS in the firmware) before any draw is honored, and
 *     rf_presence::init() hands the restored epoch to restore_budget(),
 *     which picks the stored spend back up whatever epoch it names at or
 *     below the current one, and restarts the window at boot. So a reboot
 *     only ever postpones the next refill. A write that fails refuses the
 *     release. A ledger that cannot be read, or that names a later epoch or
 *     more than the budget, reads as spent until the next refill; so does a
 *     budget nobody restored (safe mode skips rf_presence::init(), and an
 *     unrestored ledger never refills). No stored ledger at all (a first
 *     boot) is a fresh budget.
 *   - A refill cannot be raced. The ledger carries a generation that every
 *     refill and restore moves, and a Release remembers the generation that
 *     paid for it: a draw, or complete(), after a refill is refused. So a
 *     release in flight on another task cannot finish in the new window,
 *     and the new window's releases never spend a reservation the refill
 *     erased.
 *   - The ledger's NVS writes are bounded by the clock, not by the caller:
 *     at most one refill plus budget / ε reservations (5 at the shipped
 *     values) per window.
 * What this bounds is ε per 4 h of uptime on this device. It is not a
 * lifetime bound: an observer who keeps querying across windows composes
 * across them, 4 ε a window.
 */

#ifndef SECURACV_DP_H
#define SECURACV_DP_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace dp {

// ════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

// Default privacy parameters.
// ε = 1.0 (strong but usable); expressed as 1000 in millis for integer math.
static const uint16_t DEFAULT_EPSILON_X1000 = 1000;
// δ = 1e-5 (per-query); stored as 1/δ for the integer log computation.
static const uint32_t DEFAULT_DELTA_INV     = 100000;

// Per-window budget (millis of ε). 4.0 = 4.0 × 1000.
// Chosen to allow ~4 noisy queries per window at the default ε=1.0,
// which matches the typical polling cadence of a user-facing UI.
static const uint32_t DEFAULT_BUDGET_X1000  = 4000;

// The budget window: uptime between refills. rf_presence's timed rotation
// period (rf_presence::SESSION_ROTATE_MS, static_asserted equal there).
static const uint32_t BUDGET_WINDOW_MS      = 4UL * 60UL * 60UL * 1000UL;

// ════════════════════════════════════════════════════════════════════════════
// NOISE GENERATION
// ════════════════════════════════════════════════════════════════════════════

// Draw a zero-mean Gaussian with sigma = sigma_units (integer units).
// Uses the Irwin–Hall sum-of-12-uniforms approximation. Good to ~3σ;
// beyond that the tails are slightly thinner than true Gaussian. Fine
// for counter noise — we aren't doing cryptographic simulation here.
int32_t gaussian_sample(uint32_t sigma_units);

// ════════════════════════════════════════════════════════════════════════════
// CALIBRATED COUNTER NOISE — the only way to draw it
// ════════════════════════════════════════════════════════════════════════════

// One DP release: a fixed number of noisy draws paid for up front. See the
// BUDGET section above. Not copyable: a copy would be a second release that
// never paid. Use one per export call, on one task.
class Release {
 public:
  explicit Release(uint16_t draws,
                   uint16_t epsilon_x1000 = DEFAULT_EPSILON_X1000,
                   uint32_t delta_inv     = DEFAULT_DELTA_INV);
  Release(const Release&) = delete;
  Release& operator=(const Release&) = delete;

  // True when the window's budget paid for every draw this release asked for.
  bool ok() const { return ok_; }
  // True when ok(), no draw was refused, and the budget that paid for the
  // release has not been refilled (or restored) since. An exporter releases
  // its values only when this holds.
  bool complete() const;

  // Noisy draws. A refused draw returns 0 (independent of `value`) and
  // makes complete() false. Counters clamp to their range.
  uint32_t u32(uint32_t value, uint32_t sensitivity);
  int32_t  i32(int32_t  value, uint32_t sensitivity);

 private:
  bool take_draw(uint32_t sensitivity, uint32_t* sigma_units);
  bool current() const;

  uint16_t draws_left_;
  uint16_t epsilon_x1000_;
  uint32_t delta_inv_;
  uint32_t gen_;   // the ledger generation that paid for this release
  bool     ok_;
  bool     short_;
};

// ════════════════════════════════════════════════════════════════════════════
// BUDGET (enforced — see above; a Release is the only way to spend it)
// ════════════════════════════════════════════════════════════════════════════

// Remaining budget in millis of ε.
uint32_t remaining_budget_x1000();

// Total consumed in this window, for diagnostics.
uint32_t consumed_budget_x1000();

// Pick up the stored spend from ledger_store at boot, for session `epoch`,
// and start the window at `now_ms`. Call from rf_presence::init() with the
// epoch it restored. The stored spend is carried over whatever epoch it was
// charged in (at or below `epoch`): a manual rotation or a reboot never
// refills. False when the stored ledger could not be read, holds more than
// the budget, or names a later epoch: the budget then reads as spent until
// the next refill. Until this runs, it reads as spent and never refills.
bool restore_budget(uint32_t epoch, uint32_t now_ms);

// The only refill. When BUDGET_WINDOW_MS of uptime has passed since the
// last refill or restore, start a fresh budget charged to `epoch`, persist
// it, and return true; otherwise do nothing and return false. Call it from
// rf_presence::update() on the loop task. Releases reserved before a refill
// can no longer draw or complete.
bool refill_if_due(uint32_t epoch, uint32_t now_ms);

// Convenience: is this window's budget spent?
bool budget_exhausted();

// Releases refused since boot (whole releases the budget could not cover,
// plus releases cut short by a refused draw). Diagnostics only.
uint32_t withheld_releases();

// ════════════════════════════════════════════════════════════════════════════
// LEDGER PERSISTENCE (implemented by the firmware over NVS in rf_presence.cpp;
// by a fake in tests_host/test_dp_budget.cpp)
// ════════════════════════════════════════════════════════════════════════════

namespace ledger_store {
enum Read : uint8_t {
  READ_OK,      // *epoch and *consumed_x1000 hold the stored ledger
  READ_ABSENT,  // no ledger stored (first boot)
  READ_FAILED,  // storage unavailable, or a malformed record
};
// Called under the ledger lock; must not call back into dp.
Read read(uint32_t* epoch, uint32_t* consumed_x1000);
// True only when the record is durably written.
bool write(uint32_t epoch, uint32_t consumed_x1000);
}  // namespace ledger_store

// ════════════════════════════════════════════════════════════════════════════
// INTROSPECTION
// ════════════════════════════════════════════════════════════════════════════

// Returns the Gaussian-mechanism sigma (in integer units × 1000) for
// the given (sensitivity, ε, δ). Exposed for diagnostics / the setup
// wizard's "privacy level" display.
uint32_t compute_sigma_x1000(uint32_t sensitivity,
                             uint16_t epsilon_x1000,
                             uint32_t delta_inv);

// ════════════════════════════════════════════════════════════════════════════
// CONFORMANCE
// ════════════════════════════════════════════════════════════════════════════

// Draw N samples of gaussian_sample(sigma), verify:
//   |sample mean| < sigma/10           (centering)
//   |sample stddev - sigma| < sigma/5   (scale)
// Uses N=1024 for reasonable statistical stability at compile-time-
// fixed cost (~32 KB of esp_fill_random draws).
bool conformance_self_test();

}  // namespace dp

#endif  // SECURACV_DP_H
