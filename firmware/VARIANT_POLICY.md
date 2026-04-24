# Firmware Variant Policy

**Last updated:** 2026-04-17
**Owner:** firmware maintainers
**Companion docs:** [ARCHITECTURE.md](ARCHITECTURE.md), [FIRMWARE_VARIANT_AUDIT.md](FIRMWARE_VARIANT_AUDIT.md), [FEATURES.md](FEATURES.md)

This document defines the **lifecycle**, **ownership**, and **maintenance cadence** for each firmware variant under `firmware/`. It exists so contributors and reviewers know, at a glance, which tree is the source of truth for a given change.

---

## Classification

Every firmware variant is labelled with exactly one of the following statuses:

| Label | Meaning | Accepts new features? | Accepts security fixes? | Build-gated? |
|---|---|:---:|:---:|:---:|
| **ACTIVE** | Primary development lane. Source of truth for new work. | Yes | Yes | No |
| **COMPATIBILITY** | Maintained lane for a specific user base (e.g. Arduino IDE). Changes backported from ACTIVE when feasible. | Selective | Yes | No |
| **SPECIALIZED** | Dedicated-purpose tree (board-specific, subsystem-focused). Scope is narrow by design. | Yes, within scope | Yes | No |
| **ARCHIVED** | Frozen historical reference. Read-only. CI blocks edits unless the commit contains `[archive-edit]`. | No | No (fork forward instead) | Yes (`#error` unless override defined) |

---

## Current Variants

| Path | Status | Board target | Primary use | Owner of new features |
|---|---|---|---|---|
| [`firmware/canary/`](./canary) | **ACTIVE** | XIAO ESP32-S3 Sense | Modular PlatformIO firmware, canonical onboarding UI/API | This tree |
| [`firmware/projects/canary-wap/`](./projects/canary-wap) | **COMPATIBILITY** | XIAO ESP32-S3 Sense | Arduino-IDE-first monolithic sketch, feature-rich WAP UX | Backport from `firmware/canary` when viable |
| [`firmware/projects/canary-vision/`](./projects/canary-vision) | **SPECIALIZED** | ESP32-C3 + Grove Vision AI V2 | Person detection + MQTT/HA publishing | This tree (scope: vision + MQTT) |
| [`firmware/projects/canary-ota/`](./projects/canary-ota) | **SPECIALIZED** | ESP32 family | OTA A/B update subsystem + rollback safety | This tree (scope: OTA) |
| [`firmware/projects/_archive/canary-wap-snapshot/`](./projects/_archive/canary-wap-snapshot) | **ARCHIVED** | XIAO ESP32-S3 Sense | Frozen reference (2026-02-20) of the WAP sketch before modularization | None — do not edit |
| [`firmware/common/`](./common) | shared | all | Board-agnostic module headers and implementations | ACTIVE tree changes drive this |
| [`firmware/boards/`](./boards) | shared | all | Pin maps + capability flags per board | Added alongside new board bring-up |
| [`firmware/configs/`](./configs) | shared | all | Product composition (feature flags, build profiles) | Updated with ACTIVE tree |

---

## Maintenance Cadence

| Label | Expected review cadence | Acceptable staleness before triage |
|---|---|---|
| ACTIVE | Every sprint; CI must stay green | 14 days without a push triggers review |
| COMPATIBILITY | Monthly security + regression sweep | 60 days without a push triggers review |
| SPECIALIZED | Cadence driven by the owning subsystem roadmap | Flag if `git log` shows >90 days since last commit |
| ARCHIVED | Never touched except to graduate fixes forward | N/A (frozen) |

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

1. **Promoting to ACTIVE** requires retiring or re-labelling the current ACTIVE tree in the same change.
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
- [ ] No files under `firmware/projects/_archive/**` are touched unless the PR is labelled `[archive-edit]` and the change is a forward-graduation.
- [ ] [FIRMWARE_VARIANT_AUDIT.md](FIRMWARE_VARIANT_AUDIT.md) and [FEATURES.md](FEATURES.md) are updated if feature parity shifts.
- [ ] Label changes (ACTIVE/COMPATIBILITY/SPECIALIZED/ARCHIVED) are reflected in this file in the same PR.

---

## See Also

- [ARCHITECTURE.md](ARCHITECTURE.md) — module composition rules (the "what can import what" contract)
- [FIRMWARE_VARIANT_AUDIT.md](FIRMWARE_VARIANT_AUDIT.md) — per-variant risk analysis
- [FEATURES.md](FEATURES.md) — feature-parity matrix across variants
- [canary/CONSOLIDATION.md](canary/CONSOLIDATION.md) — roadmap for collapsing the Arduino COMPATIBILITY tree into the ACTIVE canary PIO tree
- [scripts/regression_check.sh](./scripts/regression_check.sh) — automated regression gate
