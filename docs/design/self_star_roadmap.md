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

## TODO 1 — Fleet map (self-modeling) · *in progress*

**What.** The device already models its fleet and can reach peers over the
direct BLE link (PR #1026, no broker/WiFi). Surface that as a **live fleet
view**: an ASCII topology on the serial console (a new read-only `Tier::Diag`
command, e.g. `n` for "network/nearby"), and a `/fleet` page on the website that
renders the fleet the connected device reports.

**Why (user value).** "Your Canaries at a glance," offline. Reinforces
self-modeling and makes multi-device ownership legible without a cloud.

**Status (2026-07).** The **pure render layer landed** —
`firmware/common/ui/fleet_view.h` (`scene::fleet_card`) turns a snapshot of the
roster into the width-aligned, ASCII-floor-safe, dual-tier console card, honest
that presence is UNSIGNED, host-tested with a golden in
`firmware/tests_host/test_fleet_view.cpp`. This mirrors the `boot_policy.h`
approach (PR #1085): the format that could silently drift is pinned in CI first,
ahead of the device wiring. **Data-source note (a real trap):** on the main
Canary the fleet source of truth is the *shared pure roster*
`firmware/common/fleet_link/fleet_roster.h`, fed by
`firmware/canary/lib/securacv_ble_scan/src/fleet_roster_feed.{h,cpp}` — NOT the
display-only `canary-display/.../fleet_model.h` (`FleetModel`/`Witness`, MQTT-fed).
**Still to wire:** (1) a per-peer snapshot accessor on `fleet_roster_feed`
(it exposes only `peer_count()`/`seen()` today); (2) the `n`/"nearby" Diag
command in `firmware/canary/src/main.cpp` (registry + dispatch + help), gated on
`FEATURE_BLE_SCAN` like the feed; (3) the manifest `fleet[]` field — a
cross-repo wire contract the website `/fleet` page reads, so make the schema
decision deliberately (the website pins the schema string).

**Surfaces / files.**
- Firmware: the pure renderer is done (`firmware/common/ui/fleet_view.h`,
  host-tested — mirrors the `console_scenes.h` pattern: ASCII-floor safe,
  width-aligned). Remaining: a new command in `kConsoleCommands` (registry is
  the single source — see how `j`/`c`/`f` were added) plus its dispatch/help,
  and the snapshot accessor it pulls the peer list from (`fleet_roster_feed`
  over the shared `fleet_roster.h`).
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

**Status (2026-07).** The **app-level anti-rollback version floor** is done and
host-tested in `test_ota_logic.cpp`. The **A/B rollback engine** (post-flash
self-test → mark-valid-or-revert, plus the `verifyRollbackLater` ownership that
keeps a new image `PENDING_VERIFY`) is written in
`firmware/common/ota/securacv_ota.*` and active in the `canary-ota` ESP-IDF
project, where `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` — but **that bootloader
config is not yet enabled in the shipping Arduino/Canary builds**, so there the
Arduino core auto-confirms the image and the revert net is inert until the config
lands (part of the Phase 1 boot-path work; see "What already exists"). The
crash-loop → safe-mode half — for a *confirmed* image that can no longer come up,
when there is no A/B image to revert to — begins with the pure decision layer
`firmware/common/health/boot_policy.h` (host-tested in
`tests_host/test_boot_policy.cpp`). **Still to land, and gated on hardware
validation:** enabling the bootloader rollback config in the shipping builds,
wiring the crash-loop counter into the boot path, and the safe-mode console
(steps 1–2 below).

**Why (user value).** Resilience — the highest-stakes self-healing. A failed
update or corrupted image degrades to a recoverable state instead of a dead
device, and the evidence on the SD card stays intact and verifiable.

**What already exists to build on.**
- 🟡 **A/B rollback engine is written, but gated on the bootloader rollback
  config.** `FEATURE_OTA_PULL` + `firmware/common/ota/securacv_ota.*` register the
  post-flash self-tests (see `main.cpp` `k_ota_selftests[]`,
  `securacv_ota_register_selftest`); `securacv_ota_boot_self_test()` runs them
  and, on a required failure, calls `esp_ota_mark_app_invalid_rollback_and_reboot()`.
  The engine overrides the Arduino core's weak `verifyRollbackLater()` so a new
  image stays `PENDING_VERIFY` until it confirms itself — a crash/hang/brownout
  before confirmation reverts to the previous image on the next boot. **This only
  functions where `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is set** — the
  `verifyRollbackLater` override is `#if`-guarded on that symbol in
  `securacv_ota.cpp`. Today that config lives only in the `canary-ota` ESP-IDF
  project (`sdkconfig.defaults`/`.production`); in the main Arduino/Canary builds
  the core auto-confirms the image and the revert net is inert. **Enabling that
  config in the shipping builds is remaining Phase 1 boot-path work.**
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
3. **A/B: engine code in place; the crash-loop/safe-mode *decision* is now
   host-tested.** The post-flash self-test gates `mark_app_valid` and the
   rollback-on-failure path is written (see "What already exists") — live only
   where `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is enabled (`canary-ota`), still
   to be turned on in the shipping builds. The crash-loop/safe-mode *decision*
   logic now lives in the pure header `boot_policy.h`, proven in `tests_host` —
   mirroring `test_console.h`'s pure-policy pattern. Host-tested here: the
   decision half. Needs hardware: the boot-path glue **and** the bootloader
   rollback config in the shipping builds.

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
4. Fleet map's pure render layer landed (`fleet_view.h`, host-tested); what's
   left is the console-command wiring + roster accessor + manifest `fleet[]`
   (see TODO 1 Status). All host-testable, no boot-path risk. Safe-mode/rollback
   has its design doc
   ([`hardware_root_of_trust.md`](hardware_root_of_trust.md) §7 Phase 1, signed
   off 2026-07-22) and its pure decision layer landed (`boot_policy.h`); the
   remaining boot-path wiring still needs a hardware smoke test before it merges
   — a bug there could brick the one thing we promise you can't brick.
