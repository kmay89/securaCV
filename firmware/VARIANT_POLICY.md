# Firmware Variant Policy

**Last updated:** 2026-08-21 (added canary-sentinel / canary-companion / canary-fence-guard rows; reworded canary-ota after the engine's promotion to `common/ota`)
**Owner:** firmware maintainers
**Companion docs:** [ARCHITECTURE.md](ARCHITECTURE.md), [FIRMWARE_VARIANT_AUDIT.md](FIRMWARE_VARIANT_AUDIT.md), [FEATURES.md](FEATURES.md)

This document defines the **lifecycle**, **ownership**, and **maintenance cadence** for each firmware variant under `firmware/`. It exists so contributors and reviewers know, at a glance, which tree is the source of truth for a given change.

---

## Classification

Every firmware variant is labeled with exactly one of the following statuses:

| Label | Meaning | Accepts new features? | Accepts security fixes? | Build-gated? |
|---|---|:---:|:---:|:---:|
| **ACTIVE** | Primary development lane. Source of truth for new work. | Yes | Yes | No |
| **COMPATIBILITY** | Maintained lane for a specific user base (e.g. Arduino IDE). Changes backported from ACTIVE when feasible. | Selective | Yes | No |
| **SPECIALIZED** | Dedicated-purpose tree (board-specific, subsystem-focused). Scope is narrow by design. | Yes, within scope | Yes | No |
| **ARCHIVED** | Frozen historical reference. Read-only. CI blocks edits unless the commit contains `[archive-edit]`. | No | No (fork forward instead) | Yes (`#error` unless override defined) |
| **CONCEPT** | Research staged before any firmware exists: requirements, architecture sketch, open questions. Deliberately unbuildable (stub `#error`s on purpose); ships nothing. Becomes SPECIALIZED Phase 0 when work starts. | Design docs only | N/A (no code to fix) | Yes (`#error` stub) |

---

## Current Variants

| Path | Status | Board target | Primary use | Owner of new features |
|---|---|---|---|---|
| [`firmware/canary/`](./canary) | **ACTIVE** | XIAO ESP32-S3 Sense | Modular PlatformIO firmware, canonical onboarding UI/API | This tree |
| [`firmware/projects/canary-wap/`](./projects/canary-wap) | **COMPATIBILITY** | XIAO ESP32-S3 Sense | Arduino-IDE-first monolithic sketch, feature-rich WAP UX | Backport from `firmware/canary` when viable |
| [`firmware/projects/canary-vision/`](./projects/canary-vision) | **SPECIALIZED** | ESP32-C3 DevKit / XIAO ESP32-C3 / XIAO ESP32-S3 + Grove Vision AI V2 | Person detection + MQTT/HA publishing | This tree (scope: vision + MQTT) |
| [`firmware/projects/canary-sense/`](./projects/canary-sense) | **SPECIALIZED** | XIAO ESP32-C6 + Seeed MR60BHA2 (60GHz mmWave) | Radar presence (+ opt-in wellbeing vitals) + signed witness chain + MQTT/HA publishing | This tree (scope: radar sensing + MQTT) |
| [`firmware/projects/canary-display/`](./projects/canary-display) | **SPECIALIZED** | XIAO ESP32-S3 + Round Display (`watch`) / Waveshare ESP32-S3-Touch-LCD-4.3 (`dash`) | Fleet status display: MQTT subscribe + on-device Ed25519 chain verify + glance UI | This tree (scope: display + fleet rendering) |
| [`firmware/projects/canary-tincan/`](./projects/canary-tincan) | **SPECIALIZED** | Waveshare ESP32-S3-Touch-AMOLED-2.06 (`waveshare-esp32s3-amoled206`) | The Tin Can: a kids' wrist Canary — parent-witnessed LAN-only "strings" carrying knocks/tugs/stamps, plus the parent's Ring. **Phase 0: pure cores host-tested in CI, no build env yet** | This tree (scope: paired kid messaging + the Tin Can UI) |
| [`firmware/projects/canary-sentinel/`](./projects/canary-sentinel) | **SPECIALIZED** | XIAO ESP32-C6 (Standard/Heavy tiers) / XIAO ESP32-C3 (Lite) | Doorway/window multi-sensor fusion guardian (five presets on the host-tested `common/fusion` brain). **Phase 0: fusion core host-tested in CI, build envs compile-checked, not in `flavors.json`; hardware bench pending** | This tree (scope: sensor fusion presets) |
| [`firmware/projects/canary-companion/`](./projects/canary-companion) | **SPECIALIZED** | Waveshare ESP32-S3-Touch-AMOLED-2.06 (`waveshare-esp32s3-amoled206`) | The Night Watch (truly-dark AMOLED bedside clock) + the Pocket Canary (virtual pet). **Phase 0: pure cores host-tested in CI, no build env or runtime yet** | This tree (scope: Night Watch + Pocket Canary UI) |
| [`firmware/projects/canary-fence-guard/`](./projects/canary-fence-guard) | **CONCEPT** | XIAO ESP32-S3 + Wio-SX1262 (proposed) | Perimeter fence witness over Meshtastic/LoRa — research staged, firmware pending; `src/main_stub.cpp` `#error`s on purpose, nothing builds or ships | This tree (scope: fence sensing over LoRa), if requested |
| [`firmware/projects/canary-ota/`](./projects/canary-ota) | **SPECIALIZED** | ESP32 family | Standalone ESP-IDF OTA test/demo harness. The OTA engine itself was promoted to [`firmware/common/ota/`](./common/ota), which the ACTIVE variants consume; this tree is the harness that exercises it, and is built by no CI workflow | Engine: `common/ota` (CI-covered). Harness: this tree |
| [`firmware/common/`](./common) | shared | all | Board-agnostic module headers and implementations | ACTIVE tree changes drive this |
| [`firmware/boards/`](./boards) | shared | all | Pin maps + capability flags per board | Added alongside new board bring-up |
| [`firmware/configs/`](./configs) | shared | all | Product composition (feature flags, build profiles) | Updated with ACTIVE tree |

> **Removed:** the `canary-wap-snapshot` ARCHIVED tree (formerly `firmware/projects/_archive/canary-wap-snapshot/`, frozen 2026-02-20) was deleted on 2026-05-29. Its history remains in git; the WAP UX it captured lives on in the COMPATIBILITY tree (`firmware/projects/canary-wap/`) and the ACTIVE tree (`firmware/canary/`). The archiving process and `archive-guard` CI job below remain in force for any future archived trees.

---

## Maintenance Cadence

| Label | Expected review cadence | Acceptable staleness before triage |
|---|---|---|
| ACTIVE | Every sprint; CI must stay green | 14 days without a push triggers review |
| COMPATIBILITY | Monthly security + regression sweep | 60 days without a push triggers review |
| SPECIALIZED | Cadence driven by the owning subsystem roadmap | Flag if `git log` shows >90 days since last commit |
| ARCHIVED | Never touched except to graduate fixes forward | N/A (frozen) |
| CONCEPT | Revisited when a request issue lands (that is the graduation trigger) | N/A (no code to rot) |

"Triage" means: open an issue to either (a) push an update, (b) re-label the tree, or (c) graduate it to ARCHIVED.

---

## Graduation Paths

Variants move between labels via explicit, reviewed transitions. No label change happens silently.

```
      ┌─────────────────────────────────────────────────┐
      │                                                 │
      ▼                                                 │
  SPECIALIZED ──(broadened scope)──► ACTIVE             │
                                        │               │
                                        │(new           │
                                        │ primary takes │
                                        │ over)         │
                                        ▼               │
                                   COMPATIBILITY        │
                                        │               │
                                        │(user base     │
                                        │ sunset)       │
                                        ▼               │
                                   ARCHIVED ────────────┘
                                   (fork-forward fixes
                                    graduate forward,
                                    never back into
                                    archive)
```

### Rules

1. **Promoting to ACTIVE** requires retiring or re-labeling the current ACTIVE tree in the same change.
2. **Demoting ACTIVE → COMPATIBILITY** requires:
   - A replacement ACTIVE tree already present and passing CI
   - An explicit row update in this file and in [FIRMWARE_VARIANT_AUDIT.md](FIRMWARE_VARIANT_AUDIT.md)
3. **Demoting to ARCHIVED** requires:
   - Moving the tree under `firmware/projects/_archive/`
   - Adding a build-time `#error` guarded by a `SECURACV_ALLOW_ARCHIVED_BUILD` override macro
   - Replacing the tree's README with a FROZEN notice (date, reason, pointer to the active successor)
   - CI archive-guard job continues to fail on edits lacking the `[archive-edit]` marker
4. **Un-archiving is forbidden.** If archived code must return to active development, fork it forward as a new ACTIVE or SPECIALIZED tree and leave the archive untouched.

---

## CI Enforcement

- `archive-guard` job in `.github/workflows/firmware.yml` fails if any path under `firmware/projects/_archive/**` is modified without the string `[archive-edit]` in either the PR title/body or a commit message.
- `firmware/scripts/regression_check.sh` runs on every firmware PR and enforces:
  - No hardcoded default passwords in ACTIVE/COMPATIBILITY trees
  - `mbedTLS` APIs use the `_ret` suffix only (no deprecated signatures)
  - Camera `PWDN=-1` for XIAO ESP32-S3 Sense
  - SD SPI pins match the board pin map
  - BLE guarded by feature flag (CVE-2025-27840)
  - `web_ui.h` stays under the size ceiling
  - No `HTTP` URLs where `HTTPS` is expected

Archived trees are exempt from `regression_check.sh` since they cannot build.

---

## Reviewer Checklist

When reviewing a firmware PR, confirm:

- [ ] The PR touches the ACTIVE tree for new features (or a SPECIALIZED tree within scope).
- [ ] Any COMPATIBILITY-tree change is either a security fix or an explicitly scoped backport.
- [ ] No files under `firmware/projects/_archive/**` are touched unless the PR is labeled `[archive-edit]` and the change is a forward-graduation.
- [ ] [FIRMWARE_VARIANT_AUDIT.md](FIRMWARE_VARIANT_AUDIT.md) and [FEATURES.md](FEATURES.md) are updated if feature parity shifts.
- [ ] Label changes (ACTIVE/COMPATIBILITY/SPECIALIZED/ARCHIVED) are reflected in this file in the same PR.

---

## See Also

- [ARCHITECTURE.md](ARCHITECTURE.md) — module composition rules (the "what can import what" contract)
- [HARDWARE.md](HARDWARE.md) — board support tiers (verified/community/compile-tested) — the *hardware* axis, orthogonal to the variant (code-lane) axis here
- [PORTING.md](PORTING.md) — new-board bring-up guide and submission checklist
- [FIRMWARE_VARIANT_AUDIT.md](FIRMWARE_VARIANT_AUDIT.md) — per-variant risk analysis
- [FEATURES.md](FEATURES.md) — feature-parity matrix across variants
- [canary/CONSOLIDATION.md](canary/CONSOLIDATION.md) — roadmap for collapsing the Arduino COMPATIBILITY tree into the ACTIVE canary PIO tree
- [scripts/regression_check.sh](./scripts/regression_check.sh) — automated regression gate
