# Hardware Support Policy

**Last updated:** 2026-07-11
**Owner:** firmware maintainers
**Companion docs:** [PORTING.md](PORTING.md) (how to add a board),
[VARIANT_POLICY.md](VARIANT_POLICY.md) (firmware *tree* lifecycle — a
different axis: variants are code lanes, this document is about *hardware*),
[boards/boards.json](boards/boards.json) (the machine-readable registry).

This document defines what "supported" means for a board in the SecuraCV
firmware tree, how a board earns (and loses) that status, and how community
members contribute hardware support without maintainers owning every device.

The model borrows deliberately from the two open-source firmware projects
that support the widest hardware fleets in existence — Marlin and Klipper —
adapted to this repo's scale (see
[docs/marlin_klipper_learnings.md](../docs/marlin_klipper_learnings.md) for
the full study).

---

## Support Tiers

Every board in [`boards/boards.json`](boards/boards.json) carries exactly one
tier:

| Tier | Meaning | CI | Real-hardware evidence |
|------|---------|----|------------------------|
| **verified** | Validated on physical hardware by a maintainer | Built on every PR | Required: `tier_evidence` names the runbook/doc/report |
| **community** | Validated on physical hardware by a community member | Built on every PR | Required: `tier_evidence` links the hardware test report issue |
| **compile-tested** | Compiles in CI; never validated on real hardware | Built on every PR | None — flash at your own risk, expect `VERIFY` notes in the pin map |

Two rules make the tiers trustworthy:

1. **Every registered board is CI-built on every PR, forever.** A board
   enters the registry only together with a build environment and a
   [`flavors.json`](flavors.json) entry, so no contribution can silently
   rot. (This is Klipper's strongest property: every shipped config is
   loaded by CI on every commit — a breaking change *must* update all
   configs it breaks, in the same PR.)
2. **No tier above compile-tested without evidence.** The registry's
   `tier_evidence` field must point at a repo document or a GitHub issue
   proving the hardware test. CI
   ([`scripts/check_board_registry.py`](scripts/check_board_registry.py))
   fails if it's missing or dangling. Honesty is enforced mechanically:
   the badge can never overclaim.

### Promotion

- **compile-tested → community**: someone runs the board's bring-up runbook
  (or an equivalent documented test) on real hardware and files a
  **Hardware Test Report** issue (template in `.github/ISSUE_TEMPLATE/`)
  with serial logs and per-check results. A maintainer reviews the report,
  sets `tier: community` and `tier_evidence: <issue URL>` in `boards.json`,
  and clears any retired `VERIFY` notes in the pin map in the same PR.
- **community → verified**: a maintainer reproduces the validation on their
  own hardware, typically as part of a release gate runbook.

### Demotion

- A board whose flavor is demoted to ARCHIVED under
  [VARIANT_POLICY.md](VARIANT_POLICY.md) leaves the registry with it.
- If a supported board is revised by its vendor such that the pin map no
  longer holds (this happens — Seeed has revised the Round Display before),
  the tier drops back to **compile-tested** until someone re-validates on
  the new revision, and the board README documents the revision split.

---

## What we support at all (scope)

SecuraCV is not trying to run on everything with a radio. A board belongs in
this tree when:

- **It serves a witness role**: sensing (camera, radar, CSI), display, or
  transport for the witness chain. Novelty ports don't carry their
  maintenance cost.
- **It's obtainable**: a community member can buy one today and reproduce
  the setup. (Klipper's rule of thumb — support hardware with a real user
  base, not one-off builds — scaled to our size: if only one unit exists,
  it lives in a fork until others can follow.)
- **The privacy invariants hold on it**: a board whose physical build
  contradicts a flavor's privacy posture (e.g. a hidden microphone array on
  a "no microphone by construction" display) is documented as unsuitable in
  the board README, even if the firmware runs — see the Waveshare 4.3C note
  in [`boards/waveshare-esp32s3-lcd43/README.md`](boards/waveshare-esp32s3-lcd43/README.md).

Hardware requests that don't fit are still welcome as **Hardware Support
Request** issues — the answer may be "here's how to port it yourself"
([PORTING.md](PORTING.md)) rather than "we'll own it."

---

## Current Boards

The authoritative list — with tier, evidence, and consuming flavors — is
[`boards/boards.json`](boards/boards.json); the human-readable index is
[`boards/README.md`](boards/README.md). CI keeps both consistent, so this
document doesn't duplicate the table.

---

## The contract that keeps this cheap to maintain

Community hardware support only scales when a new board is a **data
contribution, not a code fork** (the single most important lesson from both
Marlin and Klipper). The layering rules in
[ARCHITECTURE.md](ARCHITECTURE.md) exist for exactly this:

- **Pins are data.** `boards/<id>/pins/pins.h` contains only `#define`s —
  CI rejects logic in pin files. A board PR can't smuggle in behavior.
- **Configs are data.** Feature selection lives in `configs/`, never in
  `#ifdef BOARD_X` forks inside `common/`. If code must diverge per board,
  it becomes a capability abstraction (`HAS_*` flag + HAL module), not a
  fork.
- **Misconfiguration fails at compile time.** A config that enables a
  feature the board can't carry dies with an actionable `#error` naming the
  exact flag and the two ways out
  ([`common/core/feature_sanity.h`](common/core/feature_sanity.h) — the
  Marlin SanityCheck.h pattern). Support burden shifts from "debug a field
  device" to "read the build error."
- **One manifest entry per axis.** New board → one `boards.json` entry;
  new build target → one `flavors.json` entry. CI matrices are generated
  from the manifests; nobody hand-writes workflow jobs for new hardware.

---

## Config compatibility across releases

Breaking changes to configuration files (renamed flags, changed defaults
with behavioral impact, removed options) are logged in
[CONFIG_CHANGES.md](CONFIG_CHANGES.md), newest first, in the same PR that
makes the change — Klipper's `Config_Changes.md` discipline. A PR that
breaks shipped configs must also update every shipped config it breaks.

---

## Reviewer checklist for hardware PRs

- [ ] `boards.json` entry present; tier is `compile-tested` for new boards
      (hardware evidence comes later, from someone who owns the device).
- [ ] `python3 firmware/scripts/check_board_registry.py` passes.
- [ ] Pin map cites its source (vendor schematic/wiki) and marks every
      unverified line with a `VERIFY` note.
- [ ] Board README documents constraints, gotchas, and sibling SKUs —
      especially any privacy-posture caveat.
- [ ] Build env + `flavors.json` entry exist → the board is in the CI
      matrix.
- [ ] No new `#ifdef BOARD_*` forks in `common/` (capability flags + HAL
      instead).
- [ ] Tier claims match reality: no "verified" without evidence.
