# Community Hardware Support — Lessons from Marlin and Klipper

**Date:** 2026-07-11
**Status:** Adopted — this study drove [`firmware/HARDWARE.md`](../firmware/HARDWARE.md),
[`firmware/PORTING.md`](../firmware/PORTING.md),
[`firmware/boards/boards.json`](../firmware/boards/boards.json),
[`firmware/CONFIG_CHANGES.md`](../firmware/CONFIG_CHANGES.md), and
[`firmware/common/core/feature_sanity.h`](../firmware/common/core/feature_sanity.h).
Companion to [`openipc_architecture_learnings.md`](openipc_architecture_learnings.md).

Marlin and Klipper are the two open-source firmware projects that support
the widest community hardware fleets in existence — Marlin runs on 300+
boards across a dozen MCU architectures; Klipper ships ~150 example
configurations and compiles ~50 MCU targets on every commit. Both survive
thousands of community hardware contributions per year with small
maintainer teams. This study looked at *how*, and what transfers to
SecuraCV's multi-board Canary firmware.

---

## How Marlin does it

- **Configuration is data, in a separate repo.**
  `MarlinFirmware/Configurations` holds community configs in a
  vendor/model tree (`config/examples/Creality/…`). Contributions land on
  dedicated `import-*` branches; `release-*` branches pair configs with
  firmware releases. "Add my printer" never touches core-code review.
- **A central board registry + per-board pin files.**
  `boards.h` assigns every board an ID in MCU-family-partitioned ranges;
  `src/pins/<arch>/pins_<BOARD>.h` holds pins only. A HAL directory per
  platform (`src/HAL/AVR`, `STM32`, `ESP32`, …) keeps board breadth from
  forking app logic.
- **Misconfiguration dies at compile time, with the fix in the message.**
  `SanityCheck.h` — thousands of `#error`/`static_assert` checks like
  *"MOTHERBOARD is required. You must '#define MOTHERBOARD BOARD_MYNAME'"*.
  Stale configs are caught by comparing `CONFIGURATION_H_VERSION` against
  the firmware's expected value. The error message *is* the documentation.
- **CI compiles a 100+ target matrix on every PR** (`buildroot/tests/`,
  one scripted feature-combo build per platform), plus dedicated
  workflows validating `boards.h` and the pins files themselves.

## How Klipper does it

- **One `config/` directory, five filename prefixes, a written rulebook.**
  `printer-<vendor>-<model>-<year>.cfg`, `generic-<board>.cfg`, `kit-`,
  `sample-`, `example-`. `docs/Example_Configs.md` is a reviewable
  contract: stock configuration only, required header stating the MCU,
  no filament-specific values, no disabled safety systems, no debug
  features, ~100-active-users popularity threshold. Review is against the
  rulebook, not maintainer taste.
- **Every shipped config is CI-loaded forever.** A new config must be
  added to `test/klippy/printers.test`; CI starts Klipper against every
  example config on every commit. A breaking change *must* update all
  shipped configs in the same PR…
- **…and be logged for users.** `docs/Config_Changes.md` is a dated,
  running log of non-backwards-compatible config changes — the "your
  config needs editing" list, separate from the changelog.
- **One canonical option reference.** `docs/Config_Reference.md`
  documents every option once; example configs are forbidden from
  duplicating field docs ("doing so creates a maintenance burden").
- **Bounded scope, explicit escape valve.** Customized/long-tail configs
  go to the community Discourse, not the repo; GitHub issues are not the
  support channel. Coverage is traded for a config set small enough that
  CI exercises all of it.
- **A documented MCU porting recipe** (`docs/Code_Overview.md`): Kconfig +
  Makefile, serial up, timers, GPIO, then peripherals.

## The cross-cutting pattern

Three properties recur, and they're the actual lesson:

1. **Hardware contributions are data-only PRs** reviewed against a written
   rulebook. Pins/configs never carry logic, so a hardware PR can't break
   behavior and doesn't need deep review.
2. **Every accepted contribution is CI-enforced forever.** Registration
   buys rot-protection; in exchange the port must keep building. Nothing
   supported is untested; nothing untested is called supported.
3. **Misconfiguration fails at build/load time with an actionable
   message**, shifting support burden from "debug a field device" to
   "read the error."

---

## What SecuraCV already had

The firmware tree was Marlin-inspired from the start
([`firmware/ARCHITECTURE.md`](../firmware/ARCHITECTURE.md) says so
explicitly): `boards/` pin maps with capability flags, `configs/` as data,
composition only in `envs/`+`projects/`, a CI matrix generated from
[`flavors.json`](../firmware/flavors.json) (one manifest entry = one CI
leg, no hand-written jobs), `regression_check.sh` guards, and
[`VARIANT_POLICY.md`](../firmware/VARIANT_POLICY.md) lifecycle labels.

## What we adopted (2026-07)

| Lesson | Source | What landed |
|--------|--------|-------------|
| Machine-readable board registry | Marlin `boards.h` | [`boards/boards.json`](../firmware/boards/boards.json) — id, MCU, tier, evidence, consuming flavors; the board-level `flavors.json` |
| Registry enforced by CI | Marlin `ci-validate-boards/pins` | [`scripts/check_board_registry.py`](../firmware/scripts/check_board_registry.py): registry ↔ dirs ↔ pins ↔ envs ↔ README consistency; "pins are data" (`#define`-only) mechanically enforced |
| Support tiers with mandatory evidence | Both (adapted) | [`HARDWARE.md`](../firmware/HARDWARE.md): verified / community / compile-tested; `tier_evidence` required above compile-tested, CI-checked — the badge can't overclaim (the two display boards honestly carry compile-tested + a promotion runbook) |
| Compile-time sanity checks with actionable messages | Marlin `SanityCheck.h` | [`common/core/feature_sanity.h`](../firmware/common/core/feature_sanity.h): `FEATURE_*` vs `HAS_*` cross-checks, host-tested by [`scripts/test_feature_sanity.sh`](../firmware/scripts/test_feature_sanity.sh), wired into vision/sense/display trees |
| Written porting rulebook + reviewer checklist | Klipper `Example_Configs.md`, Marlin docs | [`PORTING.md`](../firmware/PORTING.md) — data-only contract, VERIFY-note currency, tier path; reviewer checklist in `HARDWARE.md` |
| Dated config breaking-change log | Klipper `Config_Changes.md` | [`CONFIG_CHANGES.md`](../firmware/CONFIG_CHANGES.md) + rule 5 in [`configs/README.md`](../firmware/configs/README.md): break a config → log it and fix every shipped config in the same PR |
| Structured hardware intake | Both (issue forms) | `.github/ISSUE_TEMPLATE/hardware_support_request.yml` + `hardware_test_report.yml` — the test report doubles as permanent tier evidence |

## What we deliberately did NOT adopt

- **A separate Configurations repository** (Marlin). Right at 300 boards;
  overhead at 7. Our configs stay in-tree where CI builds them. Revisit if
  community configs ever outnumber shipped ones.
- **Per-release config branches / `CONFIGURATION_H_VERSION` stamps**
  (Marlin). Heavy machinery for a project that doesn't yet have config
  populations spanning many releases. `CONFIG_CHANGES.md` covers today's
  need; version stamps are the natural next step if it stops sufficing.
- **Kconfig/menuconfig** (Klipper). PlatformIO envs + config headers
  already give us reproducible, reviewable build targets.
- **A numeric popularity threshold** (Klipper's ~100 users). Scaled down
  to a principle in `HARDWARE.md`: obtainable hardware serving a witness
  role; one-off builds live in forks.
- **Runtime configuration files** (Klipper `printer.cfg`). Our privacy
  invariants are compile-time enforced on purpose — runtime
  reconfiguration of witness behavior is the thing this project refuses
  to build (see `CONTRIBUTING.md`).
