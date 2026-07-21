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

## TODO 2 — Boot safe-mode + A/B auto-rollback (self-healing) · *coming soon, design-first*

**What.** A bad firmware image must not be able to brick trust. On boot, run
the existing self-test; if it fails hard (or the app crashes N times), fall
back to **last-known-good**: OTA A/B partitions with automatic rollback when
the new slot fails its post-flash self-test, plus a minimal **safe-mode
console** that still prints the trust card + a recovery URL even when the main
app won't come up.

**Why (user value).** Resilience — the highest-stakes self-healing. A failed
update or corrupted image degrades to a recoverable state instead of a dead
device, and the evidence on the SD card stays intact and verifiable.

**What already exists to build on.**
- `FEATURE_OTA_PULL` + `firmware/common/ota/securacv_ota.*` already register
  post-flash self-tests (see `main.cpp` `k_ota_selftests[]`,
  `securacv_ota_register_selftest`) and the ESP OTA path can roll back
  (`esp_ota_mark_app_invalid_rollback_and_reboot`).
- `diag_run_selftest()` / `diag_get_selftest()` provide the health gate.
- The themed console (`console_scenes.h`) can render the safe-mode card.

**Approach (sketch).**
1. Boot-count-in-NVS crash loop detector (increment early in boot, clear once
   the app reaches "healthy"); after N failures, boot into safe-mode.
2. Safe-mode: minimal init, print `welcome_card`/`trust_card` + recovery URL,
   accept only the read-only diagnostic console (`Tier::Diag`), offer re-flash.
3. A/B: verify the post-flash self-test already gates `mark_app_valid`; add the
   rollback-on-failure path and a host-tested decision function (mirror
   `test_console.h`'s pure-policy pattern — the *decision* logic lives in a pure
   header, proven in `tests_host`).

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
4. Fleet map is safe to build now. Safe-mode/rollback needs a design doc and a
   human yes first.
