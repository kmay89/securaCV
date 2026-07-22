# Self-* roadmap — "plug it in and it proves itself"

> **For a future AI or contributor picking this up.** This captures the
> in-flight product arc, what has already shipped, and the two remaining
> "coming soon" features with enough detail to build them without re-deriving
> the context. Grep tokens: `self-manifest`, `randomart handshake`,
> `fleet map`, `safe-mode`, `A/B rollback`.

## The through-line

SecuraCV's promise is **evidence you can trust without trusting the vendor** —
verifiable offline, sovereign, tamper-evident. Everything here deepens that by
making the device **describe, prove, model, and heal itself**, with every
surface (device console, `/canary`, `/checkup`, flasher) telling the *same*
story from *one* source of truth (anti-rot).

## Already shipped (context for what follows)

| Capability | Where | Notes |
|---|---|---|
| **Self-describing** | `firmware/common/attest/self_manifest.h`, `main.cpp` `emit_self_manifest()` (`j` command) | One JSON line: board, fw+git, **public** key hex, fp, chain head, seq/boots, health, tamper, `features[]` (from compile-time `FEATURE_*`), `commands[]` (from `kConsoleCommands`). Pinned host tests in `firmware/tests_host/test_self_manifest.cpp`. Schema `securacv.canary.manifest/v1`. |
| **Self-verifying** | website `js/randomart.js` + `js/verify.js` + `canary.html` panel | Reads the manifest over **WebSerial**, draws the *same* drunken-bishop randomart from the pubkey. Byte-exact port of `firmware/common/ui/randomart.h`. |
| **Anti-rot** | `test_console_scene.cpp::test_randomart_golden` + website `tests/randomart.test.mjs` | Both repos pin the identical golden vector (key `00..1f`). Device shape and browser shape can't drift. |
| **Self-repair** | website `onboarding-spec.json` `selftest.probes[].fix` + `js/checkup.js` `showFixes()` | Each self-test probe carries `{means, selfHeals, steps[]}`; `/checkup` renders a fix per failed probe. |
| **One logo** | `firmware/common/ui/console_scenes.h` `CANARY_LOGO`/`draw_canary()` | Single silhouette across trust card, welcome card, and the site. |

Deferred and **not** worth building as-is: an ASCII/real **QR in the welcome
card** — the only thing to encode is `securacv.com/canary?d=<id>`, but the
device already auto-opens that page on the connected computer, so scanning a
terminal to move it to a phone is a narrow, awkward use. Skip unless a concrete
phone-first flow appears.

---

## TODO 1 — Fleet map (self-modeling) · *coming soon*

**What.** The device already models its fleet (`firmware/.../fleet_model.h`,
`NVS_FLEET_ID`) and can reach peers over the direct BLE link (PR #1026,
`fleet_link.cpp`, no broker/WiFi). Surface that as a **live fleet view**: an
ASCII topology on the serial console (a new read-only `Tier::Diag` command,
e.g. `n` for "network/nearby"), and a `/fleet` page on the website that renders
the fleet the connected device reports.

**Why (user value).** "Your Canaries at a glance," offline. Reinforces
self-modeling and makes multi-device ownership legible without a cloud.

**Surfaces / files.**
- Firmware: new command in `kConsoleCommands` (registry is the single source —
  see how `j`/`c`/`f` were added); a pure, host-testable renderer in
  `firmware/common/ui/` (mirror the `console_scenes.h` pattern: ASCII-floor
  safe, width-aligned, host-tested). Pull peer list from `fleet_link`/`fleet_model`.
- Manifest: extend `self_manifest.h` with a `fleet[]` array (peer id + short
  fp + last-seen), single-sourced from the same model, so `/fleet` renders the
  live truth (same approach as `commands[]`).
- Website: `/fleet` page (add a `_redirects` extensionless route like `/canary`)
  reading the manifest over WebSerial (reuse `js/verify.js` connection logic —
  factor the serial read into a shared helper). Anti-rot test pinning the shape.

**Risks / gotchas.** Keep it read-only and public-only (peer **public** fps, no
secrets — same rule as the manifest). BLE peer enumeration timing: bound the
scan like `verify.js` bounds its read window. Don't call a group of Canaries a
"flock" — it's **fleet** (see `CLAUDE.md`).

**Effort.** Medium. Mostly plumbing + a pure renderer; no boot-path risk.

---

## TODO 2 — Boot safe-mode + A/B auto-rollback (self-healing) · *in progress*

**What.** A bad firmware image must not be able to brick trust. On boot, run
the existing self-test; if it fails hard (or the app crashes N times), fall
back to **last-known-good**: OTA A/B partitions with automatic rollback when
the new slot fails its post-flash self-test, plus a minimal **safe-mode
console** that still prints the trust card + a recovery URL even when the main
app won't come up.

**Status (2026-07).** The A/B half already ships in
`firmware/common/ota/securacv_ota.*` (host-tested in `test_ota_logic.cpp`): the
anti-rollback version floor and automatic rollback when a *new* image fails its
post-flash self-test are done (see "What already exists"). The crash-loop →
safe-mode half — for a *confirmed* image that can no longer come up, when there
is no A/B image to revert to — begins with the pure decision layer
`firmware/common/health/boot_policy.h` (host-tested in
`tests_host/test_boot_policy.cpp`). **Still to land, and gated on hardware
validation:** wiring that counter into the boot path + the safe-mode console
(steps 1–2 below).

**Why (user value).** Resilience — the highest-stakes self-healing. A failed
update or corrupted image degrades to a recoverable state instead of a dead
device, and the evidence on the SD card stays intact and verifiable.

**What already exists to build on.**
- ✅ **A/B rollback on a failed post-flash self-test is done.**
  `FEATURE_OTA_PULL` + `firmware/common/ota/securacv_ota.*` register the
  post-flash self-tests (see `main.cpp` `k_ota_selftests[]`,
  `securacv_ota_register_selftest`); `securacv_ota_boot_self_test()` runs them
  and, on a required failure, calls `esp_ota_mark_app_invalid_rollback_and_reboot()`.
  The engine overrides the Arduino core's weak `verifyRollbackLater()` so a new
  image stays `PENDING_VERIFY` until it confirms itself — a crash/hang/brownout
  before confirmation reverts to the previous image on the next boot.
- ✅ **App-level anti-rollback floor is done.** `securacv_ota_update_decision()`
  compares against `max(running_version, nvs_floor)`; the floor is raised on
  confirmation. Host-tested in `firmware/common/ota/test_ota_logic.cpp`.
- `diag_run_selftest()` / `diag_get_selftest()` provide the health gate.
- The themed console (`console_scenes.h`) can render the safe-mode card.

**Approach (sketch).**
1. Boot-count-in-NVS crash loop detector (increment early in boot, clear once
   the app reaches "healthy"); after N failures, boot into safe-mode. **The pure
   decision for this is done** — `firmware/common/health/boot_policy.h`
   (`bootpolicy::decide()`), host-tested in `tests_host/test_boot_policy.cpp`.
   *Pending (boot-path, hardware-validated):* the NVS counter + the early-boot
   increment/reset wiring that calls it.
2. Safe-mode: minimal init, print `welcome_card`/`trust_card` + recovery URL,
   accept only the read-only diagnostic console (`Tier::Diag`), offer re-flash.
   *Pending (boot-path, hardware-validated).*
3. ✅ **A/B decision function done.** The post-flash self-test already gates
   `mark_app_valid` and the rollback-on-failure path exists (see "What already
   exists"). The crash-loop/safe-mode *decision* logic now lives in the pure
   header `boot_policy.h`, proven in `tests_host` — mirroring `test_console.h`'s
   pure-policy pattern (the decision half is host-tested; the boot-path glue that
   calls it is the only part that needs hardware).

**Risks / gotchas.** **This touches the boot/OTA path — real bricking risk if
wrong.** Do a design doc + explicit human sign-off before code. Must be proven
on hardware, not just host tests. Preserve Invariant IV (fully offline) and
never weaken a secure default without the `LESSONS_LEARNED.md` +
`THREAT_MODEL.md` process in the PR template. Partition-table changes are
migration-sensitive for already-deployed devices.

**Effort.** Large. Design-first; do not start coding without sign-off.

---

## How to resume

1. Read this file + `CLAUDE.md` (voice/naming: **fleet**, never "flock").
2. The manifest (`self_manifest.h`) is the keystone — extend it, don't fork it.
   Keep everything **read-only, public-only** on the diagnostic console.
3. Anti-rot is non-negotiable: pin new wire formats with host tests in *both*
   repos, single-source from the firmware.
4. Fleet map is safe to build now. Safe-mode/rollback has its design doc
   ([`hardware_root_of_trust.md`](hardware_root_of_trust.md) §7 Phase 1, signed
   off 2026-07-22) and its pure decision layer landed (`boot_policy.h`); the
   remaining boot-path wiring still needs a hardware smoke test before it merges
   — a bug there could brick the one thing we promise you can't brick.
