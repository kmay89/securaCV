# SecuraCV Canary — RF Sensing Phase 12: HTTP Route Surface

Status: Draft v0.1
Last Updated: 2026-04-25

## 1. Purpose

Phases 4–11 of the rf-sensing v2 stack landed six firmware modules
(`household`, `familiar`, `baseline`, `notify`, `federated`, `wizard`)
plus a unified test harness, but every entry point is currently
reachable only from C++ inside `rf_presence::update`. The Phase 10 PR
called this out explicitly:

> HTTP route registration in `wap_server.cpp` is deliberately deferred —
> the wizard is transport-agnostic; the route layer is its own phase.

Phase 12 closes that gap. It exposes the rf-sensing module surface over
the existing HTTP server registered in `canary_wap.ino` so the SPA can
drive setup, surface DP-noised counters, and let the user act on alerts.

This document is the spec. Implementation is a follow-up PR.

## 2. Scope

### In scope
- A read-only `/api/rf/status` endpoint that returns a single
  `wizard::Status` snapshot via `wizard::get_status_for_export()`.
- Setup-flow endpoints that drive the four-state wizard machine
  (`/api/rf/wizard/{zone,pair-start,pair-finish,restart-training,context}`).
- Per-decision actions surfaced from the alert UI
  (`/api/rf/notify/{always-ignore-last,forget-always-ignored}`).
- A bounded admin endpoint that triggers the conformance harness
  (`/api/rf/self-test`) and returns the structured report.
- Rate limiting + auth using the existing `*_auth` pattern; no new
  permission model.
- `web_ui.h` SPA wiring sufficient to demonstrate the wizard end-to-end
  (zone tag, pair, training bar, "always ignore", context switch).

### Out of scope (deferred)
- **Phase 13: federated mesh transport.** `federated::build_*_share` /
  `handle_*_share` produce / consume the wire format, but the actual
  byte transport over the Opera mesh is a separate phase. Phase 12 does
  not expose `/api/rf/federated/*`.
- **Phase 1b: ESP32-C6 board port.** CSI HAL already has a C6 code path
  stub; bring-up + flash recipes are tracked separately.
- **MQTT / Home Assistant** — covered by `homeassistant_setup.md`; the
  HTTP surface specified here is the source of truth and the MQTT
  bridge can read from it later.
- **WebSocket push** for live CSI meter — Phase 12 polls `/api/rf/status`
  at 2 Hz from the SPA. Push is a perf optimization, not a correctness
  requirement.

## 3. Auth model

All `/api/rf/*` endpoints follow the existing convention in
`canary_wap.ino:register_api_routes`:

- Read endpoints (`GET`) use `handle_*_auth` — Bearer token in
  `Authorization: Bearer <token>` header. The token is the SPA-injected
  bearer from PR #309.
- Mutating endpoints (`POST`) use the same auth and additionally pass
  through `wap_server::check_rate_limit(client_ip, /*is_action=*/true)`.
- No physical-gate endpoints in Phase 12. Nothing here mints credentials
  or unseals secrets; the existing Bearer-only gate is sufficient.

Failures return JSON via `wap_server::send_json_error`:

```json
{ "error": "<code>", "message": "<human-readable>" }
```

Error codes used in this spec: `unauthorized`, `rate_limited`,
`bad_request`, `not_initialized`, `conflict`, `internal`.

## 4. Endpoint inventory

URI base: `/api/rf` — chosen to mirror the existing `rf_presence`
namespace and to leave room for future RF-adjacent endpoints
(e.g. `/api/rf/csi/snapshot` if a debug build adds it).

### 4.1 GET `/api/rf/status` — aggregate snapshot

Fills a `wizard::Status` via `wizard::get_status_for_export(&out)`
(DP-noised counters; safe to ship to the SPA on every poll).

Request: no body, no query params.

Response (200):
```json
{
  "state": "training",
  "zone_name": "Back Door",
  "context": "home",
  "training_progress_bps": 4200,
  "training_complete": false,
  "household_paired_count": 2,
  "household_enrolling": false,
  "household_enrollment_ms_remaining": 0,
  "baseline_populated_buckets": 91,
  "total_alerts_fired": 4,
  "total_events_evaluated": 1281,
  "total_ambient_suppressed": 612,
  "total_household_suppressed": 38
}
```

Field rules — flat, mirroring `wizard::Status` 1:1 so the handler
serialises with no manual rename layer:

- `state` ∈ `{unconfigured, pairing, training, ready}` — 1:1 with
  `wizard::SetupState`.
- `context` ∈ `{home, away, quiet_hours, traveling}` — 1:1 with
  `notify::Context`.
- `training_progress_bps` is basis points (0..10000), clamped
  server-side.
- All four `total_*` counters are post-DP, fed by `wizard::get_status()`
  which already calls the underlying `*_for_export` (DP-noised) paths.
  The raw (un-noised) counters are **not** exposed over HTTP. If a
  debug build needs them, the route MUST be gated behind a
  `FEATURE_RF_DEBUG_STATS` compile flag and never default-on.

Errors:
- `503 not_initialized` if `wizard::init()` was not called (covers the
  case where rf_presence is compiled out).

### 4.2 POST `/api/rf/wizard/zone` — set zone name

Request:
```json
{ "name": "Back Door" }
```

- `name` is bounded by `wizard::MAX_ZONE_NAME_LEN - 1 = 31` *bytes*
  (not chars) — UTF-8 multi-byte sequences eat into the budget and
  the server measures against the byte length.
- Server trims trailing whitespace and rejects only ASCII control
  bytes (`< 0x20`, plus `0x7F` DEL) with `400 bad_request`. Bytes
  `>= 0x80` are passed through so legal UTF-8 sequences (including
  emoji) survive — they would otherwise have been rejected by the
  earlier "reject bytes ≥ 0x7F" wording, which contradicted the
  "accept UTF-8" claim immediately after.
- Empty name resets the zone (delegates to `wizard::set_zone_name("")`).

Response (200): the same shape as `/api/rf/status` after the change
applies, so the SPA can update with one round-trip.

### 4.3 POST `/api/rf/wizard/pair-start` — open enrollment window

Request: no body.

Returns `409 conflict` if the zone is unset (`wizard::start_pairing`
returns false in that case). Returns the new status snapshot on 200.

### 4.4 POST `/api/rf/wizard/pair-finish` — close enrollment window

Request: no body.

`wizard::finish_pairing()` decides whether to advance to TRAINING (≥ 1
phone enrolled) or fall back to UNCONFIGURED. Either outcome is a 200;
the SPA reads `state` from the response.

### 4.5 POST `/api/rf/wizard/restart-training` — wipe + retrain

Request:
```json
{ "confirm": true }
```

Hard guardrail: the wizard wipes 72 h of baseline history. The SPA must
include `"confirm": true`; missing or false returns
`400 bad_request`. Mirrors the pattern used by `/api/reboot`.

### 4.6 POST `/api/rf/wizard/context` — set user context

Request:
```json
{ "context": "away" }
```

Accepted values: `home`, `away`, `quiet_hours`, `traveling`. Unknown
values: `400 bad_request`. Delegates to `wizard::set_context(c)` which
persists to NVS.

### 4.7 POST `/api/rf/notify/always-ignore-last` — mute last pattern

Request: no body.

The handler calls `wizard::always_ignore_last_decision()`. The current
helper returns `bool` only, which collapses both failure cases — *no
decision yet* and *last decision was a fire* — into a single `false`.
Producing distinct reasons in the HTTP response therefore requires an
additive change in `wizard.h` before this endpoint ships:

```c
namespace wizard {
enum class MuteOutcome : uint8_t {
  Muted              = 0,
  NoDecisionYet      = 1,
  LastDecisionFired  = 2,
};
MuteOutcome always_ignore_last_decision_v2();
}
```

The bool-returning `always_ignore_last_decision()` stays for the
existing call sites (`wizard::tick`'s SMS-trigger pathway), so the
v2 entry point is purely additive.

Responses:

- `200` `{ "muted": true }` when `MuteOutcome::Muted`
- `200` `{ "muted": false, "reason": "no_decision_yet" }` for
  `MuteOutcome::NoDecisionYet`
- `200` `{ "muted": false, "reason": "last_decision_fired" }` for
  `MuteOutcome::LastDecisionFired` (deliberate Phase-8 UX: don't mute
  things we just alerted on; user should cool off first)

This endpoint MUST NOT include the fingerprint in the response. The
fingerprint is internal-only.

### 4.8 POST `/api/rf/notify/forget-always-ignored` — clear filter

Request: no body.

Delegates to `familiar::forget_always_ignored()`. Returns the new
status snapshot.

### 4.9 POST `/api/rf/self-test` — run conformance + red-team harness

Request: no body. Bearer-auth + rate-limited (admin action).

Calls `tests::run_all(&report)` (NOT `run_all_conformance` alone — the
red-team scenarios are part of the spec coverage and the response
example below includes them). The response mirrors `tests::Report`
field-for-field so the handler is a thin serialiser:

```json
{
  "conformance": [
    { "name": "csi_hal::self_test",      "passed": true },
    { "name": "household::self_test",    "passed": true },
    { "name": "familiar::self_test",     "passed": true },
    { "name": "baseline::self_test",     "passed": true },
    { "name": "notify::self_test",       "passed": true },
    { "name": "federated::self_test",    "passed": true },
    { "name": "wizard::self_test",       "passed": true },
    { "name": "dp::self_test",           "passed": true }
  ],
  "conformance_count":  8,
  "conformance_passed": 8,
  "red_team": [
    { "name": "redteam::mac_replay",      "passed": true },
    { "name": "redteam::cloned_rpa",      "passed": true },
    { "name": "redteam::baseline_poison", "passed": true }
  ],
  "red_team_count":  3,
  "red_team_passed": 3,
  "total_ms":   142,
  "all_passed": true
}
```

The `*_count` / `*_passed` fields come straight from `Report.{conformance,red_team}_{count,passed}`;
the array entries map from `Report.{conformance,red_team}[i].{name,passed}`.

Concurrency: this endpoint runs synchronously on the HTTP task and
the handler holds a process-wide busy flag (a static `bool` inside
the handler TU, guarded by an atomic CAS — keeps the contract local
to the HTTP layer and `tests::` stays pure). A second concurrent
request returns `409 conflict` with `"reason": "self_test_busy"`
rather than starting a parallel run — several of the scenarios
mutate global module state (NVS slots, baseline buckets, federated
peer table) and restore it before exit, so two overlapping runs
would corrupt each other's restore step.

Wall-time budget is 2 s under nominal load. The handler does NOT
abandon a run mid-flight: aborting mid-restore would leave NVS in
the half-restored state we're trying to prevent. The handler waits
for `tests::run_all` to return, then if `report.total_ms > 2000`
sends `503 unavailable` with `"reason": "self_test_overran"` AND
the same JSON body shown above (so the caller can still see which
scenario was slow). The busy flag releases at the same point. Per-
scenario timeouts are out of scope for Phase 12 — `tests::` itself
would have to grow internal time slicing for that, and none of the
existing scenarios hit even half the budget on the canary-wap
hardware today.

## 5. Privacy invariants

Every route handler in Phase 12 MUST satisfy:

1. **No raw fingerprints, IRKs, MACs, or session tokens leave the
   firmware over HTTP.** Existing `*_for_export` paths already enforce
   this on the C++ side; the HTTP layer must call only those, never the
   raw `get_stats()` siblings.
2. **No precise timestamps.** Use `progress_bps` and
   `enrollment_ms_remaining` (relative durations); never publish a
   wall-clock `time_t`. The wizard is transport-agnostic about time;
   the HTTP layer must not regress that.
3. **Counters are DP-noised.** Phase 12 reads `_for_export` paths, full
   stop. Adding a `?raw=1` query param is forbidden by this spec; if a
   debug build needs raw counters it MUST be a separate compile flag
   and a separate URL prefix.
4. **The fingerprint of the last decision is internal.**
   `/api/rf/notify/always-ignore-last` must not echo it back even on
   success; only the boolean outcome leaves the device.
5. **Zone name handling.** The zone name is user-chosen and intended to
   be user-facing. Server stores and returns it as-is, and that is
   documented in the response. We do not coarsen or hash it; the user's
   choice to put an identifier there is theirs to make.

`/THREAT_MODEL.md` Principle 3 (no new identifier leaks) is the spec
checklist this section satisfies.

## 6. Error model

| HTTP | Code | When |
|------|------|------|
| 400 | `bad_request`     | Malformed JSON, unknown enum, missing `confirm` |
| 401 | `unauthorized`    | Missing/invalid Bearer token |
| 409 | `conflict`        | State-machine refused (e.g. pair before zone set) |
| 429 | `rate_limited`    | `wap_server::check_rate_limit` returned false |
| 500 | `internal`        | C++ helper returned `false` for unrecoverable reason |
| 503 | `not_initialized` | rf-sensing not compiled in / `wizard::init` failed |

Bodies use the standard envelope from `wap_server::send_json_error`.

## 7. SPA integration sketch

Minimum SPA changes (in `web_ui.h`):

- A new "Privacy radar" view rendered when `state != unconfigured` or
  user explicitly opens it.
- Polls `GET /api/rf/status` at 2 Hz while the view is foregrounded.
- Wire each setup button to its POST endpoint; show the response status
  inline, no toast spam.
- "Mute this pattern" button on each fired-alert row calls
  `/api/rf/notify/always-ignore-last`. UI must disable the button
  briefly after success so the user does not double-tap.
- `web_ui.h` size budget: under 64 KB per CONTRIBUTING.md. Phase 12 must
  fit inside the existing budget; if not, the spec is wrong and the
  SPA should be split before merging.

The SPA does NOT issue `/api/rf/self-test` automatically. It is a
debug-screen button only.

## 8. Acceptance criteria

Implementation of this spec is "done" when:

- [ ] All nine endpoints in §4 are registered in `register_api_routes`
      and respond with the documented schemas.
- [ ] `wap_server.cpp` either (a) provides thin helpers used by the
      handlers, or (b) the stub-only file is replaced with the real
      implementation. Either is acceptable as long as the rf-sensing
      handlers themselves live next to the other API handlers in
      `canary_wap.ino`.
- [ ] `register_api_routes`'s handler-count constant
      (`base_handlers + …`) is bumped to cover the nine new routes plus
      a small headroom; a regression test verifies start succeeds.
- [ ] **Additive `wizard.h` change**: `wizard::MuteOutcome` enum +
      `wizard::always_ignore_last_decision_v2()` landed alongside this
      Phase 12 implementation so §4.7 can produce its three documented
      reasons. The bool-returning `always_ignore_last_decision()` stays
      for `wizard::tick`'s SMS-trigger path; no caller migrates.
- [ ] `tests::run_all_conformance()` still passes and gains a
      Phase 12 entry that exercises a representative endpoint via the
      same in-process httpd path used by other API tests.
- [ ] `regression_check.sh` passes.
- [ ] Firmware compiles in **both** Arduino IDE and PlatformIO.
- [ ] Bin size delta documented in the PR description; expected
      ≤ 8 KB flash, ≤ 1 KB RAM (handlers are thin JSON glue).
- [ ] No new strings reference fingerprints, IRKs, MACs, or precise
      timestamps; `grep` audit in the PR description.
- [ ] `LESSONS_LEARNED.md` entry if any new pattern is introduced
      (e.g. JSON-shape pattern for state-machine endpoints).

## 9. Open questions

1. **Pairing UX.** `wizard::start_pairing` opens a household enrollment
   window but does not itself complete a BLE SMP handshake — the user
   must still do that on their phone. Should the wizard polling
   response include a `next_action_hint` string? Defer to the SPA
   author; not a blocker for Phase 12.

2. **TLS.** The HTTPS path in `canary_wap.ino` is conditional on a
   provisioned cert. Phase 12 inherits whatever the parent server is
   running on; no new TLS work. If TLS is unavailable the existing
   `[WARN] API traffic is NOT encrypted` banner already covers this
   surface.

3. **Auth scope.** Today's Bearer token is binary (have it or don't).
   We do not split read-vs-admin scopes for rf-sensing in Phase 12.
   Adding scopes later is non-breaking; the spec leaves the door open
   by not putting admin endpoints under a different prefix.

## 10. Follow-up phases (deferred)

- **Phase 13: federated mesh transport.** Wire
  `federated::build_baseline_share` / `handle_baseline_share` into
  `mesh_network.cpp` so multi-canary households actually exchange
  shares. The HTTP surface exposes `/api/rf/federated/status` only
  (DP-noised stats) so the SPA can show "X shares sent / Y received"
  without driving the transport directly.

- **Phase 1b: ESP32-C6 board port.** CSI HAL has C6 stubs; full
  bring-up plus PlatformIO board config plus a smoke test on real
  hardware closes this out. Independent of Phase 12.

- **Phase 14: OTA-aware self-test.** Have `/api/rf/self-test` run
  automatically on first boot after an OTA, write the report to NVS,
  and refuse to advance the wizard past PAIRING if any test failed.
  Belongs after Phase 13 because the federated tests assume mesh peers
  exist.
